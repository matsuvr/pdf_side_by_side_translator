#include "inference/engine.hpp"
#include "core/utf8.hpp"
#include "platform/file_snapshot.hpp"
#include "llama.h"
#include "ggml-backend.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <limits>
#include <stdexcept>
#include <thread>

namespace opt {
namespace {
std::string piece(const llama_vocab* vocab, llama_token token, bool special) {
    std::string result(64, '\0');
    int n = llama_token_to_piece(vocab, token, result.data(), static_cast<int>(result.size()), 0, special);
    if (n == INT32_MIN) throw std::runtime_error("Token piece overflow");
    if (n < 0) {
        result.resize(static_cast<std::size_t>(-n));
        n = llama_token_to_piece(vocab, token, result.data(), static_cast<int>(result.size()), 0, special);
    }
    if (n < 0) throw std::runtime_error("Token piece conversion failed");
    result.resize(static_cast<std::size_t>(n));
    return result;
}
std::string metadata(const llama_model* model, const char* key) {
    const int n = llama_model_meta_val_str(model, key, nullptr, 0);
    if (n < 0) return {};
    std::string value(static_cast<std::size_t>(n) + 1, '\0');
    if (llama_model_meta_val_str(model, key, value.data(), value.size()) < 0) return {};
    value.resize(static_cast<std::size_t>(n));
    return value;
}
bool abort_decode(void* data) noexcept { return static_cast<std::atomic_bool*>(data)->load(); }
bool load_progress(float, void* data) noexcept { return !abort_decode(data); }
}

struct Engine::Impl {
    std::atomic_bool& cancelled;
    llama_model* model = nullptr;
    llama_context* context = nullptr;
    const llama_vocab* vocab = nullptr;
    llama_sampler* sampler = nullptr;
    llama_token bos = LLAMA_TOKEN_NULL;
    llama_token tag = LLAMA_TOKEN_NULL;
    std::vector<llama_token> protocol;
    std::vector<llama_token> stops;
    bool cpu = true;
    std::shared_ptr<const FileSnapshot> file;
    std::FILE* stream=nullptr;
    explicit Impl(std::atomic_bool& c) : cancelled(c) { llama_backend_init(); }
    void clear() {
        if (sampler) llama_sampler_free(sampler);
        if (context) llama_free(context);
        if (model) llama_model_free(model);
        sampler = nullptr; context = nullptr; model = nullptr; vocab = nullptr;
        if(stream)std::fclose(stream);stream=nullptr;file.reset();
        protocol.clear();stops.clear();
    }
    ~Impl() { clear(); llama_backend_free(); }
};
Engine::Engine(std::atomic_bool& c) : impl_(std::make_unique<Impl>(c)) {}
Engine::~Engine() = default;
void Engine::unload() { impl_->clear(); }

void Engine::load(const std::filesystem::path& path, bool cpu_only) {
    unload();
    auto& p = *impl_;
    auto mp = llama_model_default_params();
    // Register only an actual Metal device, or explicitly use the CPU path.
    std::array<ggml_backend_dev_t,2> devices{nullptr,nullptr};
    if(!cpu_only)for(std::size_t i=0;i<ggml_backend_dev_count();++i){
        auto device=ggml_backend_dev_get(i);const auto type=ggml_backend_dev_type(device);
        if((type==GGML_BACKEND_DEVICE_TYPE_GPU||type==GGML_BACKEND_DEVICE_TYPE_IGPU)&&
           std::string_view(ggml_backend_reg_name(ggml_backend_dev_backend_reg(device)))=="MTL"){
            devices[0]=device;break;
        }
    }
    p.cpu=devices[0]==nullptr;mp.devices=devices.data();mp.n_gpu_layers=p.cpu?0:-1;
    mp.load_mode = llama_supports_mmap() ? LLAMA_LOAD_MODE_MMAP : LLAMA_LOAD_MODE_NONE;
    mp.progress_callback = load_progress;
    mp.progress_callback_user_data = &p.cancelled;
    try {
        p.file=FileSnapshot::open(path);p.stream=p.file->open_stream();
        p.model=llama_model_load_from_file_ptr(p.stream,mp);
        if(!p.model)throw std::runtime_error("GGUFを読み込めません。別のモデルまたはCPUを選択してください。");
        if(p.file->signature()!=p.file->initial_signature())throw std::runtime_error("GGUFが読込中に変更されました。再度選択してください。");
        p.vocab = llama_model_get_vocab(p.model);
        if (metadata(p.model, "general.architecture") != "plamo3" ||
            metadata(p.model, "tokenizer.ggml.model") != "plamo3" ||
            llama_vocab_type(p.vocab) != LLAMA_VOCAB_TYPE_PLAMO3 ||
            metadata(p.model, "general.name").find("translate-0.8b") == std::string::npos)
            throw std::runtime_error("未対応のモデルです。PLaMo 3 Translate 0.8Bのplamo3 GGUFを選んでください。");
        for (const auto* marker : {"<|plamo:bos|>", "<|plamo:tag|>", "<|plamo:msg|>",
                                    "<|plamo:key|>", "<|plamo:val|>"}) {
            auto ids = tokenize(marker, false, true);
            if (ids.size() != 1 || piece(p.vocab, ids[0], true) != marker)
                throw std::runtime_error("必要なPFML3 tokenがありません。");
            p.protocol.push_back(ids[0]);
        }
        p.bos = p.protocol[0]; p.tag = p.protocol[1];
        p.stops={p.bos,p.tag};
        for(llama_token id=0;id<llama_vocab_n_tokens(p.vocab);++id){
            if(llama_vocab_is_eog(p.vocab,id)&&std::find(p.stops.begin(),p.stops.end(),id)==p.stops.end())p.stops.push_back(id);
            const auto* raw=llama_vocab_get_text(p.vocab,id);if(!raw)continue;
            const std::string_view name(raw);
            if(name.starts_with("<|plamo:")&&name.ends_with("|>")&&
               std::find(p.protocol.begin(),p.protocol.end(),id)==p.protocol.end())p.protocol.push_back(id);
        }
        if (p.bos != llama_vocab_bos(p.vocab) || !llama_vocab_get_add_bos(p.vocab))
            throw std::runtime_error("BOS設定が対応profileと一致しません。");
        const int training_context = llama_model_n_ctx_train(p.model);
        if (training_context <= 0) throw std::runtime_error("モデルのcontext容量を取得できません。");
        auto cp = llama_context_default_params();
        cp.n_ctx = std::min(8192, training_context);
        cp.n_batch = 512; cp.n_ubatch = 128; cp.n_seq_max = 1;
        const unsigned hw = std::thread::hardware_concurrency();
        cp.n_threads = cp.n_threads_batch = static_cast<int>(hw <= 1 ? 1 : std::min(8U, hw - 1));
        cp.abort_callback = abort_decode; cp.abort_callback_data = &p.cancelled;
        cp.offload_kqv = !p.cpu; cp.op_offload = !p.cpu;
        p.context = llama_init_from_model(p.model, cp);
        if (!p.context) throw std::runtime_error("推論contextを確保できません。CPUまたは別のGGUFを選んでください。");
        p.sampler = llama_sampler_init_greedy();
        if (!p.sampler) throw std::runtime_error("Samplerを確保できません。");
        if(p.file->signature()!=p.file->initial_signature())throw std::runtime_error("GGUFが読込中に変更されました。再度選択してください。");
    } catch (...) { unload(); throw; }
}

std::vector<std::int32_t> Engine::tokenize(std::string_view text, bool add_bos, bool special) const {
    if (!impl_->vocab || text.size() > INT32_MAX) throw std::runtime_error("Invalid tokenizer input");
    int n = llama_tokenize(impl_->vocab, text.data(), static_cast<int>(text.size()), nullptr, 0, add_bos, special);
    if (n == INT32_MIN) throw std::runtime_error("Tokenizer overflow");
    std::vector<llama_token> result(static_cast<std::size_t>(n < 0 ? -n : n));
    n = llama_tokenize(impl_->vocab, text.data(), static_cast<int>(text.size()), result.data(),
                       static_cast<int>(result.size()), add_bos, special);
    if (n < 0) throw std::runtime_error("Tokenization failed");
    result.resize(static_cast<std::size_t>(n));
    return result;
}

PreparedInput Engine::prepare(std::string_view source, Form form) const {
    auto& p = *impl_;
    PreparedInput prepared;
    prepared.stop_ids=p.stops;
    auto body = tokenize(source, false, false);
    for (auto token : body) {
        if ((llama_vocab_get_attr(p.vocab, token) & LLAMA_TOKEN_ATTR_CONTROL) ||
            llama_vocab_is_eog(p.vocab, token) ||
            std::find(p.protocol.begin(), p.protocol.end(), token) != p.protocol.end())
            throw ModelInputUnsupported("ModelInputUnsupported: 本文の予約表記を通常文字として表現できません。");
    }
    prepared.tokens = tokenize("<|plamo:tag|>dataset<|plamo:msg|>translation"
                              "<|plamo:tag|>input<|plamo:key|>lang<|plamo:val|>en<|plamo:msg|>", true, true);
    prepared.tokens.insert(prepared.tokens.end(), body.begin(), body.end());
    const std::string output = "<|plamo:tag|>output<|plamo:key|>lang<|plamo:val|>ja"
                              "<|plamo:key|>form<|plamo:val|>" +
                              std::string(form == Form::Plain ? "plain" : "polite") + "<|plamo:msg|>";
    auto tail = tokenize(output, false, true);
    prepared.tokens.insert(prepared.tokens.end(), tail.begin(), tail.end());
    prepared.source_tokens = static_cast<std::uint32_t>(body.size());
    prepared.output_limit = static_cast<std::uint32_t>(std::min<std::size_t>(4096,
        std::max<std::size_t>(512, 2 * body.size() + 128)));
    return prepared;
}
std::uint32_t Engine::capacity() const { return impl_->context ? llama_n_ctx(impl_->context) : 0; }
std::string Engine::backend() const { return impl_->cpu ? "CPU" : "Metal"; }
std::shared_ptr<const FileSnapshot> Engine::file_snapshot() const{return impl_->file;}

GenerationResult Engine::generate(const PreparedInput& input,
                                 const std::function<void(std::string_view)>& delta) {
    auto& p = *impl_;
    GenerationResult result;
    if (!p.context) return result;
    if(input.stop_ids!=p.stops){result.reason=FinishReason::ModelInputUnsupported;return result;}
    if(p.file->signature()!=p.file->initial_signature())return result;
    if (input.tokens.empty() || input.tokens.size() + input.output_limit > capacity()) {
        result.reason = FinishReason::ContextLimit; return result;
    }
    llama_memory_clear(llama_get_memory(p.context), true);
    llama_sampler_reset(p.sampler);
    Utf8Stream decoder;
    auto batch = llama_batch_init(512, 0, 1);
    struct BatchGuard { llama_batch& b; ~BatchGuard() { llama_batch_free(b); } } guard{batch};
    if (!batch.token || !batch.pos || !batch.n_seq_id || !batch.seq_id || !batch.logits)
        throw std::runtime_error("推論batchを確保できません。");
    auto decode = [&](const llama_token* ids, int n, int pos) {
        batch.n_tokens = n;
        for (int i = 0; i < n; ++i) {
            batch.token[i] = ids[i]; batch.pos[i] = pos + i;
            batch.n_seq_id[i] = 1; batch.seq_id[i][0] = 0;
            batch.logits[i] = static_cast<int8_t>(i == n - 1);
        }
        return llama_decode(p.context, batch);
    };
    for (std::size_t i = 0; i < input.tokens.size();) {
        if (p.cancelled.load()) { result.reason = FinishReason::Cancelled; return result; }
        const int n = static_cast<int>(std::min<std::size_t>(512, input.tokens.size() - i));
        if (decode(input.tokens.data() + i, n, static_cast<int>(i)) != 0) {
            result.reason = p.cancelled.load() ? FinishReason::Cancelled : FinishReason::DecodeError;
            return result;
        }
        i += n;
    }
    std::string pending_delta;
    auto last = std::chrono::steady_clock::now();
    auto flush = [&] { if (!pending_delta.empty()) { delta(pending_delta); pending_delta.clear(); } };
    result.reason = FinishReason::OutputLimit;
    for (std::uint32_t i = 0; i < input.output_limit; ++i) {
        if (p.cancelled.load()) { result.reason = FinishReason::Cancelled; break; }
        const auto token = llama_sampler_sample(p.sampler, p.context, -1);
        if(token<0||token>=llama_vocab_n_tokens(p.vocab)){result.reason=FinishReason::InvalidModelOutput;break;}
        if (std::find(input.stop_ids.begin(),input.stop_ids.end(),token)!=input.stop_ids.end()) {
            result.reason = has_usable_text(result.text) ? FinishReason::Completed : FinishReason::EmptyOutput;
            break;
        }
        if ((llama_vocab_get_attr(p.vocab, token) & (LLAMA_TOKEN_ATTR_CONTROL|LLAMA_TOKEN_ATTR_UNKNOWN|LLAMA_TOKEN_ATTR_UNUSED)) ||
            std::find(p.protocol.begin(), p.protocol.end(), token) != p.protocol.end()) {
            result.reason = FinishReason::InvalidModelOutput; break;
        }
        try {
            auto complete = decoder.push(piece(p.vocab, token, false));
            result.text += complete; pending_delta += complete;
        } catch (const Utf8Error&) { result.reason = FinishReason::InvalidEncoding; break; }
        catch (const std::exception&) {result.reason=FinishReason::DecodeError;break;}
        ++result.output_tokens;
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::milliseconds(50)) { flush(); last = now; }
        const auto position = input.tokens.size() + i;
        if (position >= capacity()) { result.reason = FinishReason::ContextLimit; break; }
        if (i + 1 < input.output_limit && decode(&token, 1, static_cast<int>(position)) != 0) {
            result.reason = p.cancelled.load() ? FinishReason::Cancelled : FinishReason::DecodeError;
            break;
        }
    }
    flush();
    if (!decoder.complete() && (result.reason == FinishReason::Completed||result.reason==FinishReason::EmptyOutput))
        result.reason = FinishReason::InvalidEncoding;
    if(result.reason==FinishReason::Completed&&p.file->signature()!=p.file->initial_signature())result.reason=FinishReason::ModelUnavailable;
    return result;
}
}
