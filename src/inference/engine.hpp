#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace opt {
enum class FinishReason {
    Completed, Cancelled, OutputLimit, ContextLimit, EmptyOutput,
    InvalidEncoding, InvalidModelOutput, ModelInputUnsupported, DecodeError, ModelUnavailable
};
enum class Form { Plain, Polite };
struct PreparedInput {
    std::vector<std::int32_t> tokens;
    std::uint32_t source_tokens = 0;
    std::uint32_t output_limit = 0;
};
struct GenerationResult {
    FinishReason reason = FinishReason::ModelUnavailable;
    std::string text;
    std::uint32_t output_tokens = 0;
};
// All methods, including construction and destruction, belong to one inference thread.
// Cancellation is the sole cross-thread input.
class Engine {
public:
    explicit Engine(std::atomic_bool& cancelled);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    void load(const std::filesystem::path& path, bool cpu_only);
    void unload();
    PreparedInput prepare(std::string_view source, Form form) const;
    std::vector<std::int32_t> tokenize(std::string_view text, bool add_bos, bool special) const;
    GenerationResult generate(const PreparedInput& input,
                              const std::function<void(std::string_view)>& delta);
    std::uint32_t capacity() const;
    std::string backend() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
