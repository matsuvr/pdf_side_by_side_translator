#include "storage/io_service.hpp"
#include "platform/access_path.hpp"
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
std::string utf8(const std::filesystem::path& p){auto s=p.u8string();return {reinterpret_cast<const char*>(s.data()),s.size()};}
bool formal(std::string_view key){return key.starts_with("sha256:");}
void export_header(std::ostream& out,Form form){out<<"Offline PDF Translator\nモデル: PLaMo 3 Translate 0.8B\n文体: "<<(form==Form::Plain?"常体":"敬体")
    <<"\nPLaMoを翻訳に利用しています。この生成物を利用して学習したモデルにはPLaMoコミュニティライセンス契約が適用されます。"
    <<"\n利用条件: https://huggingface.co/pfnet/plamo-3-translate-0.8b-GGUF/blob/1cf96ae02133958a20074bfdfab76517bb737d00/LICENSE/ja\n\n";}
void atomic_export(const std::filesystem::path& destination,const std::function<void(std::ostream&)>& writer){
    auto temporary=destination;temporary+=std::filesystem::path(".tmp-"+new_uuid());
    try{
        {std::ofstream file(access_path(temporary),std::ios::binary|std::ios::trunc);if(!file)throw std::runtime_error("Export destination cannot be opened");writer(file);file.flush();if(!file)throw std::runtime_error("Export write failed");}
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
    struct Job {std::string coalesce;Task execute;};
    struct Hash {
        bool model=false;std::uint64_t epoch=0,offset=0;
        std::string provisional,native_provisional,path;
        std::shared_ptr<const FileSnapshot> file;
        FileSignature signature;
        Sha256 digest;
    };
    Delivery delivery;
    std::function<void()> before_job;
    std::filesystem::path state_directory,cache_directory;
    std::mutex mutex;std::condition_variable cv;std::deque<Job> jobs;
    std::atomic_bool stopping{false},finished{false};
    std::atomic<DocumentEpoch> desired_document{0};
    std::atomic<ModelEpoch> desired_model{0};
    std::unique_ptr<Hash> document_hash,model_hash;
    std::map<std::string,std::string> aliases,state_keys,native_keys;
    std::vector<std::byte> hash_buffer=std::vector<std::byte>(1024*1024);
    bool hash_turn=false,dirty=false;
    std::thread worker;
    Impl(std::filesystem::path s,std::filesystem::path c,Delivery sink,std::function<void()> hook):delivery(std::move(sink)),before_job(std::move(hook)),state_directory(std::move(s)),cache_directory(std::move(c)){
        worker=std::thread([this]{run();});
    }
    ~Impl(){stop();if(worker.joinable())worker.join();}
    void emit(IoEvent e){delivery(std::make_shared<const IoEvent>(std::move(e)));}
    void error(std::string detail){IoEvent e;e.message="保存または書き出しができません。現在の閲覧・翻訳は続けられます。";e.detail=std::move(detail);emit(std::move(e));}
    std::string resolve(std::string key)const{for(unsigned i=0;i<4;++i){const auto at=aliases.find(key);if(at==aliases.end())break;key=at->second;}return key;}
    CacheContext resolve(CacheContext context)const{context.document_key=resolve(std::move(context.document_key));context.model_key=resolve(std::move(context.model_key));return context;}
    bool enqueue(std::string key,Task task){
        std::lock_guard lock(mutex);if(stopping.load())return false;
        if(!key.empty())for(auto& job:jobs)if(job.coalesce==key){job.execute=std::move(task);cv.notify_one();return true;}
        if(jobs.size()>=64)return false;jobs.push_back({std::move(key),std::move(task)});cv.notify_one();return true;
    }
    void stop()noexcept{stopping=true;cv.notify_one();}
    void promote(Store& store,Hash& hash,std::string digest){
        if(hash.file->signature()!=hash.signature)throw std::runtime_error("File changed while fingerprinting");
        const auto key="sha256:"+digest;
        store.prepare_cache_promotion(hash.model,hash.provisional,key);
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
            Store store(state_directory,cache_directory);
            IoEvent initialized;initialized.kind=IoEvent::Kind::Initialized;initialized.settings=store.settings();initialized.recent_paths=store.recent_documents();
            initialized.warnings=store.warnings();initialized.persistent=store.state_persistent()&&store.cache_persistent();emit(std::move(initialized));
            for(;;){
                std::optional<Job> job;
                {std::unique_lock lock(mutex);cv.wait(lock,[&]{return stopping.load()||!jobs.empty()||document_hash||model_hash||dirty;});
                    if(!jobs.empty()){job=std::move(jobs.front());jobs.pop_front();}else if(stopping.load())break;}
                if(job){
                    try{if(before_job)before_job();job->execute(store);dirty=true;}
                    catch(const StorageError& e){
                        if(e.permits_memory_fallback()){
                            try{
                                store.switch_to_memory();
                                IoEvent warning;warning.message="保存先を利用できなくなりました。変更はこの起動中だけ保持します。状態データの退避コピーを保存できます。";emit(std::move(warning));
                                // Retry the immutable accepted request after all failed
                                // transactions have unwound; do not lose the newest edit.
                                job->execute(store);dirty=true;
                            }catch(const std::exception& retry){error(retry.what());}
                        }else error(e.what());
                    }catch(const std::exception& e){error(e.what());}
                }
                if(!stopping.load())hash_step(store);
                bool idle=false;{std::lock_guard lock(mutex);idle=jobs.empty()&&!document_hash&&!model_hash;}
                if(idle&&dirty&&!stopping.load()){dirty=false;try{dirty=store.maintain();}catch(const std::exception& e){error(e.what());}}
            }
        }catch(const std::exception& e){stopping=true;try{error(e.what());}catch(...){}}
        finished=true;
    }
};
IoService::IoService(std::filesystem::path s,std::filesystem::path c,Delivery sink,std::function<void()> hook):impl_(std::make_unique<Impl>(std::move(s),std::move(c),std::move(sink),std::move(hook))){}
IoService::~IoService()=default;
bool IoService::open_document(DocumentEpoch epoch,std::string provisional,std::filesystem::path path,std::shared_ptr<const FileSnapshot> file){
    impl_->desired_document=epoch;
    return impl_->enqueue("document",[p=impl_.get(),epoch,provisional=std::move(provisional),path=std::move(path),file=std::move(file)](Store& store){
        p->document_hash.reset();if(epoch!=p->desired_document.load()||!file)return;
        const auto signature=file->signature();if(signature!=file->initial_signature())throw std::runtime_error("PDF changed before fingerprint request");
        const auto native=provisional_file_key(signature),path_text=utf8(path);p->state_keys[provisional]=native;
        p->native_keys[provisional]=native;
        auto known=store.fingerprint(path_text,signature);auto key=provisional;
        if(!known)if(auto alias=store.promoted_key(false,provisional);alias&&formal(*alias))known=alias->substr(7);
        if(known){key="sha256:"+*known;store.prepare_cache_promotion(false,provisional,key);store.promote_overrides(native,key);store.promote_cache(false,provisional,key);p->aliases[provisional]=key;p->state_keys[provisional]=key;p->native_keys[key]=native;}
        store.pin_document(key);
        IoEvent event;event.kind=IoEvent::Kind::DocumentLoaded;event.document=epoch;event.from_key=provisional;event.key=key;
        event.saved_document=store.document(known?key:native,native,true);event.saved_document->document_key=key;event.persistent=store.state_persistent();p->emit(std::move(event));
        if(!known){auto h=std::make_unique<Impl::Hash>();h->epoch=epoch;h->provisional=provisional;h->native_provisional=native;h->path=path_text;h->file=file;h->signature=signature;p->document_hash=std::move(h);}
    });
}
bool IoService::close_document(){
    impl_->desired_document=0;return impl_->enqueue("document",[p=impl_.get()](Store& store){p->document_hash.reset();store.pin_document("");});
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
    impl_->desired_model=epoch;
    return impl_->enqueue("model",[p=impl_.get(),epoch,provisional=std::move(provisional),path=std::move(path),file=std::move(file)](Store& store){
        p->model_hash.reset();if(epoch!=p->desired_model.load()||!file)return;
        const auto signature=file->signature();if(signature!=file->initial_signature())throw std::runtime_error("Model changed before fingerprint request");
        auto h=std::make_unique<Impl::Hash>();h->model=true;h->epoch=epoch;h->provisional=provisional;h->path=utf8(path);h->file=file;h->signature=signature;
        auto known=store.fingerprint(h->path,signature);if(!known)if(auto alias=store.promoted_key(true,provisional);alias&&formal(*alias))known=alias->substr(7);
        if(known)p->promote(store,*h,*known);else p->model_hash=std::move(h);
    });
}
bool IoService::settings(std::map<std::string,std::string> values){return impl_->enqueue("",[values=std::move(values)](Store& store){store.settings(values);});}
bool IoService::save_override(DocumentEpoch epoch,std::string provisional,std::string key,std::filesystem::path path,SavedOverride value){
    const auto coalesce="override:"+provisional+":"+std::to_string(value.page);
    return impl_->enqueue(coalesce,[p=impl_.get(),epoch,provisional=std::move(provisional),key=std::move(key),path=std::move(path),value=std::move(value)](Store& store){
        const auto resolved=p->resolve(key);const auto final=formal(resolved)?resolved:std::string{};
        store.save_override(provisional,final,utf8(path),value);if(!final.empty())store.promote_overrides(provisional,final);
        IoEvent e;e.kind=IoEvent::Kind::OverrideSaved;e.document=epoch;e.persistent=store.state_persistent();e.request=value.revision;p->emit(std::move(e));
    });
}
bool IoService::save_position(std::string key,std::filesystem::path path,ReadingPosition value){
    const auto coalesce="position:"+key+":"+std::to_string(value.page);
    return impl_->enqueue(coalesce,[p=impl_.get(),key=std::move(key),path=std::move(path),value](Store& store){auto resolved=p->resolve(key);if(!formal(resolved)){auto found=p->state_keys.find(key);if(found==p->state_keys.end())return;resolved=found->second;}store.save_position(resolved,utf8(path),value);});
}
bool IoService::save_attempt(CacheContext context,AttemptSnapshot data){const auto coalesce="attempt:"+data.id;
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
    return impl_->enqueue("",[p=impl_.get(),path=std::move(path),page,text=std::move(text),state,form](Store&){
        atomic_export(path,[&](std::ostream& out){export_header(out,form);out<<"# "<<page+1<<"ページ — "<<(state==PageState::Complete?"完了":"未完了・部分結果")<<"\n\n"<<text<<'\n';});
        IoEvent e;e.kind=IoEvent::Kind::Exported;e.message="訳文を書き出しました。";p->emit(std::move(e));});
}
bool IoService::export_complete(std::filesystem::path path,CacheContext context,Form form){
    return impl_->enqueue("",[p=impl_.get(),path=std::move(path),context=std::move(context),form](Store& store){
        const auto resolved=p->resolve(context);std::size_t count=0;
        const auto native=p->native_keys.find(resolved.document_key);
        atomic_export(path,[&](std::ostream& out){export_header(out,form);out<<"完成したページのみを収録しています。未完了ページは含みません。\n\n";count=store.for_each_complete_page(resolved.document_key,resolved.model_key,resolved.profile_digest,[&](PageIndex page,std::string_view text){out<<"# "<<page+1<<"ページ — 完了\n\n"<<text<<"\n\n";},native==p->native_keys.end()?std::string_view{}:std::string_view(native->second));if(!count)throw std::runtime_error("No completed pages to export");});
        IoEvent e;e.kind=IoEvent::Kind::Exported;e.message=std::to_string(count)+"ページの完成訳を書き出しました。";p->emit(std::move(e));});
}
bool IoService::clear_cache(){return impl_->enqueue("",[p=impl_.get()](Store& store){store.clear_cache();IoEvent e;e.kind=IoEvent::Kind::CacheCleared;e.persistent=store.cache_persistent();p->emit(std::move(e));});}
bool IoService::export_state(std::filesystem::path destination){
    return impl_->enqueue("",[p=impl_.get(),destination=std::move(destination)](Store& store){
        // A failed backup destination must not switch healthy active storage
        // to memory merely because the destination itself uses SQLite.
        try{store.export_state(destination);}catch(const StorageError& e){throw std::runtime_error(e.what());}
        IoEvent e;e.kind=IoEvent::Kind::Exported;e.message="設定・原文編集・読書位置の退避コピーを保存しました。現在の保存先は変わりません。";p->emit(std::move(e));});
}
bool IoService::diagnostic(DiagnosticRecord record){
    return impl_->enqueue("diagnostic:"+std::to_string(static_cast<unsigned>(record.code)),[p=impl_.get(),record](Store&){
        // Optional logs cannot make usable state appear unsaved. The GUI keeps
        // its own bounded recent-code summary if the log destination fails.
        try{append_diagnostic(p->state_directory/"logs",record);}catch(const std::exception&){}
    });
}
void IoService::stop()noexcept{impl_->stop();}
bool IoService::done()const noexcept{return impl_->finished.load();}
void IoService::join(){if(impl_->worker.joinable())impl_->worker.join();}
}
