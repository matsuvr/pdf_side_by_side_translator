#include "inference/engine.hpp"
#include "inference/bounded_tokenizer.hpp"
#include "core/utf8.hpp"
#include "platform/file_snapshot.hpp"
#include "plamo08/model.hpp"
#include "plamo08/tokenizer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <set>
#include <nlohmann/json.hpp>

namespace opt {
namespace {
constexpr int prefill_chunk = 256;
bool protocol_piece(std::string_view text) {
    return text.starts_with("<|plamo:") && text.ends_with("|>");
}
}

struct Engine::Impl {
    std::atomic_bool& cancelled;
    std::unique_ptr<plamo08::Model> model;
    std::unique_ptr<plamo08::Tokenizer> tokenizer;
    std::vector<std::shared_ptr<const FileSnapshot>> files;
    std::vector<std::int32_t> stops, prefix, plain_suffix, easy_suffix;
    std::vector<int> nonprinting_bytes;
    std::vector<bool> protocol;
    std::size_t max_token_bytes = 1;
    int bos = -1;
    plamo08::Backend mode = plamo08::Backend::fused;
    explicit Impl(std::atomic_bool& value) : cancelled(value) {}
    void checkpoint() const { if (cancelled.load()) throw PreparationCancelled(); }
    bool unchanged() const {
        return std::all_of(files.begin(), files.end(), [](const auto& file) { return file->matches_initial_signature(); });
    }
    void clear() {
        model.reset(); tokenizer.reset(); files.clear(); stops.clear(); prefix.clear();
        plain_suffix.clear(); easy_suffix.clear(); nonprinting_bytes.clear(); protocol.clear();
        max_token_bytes = 1; bos = -1;
    }
};

Engine::Engine(std::atomic_bool& cancelled) : impl_(std::make_unique<Impl>(cancelled)) {}
Engine::~Engine() = default;
void Engine::unload() { impl_->clear(); }

void Engine::load(const std::filesystem::path& selected, bool cpu_only) {
    unload();
    auto& p = *impl_;
    if (cpu_only) throw std::runtime_error("このMac版はApple SiliconのMLX / Metalで推論します。autoを指定してください。");
    const auto directory = std::filesystem::is_directory(selected) ? selected : selected.parent_path();
    const auto checkpoint = [&] { p.checkpoint(); };
    try {
        checkpoint();
        std::ifstream input(directory / "config.json");
        if (!input) throw std::runtime_error("config.jsonを開けません。PLaMo 3 Translate 0.8B MLX 4bitのフォルダーを選択してください。");
        const auto config = nlohmann::json::parse(input);
        const int context = config.at("max_position_embeddings").get<int>();
        std::set<std::filesystem::path> names{"config.json", "tokenizer.jsonl"};
        for (const auto* optional : {"tokenizer_config.json", "generation_config.json"})
            if (std::filesystem::exists(directory / optional)) names.insert(optional);
        const auto index = directory / "model.safetensors.index.json";
        if (std::filesystem::exists(index)) {
            names.insert(index.filename());
            std::ifstream file(index);
            const auto manifest = nlohmann::json::parse(file);
            for (const auto& item : manifest.at("weight_map").items()) names.insert(item.value().get<std::string>());
        } else names.insert("model.safetensors");
        for (const auto& name : names) p.files.push_back(FileSnapshot::open(directory / name));
        p.tokenizer = std::make_unique<plamo08::Tokenizer>(directory, checkpoint);
        const auto& tokenizer = *p.tokenizer;
        p.bos = tokenizer.token_id("<|plamo:bos|>");
        p.stops = {p.bos, tokenizer.token_id("<|plamo:tag|>")};
        p.protocol.resize(tokenizer.vocab_size());
        for (std::size_t id = 0; id < tokenizer.vocab_size(); ++id) {
            if (id % 1024 == 0) checkpoint();
            const auto piece = tokenizer.token_bytes(static_cast<int>(id));
            p.protocol[id] = protocol_piece(piece);
            p.max_token_bytes = std::max(p.max_token_bytes, piece.size());
            if (tokenizer.is_eos(static_cast<int>(id)) && std::find(p.stops.begin(), p.stops.end(), id) == p.stops.end())
                p.stops.push_back(static_cast<std::int32_t>(id));
            if (tokenizer.token_type(static_cast<int>(id)) == plamo08::TokenType::byte &&
                unsupported_text_control(piece) != std::string_view::npos)
                p.nonprinting_bytes.push_back(static_cast<int>(id));
        }
        p.prefix = tokenize("<|plamo:tag|>dataset<|plamo:msg|>translation<|plamo:tag|>input<|plamo:msg|>", true, true);
        const std::string output = "<|plamo:tag|>output<|plamo:key|>lang<|plamo:val|>";
        p.plain_suffix = tokenize(output + "ja<|plamo:key|>form<|plamo:val|>plain<|plamo:msg|>", false, true);
        p.easy_suffix = tokenize(output + "ja-easy<|plamo:msg|>", false, true);
        // The reference mode uses the same checkpoint/tokenizer for comparison
        // on the test Mac; normal launches always select the fused decoder.
        const auto* mode = std::getenv("OPT_MLX_BACKEND");
        p.mode = mode && std::string_view(mode) == "mlx" ? plamo08::Backend::mlx : plamo08::Backend::fused;
        p.model = std::make_unique<plamo08::Model>(directory, context, checkpoint);
        if (!p.unchanged()) throw std::runtime_error("モデルが読込中に変更されました。再度選択してください。");
    } catch (...) { unload(); throw; }
}

std::vector<std::int32_t> Engine::tokenize(std::string_view text, bool add_bos, bool /* special */) const {
    auto& p = *impl_;
    if (!p.tokenizer) throw std::runtime_error("Model unavailable");
    // PLaMo's native tokenizer always recognizes PFML markers. prepare()
    // removes reserved/control IDs from the PDF body, as the GGUF path does.
    const auto encoded = p.tokenizer->encode(std::string(text), [&] { p.checkpoint(); });
    std::vector<std::int32_t> result;
    result.reserve(encoded.size() + static_cast<unsigned>(add_bos));
    if (add_bos) result.push_back(p.bos);
    result.insert(result.end(), encoded.begin(), encoded.end());
    return result;
}

PreparedInput Engine::prepare(std::string_view source, Form form) const {
    auto& p = *impl_;
    const auto checkpoint = [&] { p.checkpoint(); };
    checkpoint();
    if (!p.model || !p.tokenizer) throw std::runtime_error("Model unavailable");
    const auto& tail = form == Form::Easy ? p.easy_suffix : p.plain_suffix;
    const auto overhead = p.prefix.size() + tail.size();
    if (overhead >= capacity()) throw InputContextLimit();
    auto raw = tokenize_plamo3_bounded(source, capacity() - overhead - 1, p.max_token_bytes,
        [&](std::string_view part, std::span<std::int32_t> output) {
            const auto tokens = p.tokenizer->encode(std::string(part), checkpoint);
            if (tokens.size() > output.size()) return -1;
            std::copy(tokens.begin(), tokens.end(), output.begin());
            return static_cast<int>(tokens.size());
        }, checkpoint);
    PreparedInput result;
    result.stop_ids = p.stops;
    result.tokens = p.prefix;
    for (const int id : raw) {
        checkpoint();
        if (p.protocol[id] || p.tokenizer->token_type(id) == plamo08::TokenType::control || p.tokenizer->is_eos(id)) continue;
        result.tokens.push_back(id);
        ++result.source_tokens;
    }
    if (!result.source_tokens && !raw.empty()) throw ModelInputUnsupported("本文をモデル入力として表現できません。");
    result.tokens.insert(result.tokens.end(), tail.begin(), tail.end());
    result.output_limit = std::min(capacity(), std::max(512U, 2 * result.source_tokens + 128));
    checkpoint();
    return result;
}

std::uint32_t Engine::capacity() const { return impl_->model ? impl_->model->context_size() : 0; }
std::string Engine::backend() const { return impl_->mode == plamo08::Backend::fused ? "MLX / Metal (0.8B fused)" : "MLX / Metal (reference)"; }
InferenceRuntime Engine::runtime() const { return impl_->model ? InferenceRuntime{capacity(), prefill_chunk, prefill_chunk, 0, 0} : InferenceRuntime{}; }
std::shared_ptr<const FileSnapshot> Engine::file_snapshot() const {
    // A checkpoint contains several files. Retain the service's unique model
    // identity for this load instead of promoting only the weights' hash and
    // accidentally reusing translations after a tokenizer/configuration change.
    return {};
}

GenerationResult Engine::generate(const PreparedInput& input, const std::function<void(std::string_view)>& delta) {
    auto& p = *impl_;
    GenerationResult result;
    if (!p.model || !p.unchanged()) return result;
    if (input.stop_ids != p.stops) { result.reason = FinishReason::ModelInputUnsupported; return result; }
    if (input.tokens.empty() || input.tokens.size() + input.output_limit > capacity()) {
        result.reason = FinishReason::ContextLimit; return result;
    }
    Utf8Stream decoder;
    std::string pending;
    auto last = std::chrono::steady_clock::now();
    const auto flush = [&] { if (!pending.empty()) { delta(pending); pending.clear(); } };
    result.reason = FinishReason::OutputLimit;
    plamo08::GenerationOptions options;
    options.max_tokens = static_cast<int>(input.output_limit);
    options.prefill_chunk = prefill_chunk;
    options.backend = p.mode;
    options.suppressed_tokens = p.nonprinting_bytes;
    options.cancelled = [&] { return p.cancelled.load(); };
    try {
        const auto generated = p.model->generate(std::vector<int>(input.tokens.begin(), input.tokens.end()),
            [&](int id) { return std::find(p.stops.begin(), p.stops.end(), id) != p.stops.end(); }, options,
            [&](int id) {
                const auto type = p.tokenizer->token_type(id);
                if (p.protocol[id] || type == plamo08::TokenType::control || type == plamo08::TokenType::unknown || type == plamo08::TokenType::unused) {
                    result.reason = FinishReason::InvalidModelOutput; return false;
                }
                try {
                    const auto complete = decoder.push(p.tokenizer->token_bytes(id));
                    if (complete.size() > max_translation_bytes - result.text.size()) return false;
                    const auto control = unsupported_text_control(complete);
                    if (control != std::string_view::npos) {
                        result.text.append(complete, 0, control); pending.append(complete, 0, control);
                        result.reason = FinishReason::InvalidModelOutput; return false;
                    }
                    result.text += complete; pending += complete; ++result.output_tokens;
                } catch (const Utf8Error&) { result.reason = FinishReason::InvalidEncoding; return false; }
                const auto now = std::chrono::steady_clock::now();
                if (now - last >= std::chrono::milliseconds(50)) {
                    flush(); last = now;
                    if (!p.unchanged()) { result.reason = FinishReason::ModelUnavailable; return false; }
                }
                return true;
            });
        if (generated.cancelled) result.reason = FinishReason::Cancelled;
        else if (generated.stopped_on_eos)
            result.reason = has_usable_text(result.text) ? FinishReason::Completed : FinishReason::EmptyOutput;
    } catch (const std::exception&) {
        flush();
        if (!p.cancelled.load()) throw; // Preserve Metal compiler/runtime diagnostics.
        result.reason = FinishReason::Cancelled;
    }
    flush();
    if (!decoder.complete() && (result.reason == FinishReason::Completed || result.reason == FinishReason::EmptyOutput))
        result.reason = FinishReason::InvalidEncoding;
    if (!p.unchanged()) result.reason = FinishReason::ModelUnavailable;
    return result;
}
}
