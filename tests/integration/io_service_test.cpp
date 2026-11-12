#include "storage/io_service.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace opt;
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
class Inbox {
    std::mutex mutex;std::condition_variable cv;std::deque<std::shared_ptr<const IoEvent>> queue;
public:
    void push(std::shared_ptr<const IoEvent> e){std::lock_guard lock(mutex);queue.push_back(std::move(e));cv.notify_one();}
    auto wait(IoEvent::Kind kind){
        const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(20);
        std::unique_lock lock(mutex);
        for(;;){
            if(!cv.wait_until(lock,until,[&]{return !queue.empty();}))throw std::runtime_error("I/O event timeout");
            auto e=queue.front();queue.pop_front();if(e->kind==IoEvent::Kind::Error&&kind!=IoEvent::Kind::Error)throw std::runtime_error(e->detail);if(e->kind==kind)return e;
        }
    }
};
std::string bytes(const std::filesystem::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void write(const std::filesystem::path& path,std::string_view value){std::ofstream out(path,std::ios::binary);out<<value;}
int main(){const auto root=std::filesystem::temp_directory_path()/std::filesystem::path("opt-viewer-io-"+new_uuid());
    try{
        // Test fixtures are created by the test, never by the product service.
        std::filesystem::create_directories(root);const auto file=root/"document.pdf",model=root/"model.gguf";
        const std::string input(8*1024*1024,'x');write(file,input);write(model,"model input");
        const auto pdf_time=std::filesystem::last_write_time(file),model_time=std::filesystem::last_write_time(model);
        auto snapshot=FileSnapshot::open(file);const auto native=provisional_file_key(snapshot->initial_signature());
        Inbox inbox;
        {
            IoService service([&](auto e){inbox.push(std::move(e));});
            require(!inbox.wait(IoEvent::Kind::Initialized)->persistent,"worker starts directly in memory");
            require(service.settings({{"form","easy"}}),"session settings accepted");
            const auto started=std::chrono::steady_clock::now();
            require(service.open_document(1,"provisional:session:1",file,snapshot),"open request accepted");
            require(std::chrono::steady_clock::now()-started<std::chrono::milliseconds(100),"caller does not hash the file");
            auto opened=inbox.wait(IoEvent::Kind::DocumentLoaded);require(opened->key.starts_with("provisional:"),"document usable before content digest");
            require(service.save_position(opened->key,file,{2,PdfPoint{3.5,4.25},8}),"reading position accepted during hashing");
            auto digest=inbox.wait(IoEvent::Kind::Fingerprint);require(digest->key=="sha256:"+sha256(input),"incremental same-object digest matches reference");
            service.read_page(1,opened->key,native,2);auto saved=inbox.wait(IoEvent::Kind::PageLoaded);
            require(saved->saved_document->position==ReadingPosition{2,PdfPoint{3.5,4.25},8},"reading position survives hash completion");
            service.track_model(1,"provisional:model:1",model,FileSnapshot::open(model));
            require(inbox.wait(IoEvent::Kind::Fingerprint)->key=="sha256:"+sha256("model input"),"model fingerprint is computed read-only");
            service.stop();service.join();require(service.done(),"worker stops after memory requests");
        }
        {
            Inbox restarted;IoService service([&](auto e){restarted.push(std::move(e));});auto initialized=restarted.wait(IoEvent::Kind::Initialized);
            require(initialized->settings.empty()&&initialized->recent_paths.empty(),"new session has no saved settings or history");
            service.read_page(2,"sha256:"+sha256(input),native,2);
            const auto position=restarted.wait(IoEvent::Kind::PageLoaded)->saved_document->position;
            require(!position.pdf_anchor&&position.text_anchor==0,"new session cannot recover previous reading state");
            service.stop();service.join();
        }
        // Pause the owner and verify coalesced snapshots keep the queue bounded.
        {
            Inbox queued;std::mutex gate_mutex;std::condition_variable gate_cv;bool paused=true,entered=false;
            IoService service([&](auto e){queued.push(std::move(e));},[&]{std::unique_lock lock(gate_mutex);entered=true;gate_cv.notify_one();gate_cv.wait(lock,[&]{return !paused;});});
            queued.wait(IoEvent::Kind::Initialized);service.read_page(3,"",native,0);
            {std::unique_lock lock(gate_mutex);gate_cv.wait(lock,[&]{return entered;});}
            CacheContext context{"queue-document","queue-model","profile","source",0};AttemptSnapshot data;data.id=new_uuid();data.condition_key=context.condition_key();
            for(unsigned i=0;i<256;++i){data.plan=i+1;require(service.save_attempt(context,data),"snapshot updates coalesce");}
            for(unsigned i=0;i<63;++i)require(service.read_page(3,"",native,i),"fill remaining mailbox slots");
            require(!service.read_page(3,"",native,65),"mailbox remains bounded");
            {std::lock_guard lock(gate_mutex);paused=false;}gate_cv.notify_one();
            queued.wait(IoEvent::Kind::AttemptSaved);
            service.read_attempt(context,1,3);auto saved=queued.wait(IoEvent::Kind::AttemptLoaded);
            require(saved->attempt&&saved->attempt->id==data.id&&saved->attempt->plan==256,"latest snapshot survives coalescing");
            service.stop();service.join();
        }
        require(bytes(file)==input&&bytes(model)=="model input","inputs remain byte-for-byte unchanged");
        require(std::filesystem::last_write_time(file)==pdf_time&&std::filesystem::last_write_time(model)==model_time,"input modification times remain unchanged");
        require(std::distance(std::filesystem::directory_iterator(root),std::filesystem::directory_iterator{})==2,"viewer creates no output, state, cache, logs, locks or temporary files");
        snapshot.reset();std::filesystem::remove_all(root);
        std::cout<<"memory session isolation, read-only fingerprints, reading state, bounded queue and no file output passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<" (fixtures at "<<root<<")\n";return 1;}
}
