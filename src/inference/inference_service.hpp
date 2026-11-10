#pragma once
#include "inference/chunk_planner.hpp"
#include <atomic>

namespace opt {
enum class BackendPreference { Automatic, Cpu };
struct InferenceEvent {
    enum class Kind { ModelReady, ModelFailed, Prepared, PreparationFailed, Started, TextDelta, Finished };
    Kind kind=Kind::ModelFailed;
    ModelEpoch model=0;
    WorkStamp stamp;
    ModelProfile profile;
    std::string backend,message,detail,delta;
    std::uint32_t context_capacity=0;
    InferenceRuntime runtime;
    std::shared_ptr<const FileSnapshot> file;
    std::shared_ptr<const PreparedChunk> chunk;
    GenerationResult result;
    double seconds=0;
};
class InferenceService {
public:
    using Delivery=std::function<void(std::shared_ptr<const InferenceEvent>)>;
    using Factory=std::function<std::unique_ptr<InferenceBackend>(std::atomic_bool&)>;
    explicit InferenceService(Delivery delivery,Factory factory={});
    ~InferenceService();
    InferenceService(const InferenceService&)=delete;
    InferenceService& operator=(const InferenceService&)=delete;
    void load(ModelEpoch epoch,std::filesystem::path path,BackendPreference preference);
    bool plan(ChunkRequest request);
    bool execute(std::shared_ptr<const PreparedChunk> chunk);
    void cancel_job() noexcept;
    void stop() noexcept;
    bool done() const noexcept;
    void join();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
