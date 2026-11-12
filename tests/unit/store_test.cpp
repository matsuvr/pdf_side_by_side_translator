#include "storage/store.hpp"
#include <sqlite3.h>
#include <iostream>
#include <stdexcept>

using namespace opt;
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
std::shared_ptr<PreparedChunk> prepare(const ChunkRequest& r){return std::make_shared<PreparedChunk>(ChunkPlanner::prepare(r,8192,[](std::string_view s,Form){PreparedInput p;p.tokens.assign(s.size(),17);p.source_tokens=s.size();p.output_limit=512;return p;}));}
namespace {
sqlite3_mem_methods allocator{};
int fail_at=0,allocations=0;
void* fault_malloc(int n){++allocations;if(fail_at>0&&allocations==fail_at)return nullptr;return allocator.xMalloc(n);}
void* fault_realloc(void* p,int n){++allocations;if(fail_at>0&&allocations==fail_at)return nullptr;return allocator.xRealloc(p,n);}
void constructor_failures(){
    require(sqlite3_shutdown()==SQLITE_OK,"shutdown before allocator injection");
    require(sqlite3_config(SQLITE_CONFIG_GETMALLOC,&allocator)==SQLITE_OK,"get SQLite allocator");
    auto injected=allocator;injected.xMalloc=fault_malloc;injected.xRealloc=fault_realloc;
    require(sqlite3_config(SQLITE_CONFIG_MALLOC,&injected)==SQLITE_OK,"set SQLite fault allocator");
    require(sqlite3_initialize()==SQLITE_OK,"initialize SQLite");
    allocations=0;{Store warm;}const auto constructor_allocations=allocations;
    const auto baseline=sqlite3_memory_used();int failures=0;
    // Exercise every allocation site reached by successful construction,
    // including second-database and later schema/pragma failures.
    for(int at=1;at<=constructor_allocations;++at){
        allocations=0;fail_at=at;try{Store failed;}catch(const std::exception&){++failures;}
        fail_at=0;
        require(sqlite3_memory_used()==baseline,"constructor failure leaked a SQLite allocation or handle");
    }
    require(failures>10,"allocation injection reached partial initialization failures");
}
}
int main(){try{
    require(sqlite3_compileoption_used("TEMP_STORE=3"),"SQLite must never spill temporary data to disk");
    constructor_failures();
    const auto source=std::make_shared<const std::string>("Original paragraph.");
    const std::string native="provisional:file:original",doc="sha256:document",model="sha256:model";
    CacheContext context{"provisional:session:document","provisional:session:model","profile","source",0};
    auto full=context;full.document_key=doc;full.model_key=model;
    std::shared_ptr<PreparedChunk> prepared;AttemptSnapshot complete;
    {
        Store store;require(!store.state_persistent()&&!store.cache_persistent(),"state and cache start in isolated memory");
        require(store.warnings().empty(),"session-only operation is normal");
        store.settings({{"form","plain"},{"model","日本語/model.gguf"}});
        {Store second;require(second.settings().empty(),"separate instances have isolated session state");}
        FileSignature signature{"native-identity",123,456};store.fingerprint("日本語.pdf",signature,sha256("object"));
        require(store.fingerprint("日本語.pdf",signature)==sha256("object"),"matching signature reuses a session fingerprint");
        signature.identity="replaced";require(!store.fingerprint("日本語.pdf",signature),"replacement file misses fingerprint");
        store.save_position(native,"日本語.pdf",{2,PdfPoint{3.5,4.25},8});store.promote_position(native,doc);
        require(store.document(doc).position==ReadingPosition{2,PdfPoint{3.5,4.25},8},"reading position survives fingerprint promotion");
        require(store.document(native).position.page==0,"provisional reading position retired");
        PageAttempt attempt({1,0,1,1,1},context.condition_key(),source,Form::Plain);
        auto request=attempt.next_request();prepared=prepare(*request);attempt.prepared(prepared);attempt.finished(prepared->stamp,{FinishReason::Completed,"翻訳された文章。",4});
        complete=attempt.snapshot();store.save_attempt(context,complete);
        require(store.completed(context,*prepared)=="翻訳された文章。","completed page is reused within session");
        auto invalid=complete;invalid.chunks.front().text=std::string("prefix\0hidden",13);bool rejected=false;
        try{store.save_attempt(context,invalid);}catch(const std::invalid_argument&){rejected=true;}
        require(rejected&&store.completed(context,*prepared)=="翻訳された文章。","invalid text cannot replace valid completion");
        store.promote_cache(false,context.document_key,doc);store.promote_cache(true,context.model_key,model);
        require(!store.completed(context,*prepared)&&store.completed(full,*prepared)=="翻訳された文章。","fingerprints rekey completion and manifest references");
        auto restored=store.attempt(full);require(restored&&restored->state==PageState::Complete,"complete session attempt restores");
        store.clear_cache();require(!store.attempt(full)&&store.document(doc).position.text_anchor==8,"cache maintenance preserves reading position");
    }
    {
        Store restarted;require(restarted.settings().empty()&&restarted.document(doc).position.page==0&&!restarted.attempt(full),"settings, positions and translations do not survive restart");
        require(!restarted.fingerprint("日本語.pdf",{"native-identity",123,456}),"fingerprints do not survive restart");
    }
    {
        Store store;auto older=complete;older.condition_key=full.condition_key();store.save_attempt(full,older);
        auto newer=complete;newer.id=new_uuid();newer.chunks.front().text="新しいセッションの翻訳結果";store.save_attempt(context,newer);
        store.promote_cache(false,context.document_key,doc);store.promote_cache(true,context.model_key,model);
        auto promoted=store.attempt(full);require(promoted&&promoted->id==newer.id&&promoted->chunks.front().text=="新しいセッションの翻訳結果","promotion collision retains latest result");
        require(store.cache_usage().attempts==1&&store.cache_usage().chunks==1,"promotion removes duplicate payloads");
    }
    {
        constexpr std::uint64_t budget=64*1024;Store store(budget);
        auto data=complete;data.chunks.front().text=std::string(8192,'x');
        for(int i=0;i<256;++i){store.save_attempt(context,data);auto usage=store.cache_usage();require(usage.bytes<=budget&&usage.attempts==1&&usage.chunks==1,"snapshot updates do not duplicate manifests or translated payloads");}
        for(PageIndex page=1;page<128;++page){auto next=context;next.page=page;data.id=new_uuid();data.condition_key=next.condition_key();store.save_attempt(next,data);require(store.cache_usage().bytes<=budget,"active document obeys allocation budget");}
        require(store.cache_usage().attempts<16,"old active-document pages are evicted under pressure");
        data.condition_key=context.condition_key();data.chunks.front().text=std::string(2*1024*1024,'x');store.save_attempt(context,data);
        require(!store.attempt(context)&&store.cache_usage().bytes<=budget,"oversized single attempt is not admitted");
    }
    std::cout<<"memory-only viewer state, bounded cache, reading positions and constructor failure cleanup passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
