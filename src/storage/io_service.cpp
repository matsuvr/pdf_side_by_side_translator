#include "storage/io_service.hpp"
#include "storage/legacy_data.hpp"
#include "platform/access_path.hpp"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace opt {
namespace {
class ExportError final:public std::runtime_error {public:using std::runtime_error::runtime_error;};
std::string utf8(const std::filesystem::path& p){auto s=p.u8string();return {reinterpret_cast<const char*>(s.data()),s.size()};}
bool formal(std::string_view key){return key.starts_with("sha256:");}
void export_header(std::ostream& out,Form form){out<<"Offline PDF Translator\nモデル: PLaMo 3 Translate 0.8B\n文体: "<<(form==Form::Plain?"標準文体":"やさしい文体")
    <<"\nPLaMoを翻訳に利用しています。この生成物を利用して学習したモデルにはPLaMoコミュニティライセンス契約が適用されます。"
    <<"\n利用条件: https://huggingface.co/pfnet/plamo-3-translate-0.8b-GGUF/blob/1cf96ae02133958a20074bfdfab76517bb737d00/LICENSE/ja\n\n";}
void atomic_export(const std::filesystem::path& destination,const std::function<void(std::ostream&)>& writer,const std::function<void()>& guard){
    guard();
    auto temporary=destination;temporary+=std::filesystem::path(".tmp-"+new_uuid());
    try{
        {std::ofstream file(access_path(temporary),std::ios::binary|std::ios::trunc);if(!file)throw std::runtime_error("Export destination cannot be opened");writer(file);file.flush();if(!file)throw std::runtime_error("Export write failed");file.close();if(!file)throw std::runtime_error("Export close failed");}
        guard();
#ifdef _WIN32
        if(!MoveFileExW(access_path(temporary).c_str(),access_path(destination).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::system_error(GetLastError(),std::system_category(),"Replace export");
#else
        std::filesystem::rename(temporary,destination);
#endif
    }catch(...){std::error_code ignored;std::filesystem::remove(access_path(temporary),ignored);throw;}
}
}
struct IoService::Impl {
    using Task=std::function<void(Store&)>;
    struct Job {std::string coalesce;Task execute;bool required=false,export_operation=false;};
    struct Input {std::filesystem::path path;std::shared_ptr<const FileSnapshot> file;};
    std::filesystem::path selected_document,selected_model;
    Input opened_document,opened_model;
    struct Hash {
        bool model=false;std::uint64_t epoch=0,offset=0;
        std::string provisional,native_provisional,path;
        std::shared_ptr<const FileSnapshot> file;
        FileSignature signature;
        Sha256 digest;
    };
    Delivery delivery;
    std::function<void()> before_job;
    std::filesystem::path legacy_directory;
    std::mutex mutex;std::condition_variable cv;std::deque<Job> jobs;
    std::atomic_bool stopping{false},finished{false};
    std::atomic<std::size_t> pending_exports{0};
    std::atomic<DocumentEpoch> desired_document{0};
    std::atomic<ModelEpoch> desired_model{0};
    std::unique_ptr<Hash> document_hash,model_hash;
    std::map<std::string,std::string> aliases,state_keys,native_keys;
    std::map<std::string,std::map<PageIndex,SourceRevision>> failed_overrides;
    bool source_tracking_failed=false;
    std::vector<std::byte> hash_buffer=std::vector<std::byte>(1024*1024);
    bool hash_turn=false,dirty=false;
    std::thread worker;
    Impl(Delivery sink,std::function<void()> hook,std::filesystem::path old_directory):delivery(std::move(sink)),before_job(std::move(hook)),legacy_directory(std::move(old_directory)){
        worker=std::thread([this]{run();});
    }
    ~Impl(){stop();if(worker.joinable())worker.join();}
    void emit(IoEvent e){delivery(std::make_shared<const IoEvent>(std::move(e)));}
    void error(std::string detail,bool exporting=false,std::string message={}){IoEvent e;e.export_operation=exporting;e.message=message.empty()?(exporting?"書き出しができません。保存先を確認して、もう一度書き出してください。":"セッション内のデータを更新できませんでした。現在の閲覧・翻訳は続けられます。"):std::move(message);e.detail=std::move(detail);emit(std::move(e));}
    std::string resolve(std::string key)const{for(unsigned i=0;i<4;++i){const auto at=aliases.find(key);if(at==aliases.end())break;key=at->second;}return key;}
    CacheContext resolve(CacheContext context)const{context.document_key=resolve(std::move(context.document_key));context.model_key=resolve(std::move(context.model_key));return context;}
    bool enqueue(std::string key,Task task,bool required=false,bool exporting=false){
        std::lock_guard lock(mutex);if(stopping.load())return false;
        if(!key.empty())for(auto& job:jobs)if(job.coalesce==key){job.execute=std::move(task);cv.notify_one();return true;}
        if(!required&&std::count_if(jobs.begin(),jobs.end(),[](const Job& j){return !j.required;})>=64)return false;
        jobs.push_back({std::move(key),std::move(task),required,exporting});
        if(exporting)++pending_exports;
        cv.notify_one();return true;
    }
    std::vector<Input> inputs(){
        std::lock_guard lock(mutex);return {{selected_document,{}},{selected_model,{}},opened_document,opened_model};
    }
    void protect(bool model,std::filesystem::path path,std::shared_ptr<const FileSnapshot> file={}){
        std::lock_guard lock(mutex);
        if(file){auto& input=model?opened_model:opened_document;input.path=std::move(path);input.file=std::move(file);}
        else (model?selected_model:selected_document)=std::move(path);
    }
    void check_destination(const std::filesystem::path& destination,const std::vector<Input>& captured){
        auto protected_inputs=inputs();protected_inputs.insert(protected_inputs.end(),captured.begin(),captured.end());
        auto normalized=[](const std::filesystem::path& path){std::error_code ec;auto value=std::filesystem::weakly_canonical(access_path(path),ec);return ec?std::filesystem::absolute(path).lexically_normal():value;};
        const auto target=normalized(destination);
        std::shared_ptr<const FileSnapshot> existing;
        std::error_code ec;if(std::filesystem::is_regular_file(access_path(destination),ec)&&!ec)existing=FileSnapshot::open(destination);
        for(const auto& input:protected_inputs){
            if(input.path.empty())continue;
            ec.clear();const bool same_file=std::filesystem::equivalent(access_path(destination),access_path(input.path),ec)&&!ec;
            if(target==normalized(input.path)||same_file||(existing&&input.file&&existing->initial_signature().identity==input.file->initial_signature().identity))
                throw ExportError("選択中のPDFまたはGGUFモデルを上書きする保存先は使えません。別のファイル名を選んでください。");
        }
    }
    void stop()noexcept{std::lock_guard lock(mutex);stopping=true;cv.notify_one();}
    void promote(Store& store,Hash& hash,std::string digest){
        if(hash.file->signature()!=hash.signature)throw std::runtime_error("File changed while fingerprinting");
        const auto key="sha256:"+digest;
        store.fingerprint(hash.path,hash.signature,digest);
        if(!hash.model)store.promote_overrides(hash.native_provisional,key);
        store.promote_cache(hash.model,hash.provisional,key);aliases[hash.provisional]=key;
        if(!hash.model){state_keys[hash.provisional]=key;native_keys[key]=hash.native_provisional;}
        IoEvent e;e.kind=IoEvent::Kind::Fingerprint;e.model_fingerprint=hash.model;e.from_key=hash.provisional;e.key=key;
        if(hash.model)e.model=hash.epoch;else e.document=hash.epoch;emit(std::move(e));dirty=true;
    }
    void hash_step(Store& store){
        hash_turn=!hash_turn;auto* slot=hash_turn?&model_hash:&document_hash;if(!*slot)slot=hash_turn?&document_hash:&model_hash;if(!*slot)return;
        auto& h=**slot;
        if(stopping.load()||(h.model?h.epoch!=desired_model.load():h.epoch!=desired_document.load())){slot->reset();return;}
        try{
            if(h.file->signature()!=h.signature)throw std::runtime_error("File changed while fingerprinting");
            const auto count=h.file->read_at(h.offset,hash_buffer);
            if(count){h.digest.update(std::span(hash_buffer).first(count));h.offset+=count;}
            if(!count||h.offset==h.signature.size){
                if(h.offset!=h.signature.size)throw std::runtime_error("Unexpected file length while fingerprinting");
                promote(store,h,h.digest.finish());slot->reset();
            }
        }catch(const std::exception& e){slot->reset();error(e.what());}
    }
    void run()noexcept{
        try{
            const auto cleanup=discard_legacy_data(legacy_directory);
            Store store;
            IoEvent initialized;initialized.kind=IoEvent::Kind::Initialized;initialized.settings=store.settings();initialized.recent_paths=store.recent_documents();
            initialized.warnings=store.warnings();initialized.persistent=store.state_persistent()&&store.cache_persistent();emit(std::move(initialized));
            if(cleanup.error)error(utf8(cleanup.failed_path)+": "+cleanup.error.message(),false,
                "旧版の保存データを一部削除できませんでした。旧版のアプリを終了し、保存先の権限を確認してください。この起動中の閲覧・翻訳は続けられます。");
            for(;;){
                std::optional<Job> job;
                {std::unique_lock lock(mutex);cv.wait(lock,[&]{return stopping.load()||!jobs.empty()||document_hash||model_hash||dirty;});
                    if(!jobs.empty()){job=std::move(jobs.front());jobs.pop_front();}else if(stopping.load())break;}
                if(job){
                    struct ExportCompletion {Impl& owner;bool exporting;~ExportCompletion(){if(exporting)--owner.pending_exports;}} completion{*this,job->export_operation};
                    try{if(before_job)before_job();job->execute(store);dirty=true;}
                    catch(const ExportError& e){error(e.what(),true,e.what());}
                    catch(const std::exception& e){error(e.what(),job->export_operation);}
                }
                if(!stopping.load())hash_step(store);
                bool idle=false;{std::lock_guard lock(mutex);idle=jobs.empty()&&!document_hash&&!model_hash;}
                if(idle&&dirty&&!stopping.load()){dirty=false;try{dirty=store.maintain();}catch(const std::exception& e){error(e.what());}}
            }
        }catch(const std::exception& e){stopping=true;try{error(e.what(),pending_exports.load()!=0);}catch(...){}}
        // A fatal initialization/owner failure abandons queued requests. Its
        // export error is delivered above before reporting the queue drained.
        {std::lock_guard lock(mutex);jobs.clear();pending_exports=0;}
        finished=true;
    }
};
IoService::IoService(Delivery sink,std::function<void()> hook,std::filesystem::path old_directory):impl_(std::make_unique<Impl>(std::move(sink),std::move(hook),std::move(old_directory))){}
IoService::~IoService()=default;
void IoService::protect_document(std::filesystem::path path){impl_->protect(false,std::move(path));}
void IoService::protect_model(std::filesystem::path path){impl_->protect(true,std::move(path));}
bool IoService::open_document(DocumentEpoch epoch,std::string provisional,std::filesystem::path path,std::shared_ptr<const FileSnapshot> file){
    impl_->protect(false,path,file);impl_->desired_document=epoch;
    return impl_->enqueue("document",[p=impl_.get(),epoch,provisional=std::move(provisional),path=std::move(path),file=std::move(file)](Store& store){
        p->document_hash.reset();if(epoch!=p->desired_document.load()||!file)return;
        const auto signature=file->signature();if(signature!=file->initial_signature())throw std::runtime_error("PDF changed before fingerprint request");
        const auto native=provisional_file_key(signature),path_text=utf8(path);p->state_keys[provisional]=native;
        p->native_keys[provisional]=native;
        auto known=store.fingerprint(path_text,signature);auto key=provisional;
        if(known){key="sha256:"+*known;store.promote_overrides(native,key);store.promote_cache(false,provisional,key);p->aliases[provisional]=key;p->state_keys[provisional]=key;p->native_keys[key]=native;}
        IoEvent event;event.kind=IoEvent::Kind::DocumentLoaded;event.document=epoch;event.from_key=provisional;event.key=key;
        event.saved_document=store.document(known?key:native,native,true);event.saved_document->document_key=key;event.persistent=store.state_persistent();p->emit(std::move(event));
        if(!known){auto h=std::make_unique<Impl::Hash>();h->epoch=epoch;h->provisional=provisional;h->native_provisional=native;h->path=path_text;h->file=file;h->signature=signature;p->document_hash=std::move(h);}
    });
}
bool IoService::close_document(){
    impl_->desired_document=0;return impl_->enqueue("document",[p=impl_.get()](Store&){p->document_hash.reset();});
}
bool IoService::read_page(DocumentEpoch epoch,std::string key,std::string native,PageIndex page){
    return impl_->enqueue("",[p=impl_.get(),epoch,key=std::move(key),native=std::move(native),page](Store& store){
        IoEvent e;e.kind=IoEvent::Kind::PageLoaded;e.document=epoch;
        try{const auto resolved=p->resolve(key);e.saved_document=store.document(formal(resolved)?resolved:native,native,true,page);}
        catch(const std::exception& error){e.detail=error.what();e.saved_document=SavedDocument{};e.saved_document->position.page=page;}
        p->emit(std::move(e));
    });
}
bool IoService::track_model(ModelEpoch epoch,std::string provisional,std::filesystem::path path,std::shared_ptr<const FileSnapshot> file){
    impl_->protect(true,path,file);impl_->desired_model=epoch;
    return impl_->enqueue("model",[p=impl_.get(),epoch,provisional=std::move(provisional),path=std::move(path),file=std::move(file)](Store& store){
        p->model_hash.reset();if(epoch!=p->desired_model.load()||!file)return;
        const auto signature=file->signature();if(signature!=file->initial_signature())throw std::runtime_error("Model changed before fingerprint request");
        auto h=std::make_unique<Impl::Hash>();h->model=true;h->epoch=epoch;h->provisional=provisional;h->path=utf8(path);h->file=file;h->signature=signature;
        auto known=store.fingerprint(h->path,signature);
        if(known)p->promote(store,*h,*known);else p->model_hash=std::move(h);
    });
}
bool IoService::settings(std::map<std::string,std::string> values){return impl_->enqueue("",[values=std::move(values)](Store& store){store.settings(values);});}
bool IoService::save_override(DocumentEpoch epoch,std::string provisional,std::string key,std::filesystem::path path,SavedOverride value){
    const auto coalesce="override:"+provisional+":"+std::to_string(value.page);
    return impl_->enqueue(coalesce,[p=impl_.get(),epoch,provisional=std::move(provisional),key=std::move(key),path=std::move(path),value=std::move(value)](Store& store){
        const auto resolved=p->resolve(key);const auto final=formal(resolved)?resolved:std::string{};
        // Register before the write: failures must not make completed-page or
        // session-state export silently describe an older source as current.
        try{auto& failed=p->failed_overrides[provisional][value.page];failed=std::max(failed,value.revision);}catch(...){p->source_tracking_failed=true;throw;}
        store.save_override(provisional,final,utf8(path),value);if(!final.empty())store.promote_overrides(provisional,final);
        auto failed=p->failed_overrides.find(provisional);if(value.revision>=failed->second.at(value.page))failed->second.erase(value.page);if(failed->second.empty())p->failed_overrides.erase(failed);
        IoEvent e;e.kind=IoEvent::Kind::OverrideSaved;e.document=epoch;e.persistent=store.state_persistent();e.request=value.revision;p->emit(std::move(e));
    },true);
}
bool IoService::save_position(std::string key,std::filesystem::path path,ReadingPosition value){
    const auto coalesce="position:"+key+":"+std::to_string(value.page);
    return impl_->enqueue(coalesce,[p=impl_.get(),key=std::move(key),path=std::move(path),value](Store& store){auto resolved=p->resolve(key);if(!formal(resolved)){auto found=p->state_keys.find(key);if(found==p->state_keys.end())return;resolved=found->second;}store.save_position(resolved,utf8(path),value);});
}
bool IoService::save_attempt(CacheContext context,AttemptSnapshot data){
    // Retranslation creates a new attempt id. Coalesce by its page/profile so
    // repeated retries cannot fill the mailbox with superseded snapshots.
    const auto coalesce="attempt:"+CanonicalKey{}.text("document",context.document_key).number("page",context.page).text("model",context.model_key).text("profile",context.profile_digest).finish();
    return impl_->enqueue(coalesce,[p=impl_.get(),context=std::move(context),data=std::move(data)](Store& store)mutable{
        auto resolved=p->resolve(context);data.condition_key=resolved.condition_key();store.save_attempt(resolved,data);
        IoEvent e;e.kind=IoEvent::Kind::AttemptSaved;e.key=data.id;e.persistent=store.cache_persistent();p->emit(std::move(e));
    });
}
bool IoService::read_attempt(CacheContext context,std::uint64_t request,DocumentEpoch epoch){
    return impl_->enqueue("",[p=impl_.get(),context=std::move(context),request,epoch](Store& store){IoEvent e;e.kind=IoEvent::Kind::AttemptLoaded;e.document=epoch;e.request=request;
        try{const auto resolved=p->resolve(context);e.key=resolved.condition_key();e.attempt=store.attempt(resolved);}catch(const std::exception& error){e.detail=error.what();}p->emit(std::move(e));});
}
bool IoService::lookup(CacheContext context,std::shared_ptr<const PreparedChunk> chunk){
    return impl_->enqueue("",[p=impl_.get(),context=std::move(context),chunk=std::move(chunk)](Store& store){IoEvent e;e.kind=IoEvent::Kind::CacheResult;e.stamp=chunk->stamp;
        try{auto resolved=p->resolve(context);e.cached_text=store.completed(resolved,*chunk,!formal(resolved.document_key)||!formal(resolved.model_key));}catch(const std::exception& error){e.detail=error.what();}p->emit(std::move(e));});
}
bool IoService::export_current(std::filesystem::path path,PageIndex page,std::string text,PageState state,Form form){
    auto inputs=impl_->inputs();
    return impl_->enqueue("",[p=impl_.get(),path=std::move(path),page,text=std::move(text),state,form,inputs=std::move(inputs)](Store&){
        atomic_export(path,[&](std::ostream& out){export_header(out,form);out<<"# "<<page+1<<"ページ — "<<(state==PageState::Complete?"完了":"未完了・部分結果")<<"\n\n"<<text<<'\n';},[&]{p->check_destination(path,inputs);});
        IoEvent e;e.kind=IoEvent::Kind::Exported;e.export_operation=true;e.message="訳文を書き出しました。";p->emit(std::move(e));},false,true);
}
bool IoService::export_complete(std::filesystem::path path,CacheContext context,Form form){
    auto inputs=impl_->inputs();
    return impl_->enqueue("",[p=impl_.get(),path=std::move(path),context=std::move(context),form,inputs=std::move(inputs)](Store& store){
        const auto resolved=p->resolve(context);std::size_t count=0;
        const auto native=p->native_keys.find(resolved.document_key);
        if(p->source_tracking_failed||p->failed_overrides.contains(context.document_key)||p->failed_overrides.contains(resolved.document_key)||(native!=p->native_keys.end()&&p->failed_overrides.contains(native->second)))
            throw ExportError("原文編集を反映できていないページがあります。原文編集をもう一度適用してから書き出してください。");
        atomic_export(path,[&](std::ostream& out){export_header(out,form);out<<"完成したページのみを収録しています。未完了ページは含みません。\n\n";count=store.for_each_complete_page(resolved.document_key,resolved.model_key,resolved.profile_digest,[&](PageIndex page,std::string_view text){out<<"# "<<page+1<<"ページ — 完了\n\n"<<text<<"\n\n";},native==p->native_keys.end()?std::string_view{}:std::string_view(native->second));if(!count)throw ExportError("書き出せる完成訳がありません。ページを翻訳してから、もう一度書き出してください。");},[&]{p->check_destination(path,inputs);});
        IoEvent e;e.kind=IoEvent::Kind::Exported;e.export_operation=true;e.message=std::to_string(count)+"ページの完成訳を書き出しました。";p->emit(std::move(e));},false,true);
}
bool IoService::clear_cache(){return impl_->enqueue("",[p=impl_.get()](Store& store){store.clear_cache();IoEvent e;e.kind=IoEvent::Kind::CacheCleared;p->emit(std::move(e));});}
bool IoService::export_state(std::filesystem::path destination){
    auto inputs=impl_->inputs();
    return impl_->enqueue("",[p=impl_.get(),destination=std::move(destination),inputs=std::move(inputs)](Store& store){
        if(p->source_tracking_failed||!p->failed_overrides.empty())throw ExportError("原文編集を反映できていないページがあります。原文編集をもう一度適用してから書き出してください。");
        store.export_state(destination,[&]{p->check_destination(destination,inputs);});
        IoEvent e;e.kind=IoEvent::Kind::Exported;e.export_operation=true;e.message="この起動中の設定・原文編集・読書位置を書き出しました。";p->emit(std::move(e));},false,true);
}
bool IoService::diagnostic(DiagnosticRecord){
    // The GUI owns the bounded, in-memory diagnostic summary. Do not create
    // logs or any other automatic document-related files outside exports.
    return !impl_->stopping.load();
}
void IoService::stop()noexcept{impl_->stop();}
std::size_t IoService::exports_pending()const noexcept{return impl_->pending_exports.load();}
bool IoService::done()const noexcept{return impl_->finished.load();}
void IoService::join(){if(impl_->worker.joinable())impl_->worker.join();}
}
