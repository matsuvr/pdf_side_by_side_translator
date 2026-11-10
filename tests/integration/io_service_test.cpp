#include "storage/io_service.hpp"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>

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
int main(){const auto root=std::filesystem::temp_directory_path()/std::filesystem::path("opt-io-"+new_uuid());
    try{
        std::filesystem::create_directories(root);const auto file=root/"document.pdf";
        std::string input(8*1024*1024,'x');{std::ofstream out(file,std::ios::binary);out<<input;}
        auto snapshot=FileSnapshot::open(file);const auto native=provisional_file_key(snapshot->initial_signature());
        Inbox inbox;
        {
            IoService service(root/"state",root/"cache",[&](auto e){inbox.push(std::move(e));});
            require(inbox.wait(IoEvent::Kind::Initialized)->persistent,"persistent stores initialized on worker");
            require(service.settings({{"form","polite"}})&&service.settings({{"auto","off"}}),"independent settings updates accepted");
            const auto started=std::chrono::steady_clock::now();
            require(service.open_document(1,"provisional:session:1",file,snapshot),"open request accepted");
            require(std::chrono::steady_clock::now()-started<std::chrono::milliseconds(100),"fingerprint request does not hash on caller");
            auto opened=inbox.wait(IoEvent::Kind::DocumentLoaded);require(opened->key.starts_with("provisional:"),"document usable before content digest");
            require(service.save_override(1,native,opened->key,file,{0,7,std::string("利用者の原文")}),"edit request accepted during hash");
            auto digest=inbox.wait(IoEvent::Kind::Fingerprint);require(digest->key=="sha256:"+sha256(input),"incremental same-object digest matches reference");
            // Barrier: export is sequenced after the pending edit and promotion.
            const auto output=root/"translation.txt";require(service.export_current(output,0,"日本語の訳文",PageState::Partial),"UTF-8 export queued");inbox.wait(IoEvent::Kind::Exported);
            require(bytes(output).find("未完了・部分結果")!=std::string::npos&&bytes(output).find("日本語の訳文")!=std::string::npos,"partial export labelled and encoded UTF-8");
            require(bytes(output).find("文体: 常体")!=std::string::npos&&bytes(output).find("PLaMo 3 Translate 0.8B")!=std::string::npos&&bytes(output).find("学習したモデル")!=std::string::npos,"export retains model, form and output-use notice");
            service.stop();service.join();require(service.done(),"worker drains durable mutations on stop");
        }
        {
            Store store(root/"state",root/"cache");require(store.settings().at("form")=="polite"&&store.settings().at("auto")=="off","settings coalescing never loses unrelated changes");
            auto value=store.document("sha256:"+sha256(input),native);require(value.overrides.size()==1&&*value.overrides[0].source=="利用者の原文","edit committed and promoted despite hash completion race");
        }
        Inbox second;
        {
            IoService service(root/"state",root/"cache",[&](auto e){second.push(std::move(e));});second.wait(IoEvent::Kind::Initialized);
            service.open_document(2,"provisional:session:2",file,snapshot);
            auto opened=second.wait(IoEvent::Kind::DocumentLoaded);require(opened->key=="sha256:"+sha256(input),"unchanged signature skips rehash on restart");
            service.save_override(2,native,opened->key,file,{0,8,std::nullopt});service.close_document();service.stop();service.join();
        }
        {Store store(root/"state",root/"cache");require(store.document("sha256:"+sha256(input),native).overrides.empty(),"reset tombstone survives immediate close and shutdown");}
        // A new object's unpromoted source survives cancellation before any hash.
        const auto other=root/"other.pdf";{std::ofstream out(other,std::ios::binary);out<<input;}
        auto other_snapshot=FileSnapshot::open(other);const auto other_native=provisional_file_key(other_snapshot->initial_signature());
        Inbox third;
        {IoService service(root/"state",root/"cache",[&](auto e){third.push(std::move(e));});third.wait(IoEvent::Kind::Initialized);
            service.open_document(3,"provisional:session:3",other,other_snapshot);service.save_override(3,other_native,"provisional:session:3",other,{0,9,std::string("未昇格でも保存")});service.close_document();
            const auto target=root/"preserved.txt";{std::ofstream out(target);out<<"keep original";}
            service.export_complete(target,{"nonexistent-document","nonexistent-model","profile","source",0});third.wait(IoEvent::Kind::Error);
            require(bytes(target)=="keep original","failed export preserves existing destination atomically");
            service.export_current(root/"after-error.txt",0,"処理は続けられる",PageState::Complete);third.wait(IoEvent::Kind::Exported);
            service.stop();service.join();}
        {Store store(root/"state",root/"cache");auto saved=store.document(other_native,other_native);require(saved.overrides.size()==1&&*saved.overrides[0].source=="未昇格でも保存","unpromoted user source is durable independently of cache/hash cancellation");}
        std::filesystem::remove_all(root);std::cout<<"async fingerprints, saved-signature reuse, edit/hash races, UTF-8 export and shutdown durability passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<" (fixtures at "<<root<<")\n";return 1;}
}
