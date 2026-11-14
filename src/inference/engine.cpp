#include "inference/engine.hpp"
#include "inference/bounded_tokenizer.hpp"
#include "core/utf8.hpp"
#include "platform/file_snapshot.hpp"
#include "llama.h"
#include "ggml-backend.h"
#include "ggml-cpu.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <thread>

#ifdef OPT_WINDOWS_CPU_KERNELS
extern "C" int opt_windows_has_avx2_kernels();
#endif

namespace opt {
namespace {
constexpr int max_piece_bytes=4096;
std::string piece(const llama_vocab* vocab, llama_token token, bool special) {
    std::string result(64, '\0');
    int n = llama_token_to_piece(vocab, token, result.data(), static_cast<int>(result.size()), 0, special);
    if (n < -max_piece_bytes) throw std::runtime_error("Token piece exceeds byte limit");
    if (n < 0) {
        result.resize(static_cast<std::size_t>(-n));
        n = llama_token_to_piece(vocab, token, result.data(), static_cast<int>(result.size()), 0, special);
    }
    if (n < 0 || static_cast<std::size_t>(n)>result.size()) throw std::runtime_error("Token piece conversion failed");
    result.resize(static_cast<std::size_t>(n));
    return result;
}
std::string metadata(const llama_model* model, const char* key) {
    const int n = llama_model_meta_val_str(model, key, nullptr, 0);
    if (n < 0) return {};
    if (n > 4096) throw std::runtime_error("Model profile metadata exceeds byte limit");
    std::string value(static_cast<std::size_t>(n) + 1, '\0');
    if (llama_model_meta_val_str(model, key, value.data(), value.size()) < 0) return {};
    value.resize(static_cast<std::size_t>(n));
    return value;
}
// The 8-thread default keeps the GUI responsive on typical laptops. An
// explicit OPT_INFERENCE_THREADS value lets a capable host use the remaining
// cores without introducing a persisted setting.
unsigned inference_threads(unsigned hardware) {
    const auto fallback = hardware <= 1 ? 1U : std::min(8U, hardware - 1);
    const char* configured = std::getenv("OPT_INFERENCE_THREADS");
    if (!configured || !*configured) return fallback;
    char* end = nullptr;
    errno = 0;
    const auto value = std::strtoul(configured, &end, 10);
    if (errno != 0 || end == configured || *end != '\0' || value == 0) return fallback;
    return static_cast<unsigned>(std::min<unsigned long>(value, std::max(1U, hardware)));
}
bool abort_decode(void* data) noexcept { return static_cast<std::atomic_bool*>(data)->load(); }
bool load_progress(float, void* data) noexcept { return !abort_decode(data); }
}

struct Engine::Impl {
    std::atomic_bool& cancelled;
    llama_model* model = nullptr;
    llama_context* context = nullptr;
    ggml_threadpool_t workers = nullptr;
    const llama_vocab* vocab = nullptr;
    llama_token bos = LLAMA_TOKEN_NULL;
    llama_token tag = LLAMA_TOKEN_NULL;
    std::vector<llama_token> protocol;
    std::vector<llama_token> stops;
    std::vector<llama_token> nonprinting_bytes;
    std::vector<llama_token> prompt_prefix, plain_suffix, easy_suffix;
    std::size_t max_token_bytes=1;
    bool cpu = true;
    std::shared_ptr<const FileSnapshot> file;
    std::FILE* stream=nullptr;
    explicit Impl(std::atomic_bool& c) : cancelled(c) { llama_backend_init(); }
    void clear() {
        if (context) llama_free(context);
        if (workers) ggml_threadpool_free(workers);
        if (model) llama_model_free(model);
        context = nullptr; workers = nullptr; model = nullptr; vocab = nullptr;
        if(stream)std::fclose(stream);stream=nullptr;file.reset();
        protocol.clear();stops.clear();nonprinting_bytes.clear();
        prompt_prefix.clear(); plain_suffix.clear(); easy_suffix.clear();
        max_token_bytes=1;
    }
    ~Impl() { clear(); llama_backend_free(); }
};
Engine::Engine(std::atomic_bool& c) : impl_(std::make_unique<Impl>(c)) {}
Engine::~Engine() = default;
void Engine::unload() { impl_->clear(); }
InferenceRuntime Engine::runtime()const{
    const auto* c=impl_->context;if(!c)return {};
    return {llama_n_ctx(c),llama_n_batch(c),llama_n_ubatch(c),llama_n_threads(impl_->context),llama_n_threads_batch(impl_->context)};
}

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
    // Keep tensors in owned buffers. A mapped GGUF can fault in the middle of
    // CPU/Metal work if another process truncates it; signature checks cannot
    // protect an individual tensor access. This creates no on-disk snapshot.
    mp.load_mode = LLAMA_LOAD_MODE_NONE;
    mp.lazy_mode = LLAMA_LAZY_MODE_OFF;
    mp.progress_callback = load_progress;
    mp.progress_callback_user_data = &p.cancelled;
    try {
        p.file=FileSnapshot::open(path);p.stream=p.file->open_stream();
        p.model=llama_model_load_from_file_ptr(p.stream,mp);
        if(!p.model)throw std::runtime_error("GGUFを読み込めません。モデルファイルを確認して選び直してください。");
        if(p.file->signature()!=p.file->initial_signature())throw std::runtime_error("GGUFが読込中に変更されました。再度選択してください。");
        p.vocab = llama_model_get_vocab(p.model);
        // Quantization tools may rewrite general.name, so compatibility is
        // defined by the PLaMo 3 architecture/tokenizer and the PFML3 protocol
        // checks below instead of the product name in the GGUF metadata.
        if (metadata(p.model, "general.architecture") != "plamo3" ||
            metadata(p.model, "tokenizer.ggml.model") != "plamo3" ||
            llama_vocab_type(p.vocab) != LLAMA_VOCAB_TYPE_PLAMO3)
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
            if(p.cancelled.load())throw PreparationCancelled();
            if((llama_vocab_get_attr(p.vocab,id)&LLAMA_TOKEN_ATTR_BYTE)&&
               unsupported_text_control(piece(p.vocab,id,false))!=std::string_view::npos)
                p.nonprinting_bytes.push_back(id);
            if(llama_vocab_is_eog(p.vocab,id)&&std::find(p.stops.begin(),p.stops.end(),id)==p.stops.end())p.stops.push_back(id);
            const auto* raw=llama_vocab_get_text(p.vocab,id);if(!raw)continue;
            const std::string_view name(raw);
            const auto attr=llama_vocab_get_attr(p.vocab,id);
            if((attr&LLAMA_TOKEN_ATTR_USER_DEFINED)&&!(attr&(LLAMA_TOKEN_ATTR_CONTROL|LLAMA_TOKEN_ATTR_UNKNOWN))&&
               ((attr&(LLAMA_TOKEN_ATTR_LSTRIP|LLAMA_TOKEN_ATTR_RSTRIP))||piece(p.vocab,id,true).find("\xee\xb8\x80")!=std::string::npos))
                throw std::runtime_error("未対応のPLaMo tokenizer設定です。入力を省略する特殊tokenは使用できません。");
            // The public length query includes embedded NULs, unlike get_text().
            char unused=0;
            const auto bytes=llama_token_to_piece(p.vocab,id,&unused,0,0,true);
            if(bytes < -max_piece_bytes || bytes > max_piece_bytes)
                throw std::runtime_error("Token piece exceeds byte limit");
            p.max_token_bytes=std::max(p.max_token_bytes,static_cast<std::size_t>(bytes<0?-bytes:bytes));
            if(name.starts_with("<|plamo:")&&name.ends_with("|>")&&
               std::find(p.protocol.begin(),p.protocol.end(),id)==p.protocol.end())p.protocol.push_back(id);
        }
        if (p.bos != llama_vocab_bos(p.vocab) || !llama_vocab_get_add_bos(p.vocab))
            throw std::runtime_error("BOS設定が対応profileと一致しません。");
        p.prompt_prefix = tokenize("<|plamo:tag|>dataset<|plamo:msg|>translation"
                                   "<|plamo:tag|>input<|plamo:key|>lang<|plamo:val|>en<|plamo:msg|>", true, true);
        const std::string output = "<|plamo:tag|>output<|plamo:key|>lang<|plamo:val|>";
        p.plain_suffix = tokenize(output + "ja<|plamo:key|>form<|plamo:val|>plain<|plamo:msg|>", false, true);
        // The official template normalizes target_lang="ja(easy)" to PFML3's ja-easy.
        // Easy Japanese is a target language and does not take a form parameter.
        p.easy_suffix = tokenize(output + "ja-easy<|plamo:msg|>", false, true);
        const int training_context = llama_model_n_ctx_train(p.model);
        if (training_context <= 0) throw std::runtime_error("モデルのcontext容量を取得できません。");
        auto cp = llama_context_default_params();
        cp.n_batch = 512; cp.n_ubatch = 128; cp.n_seq_max = 1;
        const unsigned hw = std::thread::hardware_concurrency();
        cp.n_threads = cp.n_threads_batch = static_cast<int>(inference_threads(hw));
        cp.abort_callback = abort_decode; cp.abort_callback_data = &p.cancelled;
        cp.offload_kqv = !p.cpu; cp.op_offload = !p.cpu;
        // Use the model's full context so a page is never truncated merely
        // because of an application-side cap. If the KV/compute allocation for
        // that size fails, halve it until it fits: the reported capacity is
        // always llama_n_ctx(), so the user sees the context actually in use.
        std::uint32_t context_size = static_cast<std::uint32_t>(training_context);
        for (;;) {
            if(p.cancelled.load())throw PreparationCancelled();
            cp.n_ctx = context_size;
            p.context = llama_init_from_model(p.model, cp);
            if (p.context || context_size <= 2048) break;
            context_size /= 2;
        }
        if (!p.context) throw std::runtime_error("推論contextを確保できません。モデルを読み直してください。");
        if (p.cpu) {
            auto params = ggml_threadpool_params_default(std::max(cp.n_threads, cp.n_threads_batch));
            params.paused = true;
            p.workers = ggml_threadpool_new(&params);
            if (!p.workers) throw std::runtime_error("CPU推論スレッドを確保できません。");
            llama_attach_threadpool(p.context, p.workers, nullptr);
        }
        if(p.file->signature()!=p.file->initial_signature())throw std::runtime_error("GGUFが読込中に変更されました。再度選択してください。");
    } catch (...) { unload(); throw; }
}

std::vector<std::int32_t> Engine::tokenize(std::string_view text, bool add_bos, bool special) const {
    if (!impl_->vocab || text.size() > INT32_MAX) throw std::runtime_error("Invalid tokenizer input");
    std::vector<llama_token> result(std::min<std::size_t>(text.size(), 4096) + 1);
    auto encode = [&] {
        return llama_tokenize(impl_->vocab, text.data(), static_cast<int>(text.size()), result.data(),
                              static_cast<int>(result.size()), add_bos, special);
    };
    int n = encode();
    if (n == INT32_MIN) throw std::runtime_error("Tokenizer overflow");
    if (n < 0) {
        result.resize(static_cast<std::size_t>(-n));
        n = encode();
    }
    if (n < 0) throw std::runtime_error("Tokenization failed");
    result.resize(static_cast<std::size_t>(n));
    return result;
}

PreparedInput Engine::prepare(std::string_view source, Form form) const {
    auto& p = *impl_;
    const auto checkpoint=[&]{if(p.cancelled.load())throw PreparationCancelled();};
    checkpoint();
    if(!p.context||!p.vocab)throw std::runtime_error("Model unavailable");
    PreparedInput prepared;
    prepared.stop_ids=p.stops;
    const auto& tail = form == Form::Easy ? p.easy_suffix : p.plain_suffix;
    const auto overhead=p.prompt_prefix.size()+tail.size();
    if(overhead>=capacity())throw InputContextLimit();
    const auto body_limit=capacity()-overhead-1; // Keep at least one output token.
    auto raw_body=tokenize_plamo3_bounded(source,body_limit,p.max_token_bytes,
        [&](std::string_view part,std::span<std::int32_t> output){
            return llama_tokenize(p.vocab,part.data(),static_cast<int>(part.size()),output.data(),
                                  static_cast<int>(output.size()),false,false);
        },checkpoint);
    // A PDF can contain literal PFML3 markers or binary control characters.
    // They cannot be represented as ordinary text in this profile; omit only
    // those tokens and translate the rest of the page instead of rejecting it.
    std::vector<std::int32_t> body;
    body.reserve(raw_body.size());
    for (auto token : raw_body) {
        checkpoint();
        if ((llama_vocab_get_attr(p.vocab, token) & LLAMA_TOKEN_ATTR_CONTROL) ||
            llama_vocab_is_eog(p.vocab, token) ||
            std::find(p.protocol.begin(), p.protocol.end(), token) != p.protocol.end())
            continue;
        body.push_back(token);
    }
    if (body.empty() && !raw_body.empty())
        throw ModelInputUnsupported("ModelInputUnsupported: 本文をモデル入力として表現できません。");
    prepared.tokens.reserve(p.prompt_prefix.size() + body.size() + tail.size());
    prepared.tokens.insert(prepared.tokens.end(), p.prompt_prefix.begin(), p.prompt_prefix.end());
    prepared.tokens.insert(prepared.tokens.end(), body.begin(), body.end());
    prepared.tokens.insert(prepared.tokens.end(), tail.begin(), tail.end());
    prepared.source_tokens = static_cast<std::uint32_t>(body.size());
    prepared.output_limit = static_cast<std::uint32_t>(std::min<std::size_t>(capacity(),
        std::max<std::size_t>(512, 2 * body.size() + 128)));
    checkpoint();
    return prepared;
}
std::uint32_t Engine::capacity() const { return impl_->context ? llama_n_ctx(impl_->context) : 0; }
std::string Engine::backend() const {
#ifdef OPT_WINDOWS_CPU_KERNELS
    return opt_windows_has_avx2_kernels() ? "CPU (AVX2)" : "CPU (portable)";
#else
    return impl_->cpu ? "CPU" : "Metal";
#endif
}
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
    // PLaMo 3 uses attention KV caches. Invalidating all cells starts a fresh
    // sequence without writing zeros across unused key and value buffers.
    llama_memory_clear(llama_get_memory(p.context), false);
    // Graph kickoff resumes the pool. Park the workers promptly on every exit,
    // including cancellation, instead of polling while the reader is idle.
    struct ParkWorkers {
        ggml_threadpool_t pool;
        ~ParkWorkers() { if (pool) ggml_threadpool_pause(pool); }
    } park{p.workers};
    Utf8Stream decoder;
    auto batch = llama_batch_init(512, 0, 1);
    struct BatchGuard { llama_batch& b; ~BatchGuard() { llama_batch_free(b); } } guard{batch};
    if (!batch.token || !batch.pos || !batch.n_seq_id || !batch.seq_id || !batch.logits)
        throw std::runtime_error("推論batchを確保できません。");
    auto decode = [&](const llama_token* ids, int n, int pos, bool need_logits) {
        batch.n_tokens = n;
        for (int i = 0; i < n; ++i) {
            batch.token[i] = ids[i]; batch.pos[i] = pos + i;
            batch.n_seq_id[i] = 1; batch.seq_id[i][0] = 0;
            batch.logits[i] = static_cast<int8_t>(need_logits && i == n - 1);
        }
        return llama_decode(p.context, batch);
    };
    for (std::size_t i = 0; i < input.tokens.size();) {
        if (p.cancelled.load()) { result.reason = FinishReason::Cancelled; return result; }
        const int n = static_cast<int>(std::min<std::size_t>(512, input.tokens.size() - i));
        if (decode(input.tokens.data() + i, n, static_cast<int>(i), i + n == input.tokens.size()) != 0) {
            result.reason = p.cancelled.load() ? FinishReason::Cancelled : FinishReason::DecodeError;
            return result;
        }
        i += n;
    }
    std::string pending_delta;
    auto last = std::chrono::steady_clock::now();
    auto flush = [&] { if (!pending_delta.empty()) { delta(pending_delta); pending_delta.clear(); } };
    result.reason = FinishReason::OutputLimit;
    const int vocab_size = llama_vocab_n_tokens(p.vocab);
    for (std::uint32_t i = 0; i < input.output_limit; ++i) {
        if (p.cancelled.load()) { result.reason = FinishReason::Cancelled; break; }
        // Greedy sampling selects the first maximum. Scan logits in place
        // instead of building a vocabulary-sized candidate array per token.
        float* logits = llama_get_logits_ith(p.context, -1);
        if (!logits || vocab_size <= 0) { result.reason = FinishReason::DecodeError; break; }
        // Byte fallback tokens for binary controls cannot appear in display text.
        // Mask them before selection so a single such candidate cannot end a page.
        for (const auto id : p.nonprinting_bytes) logits[id] = -std::numeric_limits<float>::infinity();
        const llama_token token = static_cast<llama_token>(std::max_element(logits, logits + vocab_size) - logits);
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
            if(complete.size()>max_translation_bytes-result.text.size()){
                result.reason=FinishReason::OutputLimit;break;
            }
            const auto control=unsupported_text_control(complete);
            if(control!=std::string::npos){
                result.text.append(complete,0,control);pending_delta.append(complete,0,control);
                result.reason=FinishReason::InvalidModelOutput;break;
            }
            result.text += complete; pending_delta += complete;
        } catch (const Utf8Error&) { result.reason = FinishReason::InvalidEncoding; break; }
        catch (const std::exception&) {result.reason=FinishReason::DecodeError;break;}
        ++result.output_tokens;
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::milliseconds(50)) {
            flush(); last = now;
            // Check at the existing delivery cadence rather than doing file
            // I/O for every token. Resident tensors remain valid meanwhile.
            if(p.file->signature()!=p.file->initial_signature()){
                result.reason=FinishReason::ModelUnavailable;break;
            }
        }
        const auto position = input.tokens.size() + i;
        if (position >= capacity()) { result.reason = FinishReason::ContextLimit; break; }
        if (i + 1 < input.output_limit && decode(&token, 1, static_cast<int>(position), true) != 0) {
            result.reason = p.cancelled.load() ? FinishReason::Cancelled : FinishReason::DecodeError;
            break;
        }
    }
    flush();
    if (!decoder.complete() && (result.reason == FinishReason::Completed||result.reason==FinishReason::EmptyOutput))
        result.reason = FinishReason::InvalidEncoding;
    if(p.file->signature()!=p.file->initial_signature())result.reason=FinishReason::ModelUnavailable;
    return result;
}
}
