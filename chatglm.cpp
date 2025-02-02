#include "chatglm.h"
#include <algorithm>
#include <codecvt>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <google/protobuf/stubs/strutil.h>
#include <iomanip>
#include <iostream>
#include <locale>
#include <numeric>
#include <random>
#include <regex>
#include <string>
#include <sys/stat.h>
#include <thread>

#ifdef __has_include
#if __has_include(<unistd.h>)
#include <unistd.h>
#if defined(_POSIX_MAPPED_FILES)
#include <sys/mman.h>
#endif
#if defined(_POSIX_MEMLOCK_RANGE)
#include <sys/resource.h>
#endif
#endif
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <stdio.h>
#include <windows.h>
#endif

#ifdef GGML_USE_CUBLAS
#include <ggml-cuda.h>
#endif

namespace chatglm {

static std::string shape_to_string(ggml_tensor *tensor) {
    std::ostringstream oss;
    oss << '[';
    for (int i = tensor->n_dims - 1; i >= 0; i--) {
        oss << tensor->ne[i] << (i > 0 ? ", " : "");
    }
    oss << ']';
    return oss.str();
}

static std::string strides_to_string(ggml_tensor *tensor) {
    std::ostringstream oss;
    oss << '[';
    for (int i = tensor->n_dims - 1; i >= 0; i--) {
        oss << tensor->nb[i] << (i > 0 ? ", " : "");
    }
    oss << ']';
    return oss.str();
}

std::string to_string(ggml_tensor *tensor, bool with_data) {
    std::ostringstream oss;
    oss << "ggml_tensor(";

    if (with_data) {
        if (tensor->n_dims > 3)
            oss << "[";
        for (int i3 = 0; i3 < tensor->ne[3]; i3++) {
            if (tensor->n_dims > 2)
                oss << (i3 > 0 ? ",\n\n[" : "[");
            for (int i2 = 0; i2 < tensor->ne[2]; i2++) {
                if (tensor->n_dims > 1)
                    oss << (i2 > 0 ? ",\n\n[" : "[");
                for (int i1 = 0; i1 < tensor->ne[1]; i1++) {
                    oss << (i1 > 0 ? ",\n[" : "[");
                    for (int i0 = 0; i0 < tensor->ne[0]; i0++) {
                        auto ptr = (char *)tensor->data + i3 * tensor->nb[3] + i2 * tensor->nb[2] + i1 * tensor->nb[1] +
                                   i0 * tensor->nb[0];
                        oss << (i0 > 0 ? ", " : "");
                        if (tensor->type == GGML_TYPE_I32) {
                            oss << *(int *)ptr;
                        } else {
                            float val;
                            if (tensor->type == GGML_TYPE_F32) {
                                val = *(float *)ptr;
                            } else if (tensor->type == GGML_TYPE_F16) {
                                val = ggml_fp16_to_fp32(*(ggml_fp16_t *)ptr);
                            } else {
                                CHATGLM_THROW << "unimplemented";
                            }
                            oss << std::setw(7) << std::fixed << std::setprecision(4) << val;
                        }
                    }
                    oss << "]";
                }
                if (tensor->n_dims > 1)
                    oss << "]";
            }
            if (tensor->n_dims > 2)
                oss << "]";
        }
        if (tensor->n_dims > 3)
            oss << "]";
        oss << ", ";
    }

    oss << "shape=" << shape_to_string(tensor) << ", stride=" << strides_to_string(tensor) << ")";
    return oss.str();
}

ggml_tensor *tensor_assign_buffers(ggml_tensor *tensor) {
#ifdef GGML_USE_CUBLAS
    ggml_cuda_assign_buffers(tensor);
#endif
    return tensor;
}

ggml_tensor *tensor_to_device(ggml_tensor *tensor) {
#ifdef GGML_USE_CUBLAS
    if (tensor->backend == GGML_BACKEND_CPU) {
        tensor->backend = GGML_BACKEND_GPU;
        ggml_cuda_transform_tensor(tensor->data, tensor);
    }
#endif
    return tensor;
}

ggml_tensor *tensor_to_cpu(ggml_tensor *tensor) {
#ifdef GGML_USE_CUBLAS
    if (tensor->backend != GGML_BACKEND_CPU) {
        ggml_cuda_free_data(tensor);
        tensor->backend = GGML_BACKEND_CPU;
    }
#endif
    return tensor;
}

const std::string ToolCallMessage::TYPE_FUNCTION = "function";
const std::string ToolCallMessage::TYPE_CODE = "code";

const std::string ChatMessage::ROLE_USER = "user";
const std::string ChatMessage::ROLE_ASSISTANT = "assistant";
const std::string ChatMessage::ROLE_SYSTEM = "system";
const std::string ChatMessage::ROLE_OBSERVATION = "observation";

void BaseTokenizer::check_chat_messages(const std::vector<ChatMessage> &messages) {
    std::string target_role = ChatMessage::ROLE_USER;
    for (size_t i = 0; i < messages.size(); i++) {
        if (messages[i].role != ChatMessage::ROLE_USER && messages[i].role != ChatMessage::ROLE_ASSISTANT) {
            continue;
        }
        CHATGLM_CHECK(messages[i].role == target_role)
            << "expect messages[" << i << "].role to be " << target_role << ", but got " << messages[i].role;
        target_role = (target_role == ChatMessage::ROLE_USER) ? ChatMessage::ROLE_ASSISTANT : ChatMessage::ROLE_USER;
    }
    CHATGLM_CHECK(target_role == ChatMessage::ROLE_ASSISTANT)
        << "expect last message role to be " << ChatMessage::ROLE_USER << ", but got " << ChatMessage::ROLE_ASSISTANT;
}

std::vector<ChatMessage> BaseTokenizer::filter_user_assistant_messages(const std::vector<ChatMessage> &messages) {
    std::vector<ChatMessage> user_assistant_messages;
    user_assistant_messages.reserve(messages.size());
    for (const auto &msg : messages) {
        if (msg.role == ChatMessage::ROLE_USER || msg.role == ChatMessage::ROLE_ASSISTANT) {
            user_assistant_messages.emplace_back(msg);
        }
    }
    return user_assistant_messages;
}

// Adapted from https://github.com/ggerganov/llama.cpp/blob/master/llama.cpp
void ggml_graph_compute_helper(std::vector<uninitialized_char> &buf, ggml_cgraph *graph, int n_threads) {
    struct ggml_cplan plan = ggml_graph_plan(graph, n_threads);

    if (plan.work_size > 0) {
        buf.resize(plan.work_size);
        plan.work_data = (uint8_t *)buf.data();
    }

    ggml_graph_compute(graph, &plan);
}

// for debugging purpose
[[maybe_unused]] static inline ggml_tensor *add_zero(ggml_context *ctx, ggml_tensor *tensor) {
    ggml_tensor *zeros = ggml_new_tensor(ctx, GGML_TYPE_F32, tensor->n_dims, tensor->ne);
    ggml_set_f32(zeros, 0);
    tensor_to_device(zeros);
    ggml_tensor *out = tensor_assign_buffers(ggml_add(ctx, tensor, zeros));
    return out;
}

void ModelContext::init_device_context() {
#ifdef GGML_USE_METAL
    ctx_metal = make_unique_ggml_metal_context(1);

    const size_t max_size = ggml_get_max_tensor_size(ctx_w.get());

    void *weight_data = weight_buffer.empty() ? ggml_get_mem_buffer(ctx_w.get()) : (void *)weight_buffer.data();
    size_t weight_size = weight_buffer.empty() ? ggml_get_mem_size(ctx_w.get()) : weight_buffer.size();
    CHATGLM_CHECK(ggml_metal_add_buffer(ctx_metal.get(), "weights", weight_data, weight_size, max_size));

    CHATGLM_CHECK(ggml_metal_add_buffer(ctx_metal.get(), "kv", ggml_get_mem_buffer(ctx_kv.get()),
                                        ggml_get_mem_size(ctx_kv.get()), 0));

    void *compute_data = ctx_b ? ggml_get_mem_buffer(ctx_b.get()) : compute_buffer.data();
    size_t compute_size = ctx_b ? ggml_get_mem_size(ctx_b.get()) : compute_buffer.size();
    CHATGLM_CHECK(ggml_metal_add_buffer(ctx_metal.get(), "compute", compute_data, compute_size, 0));

    CHATGLM_CHECK(ggml_metal_add_buffer(ctx_metal.get(), "scratch", scratch.data, scratch.size, 0));
#endif
}

// ===== streamer =====

void StreamerGroup::put(const std::vector<int> &output_ids) {
    for (auto &streamer : streamers_) {
        streamer->put(output_ids);
    }
}

void StreamerGroup::end() {
    for (auto &streamer : streamers_) {
        streamer->end();
    }
}

// reference: https://stackoverflow.com/questions/216823/how-to-trim-a-stdstring

// trim from start (in place)
static inline void ltrim(std::string &s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) { return !std::isspace(ch); }));
}

// trim from end (in place)
static inline void rtrim(std::string &s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), s.end());
}

// trim from both ends (in place)
static inline void trim(std::string &s) {
    rtrim(s);
    ltrim(s);
}

void TextStreamer::put(const std::vector<int> &output_ids) {
    if (is_prompt_) {
        // skip prompt
        is_prompt_ = false;
        return;
    }

    static const std::vector<char> puncts{',', '!', ':', ';', '?'};

    token_cache_.insert(token_cache_.end(), output_ids.begin(), output_ids.end());
    std::string text = tokenizer_->decode(token_cache_);
    if (is_first_line_) {
        ltrim(text);
    }
    if (text.empty()) {
        return;
    }

    std::string printable_text;
    if (text.back() == '\n') {
        // flush the cache after newline
        printable_text = text.substr(print_len_);
        is_first_line_ = false;
        token_cache_.clear();
        print_len_ = 0;
    } else if (std::find(puncts.begin(), puncts.end(), text.back()) != puncts.end()) {
        // last symbol is a punctuation, hold on
    } else if (text.size() >= 3 && text.compare(text.size() - 3, 3, "�") == 0) {
        // ends with an incomplete token, hold on
    } else {
        printable_text = text.substr(print_len_);
        print_len_ = text.size();
    }

    os_ << printable_text << std::flush;
}

void TextStreamer::end() {
    std::string text = tokenizer_->decode(token_cache_);
    if (is_first_line_) {
        ltrim(text);
    }
    os_ << text.substr(print_len_) << std::endl;
    is_prompt_ = true;
    is_first_line_ = true;
    token_cache_.clear();
    print_len_ = 0;
}

void PerfStreamer::put(const std::vector<int> &output_ids) {
    CHATGLM_CHECK(!output_ids.empty());
    if (num_prompt_tokens_ == 0) {
        // before prompt eval
        start_us_ = ggml_time_us();
        num_prompt_tokens_ = output_ids.size();
    } else {
        if (num_output_tokens_ == 0) {
            // first new token
            prompt_us_ = ggml_time_us();
        }
        num_output_tokens_ += output_ids.size();
    }
}

void PerfStreamer::reset() {
    start_us_ = prompt_us_ = end_us_ = 0;
    num_prompt_tokens_ = num_output_tokens_ = 0;
}

std::string PerfStreamer::to_string() const {
    std::ostringstream oss;
    oss << "prompt time: " << prompt_total_time_us() / 1000.f << " ms / " << num_prompt_tokens() << " tokens ("
        << prompt_token_time_us() / 1000.f << " ms/token)\n"
        << "output time: " << output_total_time_us() / 1000.f << " ms / " << num_output_tokens() << " tokens ("
        << output_token_time_us() / 1000.f << " ms/token)\n"
        << "total time: " << (prompt_total_time_us() + output_total_time_us()) / 1000.f << " ms";
    return oss.str();
}

#ifdef _POSIX_MAPPED_FILES
MappedFile::MappedFile(const std::string &path) {
    int fd = open(path.c_str(), O_RDONLY);
    CHATGLM_CHECK(fd > 0) << "cannot open file " << path << ": " << strerror(errno);

    struct stat sb;
    CHATGLM_CHECK(fstat(fd, &sb) == 0) << strerror(errno);
    size = sb.st_size;

    data = (char *)mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    CHATGLM_CHECK(data != MAP_FAILED) << strerror(errno);

    CHATGLM_CHECK(close(fd) == 0) << strerror(errno);
}

MappedFile::~MappedFile() { CHATGLM_CHECK(munmap(data, size) == 0) << strerror(errno); }
#elif defined(_WIN32)
MappedFile::MappedFile(const std::string &path) {

    int fd = open(path.c_str(), O_RDONLY);
    CHATGLM_CHECK(fd > 0) << "cannot open file " << path << ": " << strerror(errno);

    struct _stat64 sb;
    CHATGLM_CHECK(_fstat64(fd, &sb) == 0) << strerror(errno);
    size = sb.st_size;

    HANDLE hFile = (HANDLE)_get_osfhandle(fd);

    HANDLE hMapping = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    CHATGLM_CHECK(hMapping != NULL) << strerror(errno);

    data = (char *)MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(hMapping);

    CHATGLM_CHECK(data != NULL) << strerror(errno);

    CHATGLM_CHECK(close(fd) == 0) << strerror(errno);
}

MappedFile::~MappedFile() { CHATGLM_CHECK(UnmapViewOfFile(data)) << strerror(errno); }
#endif

void ModelLoader::seek(int64_t offset, int whence) {
    if (whence == SEEK_SET) {
        ptr = data + offset;
    } else if (whence == SEEK_CUR) {
        ptr += offset;
    } else if (whence == SEEK_END) {
        ptr = data + size + offset;
    } else {
        CHATGLM_THROW << "invalid seek mode " << whence;
    }
}

std::string ModelLoader::read_string(size_t length) {
    std::string s(ptr, ptr + length);
    ptr += length;
    return s;
}

void ModelLoader::checked_read_tensor_meta(const std::string &name, int target_ndim, int64_t *target_ne,
                                           ggml_type target_dtype) {
    // read and check tensor name
    {
        int name_size = read_basic<int>();
        CHATGLM_CHECK(name_size == (int)name.size())
            << "tensor " << name << " name size mismatch: expect " << name.size() << " but got " << name_size;
        std::string weight_name = read_string(name_size);
        CHATGLM_CHECK(weight_name == name) << "tensor name mismatch: expect " << name << " but got " << weight_name;
    }

    // read and check tensor shape
    {
        int ndim = read_basic<int>();
        CHATGLM_CHECK(ndim == target_ndim)
            << "tensor " << name << " ndim mismatch: expect " << target_ndim << " but got " << ndim;
        for (int i = ndim - 1; i >= 0; i--) {
            int dim_size = read_basic<int>();
            CHATGLM_CHECK(dim_size == target_ne[i]) << "tensor " << name << " shape mismatch at dim " << i
                                                    << ": expect " << target_ne[i] << " but got " << dim_size;
        }
    }

    // read and check tensor dtype
    {
        ggml_type dtype = (ggml_type)read_basic<int>();
        CHATGLM_CHECK(dtype == target_dtype)
            << "tensor " << name << " dtype mismatch: expect " << target_dtype << " but got " << dtype;
    }
}

void *ModelLoader::read_tensor_data(size_t nbytes) {
    constexpr int64_t MEM_ALIGNED = 16;
    const int64_t data_offset = (tell() + (MEM_ALIGNED - 1)) & ~(MEM_ALIGNED - 1);
    void *tensor_data = data + data_offset;
    seek(data_offset + nbytes, SEEK_SET);
    return tensor_data;
}

void ModelLoader::read_tensor(const std::string &name, ggml_tensor *tensor) {
    checked_read_tensor_meta(name, tensor->n_dims, tensor->ne, tensor->type);
    tensor->data = read_tensor_data(ggml_nbytes(tensor));
}

// ===== modules =====

ggml_tensor *Embedding::forward(ModelContext *ctx, ggml_tensor *input) const {
    ggml_tensor *output = ggml_get_rows(ctx->ctx_b.get(), weight, input);
    return output;
}

ggml_tensor *Linear::forward(ModelContext *ctx, ggml_tensor *input) const {
    // input: [seqlen, in_features]
    ggml_context *gctx = ctx->ctx_b.get();
    ggml_tensor *output = tensor_assign_buffers(ggml_mul_mat(gctx, weight, input)); // [seqlen, out_features]
    if (bias) {
        output = tensor_assign_buffers(ggml_add_inplace(gctx, output, bias));
    }
    return output;
}

ggml_tensor *LayerNorm::forward(ModelContext *ctx, ggml_tensor *input) const {
    // input: [seqlen, normalized_shape]
    ggml_context *gctx = ctx->ctx_b.get();
    auto ggml_norm_fn = inplace ? ggml_norm_inplace : ggml_norm;
    ggml_tensor *output = tensor_assign_buffers(ggml_norm_fn(gctx, input, eps));
    output = tensor_assign_buffers(ggml_mul_inplace(gctx, output, weight));
    output = tensor_assign_buffers(ggml_add_inplace(gctx, output, bias));
    return output;
}

ggml_tensor *RMSNorm::forward(ModelContext *ctx, ggml_tensor *input) const {
    ggml_context *gctx = ctx->ctx_b.get();
    auto ggml_rms_norm_fn = inplace ? ggml_rms_norm_inplace : ggml_rms_norm;
    ggml_tensor *output = tensor_assign_buffers(ggml_rms_norm_fn(gctx, input, eps));
    output = tensor_assign_buffers(ggml_mul_inplace(gctx, output, weight));
    return output;
}

static ggml_tensor *apply_activation_inplace(ggml_context *ctx, ggml_tensor *hidden_states, ActivationType hidden_act) {
    switch (hidden_act) {
    case ActivationType::GELU:
        return tensor_assign_buffers(ggml_gelu_inplace(ctx, hidden_states));
    case ActivationType::SILU:
        return tensor_assign_buffers(ggml_silu_inplace(ctx, hidden_states));
    default:
        CHATGLM_THROW << "Unknown activation type " << (int)hidden_act;
    }
}

ggml_tensor *BasicMLP::forward(ModelContext *ctx, ggml_tensor *hidden_states) const {
    ggml_context *gctx = ctx->ctx_b.get();
    hidden_states = dense_h_to_4h.forward(ctx, hidden_states);
    hidden_states = apply_activation_inplace(gctx, hidden_states, hidden_act);
    hidden_states = dense_4h_to_h.forward(ctx, hidden_states);
    return hidden_states;
}

ggml_tensor *BasicGLU::forward(ModelContext *ctx, ggml_tensor *hidden_states) const {
    ggml_context *gctx = ctx->ctx_b.get();
    ggml_tensor *gate = gate_proj.forward(ctx, hidden_states);
    gate = apply_activation_inplace(gctx, gate, hidden_act);
    hidden_states = up_proj.forward(ctx, hidden_states);
    hidden_states = tensor_assign_buffers(ggml_mul_inplace(gctx, hidden_states, gate));
    hidden_states = down_proj.forward(ctx, hidden_states);
    return hidden_states;
}

// Adapted from https://github.com/ggerganov/llama.cpp/blob/master/examples/common.cpp
int get_num_physical_cores() {
    unsigned int n_threads = std::thread::hardware_concurrency();
    return n_threads > 0 ? (n_threads <= 4 ? n_threads : n_threads / 2) : 4;
}

int get_default_num_threads() {
#if defined(GGML_USE_CUBLAS) || defined(GGML_USE_METAL)
    return 1;
#else
    return std::min(get_num_physical_cores(), 16);
#endif
}

std::string to_string(ModelType model_type) {
    switch (model_type) {
    case ModelType::CHATGLM:
        return "ChatGLM";
    case ModelType::CHATGLM2:
        return "ChatGLM2";
    case ModelType::CHATGLM3:
        return "ChatGLM3";
    case ModelType::CHATGLM4:
        return "ChatGLM4";
    case ModelType::BAICHUAN7B:
        return "Baichuan7B";
    case ModelType::BAICHUAN13B:
        return "Baichuan13B";
    case ModelType::INTERNLM:
        return "InternLM";
    default:
        CHATGLM_THROW << "unknown model type " << (int)model_type;
    }
}

static ggml_tensor *apply_rotary_emb_basic(ModelContext *ctx, ggml_tensor *layer, ggml_tensor *position_ids, int n_ctx,
                                           RopeType rope_type, float rope_theta, int dim_scale) {
    // tensor a (activation) is of shape [s, #h, d]
    // tensor b (position_ids) is of shape [s]
    ggml_context *gctx = ctx->ctx_b.get();
#ifdef GGML_USE_CUBLAS
    if (!ggml_is_contiguous(layer)) {
        layer = tensor_assign_buffers(ggml_cont(gctx, layer));
    }
#endif
    const int head_size = layer->ne[0];
    const int rope_dim = head_size / dim_scale;
    layer = tensor_assign_buffers(ggml_rope_custom_inplace(gctx, layer, position_ids, rope_dim, (int)rope_type, n_ctx,
                                                           rope_theta, 1.f)); // [s, #h, d]
    return layer;
}

static ggml_tensor *apply_rotary_emb_glm(ModelContext *ctx, ggml_tensor *layer, ggml_tensor *position_ids, int n_ctx) {
    // tensor a (activation) is of shape [s, #h, d]
    // tensor b (position_ids) is of shape [2 * s]
    ggml_context *gctx = ctx->ctx_b.get();

    const int head_size = layer->ne[0];
    const int num_heads = layer->ne[1];
    const int qlen = layer->ne[2];
    const int rope_dim = head_size / 2;

    ggml_tensor *b1 = ggml_view_1d(gctx, position_ids, qlen, 0);
    ggml_tensor *b2 = ggml_view_1d(gctx, position_ids, qlen, qlen * ggml_element_size(position_ids));

    ggml_tensor *a1 = ggml_view_3d(gctx, layer, head_size / 2, num_heads, qlen, layer->nb[1], layer->nb[2], 0);
    ggml_tensor *a2 = ggml_view_3d(gctx, layer, head_size / 2, num_heads, qlen, layer->nb[1], layer->nb[2],
                                   head_size / 2 * ggml_element_size(layer));

    ggml_tensor *a1_rope = a1;
    ggml_tensor *a2_rope = a2;
#ifdef GGML_USE_CUBLAS
    a1_rope = tensor_assign_buffers(ggml_cont(gctx, a1_rope));
    a2_rope = tensor_assign_buffers(ggml_cont(gctx, a2_rope));
#endif

    a1_rope = tensor_assign_buffers(
        ggml_rope_inplace(gctx, a1_rope, b1, rope_dim, (int)RopeType::NEOX, n_ctx)); // [s, #h, d/2]
    a2_rope = tensor_assign_buffers(
        ggml_rope_inplace(gctx, a2_rope, b2, rope_dim, (int)RopeType::NEOX, n_ctx)); // [s, #h, d/2]

#ifdef GGML_USE_CUBLAS
    a1_rope = ggml_cpy(gctx, a1_rope, a1);
    a2_rope = ggml_cpy(gctx, a2_rope, a2);
#endif
    ggml_build_forward_expand(&ctx->gf, a1_rope);
    ggml_build_forward_expand(&ctx->gf, a2_rope);

    return layer;
}

[[maybe_unused]] static ggml_tensor *apply_rotary_emb_glm2(ModelContext *ctx, ggml_tensor *layer,
                                                           ggml_tensor *position_ids) {
    // layer: [s, #h, d], position_ids: [s]
    ggml_context *gctx = ctx->ctx_b.get();
#ifdef GGML_USE_CUBLAS
    if (!ggml_is_contiguous(layer)) {
        layer = tensor_assign_buffers(ggml_cont(gctx, layer));
    }
#endif
    const int head_size = layer->ne[0];
    const int rope_dim = head_size / 2;
    ggml_tensor *roped_layer =
        tensor_assign_buffers(ggml_rope(gctx, layer, position_ids, rope_dim, (int)RopeType::GPTJ, 0)); // [s, #h, d]

    ggml_tensor *roped_layer_view = tensor_assign_buffers(
        ggml_view_3d(gctx, roped_layer, rope_dim, roped_layer->ne[1], roped_layer->ne[2], roped_layer->nb[1],
                     roped_layer->nb[2], rope_dim * roped_layer->nb[0])); // [s, #h, d/2]

    ggml_tensor *layer_view =
        tensor_assign_buffers(ggml_view_3d(gctx, layer, rope_dim, layer->ne[1], layer->ne[2], layer->nb[1],
                                           layer->nb[2], rope_dim * layer->nb[0])); // [s, #h, d/2]

    ggml_build_forward_expand(&ctx->gf, ggml_cpy(gctx, layer_view, roped_layer_view));

    return roped_layer;
}

static ggml_tensor *apply_rotary_emb(ModelContext *ctx, ggml_tensor *layer, ggml_tensor *position_ids, int n_ctx,
                                     RopeType rope_type, float rope_theta, int dim_scale) {
    switch (rope_type) {
    case RopeType::GPTJ:
    case RopeType::NEOX:
        return apply_rotary_emb_basic(ctx, layer, position_ids, n_ctx, rope_type, rope_theta, dim_scale);
    case RopeType::CHATGLM:
        return apply_rotary_emb_glm(ctx, layer, position_ids, n_ctx);
    // case RopeType::CHATGLM2:
    //     return apply_rotary_emb_glm2(ctx, layer, position_ids);
    case RopeType::DISABLED:
        return layer;
    default:
        CHATGLM_THROW << "Unknown rope type " << (int)rope_type;
    }
}

static inline ggml_tensor *apply_attention_mask_causal(ModelContext *ctx, ggml_tensor *attn_scores, int n_past) {
    return tensor_assign_buffers(ggml_diag_mask_inf_inplace(ctx->ctx_b.get(), attn_scores, n_past));
}

static ggml_tensor *apply_attention_mask_glm(ModelContext *ctx, ggml_tensor *attn_scores, int n_past) {
    // attn_scores: [#h, s, kvs]
    // semantic: attn_scores[:, :-1, -1] = -inf
    ggml_context *gctx = ctx->ctx_b.get();
    const int kvlen = attn_scores->ne[0];
    const int qlen = attn_scores->ne[1];
    const int num_attention_heads = attn_scores->ne[2];
    ggml_tensor *inf = ggml_new_tensor_3d(gctx, attn_scores->type, 1, qlen - 1, num_attention_heads);
    ggml_set_f32(inf, -INFINITY);
    tensor_to_device(inf); // TODO: optimize
    ggml_tensor *masked_attn_scores =
        tensor_assign_buffers(ggml_view_3d(gctx, attn_scores, 1, qlen - 1, num_attention_heads, attn_scores->nb[1],
                                           attn_scores->nb[2], (kvlen - 1) * attn_scores->nb[0]));
    ggml_build_forward_expand(&ctx->gf, ggml_cpy(gctx, inf, masked_attn_scores));
    return attn_scores;
}

static ggml_tensor *apply_attention_mask(ModelContext *ctx, ggml_tensor *attn_scores, int n_past,
                                         AttentionMaskType attn_mask_type) {
    switch (attn_mask_type) {
    case AttentionMaskType::CAUSAL:
        return apply_attention_mask_causal(ctx, attn_scores, n_past);
    case AttentionMaskType::CHATGLM:
        return apply_attention_mask_glm(ctx, attn_scores, n_past);
    default:
        CHATGLM_THROW << "Unknown attention mask type " << (int)attn_mask_type;
    }
}

ggml_tensor *BasicAttention::forward(ModelContext *ctx, ggml_tensor *hidden_states, ggml_tensor *position_ids,
                                     int n_past, int n_ctx) const {
    ggml_context *gctx = ctx->ctx_b.get();

    const int hidden_size = hidden_states->ne[0];
    const int qlen = hidden_states->ne[1];
    const int head_size = hidden_size / num_attention_heads;
    const int num_shared_q_heads = num_attention_heads / num_kv_heads;
    const bool is_gqa = num_shared_q_heads > 1;

    ggml_tensor *qkv = query_key_value.forward(ctx, hidden_states); // [sq, (#h + 2 * #kvh) * d]

    // split mixed qkv into separate query, key and value
    ggml_tensor *query_layer; // [s, #h, d]
    ggml_tensor *key_layer;   // [s, #kvh, d]
    ggml_tensor *value_layer; // [s, #kvh, d]

    if (interleaved_qkv) {
        CHATGLM_CHECK(!is_gqa) << "interleaved qkv is not supported for GQA";
        query_layer = ggml_view_3d(gctx, qkv, head_size, num_attention_heads, qlen,
                                   3 * head_size * ggml_element_size(qkv), qkv->nb[1], 0);
        key_layer =
            ggml_view_3d(gctx, qkv, head_size, num_attention_heads, qlen, 3 * head_size * ggml_element_size(qkv),
                         qkv->nb[1], head_size * ggml_element_size(qkv));
        value_layer =
            ggml_view_3d(gctx, qkv, head_size, num_attention_heads, qlen, 3 * head_size * ggml_element_size(qkv),
                         qkv->nb[1], 2 * head_size * ggml_element_size(qkv));
    } else {
        query_layer = ggml_view_3d(gctx, qkv, head_size, num_attention_heads, qlen, head_size * ggml_element_size(qkv),
                                   qkv->nb[1], 0);
        key_layer = ggml_view_3d(gctx, qkv, head_size, num_kv_heads, qlen, head_size * ggml_element_size(qkv),
                                 qkv->nb[1], hidden_size * ggml_element_size(qkv));
        value_layer = ggml_view_3d(gctx, qkv, head_size, num_kv_heads, qlen, head_size * ggml_element_size(qkv),
                                   qkv->nb[1], (hidden_size + head_size * num_kv_heads) * ggml_element_size(qkv));
    }

    query_layer = apply_rotary_emb(ctx, query_layer, position_ids, n_ctx, rope_type, rope_theta, rope_dim_scale);
    key_layer = apply_rotary_emb(ctx, key_layer, position_ids, n_ctx, rope_type, rope_theta, rope_dim_scale);

    query_layer = tensor_assign_buffers(ggml_cont(gctx, ggml_permute(gctx, query_layer, 0, 2, 1, 3))); // [#h, s, d]
    if (num_shared_q_heads > 1) {
        query_layer = tensor_assign_buffers(ggml_reshape_3d(gctx, query_layer, head_size, num_shared_q_heads * qlen,
                                                            num_kv_heads)); // [#kvh, (#h/#kvh) * s, d]
    }

    key_layer = tensor_assign_buffers(ggml_permute(gctx, key_layer, 0, 2, 1, 3));     // [#kvh, s, d]
    value_layer = tensor_assign_buffers(ggml_permute(gctx, value_layer, 1, 2, 0, 3)); // [#kvh, d, s]

    // store key & value to cache
    ggml_tensor *k_cache_view = tensor_assign_buffers(
        ggml_view_3d(gctx, k_cache, head_size, qlen, num_kv_heads, k_cache->nb[1], k_cache->nb[2],
                     (num_virtual_tokens + n_past) * head_size * ggml_element_size(k_cache))); // [#kvh, s, d]
    ggml_build_forward_expand(&ctx->gf, ggml_cpy(gctx, key_layer, k_cache_view));
    ggml_tensor *v_cache_view =
        tensor_assign_buffers(ggml_view_3d(gctx, v_cache, qlen, head_size, num_kv_heads, v_cache->nb[1], v_cache->nb[2],
                                           (num_virtual_tokens + n_past) * ggml_element_size(v_cache))); // [#kvh, d, s]
    ggml_build_forward_expand(&ctx->gf, ggml_cpy(gctx, value_layer, v_cache_view));

    // concat key & value with past kv
    key_layer = tensor_assign_buffers(ggml_view_3d(gctx, k_cache, head_size, num_virtual_tokens + n_past + qlen,
                                                   num_kv_heads, k_cache->nb[1], k_cache->nb[2],
                                                   0)); // [#kvh, kvs, d]
    value_layer = tensor_assign_buffers(ggml_view_3d(gctx, v_cache, num_virtual_tokens + n_past + qlen, head_size,
                                                     num_kv_heads, v_cache->nb[1], v_cache->nb[2],
                                                     0)); // [#kvh, d, kvs]

    // attention
    ggml_tensor *attn_scores =
        tensor_assign_buffers(ggml_mul_mat(gctx, key_layer, query_layer)); // [#kvh, (#h/#kvh) * s, kvs]
    attn_scores =
        tensor_assign_buffers(ggml_scale_inplace(gctx, attn_scores, ggml_new_f32(gctx, 1.f / std::sqrt(head_size))));
    if (use_alibi) {
        attn_scores = tensor_assign_buffers(ggml_alibi(gctx, attn_scores, n_past, num_attention_heads, 8));
    }
    if (n_past == 0) {
        // build attention mask for context input
        if (num_shared_q_heads > 1) {
            attn_scores = ggml_reshape_3d(gctx, attn_scores, num_virtual_tokens + n_past + qlen, qlen,
                                          num_attention_heads); // [#h, s, kvs]
        }
        attn_scores = apply_attention_mask(ctx, attn_scores, num_virtual_tokens + n_past, attn_mask_type);
        if (num_shared_q_heads > 1) {
            attn_scores =
                ggml_reshape_3d(gctx, attn_scores, num_virtual_tokens + n_past + qlen, num_shared_q_heads * qlen,
                                num_kv_heads); // [#kvh, (#h/#kvh) * s, kvs]
        }
    }
    ggml_tensor *attn_probs =
        tensor_assign_buffers(ggml_soft_max_inplace(gctx, attn_scores)); // [#kvh, (#h/#kvh) * s, kvs]

    ggml_tensor *context_layer =
        tensor_assign_buffers(ggml_mul_mat(gctx, value_layer, attn_probs)); // [#kvh, (#h/#kvh) * s, d]
    if (num_shared_q_heads > 1) {
        context_layer = ggml_reshape_3d(gctx, context_layer, head_size, qlen,
                                        num_attention_heads); // [#h, s, d]
    }
    context_layer = tensor_assign_buffers(ggml_cont(gctx, ggml_permute(gctx, context_layer, 0, 2, 1, 3))); // [s, #h, d]
    context_layer = tensor_assign_buffers(ggml_reshape_2d(gctx, context_layer, hidden_size, qlen)); // [s, #h * d]

    ggml_tensor *attn_output = dense.forward(ctx, context_layer);
    return attn_output;
}

BaseModelForCausalLM::BaseModelForCausalLM(ModelConfig config, size_t mem_size, size_t scratch_size, size_t num_weights)
    : config(config) {
    ctx_.dtype = config.dtype;
    const size_t ctx_w_size = num_weights * ggml_tensor_overhead();
    const size_t ctx_kv_size = 2 * config.num_hidden_layers *
                               ((config.max_length + config.num_virtual_tokens) * config.hidden_size /
                                    config.num_attention_heads * config.num_kv_heads * ggml_type_size(GGML_TYPE_F16) +
                                ggml_tensor_overhead());
    ctx_.ctx_w = make_unique_ggml_context(ctx_w_size, nullptr, true);
    ctx_.ctx_kv = make_unique_ggml_context(ctx_kv_size + 1 * MB, nullptr, false); // 1MB extra for MPS

    ctx_.compute_buffer.resize(mem_size);
    ctx_.scratch_buffer.resize(scratch_size);
    ctx_.scratch = {0, ctx_.scratch_buffer.size(), ctx_.scratch_buffer.data()};
#ifdef GGML_USE_CUBLAS
    ggml_cuda_set_scratch_size(scratch_size);
#endif
}

ggml_tensor *BaseModelForCausalLM::forward_graph_compute(const std::vector<int> &input_ids, int n_past, int n_ctx,
                                                         int n_threads, bool is_decoding) {
    ctx_.ctx_b = make_unique_ggml_context(ctx_.compute_buffer.size(), ctx_.compute_buffer.data(), false);
    ctx_.gf = {};

    if (n_threads <= 0) {
        n_threads = get_default_num_threads(); // default thread num
    }
    int curr_input_ids_size = input_ids.size() - n_past;
    if (curr_input_ids_size >= 32 && ggml_cpu_has_blas() && !ggml_cpu_has_gpublas()) {
        n_threads = 1; // use 1 thread if BLAS is enabled
    }

    ggml_tensor *curr_input_ids = ggml_new_tensor_1d(ctx_.ctx_b.get(), GGML_TYPE_I32, curr_input_ids_size);
    memcpy(curr_input_ids->data, input_ids.data() + n_past, ggml_nbytes(curr_input_ids));

    ggml_tensor *lm_logits = forward(&ctx_, curr_input_ids, n_past, n_ctx, is_decoding);
    lm_logits->backend = GGML_BACKEND_CPU;

    ggml_build_forward_expand(&ctx_.gf, lm_logits);
#ifdef GGML_USE_METAL
    ggml_metal_graph_compute(ctx_.ctx_metal.get(), &ctx_.gf);
#else
    ggml_graph_compute_helper(ctx_.work_buffer, &ctx_.gf, n_threads);
#endif

#ifdef GGML_PERF
    ggml_graph_print(&ctx_.gf);
#endif

    return lm_logits;
}

int BaseModelForCausalLM::generate_next_token(const std::vector<int> &input_ids, const GenerationConfig &gen_config,
                                              int n_past, int n_ctx) {
    ggml_tensor *lm_logits = forward_graph_compute(input_ids, n_past, n_ctx, gen_config.num_threads, true);

    int vocab_size = lm_logits->ne[0];
    float *next_token_logits = (float *)lm_logits->data;

    // check nan
    for (int i = 0; i < vocab_size; i++) {
        CHATGLM_CHECK(std::isfinite(next_token_logits[i])) << "nan/inf encountered at lm_logits[" << i << "]";
    }

    // logits pre-process
    if (gen_config.repetition_penalty != 1.f) {
        sampling_repetition_penalty(next_token_logits, next_token_logits + vocab_size, input_ids,
                                    gen_config.repetition_penalty);
    }

    int next_token_id;
    if (gen_config.do_sample) {
        // temperature sampling
        if (gen_config.temperature > 0) {
            sampling_temperature(next_token_logits, next_token_logits + vocab_size, gen_config.temperature);
        }

        std::vector<TokenIdScore> token_scores(vocab_size);
        for (int i = 0; i < vocab_size; i++) {
            token_scores[i] = TokenIdScore(i, next_token_logits[i]);
        }

        // top_k sampling
        if (0 < gen_config.top_k && gen_config.top_k < (int)token_scores.size()) {
            sampling_top_k(token_scores.data(), token_scores.data() + gen_config.top_k,
                           token_scores.data() + token_scores.size());
            token_scores.resize(gen_config.top_k);
        }

        // top_p sampling
        if (0.f < gen_config.top_p && gen_config.top_p < 1.f) {
            auto pos = sampling_top_p(token_scores.data(), token_scores.data() + token_scores.size(), gen_config.top_p);
            token_scores.resize(pos - token_scores.data());
        }

        // sample next token
        sampling_softmax_inplace(token_scores.data(), token_scores.data() + token_scores.size());
        for (size_t i = 0; i < token_scores.size(); i++) {
            next_token_logits[i] = token_scores[i].score;
        }

        thread_local std::random_device rd;
        thread_local std::mt19937 gen(rd());

        std::discrete_distribution<> dist(next_token_logits, next_token_logits + token_scores.size());
        next_token_id = token_scores[dist(gen)].id;
    } else {
        // greedy search
        next_token_id = std::max_element(next_token_logits, next_token_logits + vocab_size) - next_token_logits;
    }

    return next_token_id;
}

void BaseModelForCausalLM::sampling_repetition_penalty(float *first, float *last, const std::vector<int> &input_ids,
                                                       float penalty) {
    CHATGLM_CHECK(penalty > 0) << "penalty must be a positive float, but got " << penalty;
    const float inv_penalty = 1.f / penalty;
    const int vocab_size = last - first;
    std::vector<bool> occurrence(vocab_size, false);
    for (const int id : input_ids) {
        if (!occurrence[id]) {
            first[id] *= (first[id] > 0) ? inv_penalty : penalty;
        }
        occurrence[id] = true;
    }
}

void BaseModelForCausalLM::sampling_temperature(float *first, float *last, float temp) {
    const float inv_temp = 1.f / temp;
    for (float *it = first; it != last; it++) {
        *it *= inv_temp;
    }
}

void BaseModelForCausalLM::sampling_top_k(TokenIdScore *first, TokenIdScore *kth, TokenIdScore *last) {
    std::nth_element(first, kth, last, std::greater<TokenIdScore>());
}

TokenIdScore *BaseModelForCausalLM::sampling_top_p(TokenIdScore *first, TokenIdScore *last, float top_p) {
    // fast top_p in expected O(n) time complexity
    sampling_softmax_inplace(first, last);

    while (first + 1 < last) {
        const float pivot_score = (last - 1)->score; // use mid score?
        TokenIdScore *mid =
            std::partition(first, last - 1, [pivot_score](const TokenIdScore &x) { return x.score > pivot_score; });
        std::swap(*mid, *(last - 1));

        const float prefix_sum =
            std::accumulate(first, mid, 0.f, [](float sum, const TokenIdScore &x) { return sum + x.score; });
        if (prefix_sum >= top_p) {
            last = mid;
        } else if (prefix_sum + mid->score < top_p) {
            first = mid + 1;
            top_p -= prefix_sum + mid->score;
        } else {
            return mid + 1;
        }
    }
    return last;
}

void BaseModelForCausalLM::sampling_softmax_inplace(TokenIdScore *first, TokenIdScore *last) {
    float max_score = std::max_element(first, last)->score;
    float sum = 0.f;
    for (TokenIdScore *p = first; p != last; p++) {
        float s = std::exp(p->score - max_score);
        p->score = s;
        sum += s;
    }
    float inv_sum = 1.f / sum;
    for (TokenIdScore *p = first; p != last; p++) {
        p->score *= inv_sum;
    }
}

std::vector<int> BaseModelForCausalLM::generate(const std::vector<int> &input_ids, const GenerationConfig &gen_config,
                                                BaseStreamer *streamer) {
    CHATGLM_CHECK(gen_config.max_length <= config.max_length)
        << "Requested max_length (" << gen_config.max_length << ") exceeds pre-configured model max_length ("
        << config.max_length << ")";

    std::vector<int> output_ids;
    output_ids.reserve(gen_config.max_length);
    output_ids = input_ids;
    if (streamer) {
        streamer->put(input_ids);
    }

    int n_past = 0;
    const int n_ctx = input_ids.size();
    const int max_new_tokens = (gen_config.max_new_tokens > 0) ? gen_config.max_new_tokens : gen_config.max_length;

    while ((int)output_ids.size() < std::min(gen_config.max_length, n_ctx + max_new_tokens)) {
        int next_token_id = generate_next_token(output_ids, gen_config, n_past, n_ctx);

        n_past = output_ids.size();
        output_ids.emplace_back(next_token_id);

        if (streamer) {
            streamer->put({next_token_id});
        }

        if (next_token_id == config.eos_token_id ||
            std::find(config.extra_eos_token_ids.begin(), config.extra_eos_token_ids.end(), next_token_id) !=
                config.extra_eos_token_ids.end()) {
            break;
        }
    }

    if (streamer) {
        streamer->end();
    }

    return output_ids;
}

// ===== ChatGLM-6B =====

ChatGLMTokenizer::ChatGLMTokenizer(std::string_view serialized_model_proto) {
    const auto status = sp.LoadFromSerializedProto(serialized_model_proto);
    CHATGLM_CHECK(status.ok()) << status.ToString();

    bos_token_id = sp.PieceToId("<sop>");
    eos_token_id = sp.PieceToId("<eop>");
    mask_token_id = sp.PieceToId("[MASK]");
    gmask_token_id = sp.PieceToId("[gMASK]");
    pad_token_id = sp.PieceToId("<pad>");
}

std::vector<int> ChatGLMTokenizer::encode(const std::string &text, int max_length) const {
    std::string input = preprocess(text);
    std::vector<int> ids;
    sp.Encode(input, &ids);
    ids.insert(ids.end(), {gmask_token_id, bos_token_id});
    if ((int)ids.size() > max_length) {
        // sliding window: always take the last max_length tokens
        ids.erase(ids.begin(), ids.end() - max_length);
    }
    return ids;
}

std::vector<int> ChatGLMTokenizer::apply_chat_template(const std::vector<ChatMessage> &messages, int max_length) const {
    std::string prompt = apply_chat_template_text(messages);
    std::vector<int> input_ids = encode(prompt, max_length);
    return input_ids;
}

std::string ChatGLMTokenizer::apply_chat_template_text(const std::vector<ChatMessage> &messages) {
    check_chat_messages(messages);
    std::vector<ChatMessage> user_assistant_messages = filter_user_assistant_messages(messages);

    std::ostringstream oss_prompt;
    if (user_assistant_messages.size() == 1) {
        oss_prompt << user_assistant_messages.front().content;
    } else {
        for (size_t i = 0; i < user_assistant_messages.size(); i += 2) {
            oss_prompt << "[Round " << i / 2 << "]\n问：" << user_assistant_messages[i].content << "\n答：";
            if (i + 1 < user_assistant_messages.size()) {
                oss_prompt << user_assistant_messages[i + 1].content << "\n";
            }
        }
    }
    return oss_prompt.str();
}

std::string ChatGLMTokenizer::decode(const std::vector<int> &ids, bool skip_special_tokens) const {
    CHATGLM_CHECK(skip_special_tokens) << "unimplemented";
    std::string text;
    sp.Decode(ids, &text);
    text = postprocess(text);
    return text;
}

static std::string regex_replace(const std::string &input, const std::regex &regex,
                                 std::function<std::string(const std::smatch &)> format) {
    std::ostringstream oss;
    int last_index = 0;
    for (auto it = std::sregex_iterator(input.begin(), input.end(), regex); it != std::sregex_iterator(); it++) {
        oss << it->prefix() << format(*it);
        last_index = it->position() + it->length();
    }
    oss << input.substr(last_index);
    return oss.str();
}

std::string ChatGLMTokenizer::preprocess(const std::string &text) {
    std::string output;

    // newline token
    {
        static const std::regex newline_regex("\n");
        output = std::regex_replace(text, newline_regex, "<n>");
    }
    // tab token
    {
        static const std::regex tab_regex("\t");
        output = std::regex_replace(output, tab_regex, "<|tab|>");
    }
    // blank tokens
    {
        static const std::regex pattern(R"([ ]{2,80})");
        output = regex_replace(output, pattern, [](const std::smatch &sm) {
            std::ostringstream oss;
            oss << "<|blank_" << sm.str().size() << "|>";
            return oss.str();
        });
    }

    return output;
}

static inline std::string replace_punctuations(const std::string &text) {
    // reference: https://stackoverflow.com/questions/37989081/how-to-use-unicode-range-in-c-regex
    static std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
    static const std::vector<std::pair<std::wregex, std::wstring>> punct_map{
        {std::wregex(converter.from_bytes(R"(([\u4e00-\u9fff]),)")), converter.from_bytes("$1，")},
        {std::wregex(converter.from_bytes(R"(,([\u4e00-\u9fff]))")), converter.from_bytes("，$1")},
        {std::wregex(converter.from_bytes(R"(([\u4e00-\u9fff])!)")), converter.from_bytes("$1！")},
        {std::wregex(converter.from_bytes(R"(!([\u4e00-\u9fff]))")), converter.from_bytes("！$1")},
        {std::wregex(converter.from_bytes(R"(([\u4e00-\u9fff]):)")), converter.from_bytes("$1：")},
        {std::wregex(converter.from_bytes(R"(:([\u4e00-\u9fff]))")), converter.from_bytes("：$1")},
        {std::wregex(converter.from_bytes(R"(([\u4e00-\u9fff]);)")), converter.from_bytes("$1；")},
        {std::wregex(converter.from_bytes(R"(;([\u4e00-\u9fff]))")), converter.from_bytes("；$1")},
        {std::wregex(converter.from_bytes(R"(([\u4e00-\u9fff])\?)")), converter.from_bytes("$1？")},
        {std::wregex(converter.from_bytes(R"(\?([\u4e00-\u9fff]))")), converter.from_bytes("？$1")},
    };
    std::wstring w_output = converter.from_bytes(text);
    for (const auto &punct_pair : punct_map) {
        w_output = std::regex_replace(w_output, punct_pair.first, punct_pair.second);
    }
    std::string output = converter.to_bytes(w_output);
    return output;
}

std::string ChatGLMTokenizer::postprocess(const std::string &text) {
    std::string output;

    // newline token
    {
        static const std::regex pattern(R"(<n>)");
        output = std::regex_replace(text, pattern, "\n");
    }
    // tab token
    {
        static const std::regex pattern(R"(<\|tab\|>)");
        output = std::regex_replace(output, pattern, "\t");
    }
    // blank tokens
    {
        static const std::regex pattern(R"(<\|blank_(\d+)\|>)");
        output = regex_replace(output, pattern,
                               [](const std::smatch &sm) { return std::string(std::stoi(sm[1].str()), ' '); });
    }
    // punctuations
    output = replace_punctuations(output);

    return output;
}

ggml_tensor *GLMBlock::forward(ModelContext *ctx, ggml_tensor *hidden_states, ggml_tensor *position_ids, int n_past,
                               int n_ctx) const {
    ggml_context *gctx = ctx->ctx_b.get();

    ggml_tensor *alpha = ggml_new_f32(gctx, alpha_value);

    ggml_tensor *attn_input = input_layernorm.forward(ctx, hidden_states);
    ggml_tensor *attn_output = attention.forward(ctx, attn_input, position_ids, n_past, n_ctx);
    ggml_build_forward_expand(&ctx->gf, attn_output);
    attn_input = tensor_assign_buffers(ggml_scale_inplace(gctx, attn_input, alpha));
    hidden_states = tensor_assign_buffers(ggml_add_inplace(gctx, attn_input, attn_output));

    ggml_tensor *mlp_input = post_attention_layernorm.forward(ctx, hidden_states);
    ggml_tensor *mlp_output = mlp.forward(ctx, mlp_input);
    ggml_build_forward_expand(&ctx->gf, mlp_output);
    mlp_input = tensor_assign_buffers(ggml_scale_inplace(gctx, mlp_input, alpha));
    ggml_tensor *output = tensor_assign_buffers(ggml_add_inplace(gctx, mlp_input, mlp_output));

    return output;
}

ChatGLMForCausalLM::ChatGLMForCausalLM(const ModelConfig &config)
    : BasicModelForCausalLM(config, MEM_SIZE, SCRATCH_SIZE, num_weights(config.num_hidden_layers)) {
    state_dict_ = state_dict();
}

void ChatGLMForCausalLM::load(ModelLoader &loader) {
    for (auto &item : state_dict_) {
        const std::string &name = item.first;
        ggml_tensor *tensor = item.second;
        if (name != "lm_head.weight") {
            loader.read_tensor(name, tensor);
        }
    }
    lm_head.weight->data = transformer.word_embeddings.weight->data; // tied weight

    to_device();

    ctx_.weight_buffer = std::string_view(loader.data, loader.size);
    ctx_.init_device_context();
}

StateDict ChatGLMForCausalLM::state_dict() const {
    StateDict sd;
    sd.reserve(num_weights(config.num_hidden_layers));
    sd.emplace_back("transformer.word_embeddings.weight", transformer.word_embeddings.weight);
    for (int i = 0; i < config.num_hidden_layers; i++) {
        std::string layer_prefix = "transformer.layers." + std::to_string(i) + '.';
        sd.emplace_back(layer_prefix + "input_layernorm.weight", transformer.layers[i].input_layernorm.weight);
        sd.emplace_back(layer_prefix + "input_layernorm.bias", transformer.layers[i].input_layernorm.bias);
        sd.emplace_back(layer_prefix + "attention.query_key_value.weight",
                        transformer.layers[i].attention.query_key_value.weight);
        sd.emplace_back(layer_prefix + "attention.query_key_value.bias",
                        transformer.layers[i].attention.query_key_value.bias);
        sd.emplace_back(layer_prefix + "attention.dense.weight", transformer.layers[i].attention.dense.weight);
        sd.emplace_back(layer_prefix + "attention.dense.bias", transformer.layers[i].attention.dense.bias);
        sd.emplace_back(layer_prefix + "post_attention_layernorm.weight",
                        transformer.layers[i].post_attention_layernorm.weight);
        sd.emplace_back(layer_prefix + "post_attention_layernorm.bias",
                        transformer.layers[i].post_attention_layernorm.bias);
        sd.emplace_back(layer_prefix + "mlp.dense_h_to_4h.weight", transformer.layers[i].mlp.dense_h_to_4h.weight);
        sd.emplace_back(layer_prefix + "mlp.dense_h_to_4h.bias", transformer.layers[i].mlp.dense_h_to_4h.bias);
        sd.emplace_back(layer_prefix + "mlp.dense_4h_to_h.weight", transformer.layers[i].mlp.dense_4h_to_h.weight);
        sd.emplace_back(layer_prefix + "mlp.dense_4h_to_h.bias", transformer.layers[i].mlp.dense_4h_to_h.bias);
    }
    sd.emplace_back("transformer.final_layernorm.weight", transformer.final_layernorm.weight);
    sd.emplace_back("transformer.final_layernorm.bias", transformer.final_layernorm.bias);
    sd.emplace_back("lm_head.weight", lm_head.weight);
    return sd;
}

// ===== ChatGLM2-6B =====

ChatGLM2Tokenizer::ChatGLM2Tokenizer(std::string_view serialized_model_proto) {
    const auto status = sp.LoadFromSerializedProto(serialized_model_proto);
    CHATGLM_CHECK(status.ok()) << status.ToString();

    int special_id = sp.GetPieceSize();
    mask_token_id = special_id++;
    gmask_token_id = special_id++;
    smask_token_id = special_id++;
    sop_token_id = special_id++;
    eop_token_id = special_id++;
}

std::vector<int> ChatGLM2Tokenizer::encode(const std::string &text, int max_length) const {
    std::vector<int> ids;
    sp.Encode(text, &ids);
    ids.insert(ids.begin(), {gmask_token_id, sop_token_id}); // special prefix
    if ((int)ids.size() > max_length) {
        // sliding window: drop the least recent history while keeping the two special prefix tokens
        int num_drop = (int)ids.size() - max_length;
        ids.erase(ids.begin() + 2, ids.begin() + 2 + num_drop);
    }
    return ids;
}

std::string ChatGLM2Tokenizer::decode(const std::vector<int> &ids, bool skip_special_tokens) const {
    CHATGLM_CHECK(skip_special_tokens) << "unimplemented";
    // filter out special tokens
    std::vector<int> normal_ids(ids);
    normal_ids.erase(std::remove_if(normal_ids.begin(), normal_ids.end(), [this](int id) { return is_special_id(id); }),
                     normal_ids.end());

    std::string text;
    sp.Decode(normal_ids, &text);
    text = replace_punctuations(text);
    return text;
}

std::vector<int> ChatGLM2Tokenizer::apply_chat_template(const std::vector<ChatMessage> &messages,
                                                        int max_length) const {
    std::string prompt = apply_chat_template_text(messages);
    std::vector<int> input_ids = encode(prompt, max_length);
    return input_ids;
}

std::string ChatGLM2Tokenizer::apply_chat_template_text(const std::vector<ChatMessage> &messages) {
    check_chat_messages(messages);
    std::vector<ChatMessage> user_assistant_messages = filter_user_assistant_messages(messages);

    std::ostringstream oss_prompt;
    for (size_t i = 0; i < user_assistant_messages.size(); i += 2) {
        oss_prompt << "[Round " << i / 2 + 1 << "]\n\n问：" << user_assistant_messages[i].content << "\n\n答：";
        if (i < user_assistant_messages.size() - 1) {
            oss_prompt << user_assistant_messages[i + 1].content << "\n\n";
        }
    }
    return oss_prompt.str();
}

bool ChatGLM2Tokenizer::is_special_id(int id) const {
    return id == mask_token_id || id == gmask_token_id || id == smask_token_id || id == sop_token_id ||
           id == eop_token_id;
}

ChatGLM2ForCausalLM::ChatGLM2ForCausalLM(const ModelConfig &config)
    : BasicModelForCausalLM(config, MEM_SIZE, SCRATCH_SIZE, num_weights(config.num_hidden_layers)) {
    state_dict_ = state_dict();
}

void ChatGLM2ForCausalLM::load(ModelLoader &loader) {
    if (config.num_virtual_tokens > 0) {
        const int head_size = config.hidden_size / config.num_attention_heads;
        auto prefix_cache_ctx = make_unique_ggml_context(
            ggml_tensor_overhead() + config.num_hidden_layers * 2 * config.num_kv_heads * config.num_virtual_tokens *
                                         head_size * ggml_type_size(GGML_TYPE_F16),
            nullptr, false);
        ggml_tensor *past_key_values =
            ggml_new_tensor_4d(prefix_cache_ctx.get(), GGML_TYPE_F16, head_size, config.num_virtual_tokens,
                               config.num_kv_heads, config.num_hidden_layers * 2);
        CHATGLM_CHECK(ggml_used_mem(prefix_cache_ctx.get()) == ggml_get_mem_size(prefix_cache_ctx.get()))
            << "corrupted prefix cache";
        loader.read_tensor("past_key_values", past_key_values);
        load_prefix_cache(past_key_values);
    }

    std::unordered_map<std::string, std::string> glu_name_map;
    for (int i = 0; i < config.num_hidden_layers; i++) {
        std::string layer_prefix = "transformer.encoder.layers." + std::to_string(i) + '.';
        glu_name_map.emplace(layer_prefix + "mlp.gate_proj.weight", layer_prefix + "mlp.dense_h_to_4h.weight");
        glu_name_map.emplace(layer_prefix + "mlp.up_proj.weight", layer_prefix + "mlp.dense_h_to_4h.weight");
    }

    for (auto it = state_dict_.begin(); it != state_dict_.end(); it++) {
        const std::string &name = it->first;
        ggml_tensor *tensor = it->second;

        auto glu_it = glu_name_map.find(name);
        if (glu_it != glu_name_map.end()) {
            // for compatibility: load gate_proj & up_proj from dense_h_to_4h
            const std::string &dense_h_to_4h_name = glu_it->second;
            ggml_tensor *gate_proj = tensor;
            it++;
            CHATGLM_CHECK(glu_name_map.at(it->first) == dense_h_to_4h_name) << "corrupted glu weights";
            ggml_tensor *up_proj = it->second;

            int64_t target_ne[4]{gate_proj->ne[0], gate_proj->ne[1] + up_proj->ne[1]};
            loader.checked_read_tensor_meta(dense_h_to_4h_name, gate_proj->n_dims, target_ne, gate_proj->type);
            gate_proj->data = loader.read_tensor_data(ggml_nbytes(gate_proj));
            up_proj->data = loader.read_tensor_data(ggml_nbytes(up_proj));
        } else {
            loader.read_tensor(name, tensor);
        }
    }

    to_device();

    ctx_.weight_buffer = std::string_view(loader.data, loader.size);
    ctx_.init_device_context();
}

StateDict ChatGLM2ForCausalLM::state_dict() const {
    StateDict sd;
    sd.reserve(num_weights(config.num_hidden_layers));
    sd.emplace_back("transformer.embedding.word_embeddings.weight", transformer.word_embeddings.weight);
    for (int i = 0; i < config.num_hidden_layers; i++) {
        std::string layer_prefix = "transformer.encoder.layers." + std::to_string(i) + '.';
        sd.emplace_back(layer_prefix + "input_layernorm.weight", transformer.layers[i].input_layernorm.weight);
        sd.emplace_back(layer_prefix + "self_attention.query_key_value.weight",
                        transformer.layers[i].attention.query_key_value.weight);
        sd.emplace_back(layer_prefix + "self_attention.query_key_value.bias",
                        transformer.layers[i].attention.query_key_value.bias);
        sd.emplace_back(layer_prefix + "self_attention.dense.weight", transformer.layers[i].attention.dense.weight);
        sd.emplace_back(layer_prefix + "post_attention_layernorm.weight",
                        transformer.layers[i].post_attention_layernorm.weight);
        sd.emplace_back(layer_prefix + "mlp.gate_proj.weight", transformer.layers[i].mlp.gate_proj.weight);
        sd.emplace_back(layer_prefix + "mlp.up_proj.weight", transformer.layers[i].mlp.up_proj.weight);
        // for compatibility
        sd.emplace_back(layer_prefix + "mlp.dense_4h_to_h.weight", transformer.layers[i].mlp.down_proj.weight);
    }
    sd.emplace_back("transformer.encoder.final_layernorm.weight", transformer.final_layernorm.weight);
    sd.emplace_back("transformer.output_layer.weight", lm_head.weight);
    return sd;
}

// ===== ChatGLM3-6B =====

ChatGLM3Tokenizer::ChatGLM3Tokenizer(std::string_view serialized_model_proto) {
    const auto status = sp.LoadFromSerializedProto(serialized_model_proto);
    CHATGLM_CHECK(status.ok()) << status.ToString();

    int special_id = sp.GetPieceSize();
    mask_token_id = special_id++;
    gmask_token_id = special_id++;
    smask_token_id = special_id++;
    sop_token_id = special_id++;
    eop_token_id = special_id++;
    system_token_id = special_id++;
    user_token_id = special_id++;
    assistant_token_id = special_id++;
    observation_token_id = special_id++;

    special_tokens = {
        {"[MASK]", mask_token_id},
        {"[gMASK]", gmask_token_id},
        {"[sMASK]", smask_token_id},
        {"sop", sop_token_id},
        {"eop", eop_token_id},
        {"<|system|>", system_token_id},
        {"<|user|>", user_token_id},
        {"<|assistant|>", assistant_token_id},
        {"<|observation|>", observation_token_id},
    };

    for (const auto &item : special_tokens) {
        index_special_tokens[item.second] = item.first;
    }
}

std::vector<int> ChatGLM3Tokenizer::encode(const std::string &text, int max_length) const {
    std::vector<int> ids;
    sp.Encode(text, &ids);
    ids.insert(ids.begin(), {gmask_token_id, sop_token_id}); // special prefix
    truncate(ids, max_length);
    return ids;
}

std::string ChatGLM3Tokenizer::decode(const std::vector<int> &ids, bool skip_special_tokens) const {
    std::vector<std::string> pieces;
    for (int id : ids) {
        auto pos = index_special_tokens.find(id);
        if (pos != index_special_tokens.end()) {
            // special tokens
            pieces.emplace_back(pos->second);
        } else {
            // normal tokens
            pieces.emplace_back(sp.IdToPiece(id));
        }
    }

    std::string text = sp.DecodePieces(pieces);

    if (skip_special_tokens) {
        text = remove_special_tokens(text);
    }

    return text;
}

std::string ChatGLM3Tokenizer::remove_special_tokens(const std::string &text) {
    // R"(<\|assistant\|> interpreter)"
    // R"(<\|assistant\|> interpre)"
    static const std::regex re(R"(<\|assistant\|>|<\|user\|>|<\|observation\|>)");
    std::string output = std::regex_replace(text, re, "");
    return output;
}

std::vector<int> ChatGLM3Tokenizer::encode_single_message(const std::string &role, const std::string &content) const {
    std::vector<int> input_ids;
    input_ids.emplace_back(get_command("<|" + role + "|>"));
    // TODO: support metadata
    std::vector<int> newline_ids;
    sp.Encode("\n", &newline_ids);
    input_ids.insert(input_ids.end(), newline_ids.begin(), newline_ids.end());
    std::vector<int> content_ids;
    sp.Encode(content, &content_ids);
    input_ids.insert(input_ids.end(), content_ids.begin(), content_ids.end());
    return input_ids;
}

std::vector<int> ChatGLM3Tokenizer::apply_chat_template(const std::vector<ChatMessage> &messages,
                                                        int max_length) const {
    std::vector<int> input_ids{gmask_token_id, sop_token_id};
    for (const auto &msg : messages) {
        auto msg_ids = encode_single_message(msg.role, msg.content);
        input_ids.insert(input_ids.end(), msg_ids.begin(), msg_ids.end());

        // encode code block into a separate message
        if (!msg.tool_calls.empty() && msg.tool_calls.front().type == ToolCallMessage::TYPE_CODE) {
            auto code_ids = encode_single_message(msg.role, msg.tool_calls.front().code.input);
            input_ids.insert(input_ids.end(), code_ids.begin(), code_ids.end());
        }
    }
    input_ids.emplace_back(assistant_token_id);
    truncate(input_ids, max_length);
    return input_ids;
}

ChatMessage ChatGLM3Tokenizer::decode_message(const std::vector<int> &ids) const {
    ChatMessage message;
    if (!ids.empty() && ids.back() == observation_token_id) {
        // insert an <|assistant|> token before content to match possible interpreter delimiter
        std::vector<int> full_ids{assistant_token_id};
        full_ids.insert(full_ids.end(), ids.begin(), ids.end());

        std::string output = decode(full_ids, false);
        const std::string ci_delim = "<|assistant|> interpreter";
        size_t ci_pos = output.find(ci_delim);
        if (ci_pos != std::string::npos) {
            // code interpreter
            std::string chat_output = output.substr(0, ci_pos);
            chat_output = remove_special_tokens(chat_output);
            trim(chat_output);
            std::string code_output = output.substr(ci_pos + ci_delim.size());
            code_output = remove_special_tokens(code_output);
            trim(code_output);
            message = ChatMessage(ChatMessage::ROLE_ASSISTANT, std::move(chat_output),
                                  {ToolCallMessage(CodeMessage(std::move(code_output)))});
        } else {
            // tool call
            output = remove_special_tokens(output);

            // parse tool name
            std::string tool_name = "PARSE_ERROR";
            size_t pos = output.find('\n');
            if (pos != std::string::npos) {
                // split tool name and args by 1st linebreak
                tool_name = output.substr(0, pos);
                trim(tool_name);
                output.erase(0, pos + 1);
            }

            // post process output
            trim(output);

            // extract args
            std::string tool_args = "PARSE_ERROR";
            static const std::regex args_regex(R"(```.*?\n(.*?)\n```)");
            std::smatch sm;
            if (std::regex_search(output, sm, args_regex)) {
                CHATGLM_CHECK(sm.size() == 2) << "unexpected regex match results";
                tool_args = sm[1];
            }

            message = ChatMessage(ChatMessage::ROLE_ASSISTANT, std::move(output),
                                  {ToolCallMessage(FunctionMessage(std::move(tool_name), std::move(tool_args)))});
        }
    } else {
        // conversation
        message = BaseTokenizer::decode_message(ids);
        trim(message.content); // strip leading linebreak in conversation mode
    }
    return message;
}

int ChatGLM3Tokenizer::get_command(const std::string &token) const {
    auto pos = special_tokens.find(token);
    CHATGLM_CHECK(pos != special_tokens.end()) << token << " is not a special token";
    return pos->second;
}

bool ChatGLM3Tokenizer::is_special_id(int id) const { return index_special_tokens.count(id) > 0; }

void ChatGLM3Tokenizer::truncate(std::vector<int> &ids, int max_length) {
    if ((int)ids.size() > max_length) {
        // sliding window: drop the least recent history while keeping the two special prefix tokens
        int num_drop = (int)ids.size() - max_length;
        ids.erase(ids.begin() + 2, ids.begin() + 2 + num_drop);
    }
}

// ===== Baichuan =====

BaichuanTokenizer::BaichuanTokenizer(std::string_view serialized_model_proto) {
    const auto status = sp.LoadFromSerializedProto(serialized_model_proto);
    CHATGLM_CHECK(status.ok()) << status.ToString();
}

std::vector<int> BaichuanTokenizer::encode(const std::string &text, int max_length) const {
    std::vector<int> ids;
    sp.Encode(text, &ids);
    truncate(ids, max_length);
    return ids;
}

std::string BaichuanTokenizer::decode(const std::vector<int> &ids, bool skip_special_tokens) const {
    CHATGLM_CHECK(skip_special_tokens) << "unimplemented";
    std::vector<int> normal_ids(ids);
    normal_ids.erase(std::remove_if(normal_ids.begin(), normal_ids.end(), [this](int id) { return is_special_id(id); }),
                     normal_ids.end());

    std::string text;
    sp.Decode(normal_ids, &text);
    return text;
}

std::vector<int> BaichuanTokenizer::apply_chat_template(const std::vector<ChatMessage> &messages,
                                                        int max_length) const {
    check_chat_messages(messages);
    std::vector<ChatMessage> user_assistant_messages = filter_user_assistant_messages(messages);

    std::vector<int> ids;
    ids.reserve(max_length);
    for (const auto &msg : user_assistant_messages) {
        ids.push_back((msg.role == ChatMessage::ROLE_USER) ? USER_TOKEN_ID : ASSISTANT_TOKEN_ID);
        std::vector<int> content_ids = encode(msg.content, max_length);
        ids.insert(ids.end(), content_ids.begin(), content_ids.end());
    }
    ids.push_back(ASSISTANT_TOKEN_ID);

    truncate(ids, max_length);
    return ids;
}

bool BaichuanTokenizer::is_special_id(int id) const {
    return id == bos_token_id || id == eos_token_id || id == pad_token_id;
}

void BaichuanTokenizer::truncate(std::vector<int> &ids, int max_length) {
    if ((int)ids.size() > max_length) {
        ids.erase(ids.begin(), ids.end() - max_length);
    }
}

// ===== Baichuan-7B =====

Baichuan7BForCausalLM::Baichuan7BForCausalLM(const ModelConfig &config)
    : BasicModelForCausalLM(config, MEM_SIZE, SCRATCH_SIZE, num_weights(config.num_hidden_layers)) {
    state_dict_ = state_dict();
}

void Baichuan7BForCausalLM::load(ModelLoader &loader) {
    for (auto &item : state_dict_) {
        const std::string &name = item.first;
        ggml_tensor *tensor = item.second;
        loader.read_tensor(name, tensor);
    }

    to_device();

    ctx_.weight_buffer = std::string_view(loader.data, loader.size);
    ctx_.init_device_context();
}

StateDict Baichuan7BForCausalLM::state_dict() const {
    StateDict sd;
    sd.reserve(num_weights(config.num_hidden_layers));
    sd.emplace_back("model.embed_tokens.weight", transformer.word_embeddings.weight);
    for (int i = 0; i < config.num_hidden_layers; i++) {
        std::string layer_prefix = "model.layers." + std::to_string(i) + '.';
        sd.emplace_back(layer_prefix + "input_layernorm.weight", transformer.layers[i].input_layernorm.weight);
        sd.emplace_back(layer_prefix + "self_attn.W_pack.weight",
                        transformer.layers[i].attention.query_key_value.weight);
        sd.emplace_back(layer_prefix + "self_attn.o_proj.weight", transformer.layers[i].attention.dense.weight);
        sd.emplace_back(layer_prefix + "post_attention_layernorm.weight",
                        transformer.layers[i].post_attention_layernorm.weight);
        sd.emplace_back(layer_prefix + "mlp.gate_proj.weight", transformer.layers[i].mlp.gate_proj.weight);
        sd.emplace_back(layer_prefix + "mlp.down_proj.weight", transformer.layers[i].mlp.down_proj.weight);
        sd.emplace_back(layer_prefix + "mlp.up_proj.weight", transformer.layers[i].mlp.up_proj.weight);
    }
    sd.emplace_back("model.norm.weight", transformer.final_layernorm.weight);
    sd.emplace_back("lm_head.weight", lm_head.weight);
    return sd;
}

// ===== Baichuan-13B =====

Baichuan13BForCausalLM::Baichuan13BForCausalLM(const ModelConfig &config)
    : BasicModelForCausalLM(config, MEM_SIZE, SCRATCH_SIZE, num_weights(config.num_hidden_layers)) {
    state_dict_ = state_dict();
}

void Baichuan13BForCausalLM::load(ModelLoader &loader) {
    for (auto &item : state_dict_) {
        const std::string &name = item.first;
        ggml_tensor *tensor = item.second;
        loader.read_tensor(name, tensor);
    }

    to_device();

    ctx_.weight_buffer = std::string_view(loader.data, loader.size);
    ctx_.init_device_context();
}

StateDict Baichuan13BForCausalLM::state_dict() const {
    StateDict sd;
    sd.reserve(num_weights(config.num_hidden_layers));
    sd.emplace_back("model.embed_tokens.weight", transformer.word_embeddings.weight);
    for (int i = 0; i < config.num_hidden_layers; i++) {
        std::string layer_prefix = "model.layers." + std::to_string(i) + '.';
        sd.emplace_back(layer_prefix + "input_layernorm.weight", transformer.layers[i].input_layernorm.weight);
        sd.emplace_back(layer_prefix + "self_attn.W_pack.weight",
                        transformer.layers[i].attention.query_key_value.weight);
        sd.emplace_back(layer_prefix + "self_attn.o_proj.weight", transformer.layers[i].attention.dense.weight);
        sd.emplace_back(layer_prefix + "post_attention_layernorm.weight",
                        transformer.layers[i].post_attention_layernorm.weight);
        sd.emplace_back(layer_prefix + "mlp.gate_proj.weight", transformer.layers[i].mlp.gate_proj.weight);
        sd.emplace_back(layer_prefix + "mlp.down_proj.weight", transformer.layers[i].mlp.down_proj.weight);
        sd.emplace_back(layer_prefix + "mlp.up_proj.weight", transformer.layers[i].mlp.up_proj.weight);
    }
    sd.emplace_back("model.norm.weight", transformer.final_layernorm.weight);
    sd.emplace_back("lm_head.weight", lm_head.weight);
    return sd;
}

// ===== InternLM =====

InternLMTokenizer::InternLMTokenizer(std::string_view serialized_model_proto) {
    const auto status = sp.LoadFromSerializedProto(serialized_model_proto);
    CHATGLM_CHECK(status.ok()) << status.ToString();
}

std::vector<int> InternLMTokenizer::encode(const std::string &text, int max_length) const {
    std::vector<int> ids;
    sp.Encode(text, &ids);
    ids.insert(ids.begin(), {bos_token_id}); // special prefix
    if ((int)ids.size() > max_length) {
        // sliding window: drop the least recent history while keeping the special prefix
        int num_drop = (int)ids.size() - max_length;
        ids.erase(ids.begin() + 1, ids.begin() + 1 + num_drop);
    }
    return ids;
}

std::string InternLMTokenizer::decode(const std::vector<int> &ids, bool skip_special_tokens) const {
    CHATGLM_CHECK(skip_special_tokens) << "unimplemented";
    // filter out special tokens
    std::vector<int> normal_ids(ids);
    normal_ids.erase(std::remove_if(normal_ids.begin(), normal_ids.end(), [this](int id) { return is_special_id(id); }),
                     normal_ids.end());

    std::string text;
    sp.Decode(normal_ids, &text);
    // remove <eoa> and its following
    size_t eoa_pos = text.find("<eoa>");
    if (eoa_pos != std::string::npos) {
        text.erase(eoa_pos);
    }
    return text;
}

std::vector<int> InternLMTokenizer::apply_chat_template(const std::vector<ChatMessage> &messages,
                                                        int max_length) const {
    std::string prompt = build_prompt(messages);
    std::vector<int> input_ids = encode(prompt, max_length);
    return input_ids;
}

std::string InternLMTokenizer::build_prompt(const std::vector<ChatMessage> &messages) {
    check_chat_messages(messages);
    std::vector<ChatMessage> user_assistant_messages = filter_user_assistant_messages(messages);

    std::ostringstream oss_prompt;
    for (const auto &msg : user_assistant_messages) {
        if (msg.role == ChatMessage::ROLE_USER) {
            oss_prompt << "<|User|>:" << msg.content << "<eoh>\n<|Bot|>:";
        } else {
            oss_prompt << msg.content << "<eoa>\n";
        }
    }
    return oss_prompt.str();
}

InternLMForCausalLM::InternLMForCausalLM(const ModelConfig &config)
    : BasicModelForCausalLM(config, MEM_SIZE, SCRATCH_SIZE, num_weights(config.num_hidden_layers, config.hidden_size)) {
    state_dict_ = state_dict();
}

void InternLMForCausalLM::load(ModelLoader &loader) {
    for (auto &item : state_dict_) {
        const std::string &name = item.first;
        ggml_tensor *tensor = item.second;
        loader.read_tensor(name, tensor);
    }

    to_device();

    ctx_.weight_buffer = std::string_view(loader.data, loader.size);
    ctx_.init_device_context();
}

StateDict InternLMForCausalLM::state_dict() const {
    StateDict sd;
    sd.reserve(num_weights(config.num_hidden_layers, config.hidden_size));
    sd.emplace_back("model.embed_tokens.weight", transformer.word_embeddings.weight);
    for (int i = 0; i < config.num_hidden_layers; i++) {
        std::string layer_prefix = "model.layers." + std::to_string(i) + '.';
        sd.emplace_back(layer_prefix + "input_layernorm.weight", transformer.layers[i].input_layernorm.weight);
        sd.emplace_back(layer_prefix + "self_attn.qkv_proj.weight",
                        transformer.layers[i].attention.query_key_value.weight);
        if (transformer.layers[i].attention.query_key_value.bias) {
            sd.emplace_back(layer_prefix + "self_attn.qkv_proj.bias",
                            transformer.layers[i].attention.query_key_value.bias);
        }
        sd.emplace_back(layer_prefix + "self_attn.o_proj.weight", transformer.layers[i].attention.dense.weight);
        if (transformer.layers[i].attention.dense.bias) {
            sd.emplace_back(layer_prefix + "self_attn.o_proj.bias", transformer.layers[i].attention.dense.bias);
        }
        sd.emplace_back(layer_prefix + "post_attention_layernorm.weight",
                        transformer.layers[i].post_attention_layernorm.weight);
        sd.emplace_back(layer_prefix + "mlp.gate_proj.weight", transformer.layers[i].mlp.gate_proj.weight);
        sd.emplace_back(layer_prefix + "mlp.up_proj.weight", transformer.layers[i].mlp.up_proj.weight);
        sd.emplace_back(layer_prefix + "mlp.down_proj.weight", transformer.layers[i].mlp.down_proj.weight);
    }
    sd.emplace_back("model.norm.weight", transformer.final_layernorm.weight);
    sd.emplace_back("lm_head.weight", lm_head.weight);
    return sd;
}

// ===== ChatGLM4-9B =====

TiktokenCoreBPE::TiktokenCoreBPE(std::unordered_map<std::string, int> encoder,
                                 std::unordered_map<std::string, int> special_tokens_encoder,
                                 const std::string &pattern)
    : regex(std::make_unique<RE2>("(" + pattern + ")")), encoder(std::move(encoder)),
      special_tokens_encoder(std::move(special_tokens_encoder)) {
    CHATGLM_CHECK(regex->ok()) << regex->error();

    decoder.reserve(this->encoder.size());
    for (const auto &[token, rank] : this->encoder) {
        decoder.emplace(rank, token);
    }

    special_tokens_decoder.reserve(this->special_tokens_encoder.size());
    for (const auto &[token, rank] : this->special_tokens_encoder) {
        special_tokens_decoder.emplace(rank, token);
    }
}

std::vector<std::pair<size_t, int>> TiktokenCoreBPE::_byte_pair_merge(const std::unordered_map<std::string, int> &ranks,
                                                                      const std::string &piece) {
    using rank_t = int;

    std::vector<std::pair<size_t, rank_t>> parts; // (start, rank)
    parts.reserve(piece.length() + 1);

    auto min_rank = std::make_pair<rank_t, size_t>(std::numeric_limits<rank_t>::max(),
                                                   std::numeric_limits<size_t>::max()); // (rank, start)

    for (size_t i = 0; i < piece.length() - 1; i++) {
        rank_t rank = std::numeric_limits<rank_t>::max();
        if (auto it = ranks.find(piece.substr(i, 2)); it != ranks.end()) {
            rank = it->second;
        }
        if (rank < min_rank.first) {
            min_rank = std::make_pair(rank, i);
        }
        parts.emplace_back(std::make_pair(i, rank));
    }
    parts.emplace_back(std::make_pair(piece.length() - 1, std::numeric_limits<rank_t>::max()));
    parts.emplace_back(std::make_pair(piece.length(), std::numeric_limits<rank_t>::max()));

    auto get_rank = [&piece, &ranks](const std::vector<std::pair<size_t, rank_t>> &parts, size_t i) {
        if (i + 3 < parts.size()) {
            size_t start = parts[i].first;
            size_t end = parts[i + 3].first;
            if (auto it = ranks.find(piece.substr(start, end - start)); it != ranks.end()) {
                return it->second;
            }
        }
        return std::numeric_limits<rank_t>::max();
    };

    while (min_rank.first != std::numeric_limits<rank_t>::max()) {
        size_t i = min_rank.second;
        if (i > 0) {
            parts[i - 1].second = get_rank(parts, i - 1);
        }
        parts[i].second = get_rank(parts, i);
        parts.erase(parts.begin() + i + 1);

        min_rank = std::make_pair(std::numeric_limits<rank_t>::max(), std::numeric_limits<size_t>::max());
        for (size_t i = 0; i < parts.size() - 1; i++) {
            rank_t rank = parts[i].second;
            if (rank < min_rank.first) {
                min_rank = std::make_pair(rank, i);
            }
        }
    }

    return parts;
}

std::vector<int> TiktokenCoreBPE::byte_pair_encode(const std::string &piece,
                                                   const std::unordered_map<std::string, int> &ranks) {
    CHATGLM_CHECK(piece.length() > 1);

    auto parts = _byte_pair_merge(ranks, piece);

    std::vector<int> tokens;
    tokens.reserve(parts.size() - 1);

    for (size_t i = 1; i < parts.size(); i++) {
        size_t start = parts[i - 1].first;
        size_t end = parts[i].first;
        int rank = ranks.at(piece.substr(start, end - start));
        tokens.emplace_back(rank);
    }

    return tokens;
}

std::vector<int> TiktokenCoreBPE::_encode_ordinary_native(const std::string &text) const {
    std::vector<int> ret;
    re2::StringPiece input = text;
    re2::StringPiece prev_input = input;
    std::string piece;
    while (RE2::FindAndConsume(&input, *regex, &piece)) {
        // recover input in case of negative lookahead
        if (prev_input.find(piece) == 0) {
            input = prev_input.substr(piece.size());
            prev_input = input;
        } else {
            std::cerr << "[WARN] chatglm.cpp: encounter unknown token\n";
        }

        if (auto it = encoder.find(piece); it != encoder.end()) {
            ret.emplace_back(it->second);
        } else {
            std::vector<int> bpe_ids = byte_pair_encode(piece, encoder);
            ret.insert(ret.end(), bpe_ids.begin(), bpe_ids.end());
        }
    }
    return ret;
}

std::string TiktokenCoreBPE::_decode_native(const std::vector<int> &tokens) const {
    std::string ret;
    ret.reserve(tokens.size() * 2);
    for (int token : tokens) {
        if (auto it = decoder.find(token); it != decoder.end()) {
            ret.append(it->second);
        } else if (auto it = special_tokens_decoder.find(token); it != special_tokens_decoder.end()) {
            ret.append(it->second);
        } else {
            CHATGLM_THROW << "Unknown token " << token;
        }
    }
    return ret;
}

ChatGLM4Tokenizer::ChatGLM4Tokenizer(const std::string &vocab_text) {
    std::istringstream in(vocab_text);
    std::string base64_token;
    int rank;
    std::unordered_map<std::string, int> mergeable_ranks;
    while (in >> base64_token >> rank) {
        std::string token;
        CHATGLM_CHECK(google::protobuf::Base64Unescape(base64_token, &token));
        mergeable_ranks.emplace(std::move(token), rank);
    }
    size_t vocab_size = mergeable_ranks.size();

    const std::vector<std::string> all_special_tokens = {"<|endoftext|>",
                                                         "[MASK]",
                                                         "[gMASK]",
                                                         "[sMASK]",
                                                         "<sop>",
                                                         "<eop>",
                                                         "<|system|>",
                                                         "<|user|>",
                                                         "<|assistant|>",
                                                         "<|observation|>",
                                                         "<|begin_of_image|>",
                                                         "<|end_of_image|>",
                                                         "<|begin_of_video|>",
                                                         "<|end_of_video|>"};

    std::unordered_map<std::string, int> special_tokens_encoder;
    special_tokens_encoder.reserve(all_special_tokens.size());
    for (const auto &token : all_special_tokens) {
        special_tokens_encoder.emplace(token, vocab_size++);
    }
    // common special token ids
    gmask_token_id = special_tokens_encoder.at("[gMASK]");
    sop_token_id = special_tokens_encoder.at("<sop>");
    user_token_id = special_tokens_encoder.at("<|user|>");
    assistant_token_id = special_tokens_encoder.at("<|assistant|>");
    observation_token_id = special_tokens_encoder.at("<|observation|>");

    const std::string pattern =
        R"((?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}{1,3}| ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?:$|\s)|\s+)";
    core_bpe = TiktokenCoreBPE(std::move(mergeable_ranks), std::move(special_tokens_encoder), pattern);
}

std::vector<int> ChatGLM4Tokenizer::encode(const std::string &text, int max_length) const {
    std::vector<int> ids = core_bpe.encode_ordinary(text);
    ids.insert(ids.begin(), {gmask_token_id, sop_token_id}); // special prefix
    truncate(ids, max_length);
    return ids;
}

std::string ChatGLM4Tokenizer::decode(const std::vector<int> &ids, bool skip_special_tokens) const {
    std::vector<int> valid_ids = ids;
    if (skip_special_tokens) {
        valid_ids.erase(std::remove_if(valid_ids.begin(), valid_ids.end(),
                                       [this](int id) { return core_bpe.special_tokens_decoder.count(id) > 0; }),
                        valid_ids.end());
    }
    return core_bpe.decode(valid_ids);
}

ChatMessage ChatGLM4Tokenizer::decode_message(const std::vector<int> &ids) const {
    // TODO: support tool call
    ChatMessage message = BaseTokenizer::decode_message(ids);
    trim(message.content); // strip leading linebreak in conversation mode
    return message;
}

std::vector<int> ChatGLM4Tokenizer::apply_chat_template(const std::vector<ChatMessage> &messages,
                                                        int max_length) const {
    std::vector<int> input_ids{gmask_token_id, sop_token_id};
    std::vector<int> newline_ids = core_bpe.encode_ordinary("\n");
    for (const auto &msg : messages) {
        input_ids.emplace_back(core_bpe.special_tokens_encoder.at("<|" + msg.role + "|>"));
        input_ids.insert(input_ids.end(), newline_ids.begin(), newline_ids.end());
        std::vector<int> content_ids = core_bpe.encode_ordinary(msg.content);
        input_ids.insert(input_ids.end(), content_ids.begin(), content_ids.end());
    }
    input_ids.emplace_back(assistant_token_id);
    truncate(input_ids, max_length);
    return input_ids;
}

void ChatGLM4Tokenizer::truncate(std::vector<int> &ids, int max_length) {
    if ((int)ids.size() > max_length) {
        // sliding window: drop the least recent history while keeping the two special prefix tokens
        int num_drop = (int)ids.size() - max_length;
        ids.erase(ids.begin() + 2, ids.begin() + 2 + num_drop);
    }
}

// ===== pipeline =====

Pipeline::Pipeline(const std::string &path, int max_length) {
    auto _update_config_max_length = [](ModelConfig &config, int max_length) {
        if (max_length > 0) {
            CHATGLM_CHECK(max_length <= config.max_length)
                << "Requested max_length (" << max_length << ") exceeds the max possible model sequence length ("
                << config.max_length << ")";
            config.max_length = max_length;
        }
    };

    mapped_file = std::make_unique<MappedFile>(path);
    ModelLoader loader(mapped_file->data, mapped_file->size);

    // load magic
    std::string magic = loader.read_string(4);
    CHATGLM_CHECK(magic == "ggml") << "model file is broken (bad magic)";

    // load model type
    ModelType model_type = (ModelType)loader.read_basic<int>();
    // load version
    int version = loader.read_basic<int>();
    if (model_type == ModelType::CHATGLM) {
        // load config
        ModelConfig config;
        if (version == 1) {
            config = ModelConfig(model_type, loader.read_basic<ConfigRecordV1>(), 1e-5f, ActivationType::GELU, true,
                                 true, true, false, RopeType::CHATGLM, 10000.f, -1, AttentionMaskType::CHATGLM, 0);
        } else if (version == 2) {
            config = ModelConfig(model_type, loader.read_basic<ConfigRecordV2>(), ActivationType::GELU, true, true,
                                 true, false, RopeType::CHATGLM, -1, AttentionMaskType::CHATGLM);
        } else {
            CHATGLM_THROW << "only support version 1 or 2 for now but got " << version;
        }
        _update_config_max_length(config, max_length);

        // load tokenizer
        int proto_size = loader.read_basic<int>();
        std::string_view serialized_model_proto((char *)mapped_file->data + loader.tell(), proto_size);
        loader.seek(proto_size, SEEK_CUR);
        tokenizer = std::make_unique<ChatGLMTokenizer>(serialized_model_proto);

        // load model
        model = std::make_unique<ChatGLMForCausalLM>(config);
        model->load(loader);
    } else if (model_type == ModelType::CHATGLM2 || model_type == ModelType::CHATGLM3) {
        // load config
        ModelConfig config;
        if (version == 1) {
            config = ModelConfig(model_type, loader.read_basic<ConfigRecordV1GQA>(), 1e-5f, ActivationType::SILU, true,
                                 false, false, false, RopeType::GPTJ, 10000.f, 2, AttentionMaskType::CAUSAL, 0);
        } else if (version == 2) {
            config = ModelConfig(model_type, loader.read_basic<ConfigRecordV2>(), ActivationType::SILU, true, false,
                                 false, false, RopeType::GPTJ, 2, AttentionMaskType::CAUSAL);
        } else {
            CHATGLM_THROW << "only support version 1 or 2 for now but got " << version;
        }
        _update_config_max_length(config, max_length);

        // load tokenizer
        int proto_size = loader.read_basic<int>();
        std::string_view serialized_model_proto((char *)mapped_file->data + loader.tell(), proto_size);
        loader.seek(proto_size, SEEK_CUR);

        if (model_type == ModelType::CHATGLM2) {
            tokenizer = std::make_unique<ChatGLM2Tokenizer>(serialized_model_proto);
            model = std::make_unique<ChatGLM2ForCausalLM>(config);
        } else {
            auto chatglm3_tokenizer = std::make_unique<ChatGLM3Tokenizer>(serialized_model_proto);
            // TODO: read from checkpoint file
            config.extra_eos_token_ids = {chatglm3_tokenizer->observation_token_id, chatglm3_tokenizer->user_token_id};
            tokenizer = std::move(chatglm3_tokenizer);
            model = std::make_unique<ChatGLM3ForCausalLM>(config);
        }

        // load model
        model->load(loader);
    } else if (model_type == ModelType::CHATGLM4) {
        // load config
        CHATGLM_CHECK(version == 2) << "only support version 2 for now but got " << version;
        ModelConfig config(model_type, loader.read_basic<ConfigRecordV2>(), ActivationType::SILU, true, false, false,
                           false, RopeType::GPTJ, 2, AttentionMaskType::CAUSAL);
        _update_config_max_length(config, max_length);

        // load tokenizer
        int vocab_text_size = loader.read_basic<int>();
        std::string vocab_text = loader.read_string(vocab_text_size);
        auto chatglm4_tokenizer = std::make_unique<ChatGLM4Tokenizer>(vocab_text);
        config.extra_eos_token_ids = {chatglm4_tokenizer->observation_token_id, chatglm4_tokenizer->user_token_id};
        tokenizer = std::move(chatglm4_tokenizer);

        // load model
        model = std::make_unique<ChatGLM4ForCausalLM>(config);
        model->load(loader);
    } else if (model_type == ModelType::BAICHUAN7B) {
        std::cerr << "[WARN] Baichuan models are deprecated in favor of llama.cpp, and will be removed in next major "
                     "version of chatglm.cpp\n";
        CHATGLM_CHECK(version == 1) << "only support version 1 for now but got " << version;

        // load config
        ModelConfig config(model_type, loader.read_basic<ConfigRecordV1>(), 1e-6f, ActivationType::SILU, false, false,
                           false, false, RopeType::NEOX, 10000.f, 1, AttentionMaskType::CAUSAL, 0);
        _update_config_max_length(config, max_length);

        // load tokenizer
        int proto_size = loader.read_basic<int>();
        std::string_view serialized_model_proto((char *)mapped_file->data + loader.tell(), proto_size);
        loader.seek(proto_size, SEEK_CUR);
        tokenizer = std::make_unique<BaichuanTokenizer>(serialized_model_proto);

        // load model
        model = std::make_unique<Baichuan7BForCausalLM>(config);
        model->load(loader);
    } else if (model_type == ModelType::BAICHUAN13B) {
        std::cerr << "[WARN] Baichuan models are deprecated in favor of llama.cpp, and will be removed in next major "
                     "version of chatglm.cpp\n";
        CHATGLM_CHECK(version == 1) << "only support version 1 for now but got " << version;

        // load config
        ModelConfig config(model_type, loader.read_basic<ConfigRecordV1>(), 1e-6f, ActivationType::SILU, false, false,
                           false, true, RopeType::DISABLED, 10000.f, -1, AttentionMaskType::CAUSAL, 0);
        _update_config_max_length(config, max_length);

        // load tokenizer
        int proto_size = loader.read_basic<int>();
        std::string_view serialized_model_proto((char *)mapped_file->data + loader.tell(), proto_size);
        loader.seek(proto_size, SEEK_CUR);
        tokenizer = std::make_unique<BaichuanTokenizer>(serialized_model_proto);

        // load model
        model = std::make_unique<Baichuan13BForCausalLM>(config);
        model->load(loader);
    } else if (model_type == ModelType::INTERNLM) {
        std::cerr << "[WARN] InternLM models are deprecated in favor of llama.cpp, and will be removed in next major "
                     "version of chatglm.cpp\n";
        CHATGLM_CHECK(version == 1) << "only support version 1 for now but got " << version;

        // load config
        auto rec = loader.read_basic<ConfigRecordV1>();
        ModelConfig config;
        if (rec.hidden_size == 4096) {
            config = ModelConfig(model_type, rec, 1e-6f, ActivationType::SILU, true, true, false, false, RopeType::NEOX,
                                 10000.f, 1, AttentionMaskType::CAUSAL, 0);
        } else {
            config = ModelConfig(model_type, rec, 1e-6f, ActivationType::SILU, false, false, false, false,
                                 RopeType::NEOX, 10000.f, 1, AttentionMaskType::CAUSAL, 0);
        }
        _update_config_max_length(config, max_length);

        // load tokenizer
        int proto_size = loader.read_basic<int>();
        std::string_view serialized_model_proto((char *)mapped_file->data + loader.tell(), proto_size);
        loader.seek(proto_size, SEEK_CUR);
        tokenizer = std::make_unique<InternLMTokenizer>(serialized_model_proto);

        // load model
        model = std::make_unique<InternLMForCausalLM>(config);
        model->load(loader);
    } else {
        CHATGLM_THROW << "invalid model type " << (int)model_type;
    }
}

std::vector<int> Pipeline::generate(const std::vector<int> &input_ids, const GenerationConfig &gen_config,
                                    BaseStreamer *streamer) const {
    std::vector<int> output_ids = model->generate(input_ids, gen_config, streamer);
    std::vector<int> new_output_ids(output_ids.begin() + input_ids.size(), output_ids.end());
    return new_output_ids;
}

std::string Pipeline::generate(const std::string &prompt, const GenerationConfig &gen_config,
                               BaseStreamer *streamer) const {
    std::vector<int> input_ids = tokenizer->encode(prompt, gen_config.max_context_length);
    std::vector<int> new_output_ids = generate(input_ids, gen_config, streamer);
    std::string output = tokenizer->decode(new_output_ids);
    return output;
}

ChatMessage Pipeline::chat(const std::vector<ChatMessage> &messages, const GenerationConfig &gen_config,
                           BaseStreamer *streamer) const {
    std::vector<int> input_ids = tokenizer->apply_chat_template(messages, gen_config.max_context_length);
    std::vector<int> new_output_ids = generate(input_ids, gen_config, streamer);
    ChatMessage output = tokenizer->decode_message(new_output_ids);
    return output;
}

} // namespace chatglm
