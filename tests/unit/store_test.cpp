#include "storage/store.hpp"
#include <sqlite3.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace opt;
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
std::shared_ptr<PreparedChunk> prepare(const ChunkRequest& r){return std::make_shared<PreparedChunk>(ChunkPlanner::prepare(r,8192,[](std::string_view s,Form){PreparedInput p;p.tokens.assign(s.size(),17);p.source_tokens=s.size();p.output_limit=512;return p;},[](std::string_view s){return s.size();}));}
std::string file_text(const std::filesystem::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
std::string path_text(const std::filesystem::path& path){const auto s=path.u8string();return {reinterpret_cast<const char*>(s.data()),s.size()};}
int main(){const auto directory=std::filesystem::temp_directory_path()/std::filesystem::path("opt-store-"+new_uuid());
    try{
        const auto s=directory/"state",c=directory/"cache";const std::string provisional="provisional:file:original",doc="sha256:document",model="sha256:model";
        const auto source=std::make_shared<const std::string>("Original paragraph.");
        const std::vector<AttemptBlock> blocks{{"block",BlockKind::Paragraph,source,false}};
        CacheContext context{"provisional:session:document","provisional:session:model","profile","source",0};
        auto full=context;full.document_key=doc;full.model_key=model;
        std::string forced_id;
        std::shared_ptr<PreparedChunk> prepared;
        {
            Store store(s,c,1);require(store.state_persistent()&&store.cache_persistent(),"databases on disk");
            store.settings({{"form","plain"},{"model","日本語/model.gguf"}});
            {Store second(s,c);require(!second.state_persistent()&&!second.cache_persistent(),"second owner uses isolated memory instead of clearing active pins");second.settings({{"form","secondary"}});require(store.settings().at("form")=="plain","second owner cannot overwrite first owner state");}
            store.save_override(provisional,"","fixture.pdf",{0,3,std::string("old edit")});
            store.save_override(provisional,"","fixture.pdf",{0,5,std::string("latest edit")});
            store.save_override(provisional,"","fixture.pdf",{0,4,std::string("late old edit")});
            store.promote_overrides(provisional,doc);
            auto saved=store.document(doc,provisional);require(saved.overrides.size()==1&&saved.overrides[0].revision==5&&*saved.overrides[0].source=="latest edit","promotion keeps newest revision");
            store.save_override(provisional,doc,"fixture.pdf",{0,6,std::nullopt});store.promote_overrides(provisional,doc);
            require(store.document(doc,provisional).overrides.empty(),"reset tombstone deletes source override");
            FileSignature signature{"native-identity",123,456};store.fingerprint("日本語.pdf",signature,sha256("object"));
            require(store.fingerprint("日本語.pdf",signature)==sha256("object"),"matching native signature reuses hash");signature.identity="replaced";require(!store.fingerprint("日本語.pdf",signature),"same path replaced object misses fingerprint");
            store.save_position(doc,"日本語.pdf",{2,PdfPoint{3.5,4.25},8});require(store.recent_documents().at(0)=="日本語.pdf","native recent path persisted");
            PageAttempt attempt({1,0,1,1,1},context.condition_key(),blocks,Form::Plain);
            auto request=attempt.next_request();prepared=prepare(*request);attempt.prepared(prepared);attempt.finished(prepared->stamp,{FinishReason::Completed,"翻訳された文章。",4});
            store.save_attempt(context,attempt.snapshot());require(store.completed(context,*prepared)=="翻訳された文章。","completed chunk reused");
            std::size_t exported_count=store.for_each_complete_page(context.document_key,context.model_key,"profile",[](PageIndex,std::string_view text){require(text=="翻訳された文章。","export same completed attempt");});
            require(exported_count==1,"completed page exported incrementally");
            store.save_override("other-native",context.document_key,"fixture.pdf",{0,15,std::string("changed after generation")});
            require(store.for_each_complete_page(context.document_key,context.model_key,"profile",[](PageIndex,std::string_view){throw std::runtime_error("obsolete source must not export");})==0,"source change invalidates export even before retranslation starts");
            store.save_override("other-native",context.document_key,"fixture.pdf",{0,16,std::nullopt});store.promote_overrides("other-native",context.document_key);
            store.pin_document(context.document_key);store.maintain();require(store.attempt(context).has_value(),"active document pinned beyond byte limit");
            store.promote_cache(false,context.document_key,doc);store.promote_cache(true,context.model_key,model);
            require(!store.completed(context,*prepared)&&store.completed(full,*prepared)=="翻訳された文章。","both fingerprints atomically rekey completion and references");
            auto restored=store.attempt(full);require(restored&&restored->state==PageState::Complete&&restored->condition_key==full.condition_key(),"manifest follows fingerprint promotion");
            PageAttempt replay({2,0,2,1,1},full.condition_key(),blocks,Form::Plain,false,restored);require(replay.state()==PageState::Complete,"complete persisted attempt restores");
            PageAttempt forced({2,0,2,1,1},full.condition_key(),blocks,Form::Plain,true);forced_id=forced.snapshot().id;
            auto r=forced.next_request();auto p=prepare(*r);forced.prepared(p);forced.delta(p->stamp,"途中の文章");forced.interrupt();store.save_attempt(full,forced.snapshot());
            auto saved_force=store.attempt(full);require(saved_force&&saved_force->id==forced_id&&saved_force->bypass_cache,"new forced attempt is selected over old completion");
            PageAttempt resume({3,0,3,1,1},full.condition_key(),blocks,Form::Plain,false,saved_force);require(!resume.may_read_cache()&&resume.state()!=PageState::Complete&&resume.next_request(),"restart cannot fill forced attempt from prior cache");
            const auto exported=store.for_each_complete_page(doc,model,"profile",[](PageIndex,std::string_view){throw std::runtime_error("old completed attempt must not export");});require(exported==0,"new partial attempt supersedes old export");
        }
        {
            Store store(s,c,1);require(store.settings().at("model")=="日本語/model.gguf","settings survive restart");
            auto saved=store.document(doc,provisional);require(saved.position.page==2&&saved.position.pdf_anchor->x==3.5&&saved.position.text_anchor==8,"logical reading position survives restart");
            require(store.attempt(full)->id==forced_id,"forced manifest durable");
            require(!store.completed(full,*prepared,true)&&store.completed(full,*prepared).has_value(),"unfingerprinted global cache reuse is restricted to its originating session");
            store.pin_document("");store.maintain();require(!store.completed(full,*prepared)&&!store.attempt(full),"LRU deletes inactive completion with dependent manifests");
            store.save_override(provisional,doc,"fixture.pdf",{0,10,std::string("user source")});store.clear_cache();require(*store.document(doc,provisional).overrides[0].source=="user source","cache clear never removes user source");
            store.save_position(provisional,"unhashed.pdf",{4,PdfPoint{12,13},5});store.promote_overrides(provisional,doc);
            auto promoted=store.document(doc,provisional,true);require(promoted.position.page==4&&promoted.position.text_anchor==5,"reading positions promote with native file key");
        }
        // Future state schema is preserved byte-for-byte and runs in memory.
        sqlite3* db=nullptr;sqlite3_open(path_text(s/"state.sqlite3").c_str(),&db);sqlite3_exec(db,"PRAGMA user_version=999",nullptr,nullptr,nullptr);sqlite3_close(db);
        const auto original=file_text(s/"state.sqlite3");
        {Store store(s,c);require(!store.state_persistent(),"future state schema uses memory");store.settings({{"new","value"}});require(!store.warnings().empty(),"fallback warning available");}
        require(file_text(s/"state.sqlite3")==original,"future state database not overwritten");
        // Supported schema upgrades run inside a transaction, retaining state.
        sqlite3_open(path_text(s/"state.sqlite3").c_str(),&db);sqlite3_exec(db,"PRAGMA user_version=1",nullptr,nullptr,nullptr);sqlite3_close(db);
        {Store store(s,c);require(store.state_persistent()&&store.settings().at("form")=="plain","known schema upgrade retains settings");}
        {std::ofstream bad(c/"cache.sqlite3",std::ios::binary|std::ios::trunc);bad<<"not a database";}
        {Store store(s,c);require(store.cache_persistent(),"corrupt cache replaced after backup");}
        bool backup=false;for(const auto& entry:std::filesystem::directory_iterator(c))if(entry.path().filename().string().find(".corrupt-")!=std::string::npos)backup=true;
        require(backup,"corrupt cache backup retained");
        const auto blocked=directory/"not-a-directory";{std::ofstream out(blocked);out<<"preserve me";}
        {Store store(blocked,directory/"fallback-cache");require(!store.state_persistent(),"unwritable state destination falls back to memory");store.settings({{"session","usable"}});require(store.settings().at("session")=="usable","memory state remains usable");}
        require(file_text(blocked)=="preserve me","failed state destination is never replaced");
        // Simulate process loss after recording promotion intent, before either
        // state fingerprint write or cache rekey commit. The forced attempt must
        // remain the newest attempt, with bypass intact, after recovery.
        CacheContext provisional_context{"provisional:document:native-signature","provisional:model:native-signature","profile","source",0};
        auto formal_context=provisional_context;formal_context.document_key="sha256:crash-document";formal_context.model_key="sha256:crash-model";
        std::string crash_id;
        {Store store(directory/"crash-state",directory/"crash-cache");PageAttempt a({1,0,1,1,1},provisional_context.condition_key(),blocks,Form::Plain,true);crash_id=a.snapshot().id;store.save_attempt(provisional_context,a.snapshot());
            store.prepare_cache_promotion(false,provisional_context.document_key,formal_context.document_key);store.prepare_cache_promotion(true,provisional_context.model_key,formal_context.model_key);}
        {Store store(directory/"crash-state",directory/"crash-cache");auto saved=store.attempt(formal_context);require(saved&&saved->id==crash_id&&saved->bypass_cache,"interrupted cross-database promotion recovers forced attempt");
            require(store.promoted_key(true,provisional_context.model_key)==formal_context.model_key,"native signature alias survives missing state fingerprint commit");}
        std::filesystem::remove_all(directory);std::cout<<"durable state, canonical promotion, forced resume, pinning and recovery passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<" (fixtures at "<<directory<<")\n";return 1;}
}
