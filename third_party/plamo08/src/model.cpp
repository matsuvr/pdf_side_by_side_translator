// PLaMo 3 0.8B host/runtime port. See THIRD_PARTY_NOTICES.md for upstream sources.
#include "plamo08/model.hpp"
#include "plamo08/config.hpp"
#include "plamo08/kernels.hpp"
#include "plamo08/weights.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include <mlx/device.h>
#include <mlx/fast.h>
#include <mlx/io.h>
#include <mlx/backend/metal/metal.h>
#include <mlx/ops.h>
#include <mlx/stream.h>
#include <mlx/transforms.h>
#include <nlohmann/json.hpp>

namespace plamo08 {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using TensorMap = std::unordered_map<std::string, mx::array>;

double seconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double>(end - begin).count();
}

Json read_json(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("Cannot read " + path.string());
    return Json::parse(file);
}

Json load_config(const std::filesystem::path& dir, int context) {
    const auto config = read_json(dir / "config.json");
    if (config.value("model_type", "") != "plamo3")
        throw std::runtime_error("Expected a PLaMo 3 MLX checkpoint");
    for (const auto& [name, expected] : std::vector<std::pair<std::string, int>>{
             {"hidden_size", spec::hidden}, {"num_hidden_layers", spec::layers},
             {"num_attention_heads", spec::q_heads}, {"num_key_value_heads", spec::kv_heads},
             {"head_dim", spec::head_dim}, {"intermediate_size", spec::intermediate},
             {"vocab_size", spec::vocab}, {"window_size", spec::window},
             {"sliding_window_pattern", 8}}) {
        if (config.value(name, -1) != expected)
            throw std::runtime_error("This kernel targets PLaMo 3 0.8B: unexpected " + name);
    }
    if (std::abs(config.value("rms_norm_eps", 0.0) - spec::eps) > 1e-12 ||
        config.value("tie_word_embeddings", true) || config.value("scale_embedding", true))
        throw std::runtime_error("Unexpected PLaMo 3 normalization or embedding settings");
    const auto& quant = config.at("quantization");
    if (quant.value("bits", 0) != spec::bits || quant.value("group_size", 0) != spec::group_size)
        throw std::runtime_error("Expected MLX affine 4bit, group size 64");
    if (quant.value("mode", "affine") != "affine")
        throw std::runtime_error("Expected MLX affine quantization");
    if (context < 1 || context > config.value("max_position_embeddings", 262144))
        throw std::runtime_error("Context size exceeds the checkpoint's position range");
    for (const auto& kind : {"local", "full"}) {
        const auto& inverse = config.at("plamo_rope_inv_freq").at(kind);
        if (!inverse.is_array() || inverse.size() != spec::head_dim / 2)
            throw std::runtime_error("Invalid source RoPE frequency table");
    }
    return config;
}

mx::array require_tensor(const TensorMap& tensors, const std::string& name,
                         const mx::Shape& shape, mx::Dtype dtype) {
    const auto found = tensors.find(name);
    if (found == tensors.end()) throw std::runtime_error("Missing tensor: " + name);
    if (found->second.shape() != shape || found->second.dtype() != dtype)
        throw std::runtime_error("Unexpected shape or dtype: " + name);
    return found->second;
}

Linear load_linear(const TensorMap& tensors, const Json& quant, const std::string& name,
                   int input, int output, bool packed) {
    if (!packed) {
        if (quant.contains(name) && quant.at(name) != false)
            throw std::runtime_error("Expected the checkpoint's original BF16 " + name);
        return {require_tensor(tensors, name + ".weight", {output, input}, mx::bfloat16),
                std::nullopt, std::nullopt, input, output};
    }
    const auto& entry = quant.contains(name) ? quant.at(name) : quant;
    if (!entry.is_object() || entry.value("bits", 0) != spec::bits ||
        entry.value("group_size", 0) != spec::group_size ||
        entry.value("mode", "affine") != "affine")
        throw std::runtime_error("Unexpected quantization: " + name);
    return {require_tensor(tensors, name + ".weight", {output, input / 8}, mx::uint32),
            require_tensor(tensors, name + ".scales", {output, input / 64}, mx::float16),
            require_tensor(tensors, name + ".biases", {output, input / 64}, mx::float16),
            input, output};
}

Weights load_weights(const std::filesystem::path& dir, const Json& config,
                     const std::function<void()>& checkpoint) {
    std::set<std::filesystem::path> shards;
    const auto index = dir / "model.safetensors.index.json";
    if (std::filesystem::exists(index)) {
        for (const auto& [key, value] : read_json(index).at("weight_map").items())
            shards.insert(dir / value.get<std::string>());
    } else {
        shards.insert(dir / "model.safetensors");
    }
    TensorMap tensors;
    for (const auto& path : shards) {
        if (checkpoint) checkpoint();
        auto [part, metadata] = mx::load_safetensors(path.string());
        for (auto& [name, tensor] : part) {
            if (!tensors.emplace(name, std::move(tensor)).second)
                throw std::runtime_error("Duplicate weight: " + name);
        }
    }
    const auto& quant = config.at("quantization");
    auto norm = [&](const std::string& name, int size = spec::hidden) {
        return require_tensor(tensors, name + ".weight", {size}, mx::bfloat16);
    };
    Weights weights{
        require_tensor(tensors, "model.embed_tokens.weight", {spec::vocab, spec::hidden}, mx::bfloat16),
        load_linear(tensors, quant, "lm_head", spec::hidden, spec::vocab, false),
        norm("model.norm"), {}};
    weights.layers.reserve(spec::layers);
    for (int i = 0; i < spec::layers; ++i) {
        const auto prefix = "model.layers.layers." + std::to_string(i) + ".";
        weights.layers.push_back({
            load_linear(tensors, quant, prefix + "mixer.qkv_proj", spec::hidden, spec::q_dim + 2 * spec::kv_dim, true),
            load_linear(tensors, quant, prefix + "mixer.o_proj", spec::q_dim, spec::hidden, true),
            load_linear(tensors, quant, prefix + "mlp.gate_up_proj", spec::hidden, 2 * spec::intermediate, true),
            load_linear(tensors, quant, prefix + "mlp.down_proj", spec::intermediate, spec::hidden, true),
            norm(prefix + "pre_mixer_norm"), norm(prefix + "post_mixer_norm"),
            norm(prefix + "pre_mlp_norm"), norm(prefix + "post_mlp_norm"),
            norm(prefix + "mixer.q_norm", spec::head_dim), norm(prefix + "mixer.k_norm", spec::head_dim)});
    }
    // Loading pre-quantized weights performs no re-quantization or expansion of packed matrices.
    std::vector<mx::array> materialize;
    materialize.reserve(tensors.size());
    for (const auto& [name, tensor] : tensors) materialize.push_back(tensor);
    for (std::size_t start = 0; start < materialize.size(); start += 16) {
        if (checkpoint) checkpoint();
        const auto end = std::min(start + 16, materialize.size());
        mx::eval(std::vector<mx::array>(materialize.begin() + start, materialize.begin() + end));
    }
    if (checkpoint) checkpoint();
    return weights;
}

mx::array scale(const mx::array& raw, float offset) {
    return mx::add(raw, mx::array(offset, raw.dtype()));
}

mx::array rms_norm(const mx::array& x, const mx::array& weight) {
    auto wide = mx::astype(x, mx::float32);
    auto inv = mx::rsqrt(mx::add(mx::mean(mx::multiply(wide, wide), -1, true), mx::array(spec::eps)));
    return mx::multiply(mx::astype(mx::multiply(wide, inv), x.dtype()), weight);
}

mx::array linear(const Linear& layer, const mx::array& x) {
    auto result = layer.quantized()
        ? mx::quantized_matmul(x, layer.weight, *layer.scales, layer.biases, true, 64, 4, "affine")
        : mx::matmul(x, mx::transpose(layer.weight));
    return mx::astype(result, x.dtype());
}

mx::array make_rope(const Json& config, bool full, int context) {
    const auto inverse = config.at("plamo_rope_inv_freq").at(full ? "full" : "local").get<std::vector<float>>();
    const mx::array frequencies(inverse.data(), {spec::head_dim / 2}, mx::float32);
    auto phase = mx::multiply(mx::reshape(mx::arange(context, mx::float32), {context, 1}), frequencies);
    const float factor = full
        ? static_cast<float>(1.0 + 0.1 * std::log(config.at("rope_scaling_factor").get<double>())) : 1.0f;
    auto cosine = mx::multiply(mx::astype(mx::cos(phase), mx::bfloat16), mx::array(factor, mx::bfloat16));
    auto sine = mx::multiply(mx::astype(mx::sin(phase), mx::bfloat16), mx::array(factor, mx::bfloat16));
    return mx::concatenate({cosine, sine}, -1);
}

mx::array rotate(const mx::array& x, const mx::array& table, int offset, int length) {
    const int half = spec::head_dim / 2;
    auto shape = x.shape();
    auto first_stop = shape;
    first_stop.back() = half;
    auto last_start = mx::Shape(shape.size(), 0);
    last_start.back() = half;
    auto first = mx::slice(x, mx::Shape(shape.size(), 0), first_stop);
    auto last = mx::slice(x, last_start, shape);
    auto cosine = mx::slice(table, {offset, 0}, {offset + length, half});
    auto sine = mx::slice(table, {offset, half}, {offset + length, spec::head_dim});
    // Keep each multiply at BF16, as in the published MLX model.
    return mx::concatenate({mx::subtract(mx::multiply(first, cosine), mx::multiply(last, sine)),
                            mx::add(mx::multiply(last, cosine), mx::multiply(first, sine))}, -1);
}

std::pair<mx::array, mx::array> update_cache(KVCache& cache, const mx::array& keys, const mx::array& values) {
    const int count = keys.shape(1);
    if (cache.full) {
        cache.keys = mx::slice_update(cache.keys, keys, mx::Shape{0, cache.offset, 0}, mx::Shape{1, cache.offset + count, spec::head_dim});
        cache.values = mx::slice_update(cache.values, values, mx::Shape{0, cache.offset, 0}, mx::Shape{1, cache.offset + count, spec::head_dim});
        cache.offset += count;
        return {mx::slice(cache.keys, {0, 0, 0}, {1, cache.offset, spec::head_dim}),
                mx::slice(cache.values, {0, 0, 0}, {1, cache.offset, spec::head_dim})};
    }
    const int keep = std::min(cache.offset, spec::window - 1);
    std::vector<int> indices(keep);
    for (int i = 0; i < keep; ++i) indices[i] = (cache.offset - keep + i) % cache.capacity;
    mx::array old_keys = mx::slice(cache.keys, {0, 0, 0}, {1, 0, spec::head_dim});
    mx::array old_values = mx::slice(cache.values, {0, 0, 0}, {1, 0, spec::head_dim});
    if (keep > 0) {
        auto order = mx::array(indices.data(), {keep}, mx::int32);
        old_keys = mx::take(cache.keys, order, 1);
        old_values = mx::take(cache.values, order, 1);
    }
    auto merged_keys = mx::concatenate({old_keys, keys}, 1);
    auto merged_values = mx::concatenate({old_values, values}, 1);
    const int stored = std::min(count, cache.capacity);
    const int source = count - stored;
    const int slot = (cache.offset + source) % cache.capacity;
    const int first = std::min(stored, cache.capacity - slot);
    auto store = [&](mx::array& destination, const mx::array& from) {
        destination = mx::slice_update(destination,
            mx::slice(from, {0, source, 0}, {1, source + first, spec::head_dim}),
            mx::Shape{0, slot, 0}, mx::Shape{1, slot + first, spec::head_dim});
        if (first < stored)
            destination = mx::slice_update(destination,
                mx::slice(from, {0, source + first, 0}, {1, count, spec::head_dim}),
                mx::Shape{0, 0, 0}, mx::Shape{1, stored - first, spec::head_dim});
    };
    store(cache.keys, keys);
    store(cache.values, values);
    cache.offset += count;
    return {merged_keys, merged_values};
}

struct NormScales {
    mx::array pre_mixer, post_mixer, pre_mlp, post_mlp, q, k;
};

mx::array attention(const LayerWeights& layer, const NormScales& norms, const mx::array& x,
                    KVCache& cache, const RopeTables& rope) {
    const int length = x.shape(1);
    const int offset = cache.offset;
    auto qkv = linear(layer.qkv, x);
    auto parts = mx::split(qkv, mx::Shape{spec::q_dim, spec::q_dim + spec::kv_dim}, -1);
    auto q = rms_norm(mx::transpose(mx::reshape(parts[0], {1, length, spec::q_heads, spec::head_dim}), {0, 2, 1, 3}), norms.q);
    auto k = rms_norm(mx::transpose(mx::reshape(parts[1], {1, length, 1, spec::head_dim}), {0, 2, 1, 3}), norms.k);
    auto v = mx::reshape(parts[2], {1, length, spec::head_dim});
    const auto& table = cache.full ? rope.full : rope.local;
    q = rotate(q, table, offset, length);
    k = mx::reshape(rotate(k, table, offset, length), {1, length, spec::head_dim});
    auto [keys, values] = update_cache(cache, k, v);
    std::optional<mx::array> mask;
    if (length > 1) {
        const int visible = keys.shape(1);
        auto key_positions = mx::reshape(mx::arange(visible), {1, visible});
        auto query_positions = mx::reshape(mx::arange(visible - length, visible), {length, 1});
        mask = mx::less_equal(key_positions, query_positions);
        if (!cache.full)
            mask = mx::logical_and(*mask, mx::greater(key_positions, mx::subtract(query_positions, mx::array(spec::window))));
    }
    auto attended = mx::fast::scaled_dot_product_attention(
        q, mx::reshape(keys, {1, 1, keys.shape(1), spec::head_dim}),
        mx::reshape(values, {1, 1, values.shape(1), spec::head_dim}),
        1.0f / std::sqrt(float(spec::head_dim)), "", mask, std::nullopt, false);
    return linear(layer.out, mx::reshape(mx::transpose(attended, {0, 2, 1, 3}), {1, length, spec::q_dim}));
}

mx::array mlp(const LayerWeights& layer, const mx::array& x) {
    auto parts = mx::split(linear(layer.gate_up, x), 2, -1);
    auto gate = mx::astype(parts[0], mx::float32);
    auto activated = mx::astype(mx::multiply(gate, mx::sigmoid(gate)), x.dtype());
    return linear(layer.down, mx::multiply(activated, parts[1]));
}

std::vector<KVCache> make_caches(int context) {
    std::vector<KVCache> caches;
    caches.reserve(spec::layers);
    for (int i = 0; i < spec::layers; ++i) {
        const bool full = spec::full_attention(i);
        const int capacity = full ? context : spec::window;
        caches.push_back({mx::zeros({1, capacity, spec::head_dim}, mx::bfloat16),
                          mx::zeros({1, capacity, spec::head_dim}, mx::bfloat16), 0, capacity, full});
    }
    return caches;
}

void validate_tokens(const std::vector<int>& tokens) {
    for (int token : tokens)
        if (token < 0 || token >= spec::vocab) throw std::runtime_error("Token ID is outside the vocabulary");
}
}  // namespace

struct Model::Impl {
    Json config;
    Weights weights;
    RopeTables rope;
    std::vector<KVCache> caches;
    std::vector<NormScales> norms;
    mx::array final_scale;
    FastDecoder fast;
    int context;
    std::optional<mx::array> pending;

    Impl(const std::filesystem::path& dir, int capacity, const std::function<void()>& checkpoint)
        : config(load_config(dir, capacity)), weights(load_weights(dir, config, checkpoint)),
          rope{make_rope(config, false, capacity), make_rope(config, true, capacity)},
          caches(make_caches(capacity)), final_scale(scale(weights.final_norm, 1.0f)),
          fast(weights, rope), context(capacity) {
        std::vector<mx::array> to_eval{rope.local, rope.full, final_scale};
        for (const auto& layer : weights.layers) {
            norms.push_back({scale(layer.pre_mixer, 1), scale(layer.post_mixer, spec::post_mixer_offset),
                             scale(layer.pre_mlp, 1), scale(layer.post_mlp, spec::post_mlp_offset),
                             scale(layer.q_norm, 1), scale(layer.k_norm, 1)});
            const auto& n = norms.back();
            to_eval.insert(to_eval.end(), {n.pre_mixer, n.post_mixer, n.pre_mlp, n.post_mlp, n.q, n.k});
        }
        for (const auto& cache : caches) to_eval.insert(to_eval.end(), {cache.keys, cache.values});
        mx::eval(to_eval);
        if (checkpoint) checkpoint();
    }

    void wait() {
        if (pending) { mx::eval(*pending); pending.reset(); }
    }

    mx::array hidden(const mx::array& tokens) {
        auto x = mx::take(weights.embedding, tokens, 0);
        for (int i = 0; i < spec::layers; ++i) {
            const auto& layer = weights.layers[i];
            const auto& n = norms[i];
            x = mx::add(x, rms_norm(attention(layer, n, rms_norm(x, n.pre_mixer), caches[i], rope), n.post_mixer));
            x = mx::add(x, rms_norm(mlp(layer, rms_norm(x, n.pre_mlp)), n.post_mlp));
        }
        return x;
    }

    mx::array reference_logits(const mx::array& token) {
        auto x = hidden(mx::reshape(token, {1, 1}));
        return mx::reshape(mx::astype(linear(weights.head, rms_norm(x, final_scale)), mx::float32), {spec::vocab});
    }

    mx::array decode(const mx::array& token, Backend backend) {
        if (caches.front().offset >= context) throw std::runtime_error("Context is full");
        auto scores = backend == Backend::fused ? fast.logits(mx::astype(token, mx::uint32), caches)
                                               : reference_logits(token);
        pending = scores;
        return scores;
    }
};

Model::Model(const std::filesystem::path& dir, int context_size, const std::function<void()>& checkpoint) {
    if (checkpoint) checkpoint();
    if (!mx::metal::is_available()) throw std::runtime_error("Apple Silicon Metal device is required");
    mx::set_default_device(mx::Device::gpu);
    impl_ = std::make_unique<Impl>(dir, context_size, checkpoint);
}

Model::~Model() {
    if (impl_) {
        // Finish in-place writes before releasing cache storage.
        try { impl_->wait(); } catch (...) {}
    }
}
Model::Model(Model&&) noexcept = default;
Model& Model::operator=(Model&&) noexcept = default;

void Model::reset() {
    impl_->wait();
    for (auto& cache : impl_->caches) cache.offset = 0;
}

int Model::position() const { return impl_->caches.front().offset; }
int Model::context_size() const { return impl_->context; }

void Model::prefill(const std::vector<int>& tokens, int chunk_size, const std::function<void()>& checkpoint) {
    if (chunk_size <= 0) throw std::runtime_error("Prefill chunk must be positive");
    validate_tokens(tokens);
    if (tokens.size() > std::size_t(impl_->context - position()))
        throw std::runtime_error("Prompt exceeds the configured context size");
    impl_->wait();
    for (std::size_t start = 0; start < tokens.size(); start += chunk_size) {
        if (checkpoint) checkpoint();
        const int count = int(std::min<std::size_t>(chunk_size, tokens.size() - start));
        auto ids = mx::array(tokens.data() + start, {1, count}, mx::int32);
        auto x = impl_->hidden(ids);
        std::vector<mx::array> evaluated{x};
        for (const auto& cache : impl_->caches) evaluated.insert(evaluated.end(), {cache.keys, cache.values});
        mx::eval(evaluated);
        if (checkpoint) checkpoint();
    }
}

mx::array Model::logits(const mx::array& token, Backend backend) {
    if (token.size() != 1 || (token.dtype() != mx::uint32 && token.dtype() != mx::int32))
        throw std::runtime_error("Decode expects one integer token");
    // The public API also accepts independent, teacher-forced tokens. Complete
    // previous mutable-cache operations before encoding such a graph.
    impl_->wait();
    auto checked = mx::astype(token, mx::uint32);
    if (checked.item<std::uint32_t>() >= std::uint32_t(spec::vocab))
        throw std::runtime_error("Token ID is outside the vocabulary");
    return impl_->decode(checked, backend);
}

GenerationResult Model::generate(const std::vector<int>& prompt, const std::function<bool(int)>& is_eos,
                                 const GenerationOptions& options, const std::function<bool(int)>& on_token) {
    if (prompt.empty()) throw std::runtime_error("Prompt must contain at least one token");
    validate_tokens(prompt);
    validate_tokens(options.suppressed_tokens);
    if (options.max_tokens < 0) throw std::runtime_error("max_tokens must be nonnegative");
    if (prompt.size() > std::size_t(impl_->context)) throw std::runtime_error("Prompt exceeds the context size");
    reset();
    GenerationResult result;
    result.prompt_tokens = prompt.size();
    struct Cancelled {};
    const auto checkpoint = [&] {
        if (options.cancelled && options.cancelled()) throw Cancelled{};
    };
    try {
        checkpoint();
        const auto prefill_start = Clock::now();
        prefill(std::vector<int>(prompt.begin(), prompt.end() - 1), options.prefill_chunk, checkpoint);
        result.prefill_seconds = seconds(prefill_start, Clock::now());
        const auto decode_start = Clock::now();
        auto token = mx::array(std::uint32_t(prompt.back()), mx::uint32);
        std::optional<mx::array> pending, mask;
        if (!options.suppressed_tokens.empty()) {
            std::vector<float> values(spec::vocab, 0);
            for (int id : options.suppressed_tokens) values[id] = -std::numeric_limits<float>::infinity();
            mask = mx::array(values.data(), {spec::vocab}, mx::float32);
            mx::eval(*mask);
        }
        // One step runs while the CPU consumes the previous token. The token
        // array links both graphs, including their in-place KV cache writes.
        const int limit = std::min(options.max_tokens, impl_->context - int(prompt.size()) + 1);
        const auto consume = [&](const mx::array& value) {
            const int id = int(value.item<std::uint32_t>());
            checkpoint();
            if (is_eos && is_eos(id)) { result.stopped_on_eos = true; return false; }
            result.token_ids.push_back(id);
            return !on_token || on_token(id);
        };
        for (int i = 0; i < limit; ++i) {
            checkpoint();
            auto scores = impl_->decode(token, options.backend);
            if (mask) scores = mx::add(scores, *mask);
            auto following = mx::astype(mx::argmax(scores, true), mx::uint32);
            mx::async_eval(following);
            // Set before invoking callbacks: every exit must wait for this graph.
            impl_->pending = following;
            if (pending && !consume(*pending)) { pending.reset(); break; }
            pending = following;
            token = following;
        }
        if (pending) consume(*pending);
        impl_->wait();
        result.decode_seconds = seconds(decode_start, Clock::now());
    } catch (const Cancelled&) {
        impl_->wait();
        result.cancelled = true;
    } catch (...) {
        // A UI callback or a failed GPU operation must not leave mutable cache
        // writes running when the worker unloads or starts the next page.
        try { impl_->wait(); } catch (...) {}
        throw;
    }
    return result;
}

ParityResult Model::verify(const std::vector<int>& prompt, int steps, int chunk_size) {
    if (prompt.empty() || steps < 1) throw std::runtime_error("Verification needs a prompt and positive step count");
    validate_tokens(prompt);
    if (prompt.size() > std::size_t(impl_->context)) throw std::runtime_error("Prompt exceeds the context size");
    steps = std::min(steps, impl_->context - int(prompt.size()) + 1);
    std::vector<std::vector<float>> reference;
    std::vector<int> sequence{prompt.back()};
    reset();
    const std::vector<int> prefix(prompt.begin(), prompt.end() - 1);
    prefill(prefix, chunk_size);
    for (int i = 0; i < steps; ++i) {
        auto scores = logits(mx::array(std::uint32_t(sequence.back()), mx::uint32), Backend::mlx);
        mx::eval(scores);
        const float* data = scores.data<float>();
        reference.emplace_back(data, data + spec::vocab);
        sequence.push_back(int(std::max_element(reference.back().begin(), reference.back().end()) - reference.back().begin()));
    }
    reset();
    prefill(prefix, chunk_size);
    ParityResult result;
    for (int i = 0; i < steps; ++i) {
        auto scores = logits(mx::array(std::uint32_t(sequence[i]), mx::uint32), Backend::fused);
        mx::eval(scores);
        const float* actual = scores.data<float>();
        const auto& expected = reference[i];
        double squared_error = 0, squared_reference = 0;
        int best = 0;
        for (int j = 0; j < spec::vocab; ++j) {
            const double delta = double(actual[j]) - expected[j];
            result.all_finite = result.all_finite && std::isfinite(actual[j]) && std::isfinite(expected[j]);
            result.max_absolute_error = std::max(result.max_absolute_error, std::abs(delta));
            squared_error += delta * delta;
            squared_reference += double(expected[j]) * expected[j];
            if (actual[j] > actual[best]) best = j;
        }
        result.max_relative_rms_error = std::max(result.max_relative_rms_error,
            std::sqrt(squared_error / std::max(squared_reference, 1e-30)));
        result.same_argmax_steps += best == sequence[i + 1];
        ++result.compared_steps;
    }
    reset();
    return result;
}
}  // namespace plamo08
