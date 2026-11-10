#include "inference/inference_service.hpp"
#include "core/utf8.hpp"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace opt {
struct InferenceService::Impl {
    struct Load {ModelEpoch model;std::filesystem::path path;BackendPreference preference;std::uint64_t serial;};
    struct Plan {ChunkRequest request;std::uint64_t serial;};
    struct Execute {std::shared_ptr<const PreparedChunk> chunk;std::uint64_t serial;};
    enum class Phase { None,Planning,Prepared,Queued,Running,Finished };
    Delivery delivery;
    Factory factory;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic_bool cancelled{false},stopping{false},finished{false};
    std::optional<Load> loading;
    std::optional<Plan> planning;
    std::optional<Execute> execution;
    std::optional<WorkStamp> desired_work;
    std::uint64_t serial=0;
    ModelEpoch desired_model=0,loaded_model=0;
    Phase phase=Phase::None;
    std::thread worker;
    Impl(Delivery sink,Factory provider):delivery(std::move(sink)),factory(std::move(provider)){
        if(!factory)factory=[](std::atomic_bool& cancel){return std::make_unique<Engine>(cancel);};
        worker=std::thread([this]{run();});
    }
    ~Impl(){stop();if(worker.joinable())worker.join();}
    void emit(InferenceEvent event){delivery(std::make_shared<const InferenceEvent>(std::move(event)));}
    void stop() noexcept{
        std::lock_guard lock(mutex);
        stopping=true;cancelled=true;cv.notify_one();
    }
    void run() noexcept{
        try{
            std::unique_ptr<InferenceBackend> backend;
            while(!stopping.load()){
                std::optional<Load> load;
                std::optional<Plan> plan;
                std::optional<Execute> execute;
                {
                    std::unique_lock lock(mutex);
                    cv.wait(lock,[&]{return stopping.load()||loading||planning||execution;});
                    if(stopping.load())break;
                    if(loading){load=std::move(loading);loading.reset();}
                    else if(execution){execute=std::move(execution);execution.reset();}
                    else if(planning){plan=std::move(planning);planning.reset();}
                    const auto selected=load?load->serial:execute?execute->serial:plan->serial;
                    if(selected!=serial)continue;
                    cancelled=false;
                    if(execute)phase=Phase::Running;
                }
                if(load){
                    const auto started=std::chrono::steady_clock::now();
                    try{
                        if(!backend)backend=factory(cancelled);
                        backend->unload();
                        try{backend->load(load->path,load->preference==BackendPreference::Cpu);}
                        catch(...){
                            if(load->preference!=BackendPreference::Automatic||cancelled.load()||backend->backend()!="Metal")throw;
                            backend->unload();backend->load(load->path,true);
                        }
                        {
                            std::lock_guard lock(mutex);
                            if(load->serial!=serial||cancelled.load())continue;
                            loaded_model=load->model;
                        }
                        InferenceEvent e;e.kind=InferenceEvent::Kind::ModelReady;e.model=load->model;e.backend=backend->backend();
                        e.context_capacity=backend->capacity();e.runtime=backend->runtime();e.file=backend->file_snapshot();
                        e.seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();emit(std::move(e));
                    }catch(const std::exception& error){
                        if(backend)backend->unload();
                        InferenceEvent e;e.kind=InferenceEvent::Kind::ModelFailed;e.model=load->model;
                        e.message="モデルを読み込めません。別のGGUFまたはCPUを選択してください。";e.detail=error.what();emit(std::move(e));
                    }
                    continue;
                }
                if(plan){
                    try{
                        if(!backend)throw std::runtime_error("Model unavailable");
                        auto chunk=std::make_shared<const PreparedChunk>(ChunkPlanner::prepare(plan->request,backend->capacity(),
                            [&](std::string_view source,Form form){return backend->prepare(source,form);}));
                        {
                            std::lock_guard lock(mutex);if(plan->serial!=serial||cancelled.load())continue;phase=Phase::Prepared;
                        }
                        InferenceEvent e;e.kind=InferenceEvent::Kind::Prepared;e.model=plan->request.stamp.model;
                        e.stamp=plan->request.stamp;e.chunk=std::move(chunk);emit(std::move(e));
                    }catch(const std::exception& error){
                        InferenceEvent e;e.kind=InferenceEvent::Kind::PreparationFailed;e.model=plan->request.stamp.model;
                        e.stamp=plan->request.stamp;e.message="翻訳の入力を準備できません。原文を確認して再試行できます。";e.detail=error.what();
                        e.result.reason=dynamic_cast<const Utf8Error*>(&error)?FinishReason::InvalidEncoding:FinishReason::DecodeError;emit(std::move(e));
                    }
                    continue;
                }
                if(execute){
                    const auto started=std::chrono::steady_clock::now();
                    const auto& chunk=execute->chunk;
                    InferenceEvent beginning;beginning.kind=InferenceEvent::Kind::Started;beginning.model=chunk->stamp.model;
                    beginning.stamp=chunk->stamp;beginning.chunk=chunk;emit(std::move(beginning));
                    GenerationResult result;
                    std::string detail;
                    try{
                        if(chunk->failure)result.reason=*chunk->failure;
                        else if(chunk->source_only)result.reason=FinishReason::EmptyOutput;
                        else result=backend->generate(chunk->input,[&](std::string_view delta){
                            InferenceEvent e;e.kind=InferenceEvent::Kind::TextDelta;e.model=chunk->stamp.model;
                            e.stamp=chunk->stamp;e.delta=delta;emit(std::move(e));
                        });
                    }catch(const std::exception& error){result.reason=FinishReason::DecodeError;detail=error.what();}
                    {
                        std::lock_guard lock(mutex);if(execute->serial==serial)phase=Phase::Finished;
                    }
                    InferenceEvent e;e.kind=InferenceEvent::Kind::Finished;e.model=chunk->stamp.model;e.stamp=chunk->stamp;
                    e.chunk=chunk;e.result=std::move(result);e.backend=backend->backend();e.detail=std::move(detail);
                    e.seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();emit(std::move(e));
                }
            }
        }catch(...){
            stopping=true;
            try{InferenceEvent e;{std::lock_guard lock(mutex);e.model=desired_model;}e.message="推論処理を続けられません。アプリを再起動してください。";emit(std::move(e));}catch(...){}
        }
        finished=true;
    }
};
InferenceService::InferenceService(Delivery delivery,Factory factory):impl_(std::make_unique<Impl>(std::move(delivery),std::move(factory))){}
InferenceService::~InferenceService()=default;
void InferenceService::load(ModelEpoch epoch,std::filesystem::path path,BackendPreference preference){
    std::lock_guard lock(impl_->mutex);if(impl_->stopping)return;
    impl_->cancelled=true;impl_->desired_model=epoch;impl_->loaded_model=0;
    impl_->loading=Impl::Load{epoch,std::move(path),preference,++impl_->serial};
    impl_->planning.reset();impl_->execution.reset();impl_->desired_work.reset();impl_->phase=Impl::Phase::None;impl_->cv.notify_one();
}
bool InferenceService::plan(ChunkRequest request){
    std::lock_guard lock(impl_->mutex);
    if(impl_->stopping.load()||!impl_->loaded_model||impl_->loaded_model!=request.stamp.model||impl_->desired_model!=request.stamp.model)return false;
    impl_->cancelled=true;impl_->desired_work=request.stamp;impl_->execution.reset();
    impl_->planning=Impl::Plan{std::move(request),++impl_->serial};impl_->phase=Impl::Phase::Planning;impl_->cv.notify_one();return true;
}
bool InferenceService::execute(std::shared_ptr<const PreparedChunk> chunk){
    std::lock_guard lock(impl_->mutex);
    if(!chunk||chunk->failure||chunk->source_only||impl_->stopping.load()||impl_->phase!=Impl::Phase::Prepared||!impl_->desired_work||
       *impl_->desired_work!=chunk->stamp||impl_->loaded_model!=chunk->stamp.model)return false;
    impl_->execution=Impl::Execute{std::move(chunk),impl_->serial};impl_->phase=Impl::Phase::Queued;impl_->cv.notify_one();return true;
}
void InferenceService::cancel_job() noexcept{
    std::lock_guard lock(impl_->mutex);
    if(!impl_->desired_work)return;
    impl_->cancelled=true;++impl_->serial;impl_->planning.reset();impl_->execution.reset();impl_->desired_work.reset();impl_->phase=Impl::Phase::None;
}
void InferenceService::stop() noexcept{impl_->stop();}
bool InferenceService::done() const noexcept{return impl_->finished.load();}
void InferenceService::join(){if(impl_->worker.joinable())impl_->worker.join();}
}
