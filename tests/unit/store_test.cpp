#include "storage/store.hpp"
#include <sqlite3.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace opt;
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
std::shared_ptr<PreparedChunk> prepare(const ChunkRequest& r){return std::make_shared<PreparedChunk>(ChunkPlanner::prepare(r,8192,[](std::string_view s,Form){PreparedInput p;p.tokens.assign(s.size(),17);p.source_tokens=s.size();p.output_limit=512;return p;}));}
std::string bytes(const std::filesystem::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
std::string path_text(const std::filesystem::path& path){const auto s=path.u8string();return {reinterpret_cast<const char*>(s.data()),s.size()};}
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
int main(){const auto directory=std::filesystem::temp_directory_path()/std::filesystem::path("opt-store-"+new_uuid());
    try{
        constructor_failures();
        std::filesystem::create_directories(directory);
        const auto source=std::make_shared<const std::string>("Original paragraph.");
        const std::string native="provisional:file:original",doc="sha256:document",model="sha256:model";
        CacheContext context{"provisional:session:document","provisional:session:model","profile","source",0};
        auto full=context;full.document_key=doc;full.model_key=model;
        std::shared_ptr<PreparedChunk> prepared;AttemptSnapshot complete;
        {
            Store store;require(!store.state_persistent()&&!store.cache_persistent(),"state and cache start in isolated memory");
            require(store.warnings().empty(),"session-only operation is normal, not a failure");
            store.settings({{"form","plain"},{"model","日本語/model.gguf"}});
            {Store second;require(second.settings().empty(),"separate instances have isolated session state");}
            store.save_override(native,"","fixture.pdf",{0,3,std::string("old edit")});
            store.save_override(native,"","fixture.pdf",{0,5,std::string("latest edit")});
            store.save_override(native,"","fixture.pdf",{0,4,std::string("late old edit")});
            store.promote_overrides(native,doc);
            auto saved=store.document(doc,native);require(saved.overrides.size()==1&&saved.overrides[0].revision==5&&*saved.overrides[0].source=="latest edit","promotion keeps newest edit");
            store.save_override(native,doc,"fixture.pdf",{0,6,std::nullopt});store.promote_overrides(native,doc);
            require(store.document(doc,native).overrides.empty(),"reset tombstone removes an earlier source override");
            FileSignature signature{"native-identity",123,456};store.fingerprint("日本語.pdf",signature,sha256("object"));
            require(store.fingerprint("日本語.pdf",signature)==sha256("object"),"matching signature reuses a session fingerprint");
            signature.identity="replaced";require(!store.fingerprint("日本語.pdf",signature),"replacement file misses fingerprint");
            store.save_position(doc,"日本語.pdf",{2,PdfPoint{3.5,4.25},8});
            require(store.document(doc,native).position.text_anchor==8,"logical position is available during session");
            PageAttempt attempt({1,0,1,1,1},context.condition_key(),source,Form::Plain);
            auto request=attempt.next_request();prepared=prepare(*request);attempt.prepared(prepared);attempt.finished(prepared->stamp,{FinishReason::Completed,"翻訳された文章。",4});
            complete=attempt.snapshot();store.save_attempt(context,complete);
            require(store.completed(context,*prepared)=="翻訳された文章。","completed chunk is reused within session");
            auto invalid=complete;invalid.chunks.front().text=std::string("prefix\0hidden",13);bool rejected=false;
            try{store.save_attempt(context,invalid);}catch(const std::invalid_argument&){rejected=true;}
            require(rejected&&store.completed(context,*prepared)=="翻訳された文章。","invalid completed text is rejected without replacing valid cache data");
            require(store.for_each_complete_page(context.document_key,context.model_key,"profile",[](PageIndex,std::string_view text){require(text=="翻訳された文章。","export uses matching completed attempt");})==1,"complete page exports incrementally");
            store.save_override("other-native",context.document_key,"fixture.pdf",{0,15,std::string("changed after generation")});
            require(store.for_each_complete_page(context.document_key,context.model_key,"profile",[](PageIndex,std::string_view){throw std::runtime_error("obsolete source exported");})==0,"source change invalidates export before retranslation");
            store.save_override("other-native",context.document_key,"fixture.pdf",{0,16,std::nullopt});store.promote_overrides("other-native",context.document_key);
            store.promote_cache(false,context.document_key,doc);store.promote_cache(true,context.model_key,model);
            require(!store.completed(context,*prepared)&&store.completed(full,*prepared)=="翻訳された文章。","fingerprints rekey completion and manifest references");
            auto restored=store.attempt(full);require(restored&&restored->state==PageState::Complete,"complete session attempt restores");
            PageAttempt forced({2,0,2,1,1},full.condition_key(),source,Form::Plain,true);
            auto r=forced.next_request();auto p=prepare(*r);forced.prepared(p);forced.delta(p->stamp,"途中の文章");forced.interrupt();store.save_attempt(full,forced.snapshot());
            auto saved_force=store.attempt(full);require(saved_force&&saved_force->id==forced.snapshot().id&&saved_force->bypass_cache,"forced attempt supersedes completion");
            require(store.cache_usage().attempts==1&&store.cache_usage().chunks==0,"superseded manifests and orphan translations are removed");
            require(store.for_each_complete_page(doc,model,"profile",[](PageIndex,std::string_view){throw std::runtime_error("superseded completed attempt exported");})==0,"partial attempt suppresses old export");
            store.save_override(native,doc,"fixture.pdf",{0,20,std::string("session edit")});
            store.clear_cache();require(*store.document(doc,native).overrides[0].source=="session edit","cache clear does not discard user edits");
            store.export_state(directory/"explicit.sqlite3");
            int checks=0;bool replacement_rejected=false;
            try{store.export_state(directory/"explicit.sqlite3",[&]{if(++checks==2)throw std::runtime_error("destination became an input");});}catch(const std::runtime_error&){replacement_rejected=true;}
            require(checks==2&&replacement_rejected,"state export rechecks destination immediately before publication");
        }
        {
            Store restarted;require(restarted.settings().empty()&&restarted.document(doc,native).overrides.empty()&&!restarted.attempt(full),"settings, source and translations do not survive restart");
            require(!restarted.fingerprint("日本語.pdf",{"native-identity",123,456}),"fingerprints do not survive restart");
        }
        {
            Store store;auto older=complete;older.condition_key=full.condition_key();store.save_attempt(full,older);
            auto newer=complete;newer.id=new_uuid();newer.bypass_cache=true;newer.chunks.front().text="新しい強制翻訳の結果";store.save_attempt(context,newer);
            store.promote_cache(false,context.document_key,doc);store.promote_cache(true,context.model_key,model);
            auto promoted=store.attempt(full);require(promoted&&promoted->id==newer.id&&promoted->bypass_cache&&promoted->chunks.front().text=="新しい強制翻訳の結果","promotion collision retains latest forced output");
            require(store.cache_usage().attempts==1&&store.cache_usage().chunks==1,"promotion removes superseded manifests and duplicate payloads");
        }
        sqlite3* exported=nullptr;require(sqlite3_open_v2(path_text(directory/"explicit.sqlite3").c_str(),&exported,SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK,"explicit state export opens");
        sqlite3_stmt* q=nullptr;require(sqlite3_prepare_v2(exported,"SELECT source_utf8 FROM source_overrides WHERE document_hash='sha256:document' AND page_index=0",-1,&q,nullptr)==SQLITE_OK,"query explicit state export");
        require(sqlite3_step(q)==SQLITE_ROW&&std::string(reinterpret_cast<const char*>(sqlite3_column_text(q,0)))=="session edit","explicit export contains requested session edit");
        sqlite3_finalize(q);sqlite3_close(exported);
        // Repeated forced attempts for the active document used to escape the
        // cache limit. Real database pages and both kinds of rows are bounded.
        {
            constexpr std::uint64_t budget=64*1024;Store store(budget);
            auto data=complete;data.bypass_cache=true;data.chunks.front().text=std::string(8192,'x');
            for(int i=0;i<256;++i){data.id=new_uuid();store.save_attempt(context,data);auto usage=store.cache_usage();require(usage.bytes<=budget&&usage.attempts==1&&usage.chunks==1,"retranslation accumulates neither manifests nor translated payloads");}
            for(PageIndex page=1;page<128;++page){auto next=context;next.page=page;data.id=new_uuid();data.condition_key=next.condition_key();store.save_attempt(next,data);require(store.cache_usage().bytes<=budget,"active document obeys allocation budget during insertion");}
            require(store.cache_usage().attempts<16,"old active-document pages are evicted under pressure");
            data.condition_key=context.condition_key();data.chunks.front().text=std::string(2*1024*1024,'x');store.save_attempt(context,data);
            require(!store.attempt(context)&&store.cache_usage().bytes<=budget,"oversized single attempt is not admitted or allowed to resurrect old data");
            const std::string edit(128*1024,'e');store.save_override(native,doc,"fixture.pdf",{0,1,edit});store.clear_cache();store.maintain();
            require(*store.document(doc,native).overrides[0].source==edit,"user edits are independent of cache budget");
        }
        // There is no startup path that consults, migrates, deletes or appends
        // to earlier persistent stores. Only explicit exports exist here.
        require(std::distance(std::filesystem::directory_iterator(directory),std::filesystem::directory_iterator{})==1,"no automatic DB, WAL, lock, log or temporary file remains");
        std::filesystem::remove_all(directory);
        std::cout<<"memory-only state, bounded active cache, revision preservation, atomic export and constructor failure cleanup passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<" (fixtures at "<<directory<<")\n";return 1;}
}
