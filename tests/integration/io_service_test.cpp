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
int main(){const auto root=std::filesystem::temp_directory_path()/std::filesystem::path("opt-io-"+new_uuid());
    try{
        std::filesystem::create_directories(root);const auto file=root/"document.pdf",model=root/"model.gguf";
        const std::string input(8*1024*1024,'x');write(file,input);write(model,"model input");
        auto snapshot=FileSnapshot::open(file);const auto native=provisional_file_key(snapshot->initial_signature());
        Inbox inbox;
        {
            IoService* observed=nullptr;
            IoService service([&](auto e){if(e->export_operation)require(observed&&observed->exports_pending()>0,"export remains pending until result delivery completes");inbox.push(std::move(e));});observed=&service;
            require(!inbox.wait(IoEvent::Kind::Initialized)->persistent,"worker starts directly in memory");
            service.protect_document(file);service.protect_model(model);
            require(service.settings({{"form","easy"}})&&service.settings({{"auto","off"}}),"session settings updates accepted");
            const auto started=std::chrono::steady_clock::now();
            require(service.open_document(1,"provisional:session:1",file,snapshot),"open request accepted");
            require(std::chrono::steady_clock::now()-started<std::chrono::milliseconds(100),"fingerprint request does not hash on caller");
            auto opened=inbox.wait(IoEvent::Kind::DocumentLoaded);require(opened->key.starts_with("provisional:"),"document usable before content digest");
            require(service.save_override(1,native,opened->key,file,{0,7,std::string("利用者の原文")}),"edit request accepted during hash");
            auto digest=inbox.wait(IoEvent::Kind::Fingerprint);require(digest->key=="sha256:"+sha256(input),"incremental same-object digest matches reference");
            service.read_page(1,opened->key,native,0);auto saved=inbox.wait(IoEvent::Kind::PageLoaded);
            require(*saved->saved_document->overrides.at(0).source=="利用者の原文","edit retained and promoted across hash completion");
            const auto output=root/"translation.txt";require(service.export_current(output,0,"日本語の訳文",PageState::Partial),"UTF-8 export accepted");
            require(inbox.wait(IoEvent::Kind::Exported)->export_operation,"export completion distinguishable during close");
            require(bytes(output).find("未完了・部分結果")!=std::string::npos&&bytes(output).find("日本語の訳文")!=std::string::npos,"partial export labelled and encoded UTF-8");
            require(bytes(output).find("文体: 標準文体")!=std::string::npos&&bytes(output).find("PLaMo 3 Translate 0.8B")!=std::string::npos&&bytes(output).find("学習したモデル")!=std::string::npos,"export retains model, form and output-use notice");
            require(service.export_current(output,0,"やさしい日本語の訳文",PageState::Complete,Form::Easy),"ordinary existing export may be replaced");inbox.wait(IoEvent::Kind::Exported);
            require(bytes(output).find("文体: やさしい文体")!=std::string::npos,"explicit overwrite publishes new UTF-8 text");
            for(const auto& input_path:{file,model}){
                const auto original=bytes(input_path);require(service.export_current(input_path,0,"must not replace",PageState::Complete),"guarded export accepted for validation");
                auto conflict=inbox.wait(IoEvent::Kind::Error);require(conflict->export_operation&&conflict->message.find("PDF")!=std::string::npos,"input conflict has actionable user-facing export error");require(bytes(input_path)==original,"current input preserved byte-for-byte");
                require(service.export_state(input_path),"state export checked against input too");inbox.wait(IoEvent::Kind::Error);require(bytes(input_path)==original,"explicit state export cannot replace inputs");
            }
            const auto hardlink=root/"hardlink.txt";std::filesystem::create_hard_link(model,hardlink);
            service.export_current(hardlink,0,"must not replace",PageState::Complete);inbox.wait(IoEvent::Kind::Error);
            require(bytes(model)=="model input"&&std::filesystem::equivalent(model,hardlink),"hard-link alias cannot be replaced");
            const auto symlink=root/"symlink.txt";std::error_code ec;std::filesystem::create_symlink(file,symlink,ec);
            if(!ec){service.export_current(symlink,0,"must not replace",PageState::Complete);inbox.wait(IoEvent::Kind::Error);require(std::filesystem::is_symlink(symlink)&&bytes(file)==input,"symlink alias cannot be replaced");}
            // The original path can be replaced externally while the app still
            // reads its opened object. Identity protection follows that object.
            const auto moved=root/"moved.pdf";std::filesystem::rename(file,moved);write(file,"replacement input");
            service.export_current(moved,0,"must not replace",PageState::Complete);inbox.wait(IoEvent::Kind::Error);
            require(bytes(moved)==input,"opened-object identity protects renamed input");
            service.export_current(file,0,"must not replace",PageState::Complete);inbox.wait(IoEvent::Kind::Error);require(bytes(file)=="replacement input","selected path remains protected after external replacement");
            const auto preserved=root/"preserved.txt";write(preserved,"keep original");
            service.export_complete(preserved,{"missing-document","missing-model","profile","source",0});inbox.wait(IoEvent::Kind::Error);
            require(bytes(preserved)=="keep original","failed export preserves existing destination atomically");
            service.export_state(root/"missing-parent"/"state.sqlite3");inbox.wait(IoEvent::Kind::Error);
            service.save_override(1,native,opened->key,file,{0,8,std::string(1,static_cast<char>(0xff))});inbox.wait(IoEvent::Kind::Error);
            const auto failed_state=root/"failed-state.sqlite3";write(failed_state,"keep original state export");
            service.export_state(failed_state);require(inbox.wait(IoEvent::Kind::Error)->export_operation,"failed source write blocks stale session-state export");
            require(bytes(failed_state)=="keep original state export","failed source write does not publish stale state");
            service.export_complete(preserved,{opened->key,"model","profile","source",0});
            require(inbox.wait(IoEvent::Kind::Error)->message.find("原文編集")!=std::string::npos,"failed source write blocks stale completed-page export for document");
            service.save_override(1,native,opened->key,file,{0,9,std::string("成功した再適用")});inbox.wait(IoEvent::Kind::OverrideSaved);
            service.export_state(failed_state);inbox.wait(IoEvent::Kind::Exported);
            require(bytes(failed_state).starts_with("SQLite format 3"),"successful edit retry enables explicit state export again");
            service.read_page(1,opened->key,native,0);auto retry_state=inbox.wait(IoEvent::Kind::PageLoaded);
            require(service.exports_pending()==0&&!service.done()&&*retry_state->saved_document->overrides.at(0).source=="成功した再適用","failed exports can be retried while session store stays alive, before shutdown");
            service.diagnostic({});service.stop();service.join();require(service.done(),"worker completes accepted work before done");
        }
        {
            Inbox restarted;IoService service([&](auto e){restarted.push(std::move(e));});auto initialized=restarted.wait(IoEvent::Kind::Initialized);
            require(initialized->settings.empty()&&initialized->recent_paths.empty(),"new session has no saved settings or history");
            service.read_page(2,"sha256:"+sha256(input),native,0);require(restarted.wait(IoEvent::Kind::PageLoaded)->saved_document->overrides.empty(),"new session cannot recover previous document text");
            service.stop();service.join();
        }
        // Saturate all disposable request slots while the owner is paused.
        // User edits have their own coalesced ownership and cannot be rejected
        // merely because cache reads are waiting.
        {
            Inbox edits;std::mutex gate_mutex;std::condition_variable gate_cv;bool paused=true,entered=false;
            IoService service([&](auto e){edits.push(std::move(e));},[&]{std::unique_lock lock(gate_mutex);entered=true;gate_cv.notify_one();gate_cv.wait(lock,[&]{return !paused;});});
            edits.wait(IoEvent::Kind::Initialized);service.read_page(3,"",native,0);
            {std::unique_lock lock(gate_mutex);gate_cv.wait(lock,[&]{return entered;});}
            CacheContext context{"queue-document","queue-model","profile","source",0};AttemptSnapshot retry;retry.condition_key=context.condition_key();
            for(unsigned i=0;i<256;++i){retry.id=new_uuid();require(service.save_attempt(context,retry),"retranslation coalesces superseded attempt snapshots");}
            for(unsigned i=0;i<63;++i)require(service.read_page(3,"",native,i),"fill remaining disposable mailbox slots");
            require(!service.read_page(3,"",native,65),"disposable mailbox remains bounded");
            require(service.save_override(3,native,"",file,{0,1,std::string("older edit")}),"user edit accepted despite full disposable mailbox");
            require(service.save_override(3,native,"",file,{0,2,std::string("newest edit")}),"pending edit coalesces without losing newest revision");
            {std::lock_guard lock(gate_mutex);paused=false;}gate_cv.notify_one();
            require(edits.wait(IoEvent::Kind::OverrideSaved)->request==2,"newest accepted revision is acknowledged");
            service.read_page(3,"",native,0);require(*edits.wait(IoEvent::Kind::PageLoaded)->saved_document->overrides.at(0).source=="newest edit","page reload retains edit after queue pressure");
            service.read_attempt(context,1,3);require(edits.wait(IoEvent::Kind::AttemptLoaded)->attempt->id==retry.id,"only latest forced-attempt snapshot survives mailbox coalescing");
            service.stop();service.join();
        }
        {
            Inbox slow;IoService service([&](auto e){slow.push(std::move(e));},[]{std::this_thread::sleep_for(std::chrono::milliseconds(2100));});
            slow.wait(IoEvent::Kind::Initialized);const auto target=root/"slow.txt";write(target,"old export");
            require(service.export_current(target,0,"finished slow export",PageState::Complete),"slow export accepted");
            require(service.exports_pending()==1,"executing slow export remains pending");
            service.stop();service.join();require(service.done()&&bytes(target).find("finished slow export")!=std::string::npos,"shutdown drains export even beyond old two-second deadline");
            require(service.exports_pending()==0,"completed accepted exports leave no pending count");
            slow.wait(IoEvent::Kind::Exported);
        }
        for(const auto& entry:std::filesystem::directory_iterator(root)){
            const auto name=entry.path().filename().string();require(name.find(".tmp-")==std::string::npos&&name!="state"&&name!="cache"&&name!="logs"&&name!="storage.lock","no automatic state, logs, lock or orphan export temp");
        }
        std::filesystem::remove_all(root);std::cout<<"session isolation, async fingerprints, edit backpressure, input identity guards, atomic exports and shutdown draining passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<" (fixtures at "<<root<<")\n";return 1;}
}
