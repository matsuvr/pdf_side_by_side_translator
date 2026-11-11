#include "inference/inference_service.hpp"
#include "pdf/pdf_service.hpp"
#include "core/utf8.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
struct State {
    std::mutex mutex;
    std::vector<std::thread::id> owners;
    std::atomic_bool fail_metal{true},block_load{false},load_started{false};
    std::atomic_bool block_prepare{false},prepare_started{false};
    std::atomic<unsigned> loads{0},generations{0},destroyed{0};
    void record(){std::lock_guard lock(mutex);owners.push_back(std::this_thread::get_id());}
};
class FakeBackend final : public opt::InferenceBackend {
    std::atomic_bool& cancel_;
    std::shared_ptr<State> state_;
    std::string backend_="CPU";
public:
    FakeBackend(std::atomic_bool& cancel,std::shared_ptr<State> state):cancel_(cancel),state_(std::move(state)){state_->record();}
    ~FakeBackend() override{state_->record();++state_->destroyed;}
    void load(const std::filesystem::path&,bool cpu) override{
        state_->record();++state_->loads;state_->load_started=true;backend_=cpu?"CPU":"Metal";
        if(!cpu&&state_->fail_metal.exchange(false))throw std::runtime_error("Injected Metal placement failure");
        while(state_->block_load.load()&&!cancel_.load())std::this_thread::sleep_for(2ms);
        if(cancel_.load())throw std::runtime_error("Cancelled load");
    }
    void unload() override{state_->record();}
    opt::PreparedInput prepare(std::string_view source,opt::Form) const override{
        state_->record();state_->prepare_started=true;
        while(state_->block_prepare.load()&&!cancel_.load())std::this_thread::sleep_for(2ms);
        if(cancel_.load())throw opt::PreparationCancelled();
        opt::PreparedInput out;out.tokens={1};out.stop_ids={99};out.output_limit=8;
        for(auto scalar:opt::utf8_scalars(source))out.tokens.push_back(static_cast<int>(scalar.value));
        out.source_tokens=static_cast<std::uint32_t>(out.tokens.size()-1);return out;
    }
    std::vector<std::int32_t> tokenize(std::string_view source,bool,bool) const override{
        state_->record();std::vector<std::int32_t> out;for(auto scalar:opt::utf8_scalars(source))out.push_back(static_cast<int>(scalar.value));return out;
    }
    opt::GenerationResult generate(const opt::PreparedInput&,const std::function<void(std::string_view)>& delta) override{
        state_->record();++state_->generations;delta("翻");
        for(unsigned i=0;i<40;++i){if(cancel_.load())return {opt::FinishReason::Cancelled,"翻",1};std::this_thread::sleep_for(2ms);}
        delta("訳");return {opt::FinishReason::Completed,"翻訳",2};
    }
    std::uint32_t capacity() const override{state_->record();return 256;}
    std::string backend() const override{state_->record();return backend_;}
    std::shared_ptr<const opt::FileSnapshot> file_snapshot() const override{state_->record();return {};}
};
struct Inbox {
    std::mutex mutex;std::condition_variable cv;std::deque<std::shared_ptr<const opt::InferenceEvent>> events;
    void put(std::shared_ptr<const opt::InferenceEvent> e){std::lock_guard lock(mutex);events.push_back(std::move(e));cv.notify_one();}
    auto wait(opt::InferenceEvent::Kind kind,opt::JobId job=0){
        std::unique_lock lock(mutex);const auto deadline=std::chrono::steady_clock::now()+5s;
        while(true){
            while(!events.empty()){
                auto e=events.front();events.pop_front();
                if(e->kind==kind&&(!job||e->stamp.job==job))return e;
                if(e->kind==opt::InferenceEvent::Kind::PreparationFailed||e->kind==opt::InferenceEvent::Kind::ModelFailed)throw std::runtime_error(e->detail);
            }
            if(cv.wait_until(lock,deadline)==std::cv_status::timeout)throw std::runtime_error("Inference service timeout");
        }
    }
};
opt::ChunkRequest request(opt::JobId job,opt::PageIndex page){
    opt::ChunkRequest r;r.stamp={1,page,1,job,job,"attempt",1,job};r.block_id="block";r.source=std::make_shared<const std::string>("Test input.");return r;
}
int main(int argc,char** argv){
    if(argc!=2)return 2;
    try{
        const auto main_thread=std::this_thread::get_id();
        auto state=std::make_shared<State>();Inbox inbox;
        {
            opt::InferenceService service([&](auto e){inbox.put(std::move(e));},[&](auto& cancel){return std::make_unique<FakeBackend>(cancel,state);});
            check(!service.plan(request(1,0)),"Planning accepted an unconfigured model");
            service.load(1,"fake.gguf",opt::BackendPreference::Automatic);
            auto ready=inbox.wait(opt::InferenceEvent::Kind::ModelReady);
            check(ready->backend=="CPU"&&state->loads==2,"Automatic Metal failure did not rebuild once on CPU");
            check(service.plan(request(1,0)),"Current model rejected planning");
            auto first=inbox.wait(opt::InferenceEvent::Kind::Prepared,1)->chunk;
            check(service.execute(first)&&!service.execute(first),"Duplicate execution was accepted");
            inbox.wait(opt::InferenceEvent::Kind::Started,1);
            check(service.plan(request(2,1)),"A new page could not supersede active work");
            check(inbox.wait(opt::InferenceEvent::Kind::Finished,1)->result.reason==opt::FinishReason::Cancelled,"Active superseded job did not preserve cancellation");
            auto second=inbox.wait(opt::InferenceEvent::Kind::Prepared,2)->chunk;
            check(!service.execute(first)&&service.execute(second),"A delayed old cache response executed the wrong page");
            check(inbox.wait(opt::InferenceEvent::Kind::Finished,2)->result.reason==opt::FinishReason::Completed,"Independent next job failed");
            service.cancel_job();check(!service.execute(second),"Stop allowed a completed job to restart");
            state->block_prepare=true;state->prepare_started=false;
            check(service.plan(request(3,2)),"Cancellable preparation was not accepted");
            const auto deadline=std::chrono::steady_clock::now()+3s;
            while(!state->prepare_started&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(2ms);
            check(state->prepare_started,"Preparation did not start");
            service.cancel_job();state->block_prepare=false;
            check(service.plan(request(4,3)),"Cancelled preparation blocked the next page");
            auto fourth=inbox.wait(opt::InferenceEvent::Kind::Prepared,4)->chunk;
            check(service.execute(fourth),"Prepared next page could not execute");
            check(inbox.wait(opt::InferenceEvent::Kind::Finished,4)->result.reason==opt::FinishReason::Completed,"Cancelled preparation poisoned the next job");
            service.stop();service.join();check(service.done(),"Inference worker did not finish");
        }
        check(state->destroyed==1,"Backend was not destroyed");
        for(auto owner:state->owners)check(owner!=main_thread&&owner==state->owners.front(),"Inference API ownership escaped its dedicated worker");
        auto blocked=std::make_shared<State>();blocked->fail_metal=false;blocked->block_load=true;Inbox blocked_inbox;
        opt::InferenceService loading([&](auto e){blocked_inbox.put(std::move(e));},[&](auto& cancel){return std::make_unique<FakeBackend>(cancel,blocked);});
        const auto started=std::chrono::steady_clock::now();loading.load(1,"slow-fake.gguf",opt::BackendPreference::Cpu);
        check(std::chrono::steady_clock::now()-started<1s,"Model load blocked the caller");
        std::mutex pdf_mutex;std::condition_variable pdf_cv;bool opened=false;
        opt::PdfService pdf([&](auto event){if(event->kind==opt::PdfEvent::Kind::Opened){std::lock_guard lock(pdf_mutex);opened=true;pdf_cv.notify_one();}});
        pdf.open(1,std::filesystem::u8path(argv[1]));
        {std::unique_lock lock(pdf_mutex);check(pdf_cv.wait_for(lock,3s,[&]{return opened;}),"PDF worker stalled behind a blocked model loader");}
        check(blocked->block_load.load(),"Fake loader was not held during PDF work");
        loading.stop();loading.join();pdf.stop();pdf.join();
        std::cout<<"Inference ownership, Metal fallback, superseding, late-response rejection and independent PDF work passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
