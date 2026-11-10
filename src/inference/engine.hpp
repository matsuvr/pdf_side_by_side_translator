#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <stdexcept>

namespace opt {
class FileSnapshot;
enum class FinishReason {
    Completed, Cancelled, OutputLimit, ContextLimit, EmptyOutput,
    InvalidEncoding, InvalidModelOutput, ModelInputUnsupported, DecodeError, ModelUnavailable
};
enum class Form { Plain, Polite };
class ModelInputUnsupported final : public std::runtime_error {
public: using std::runtime_error::runtime_error;
};
struct PreparedInput {
    std::vector<std::int32_t> tokens;
    std::vector<std::int32_t> stop_ids;
    std::uint32_t source_tokens = 0;
    std::uint32_t output_limit = 0;
};
struct GenerationResult {
    FinishReason reason = FinishReason::ModelUnavailable;
    std::string text;
    std::uint32_t output_tokens = 0;
};
class InferenceBackend {
public:
    virtual ~InferenceBackend()=default;
    virtual void load(const std::filesystem::path& path,bool cpu_only)=0;
    virtual void unload()=0;
    virtual PreparedInput prepare(std::string_view source,Form form) const=0;
    virtual std::vector<std::int32_t> tokenize(std::string_view text,bool add_bos,bool special) const=0;
    virtual GenerationResult generate(const PreparedInput& input,const std::function<void(std::string_view)>& delta)=0;
    virtual std::uint32_t capacity() const=0;
    virtual std::string backend() const=0;
    virtual std::shared_ptr<const FileSnapshot> file_snapshot() const=0;
};
// All methods, including construction and destruction, belong to one inference thread.
// Cancellation is the sole cross-thread input.
class Engine final : public InferenceBackend {
public:
    explicit Engine(std::atomic_bool& cancelled);
    ~Engine() override;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    void load(const std::filesystem::path& path, bool cpu_only) override;
    void unload() override;
    PreparedInput prepare(std::string_view source, Form form) const override;
    std::vector<std::int32_t> tokenize(std::string_view text, bool add_bos, bool special) const override;
    GenerationResult generate(const PreparedInput& input,
                              const std::function<void(std::string_view)>& delta) override;
    std::uint32_t capacity() const override;
    std::string backend() const override;
    std::shared_ptr<const FileSnapshot> file_snapshot() const override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
