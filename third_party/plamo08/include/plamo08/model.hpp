#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <mlx/array.h>

namespace plamo08 {

enum class Backend { fused, mlx };

struct GenerationOptions {
    int max_tokens = 512;
    int prefill_chunk = 256;
    Backend backend = Backend::fused;
    std::vector<int> suppressed_tokens;
    std::function<bool()> cancelled;
};

struct GenerationResult {
    std::vector<int> token_ids;
    std::size_t prompt_tokens = 0;
    double prefill_seconds = 0;
    double decode_seconds = 0;
    bool stopped_on_eos = false;
    bool cancelled = false;
};

struct ParityResult {
    int compared_steps = 0;
    int same_argmax_steps = 0;
    double max_absolute_error = 0;
    double max_relative_rms_error = 0;
    bool all_finite = true;
};

// A model instance serializes one translation at a time. It owns the KV cache.
class Model {
public:
    explicit Model(const std::filesystem::path& model_dir, int context_size = 32768,
                   const std::function<void()>& checkpoint = {});
    ~Model();
    Model(Model&&) noexcept;
    Model& operator=(Model&&) noexcept;
    Model(const Model&) = delete;
    Model& operator=(const Model&) = delete;

    void reset();
    void prefill(const std::vector<int>& tokens, int chunk_size = 256,
                 const std::function<void()>& checkpoint = {});
    mlx::core::array logits(const mlx::core::array& token, Backend backend = Backend::fused);
    int position() const;
    int context_size() const;

    GenerationResult generate(
        const std::vector<int>& prompt,
        const std::function<bool(int)>& is_eos,
        const GenerationOptions& options = {},
        const std::function<bool(int)>& on_token = {});

    // Compares the two paths using exactly the same token sequence and fresh caches.
    ParityResult verify(const std::vector<int>& prompt, int steps = 8, int chunk_size = 256);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace plamo08
