#include "translation/page_attempt.hpp"
#include "inference/inference_service.hpp"
#include "pdf/pdf_service.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>

using namespace opt;
template<class T> class Inbox {
    std::mutex mutex;std::condition_variable cv;std::deque<std::shared_ptr<const T>> queue;
public:
    void push(std::shared_ptr<const T> e){std::lock_guard lock(mutex);queue.push_back(std::move(e));cv.notify_one();}
    auto take(){std::unique_lock lock(mutex);if(!cv.wait_for(lock,std::chrono::seconds(120),[&]{return !queue.empty();}))throw std::runtime_error("Worker event timeout");
        auto e=queue.front();queue.pop_front();return e;}
};
std::filesystem::path path(const char* value){return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value)));}
// Exercise cleanup of a real Metal context followed by a real CPU rebuild.
// This is an opt-in test adapter; no failure switch is exposed by the product.
class FailAfterMetal final:public InferenceBackend {
    Engine engine_;std::shared_ptr<std::atomic_bool> injected_;
public:
    FailAfterMetal(std::atomic_bool& cancelled,std::shared_ptr<std::atomic_bool> injected):engine_(cancelled),injected_(std::move(injected)){}
    void load(const std::filesystem::path& p,bool cpu)override{engine_.load(p,cpu);if(!cpu&&engine_.backend()=="Metal"){*injected_=true;throw std::runtime_error("Injected failure after constructing the real Metal context");}}
    void unload()override{engine_.unload();}
    PreparedInput prepare(std::string_view s,Form f)const override{return engine_.prepare(s,f);}
    std::vector<std::int32_t> tokenize(std::string_view s,bool bos,bool special)const override{return engine_.tokenize(s,bos,special);}
    GenerationResult generate(const PreparedInput& p,const std::function<void(std::string_view)>& delta)override{return engine_.generate(p,delta);}
    std::uint32_t capacity()const override{return engine_.capacity();}
    std::string backend()const override{return engine_.backend();}
    InferenceRuntime runtime()const override{return engine_.runtime();}
    std::shared_ptr<const FileSnapshot> file_snapshot()const override{return engine_.file_snapshot();}
};
int main(int argc,char** argv){
    if(argc!=4&&argc!=5){std::cerr<<"usage: translation_probe model.gguf cpu|auto fixture.pdf [fail-after-metal]\n";return 2;}
    try{
        Inbox<PdfEvent> pdf_events;Inbox<InferenceEvent> inference_events;
        PdfService pdf([&](auto e){pdf_events.push(std::move(e));});
        const bool fail_metal=argc==5&&std::string(argv[4])=="fail-after-metal";
        auto injected=std::make_shared<std::atomic_bool>(false);InferenceService::Factory factory;
        if(fail_metal)factory=[injected](auto& cancelled){return std::make_unique<FailAfterMetal>(cancelled,injected);};
        InferenceService inference([&](auto e){inference_events.push(std::move(e));},std::move(factory));
        inference.load(1,path(argv[1]),std::string(argv[2])=="cpu"?BackendPreference::Cpu:BackendPreference::Automatic);
        pdf.open(1,path(argv[3]));
        std::shared_ptr<const PageLayout> layout;
        while(!layout){
            auto e=pdf_events.take();
            if(e->kind==PdfEvent::Kind::Opened)pdf.extract(1,0);
            if(e->kind==PdfEvent::Kind::Extracted)layout=e->layout;
            if(e->kind==PdfEvent::Kind::Failed)throw std::runtime_error(e->message);
        }
        for(;;){auto e=inference_events.take();if(e->kind==InferenceEvent::Kind::ModelReady){std::cout<<"backend="<<e->backend<<" context="<<e->context_capacity<<" load_seconds="<<e->seconds<<'\n';
                if(fail_metal){if(!injected->load()||e->backend!="CPU")throw std::runtime_error("Real Metal failure / CPU rebuild was not exercised");std::cout<<"real_metal_context_then_cpu_fallback=passed\n";}break;}
            if(e->kind==InferenceEvent::Kind::ModelFailed)throw std::runtime_error(e->detail);}
        std::cout<<"source:\n"<<layout->reading_text_utf8<<"\ntranslation:\n";
        PageAttempt attempt({1,0,1,1,1},"probe-condition",PageAttempt::source(*layout),Form::Plain);
        auto pump=[&]{if(auto request=attempt.next_request())if(!inference.plan(*request))throw std::runtime_error("Plan rejected");};
        pump();
        while(attempt.state()==PageState::Running||attempt.state()==PageState::Queued){
            auto e=inference_events.take();
            switch(e->kind){
            case InferenceEvent::Kind::Prepared:
                if(attempt.prepared(e->chunk)){if(auto p=attempt.active_prepared()){if(!inference.execute(p))throw std::runtime_error("Execution rejected");}else pump();}break;
            case InferenceEvent::Kind::TextDelta:attempt.delta(e->stamp,e->delta);std::cout<<e->delta<<std::flush;break;
            case InferenceEvent::Kind::Finished:
                attempt.finished(e->stamp,e->result);
                std::cout<<"\nchunk reason="<<static_cast<int>(e->result.reason)<<" seconds="<<e->seconds
                         <<" input_tokens="<<e->chunk->input.tokens.size()<<" output_tokens="<<e->result.output_tokens<<'\n';
                pump();break;
            case InferenceEvent::Kind::PreparationFailed:throw std::runtime_error(e->detail);
            default:break;
            }
        }
        if(attempt.state()!=PageState::Complete)throw std::runtime_error("Page was not complete");
        std::cout<<"\npage_complete:\n"<<attempt.display_text()<<'\n';
        inference.stop();pdf.stop();inference.join();pdf.join();
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
