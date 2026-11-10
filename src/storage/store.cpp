#include "storage/store.hpp"
#include "platform/access_path.hpp"
#include "core/utf8.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <set>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace opt {
namespace {
std::string utf8(const std::filesystem::path& p){const auto s=p.u8string();return {reinterpret_cast<const char*>(s.data()),s.size()};}
std::filesystem::path native(std::string_view s){return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()),s.size()));}
std::int64_t now(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
void check(int code,sqlite3* db){if(code!=SQLITE_OK&&code!=SQLITE_ROW&&code!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db));}
struct Statement {
    sqlite3* db;sqlite3_stmt* stmt=nullptr;
    Statement(sqlite3* d,const char* sql):db(d){check(sqlite3_prepare_v2(db,sql,-1,&stmt,nullptr),db);}
    ~Statement(){sqlite3_finalize(stmt);}
    Statement& text(int at,std::string_view value){check(sqlite3_bind_text64(stmt,at,value.empty()?"":value.data(),value.size(),SQLITE_TRANSIENT,SQLITE_UTF8),db);return *this;}
    Statement& number(int at,std::int64_t value){check(sqlite3_bind_int64(stmt,at,value),db);return *this;}
    Statement& real(int at,double value){check(sqlite3_bind_double(stmt,at,value),db);return *this;}
    Statement& null(int at){check(sqlite3_bind_null(stmt,at),db);return *this;}
    bool step(){const auto result=sqlite3_step(stmt);check(result,db);return result==SQLITE_ROW;}
    std::string text(int at)const{auto s=sqlite3_column_text(stmt,at);return s?std::string(reinterpret_cast<const char*>(s),sqlite3_column_bytes(stmt,at)):std::string{};}
    std::int64_t number(int at)const{return sqlite3_column_int64(stmt,at);}
    double real(int at)const{return sqlite3_column_double(stmt,at);}
    bool is_null(int at)const{return sqlite3_column_type(stmt,at)==SQLITE_NULL;}
};
void sql(sqlite3* db,const char* text){char* error=nullptr;auto code=sqlite3_exec(db,text,nullptr,nullptr,&error);std::string message=error?error:"SQLite failure";sqlite3_free(error);if(code!=SQLITE_OK)throw std::runtime_error(message);}
struct Transaction {sqlite3* db;bool committed=false;explicit Transaction(sqlite3* d):db(d){sql(db,"BEGIN IMMEDIATE");}~Transaction(){if(!committed)sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);}void commit(){sql(db,"COMMIT");committed=true;}};
constexpr std::array leaf_names{"pending","planning","running","complete","source-only","partial","failed","replaced"};
constexpr std::array page_names{"not-started","waiting-text","queued","running","complete","partial","failed","no-text"};
template<class Enum,class Array> Enum parse(std::string_view name,const Array& names){for(std::size_t i=0;i<names.size();++i)if(name==names[i])return static_cast<Enum>(i);throw std::runtime_error("Unknown persisted status");}
std::string profile_key(const CacheContext& c){return CanonicalKey{}.text("model",c.model_key).text("profile",c.profile_digest).finish();}
std::string semantic_key(const AttemptChunk& c){return CanonicalKey{}.text("plan",c.plan_digest).number("actual-output-limit",c.output_limit).finish();}
bool valid_hash(std::string_view text){return text.size()==64&&std::all_of(text.begin(),text.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
// One disk writer owns cache pins. A second process gets an isolated in-memory
// Store instead of clearing the first process's pins or replacing its cache.
class StoreLease {
#ifdef _WIN32
    HANDLE file_=INVALID_HANDLE_VALUE;
#else
    int file_=-1;
#endif
public:
    bool acquire(const std::filesystem::path& directory){
        std::error_code error;std::filesystem::create_directories(access_path(directory),error);if(error)return false;
        const auto path=access_path(directory/"storage.lock");
#ifdef _WIN32
        file_=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(file_==INVALID_HANDLE_VALUE)return false;
        FILE_ATTRIBUTE_TAG_INFO info{};
        if(!GetFileInformationByHandleEx(file_,FileAttributeTagInfo,&info,sizeof(info))||(info.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){CloseHandle(file_);file_=INVALID_HANDLE_VALUE;return false;}
#else
        file_=::open(path.c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);if(file_<0)return false;
        struct stat info{};
        if(fstat(file_,&info)!=0||!S_ISREG(info.st_mode)||flock(file_,LOCK_EX|LOCK_NB)!=0){::close(file_);file_=-1;return false;}
#endif
        return true;
    }
    ~StoreLease(){
#ifdef _WIN32
        if(file_!=INVALID_HANDLE_VALUE)CloseHandle(file_);
#else
        if(file_>=0)::close(file_);
#endif
    }
};
constexpr const char* state_schema=R"SQL(
CREATE TABLE app_settings(key TEXT PRIMARY KEY,value TEXT NOT NULL);
CREATE TABLE document_state(document_hash TEXT PRIMARY KEY,last_path TEXT NOT NULL,page_index INTEGER NOT NULL,modified_at INTEGER NOT NULL);
CREATE TABLE page_state(document_hash TEXT NOT NULL,page_index INTEGER NOT NULL,pdf_anchor_x REAL,pdf_anchor_y REAL,text_anchor INTEGER,PRIMARY KEY(document_hash,page_index));
CREATE TABLE source_overrides(document_hash TEXT NOT NULL,page_index INTEGER NOT NULL,source_utf8 TEXT NOT NULL,source_revision INTEGER NOT NULL,modified_at INTEGER NOT NULL,PRIMARY KEY(document_hash,page_index));
CREATE TABLE file_fingerprints(path_key TEXT PRIMARY KEY,file_identity TEXT NOT NULL,file_size INTEGER NOT NULL,mtime_ns INTEGER NOT NULL,content_hash TEXT NOT NULL);
CREATE TABLE pending_source_overrides(provisional_key TEXT NOT NULL,page_index INTEGER NOT NULL,last_path TEXT NOT NULL,source_utf8 TEXT,use_auto_source INTEGER NOT NULL,source_revision INTEGER NOT NULL,modified_at INTEGER NOT NULL,PRIMARY KEY(provisional_key,page_index));
PRAGMA user_version=5;
)SQL";
constexpr const char* cache_schema=R"SQL(
CREATE TABLE completed_chunks(cache_key TEXT PRIMARY KEY,document_hash TEXT NOT NULL,page_index INTEGER NOT NULL,profile_key TEXT NOT NULL,block_id TEXT NOT NULL,chunk_algorithm_version INTEGER NOT NULL,chunk_index INTEGER NOT NULL,source_digest TEXT NOT NULL,prompt_ids_digest TEXT NOT NULL,text_utf8 TEXT NOT NULL,byte_size INTEGER NOT NULL,created_at INTEGER NOT NULL,last_used_at INTEGER NOT NULL,model_key TEXT NOT NULL,profile_digest TEXT NOT NULL,page_source_digest TEXT NOT NULL,semantic_key TEXT NOT NULL,session_key TEXT NOT NULL);
CREATE INDEX completed_chunks_document ON completed_chunks(document_hash,page_index,profile_key);
CREATE INDEX completed_chunks_lru ON completed_chunks(last_used_at);
CREATE TABLE page_attempts(attempt_id TEXT PRIMARY KEY,document_key TEXT NOT NULL,page_index INTEGER NOT NULL,profile_key TEXT NOT NULL,source_digest TEXT NOT NULL,force_retranslate INTEGER NOT NULL,plan_complete INTEGER NOT NULL,status TEXT NOT NULL,modified_at INTEGER NOT NULL,model_key TEXT NOT NULL,profile_digest TEXT NOT NULL,condition_key TEXT NOT NULL,plan_generation INTEGER NOT NULL,override_digest TEXT NOT NULL);
CREATE INDEX attempts_condition ON page_attempts(condition_key,modified_at);
CREATE TABLE attempt_chunks(attempt_id TEXT NOT NULL,block_id TEXT NOT NULL,chunk_id TEXT NOT NULL,source_begin INTEGER NOT NULL,source_end INTEGER NOT NULL,parent_chunk_id TEXT,status TEXT NOT NULL,cache_key TEXT,ordinal INTEGER NOT NULL,source_digest TEXT NOT NULL,prompt_digest TEXT NOT NULL,plan_digest TEXT NOT NULL,source_text TEXT NOT NULL,target_tokens INTEGER NOT NULL,source_tokens INTEGER NOT NULL,prompt_tokens INTEGER NOT NULL,output_limit INTEGER NOT NULL,output_tokens INTEGER NOT NULL,plan_generation INTEGER NOT NULL,reason INTEGER NOT NULL,split_retry INTEGER NOT NULL,block_kind INTEGER NOT NULL,PRIMARY KEY(attempt_id,block_id,chunk_id));
CREATE TABLE session_completed_chunks(session_key TEXT NOT NULL,attempt_id TEXT NOT NULL,chunk_id TEXT NOT NULL,source_digest TEXT NOT NULL,prompt_ids_digest TEXT NOT NULL,text_utf8 TEXT NOT NULL,PRIMARY KEY(session_key,attempt_id,chunk_id));
CREATE TABLE active_documents(document_key TEXT PRIMARY KEY);
CREATE TABLE fingerprint_promotions(is_model INTEGER NOT NULL,from_key TEXT NOT NULL,to_key TEXT NOT NULL,applied INTEGER NOT NULL,PRIMARY KEY(is_model,from_key));
PRAGMA user_version=5;
)SQL";
}
struct Store::Impl {
    sqlite3 *state=nullptr,*cache=nullptr;
    bool state_disk=false,cache_disk=false;
    std::uint64_t limit;
    std::vector<std::string> messages;
    std::thread::id owner=std::this_thread::get_id();
    std::string pinned;
    std::string session=new_uuid();
    StoreLease lease;
    bool memory_only=false;
    std::int64_t state_clock=0,cache_clock=0;
    static std::int64_t tick(std::int64_t& clock){
        if(clock==std::numeric_limits<std::int64_t>::max())throw std::runtime_error("Persisted update sequence is exhausted");
        return clock=std::max(now(),clock+1);
    }
    std::int64_t state_time(){return tick(state_clock);}
    std::int64_t cache_time(){return tick(cache_clock);}
    void owns()const{if(owner!=std::this_thread::get_id())throw std::logic_error("SQLite owner thread mismatch");}
    sqlite3* open(const std::filesystem::path& directory,const char* filename,const char* schema,bool disposable,bool& persistent){
        sqlite3* db=nullptr;const auto file=access_path(directory/filename);bool future=false;
        if(memory_only){check(sqlite3_open_v2(":memory:",&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_NOMUTEX,nullptr),db);sql(db,schema);return db;}
        try{
            std::filesystem::create_directories(access_path(directory));
            check(sqlite3_open_v2(utf8(file).c_str(),&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_NOMUTEX,nullptr),db);
            sqlite3_busy_timeout(db,1000);
            std::int64_t version=0;{Statement s(db,"PRAGMA user_version");if(s.step())version=s.number(0);}
            if(version>5){future=true;throw std::runtime_error("Future database schema; preserving original file");}
            {Statement s(db,"PRAGMA quick_check(1)");if(!s.step()||s.text(0)!="ok")throw std::runtime_error("Database integrity check failed");}
            if(!version){sql(db,"PRAGMA auto_vacuum=INCREMENTAL");Transaction transaction(db);sql(db,schema);transaction.commit();}
            else if(version<5){
                Transaction transaction(db);
                if(std::string_view(filename)=="cache.sqlite3"){
                    bool column=false;{Statement q(db,"PRAGMA table_info(page_attempts)");while(q.step())if(q.text(1)=="override_digest")column=true;}
                    if(!column)sql(db,"ALTER TABLE page_attempts ADD COLUMN override_digest TEXT NOT NULL DEFAULT ''");
                    sql(db,"CREATE TABLE IF NOT EXISTS fingerprint_promotions(is_model INTEGER NOT NULL,from_key TEXT NOT NULL,to_key TEXT NOT NULL,applied INTEGER NOT NULL,PRIMARY KEY(is_model,from_key))");
                    bool applied=false;{Statement q(db,"PRAGMA table_info(fingerprint_promotions)");while(q.step())if(q.text(1)=="applied")applied=true;}
                    if(!applied)sql(db,"ALTER TABLE fingerprint_promotions ADD COLUMN applied INTEGER NOT NULL DEFAULT 0");
                    bool session_column=false;{Statement q(db,"PRAGMA table_info(completed_chunks)");while(q.step())if(q.text(1)=="session_key")session_column=true;}
                    if(!session_column)sql(db,"ALTER TABLE completed_chunks ADD COLUMN session_key TEXT NOT NULL DEFAULT ''");
                }
                sql(db,"PRAGMA user_version=5");transaction.commit();
            }
            sql(db,"PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON");
            persistent=true;return db;
        }catch(const std::exception& error){
            if(db){sqlite3_close(db);db=nullptr;}
            messages.push_back(std::string(filename)+": "+error.what());
            // Only a structurally corrupt cache is replaceable. Permission,
            // locking, full disk and future schema errors preserve everything.
            std::error_code exists_error;
            if(disposable&&!future&&std::filesystem::exists(file,exists_error)){
                sqlite3* probe=nullptr;bool corrupt=false;
                if(sqlite3_open_v2(utf8(file).c_str(),&probe,SQLITE_OPEN_READONLY|SQLITE_OPEN_NOMUTEX,nullptr)==SQLITE_OK){
                    try{Statement q(probe,"PRAGMA quick_check(1)");corrupt=!q.step()||q.text(0)!="ok";}catch(...){corrupt=sqlite3_errcode(probe)==SQLITE_CORRUPT||sqlite3_errcode(probe)==SQLITE_NOTADB;}
                }if(probe)sqlite3_close(probe);
                if(corrupt){
                    std::error_code ec;const auto backup=utf8(file)+".corrupt-"+new_uuid();
                    std::filesystem::rename(file,native(backup),ec);
                    if(!ec){
                        for(auto suffix:{"-wal","-shm"}){std::error_code ignored;std::filesystem::rename(native(utf8(file)+suffix),native(backup+suffix),ignored);}
                        return open(directory,filename,schema,false,persistent);
                    }
                }
            }
            check(sqlite3_open_v2(":memory:",&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_NOMUTEX,nullptr),db);sql(db,schema);return db;
        }
    }
    Impl(std::filesystem::path s,std::filesystem::path c,std::uint64_t bytes):limit(bytes){
        memory_only=!lease.acquire(s);
        if(memory_only)messages.push_back("Storage directory is unavailable or another app instance owns it; this instance uses isolated memory state and cache");
        state=open(s,"state.sqlite3",state_schema,false,state_disk);cache=open(c,"cache.sqlite3",cache_schema,true,cache_disk);
        // An earlier session may have run with a clock ahead of this one.
        // Sequence persisted changes above that high-water mark so a new forced
        // attempt always supersedes an older completed attempt after restart.
        {Statement q(state,"SELECT MAX(t) FROM(SELECT modified_at AS t FROM document_state UNION ALL SELECT modified_at FROM source_overrides UNION ALL SELECT modified_at FROM pending_source_overrides)");if(q.step())state_clock=q.number(0);}
        {Statement q(cache,"SELECT MAX(t) FROM(SELECT modified_at AS t FROM page_attempts UNION ALL SELECT created_at FROM completed_chunks UNION ALL SELECT last_used_at FROM completed_chunks)");if(q.step())cache_clock=q.number(0);}
        sql(cache,"DELETE FROM active_documents");
    }
    ~Impl(){if(cache)sqlite3_close(cache);if(state)sqlite3_close(state);}
};
Store::Store(std::filesystem::path s,std::filesystem::path c,std::uint64_t limit):impl_(std::make_unique<Impl>(std::move(s),std::move(c),limit)){
    // A crash can occur between commits in the two independent databases.
    // Replay the durable cache-side intent before exposing any saved attempt.
    struct Pending {bool model;std::string from,to;};std::vector<Pending> pending;
    {Statement q(impl_->cache,"SELECT is_model,from_key,to_key FROM fingerprint_promotions WHERE applied=0");while(q.step())pending.push_back({bool(q.number(0)),q.text(1),q.text(2)});}
    for(const auto& p:pending)promote_cache(p.model,p.from,p.to);
}
Store::~Store()=default;
std::vector<std::string> Store::warnings()const{return impl_->messages;}
bool Store::state_persistent()const{return impl_->state_disk;}
bool Store::cache_persistent()const{return impl_->cache_disk;}
std::map<std::string,std::string> Store::settings(){impl_->owns();std::map<std::string,std::string> values;Statement q(impl_->state,"SELECT key,value FROM app_settings");while(q.step())values.emplace(q.text(0),q.text(1));return values;}
void Store::settings(const std::map<std::string,std::string>& values){impl_->owns();Transaction tx(impl_->state);for(const auto& [k,v]:values){Statement q(impl_->state,"INSERT INTO app_settings VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");q.text(1,k).text(2,v).step();}tx.commit();}
std::optional<std::string> Store::fingerprint(std::string_view path,const FileSignature& s){impl_->owns();Statement q(impl_->state,"SELECT content_hash FROM file_fingerprints WHERE path_key=? AND file_identity=? AND file_size=? AND mtime_ns=?");q.text(1,path).text(2,s.identity).number(3,s.size).number(4,s.mtime_ns);if(q.step()){auto hash=q.text(0);if(valid_hash(hash))return hash;}return {};}
void Store::fingerprint(std::string_view path,const FileSignature& s,std::string_view hash){impl_->owns();if(!valid_hash(hash))throw std::invalid_argument("Invalid content hash");Statement q(impl_->state,"INSERT INTO file_fingerprints VALUES(?,?,?,?,?) ON CONFLICT(path_key) DO UPDATE SET file_identity=excluded.file_identity,file_size=excluded.file_size,mtime_ns=excluded.mtime_ns,content_hash=excluded.content_hash");q.text(1,path).text(2,s.identity).number(3,s.size).number(4,s.mtime_ns).text(5,hash).step();}
SavedDocument Store::document(std::string key,std::string_view provisional,bool only_reading_page,std::optional<PageIndex> page){
    impl_->owns();SavedDocument result;result.document_key=std::move(key);
    {Statement q(impl_->state,"SELECT page_index FROM document_state WHERE document_hash=?");q.text(1,result.document_key);if(q.step())result.position.page=static_cast<PageIndex>(std::max<std::int64_t>(0,q.number(0)));}
    if(page)result.position.page=*page;
    {Statement q(impl_->state,"SELECT pdf_anchor_x,pdf_anchor_y,text_anchor FROM page_state WHERE document_hash=? AND page_index=?");q.text(1,result.document_key).number(2,result.position.page);if(q.step()){if(!q.is_null(0)&&!q.is_null(1))result.position.pdf_anchor=PdfPoint{q.real(0),q.real(1)};result.position.text_anchor=std::max<std::int64_t>(0,q.number(2));}}
    std::map<PageIndex,SavedOverride> values;
    {Statement q(impl_->state,"SELECT page_index,source_revision,source_utf8 FROM source_overrides WHERE document_hash=? AND (?=0 OR page_index=?)");q.text(1,result.document_key).number(2,only_reading_page).number(3,result.position.page);while(q.step()){SavedOverride v{static_cast<PageIndex>(q.number(0)),static_cast<SourceRevision>(q.number(1)),q.text(2)};values[v.page]=std::move(v);}}
    {Statement q(impl_->state,"SELECT page_index,source_revision,source_utf8,use_auto_source FROM pending_source_overrides WHERE provisional_key=? AND (?=0 OR page_index=?)");q.text(1,provisional).number(2,only_reading_page).number(3,result.position.page);while(q.step()){SavedOverride v{static_cast<PageIndex>(q.number(0)),static_cast<SourceRevision>(q.number(1)),q.number(3)?std::nullopt:std::optional(q.text(2))};if(!values.contains(v.page)||values[v.page].revision<=v.revision)values[v.page]=std::move(v);}}
    for(auto& [page,v]:values)result.overrides.push_back(std::move(v));return result;
}
void Store::save_override(std::string_view provisional,std::string_view key,std::string_view path,const SavedOverride& v){
    impl_->owns();if(v.source)utf8_scalars(*v.source);Transaction tx(impl_->state);
    {Statement q(impl_->state,"INSERT INTO pending_source_overrides VALUES(?,?,?,?,?,?,?) ON CONFLICT(provisional_key,page_index) DO UPDATE SET last_path=excluded.last_path,source_utf8=excluded.source_utf8,use_auto_source=excluded.use_auto_source,source_revision=excluded.source_revision,modified_at=excluded.modified_at WHERE excluded.source_revision>=pending_source_overrides.source_revision");q.text(1,provisional).number(2,v.page).text(3,path);if(v.source)q.text(4,*v.source);else q.null(4);q.number(5,!v.source).number(6,v.revision).number(7,impl_->state_time()).step();}
    if(!key.empty()){
        if(v.source){Statement q(impl_->state,"INSERT INTO source_overrides VALUES(?,?,?,?,?) ON CONFLICT(document_hash,page_index) DO UPDATE SET source_utf8=excluded.source_utf8,source_revision=excluded.source_revision,modified_at=excluded.modified_at WHERE excluded.source_revision>=source_overrides.source_revision");q.text(1,key).number(2,v.page).text(3,*v.source).number(4,v.revision).number(5,impl_->state_time()).step();}
        else{Statement q(impl_->state,"DELETE FROM source_overrides WHERE document_hash=? AND page_index=? AND source_revision<=?");q.text(1,key).number(2,v.page).number(3,v.revision).step();}
    }tx.commit();
}
void Store::promote_overrides(std::string_view provisional,std::string_view hash){
    impl_->owns();Transaction tx(impl_->state);
    std::vector<SavedOverride> values;
    {Statement q(impl_->state,"SELECT page_index,source_revision,source_utf8,use_auto_source FROM pending_source_overrides WHERE provisional_key=?");q.text(1,provisional);while(q.step())values.push_back({static_cast<PageIndex>(q.number(0)),static_cast<SourceRevision>(q.number(1)),q.number(3)?std::nullopt:std::optional(q.text(2))});}
    for(const auto& v:values){
        if(v.source){Statement q(impl_->state,"INSERT INTO source_overrides VALUES(?,?,?,?,?) ON CONFLICT(document_hash,page_index) DO UPDATE SET source_utf8=excluded.source_utf8,source_revision=excluded.source_revision,modified_at=excluded.modified_at WHERE excluded.source_revision>=source_overrides.source_revision");q.text(1,hash).number(2,v.page).text(3,*v.source).number(4,v.revision).number(5,impl_->state_time()).step();}
        else{Statement q(impl_->state,"DELETE FROM source_overrides WHERE document_hash=? AND page_index=? AND source_revision<=?");q.text(1,hash).number(2,v.page).number(3,v.revision).step();}
        Statement clear(impl_->state,"DELETE FROM pending_source_overrides WHERE provisional_key=? AND page_index=? AND source_revision=?");clear.text(1,provisional).number(2,v.page).number(3,v.revision).step();
    }
    // Positions written before hashing use the same stable native-file key.
    {Statement q(impl_->state,"INSERT INTO document_state SELECT ?,last_path,page_index,modified_at FROM document_state WHERE document_hash=? ON CONFLICT(document_hash) DO UPDATE SET last_path=excluded.last_path,page_index=excluded.page_index,modified_at=excluded.modified_at WHERE excluded.modified_at>=document_state.modified_at");q.text(1,hash).text(2,provisional).step();}
    {Statement q(impl_->state,"INSERT INTO page_state SELECT ?,page_index,pdf_anchor_x,pdf_anchor_y,text_anchor FROM page_state WHERE document_hash=? ON CONFLICT(document_hash,page_index) DO UPDATE SET pdf_anchor_x=excluded.pdf_anchor_x,pdf_anchor_y=excluded.pdf_anchor_y,text_anchor=excluded.text_anchor");q.text(1,hash).text(2,provisional).step();}
    {Statement q(impl_->state,"DELETE FROM document_state WHERE document_hash=?");q.text(1,provisional).step();}
    {Statement q(impl_->state,"DELETE FROM page_state WHERE document_hash=?");q.text(1,provisional).step();}
    tx.commit();
}
void Store::save_position(std::string_view key,std::string_view path,const ReadingPosition& p){
    impl_->owns();Transaction tx(impl_->state);
    {Statement q(impl_->state,"INSERT INTO document_state VALUES(?,?,?,?) ON CONFLICT(document_hash) DO UPDATE SET last_path=excluded.last_path,page_index=excluded.page_index,modified_at=excluded.modified_at");q.text(1,key).text(2,path).number(3,p.page).number(4,impl_->state_time()).step();}
    {Statement q(impl_->state,"INSERT INTO page_state VALUES(?,?,?,?,?) ON CONFLICT(document_hash,page_index) DO UPDATE SET pdf_anchor_x=excluded.pdf_anchor_x,pdf_anchor_y=excluded.pdf_anchor_y,text_anchor=excluded.text_anchor");q.text(1,key).number(2,p.page);if(p.pdf_anchor)q.real(3,p.pdf_anchor->x).real(4,p.pdf_anchor->y);else q.null(3).null(4);q.number(5,p.text_anchor).step();}
    tx.commit();
}
std::vector<std::string> Store::recent_documents(){impl_->owns();std::vector<std::string> result;Statement q(impl_->state,"SELECT last_path FROM document_state GROUP BY last_path ORDER BY MAX(modified_at) DESC LIMIT 10");while(q.step())result.push_back(q.text(0));return result;}
void Store::pin_document(std::string_view key){impl_->owns();Transaction tx(impl_->cache);sql(impl_->cache,"DELETE FROM active_documents");if(!key.empty()){Statement q(impl_->cache,"INSERT INTO active_documents VALUES(?)");q.text(1,key).step();}impl_->pinned=key;tx.commit();}
void Store::prepare_cache_promotion(bool model,std::string_view from,std::string_view to){
    impl_->owns();Statement q(impl_->cache,"INSERT INTO fingerprint_promotions VALUES(?,?,?,0) ON CONFLICT(is_model,from_key) DO UPDATE SET to_key=excluded.to_key,applied=0");q.number(1,model).text(2,from).text(3,to).step();
}
std::optional<std::string> Store::promoted_key(bool model,std::string_view from){
    impl_->owns();Statement q(impl_->cache,"SELECT to_key FROM fingerprint_promotions WHERE is_model=? AND from_key=?");q.number(1,model).text(2,from);if(q.step())return q.text(0);return {};
}
void Store::save_attempt(const CacheContext& context,const AttemptSnapshot& data){
    impl_->owns();if(data.condition_key!=context.condition_key())throw std::invalid_argument("Attempt cache condition mismatch");
    Transaction tx(impl_->cache);
    {Statement q(impl_->cache,"INSERT INTO page_attempts VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(attempt_id) DO UPDATE SET document_key=excluded.document_key,page_index=excluded.page_index,profile_key=excluded.profile_key,source_digest=excluded.source_digest,force_retranslate=excluded.force_retranslate,plan_complete=excluded.plan_complete,status=excluded.status,modified_at=excluded.modified_at,model_key=excluded.model_key,profile_digest=excluded.profile_digest,condition_key=excluded.condition_key,plan_generation=excluded.plan_generation,override_digest=excluded.override_digest");
        q.text(1,data.id).text(2,context.document_key).number(3,context.page).text(4,profile_key(context)).text(5,context.source_digest)
            .number(6,data.bypass_cache).number(7,data.planning_complete).text(8,page_names.at(static_cast<unsigned>(data.state))).number(9,impl_->cache_time())
            .text(10,context.model_key).text(11,context.profile_digest).text(12,data.condition_key).number(13,data.plan).text(14,context.override_digest).step();}
    {Statement q(impl_->cache,"DELETE FROM attempt_chunks WHERE attempt_id=?");q.text(1,data.id).step();}
    std::uint32_t ordinal=0;
    for(const auto& c:data.chunks){
        std::string cache_key;
        if(c.state==LeafState::Complete){
            utf8_scalars(c.text);if(c.reason!=FinishReason::Completed||!valid_translation(c.text)||c.source_digest.empty()||c.prompt_digest.empty())throw std::invalid_argument("Incomplete chunk cannot be cached");
            const auto semantic=semantic_key(c);cache_key=completed_cache_key(context,semantic);
            Statement q(impl_->cache,"INSERT INTO completed_chunks VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(cache_key) DO UPDATE SET text_utf8=excluded.text_utf8,byte_size=excluded.byte_size,last_used_at=excluded.last_used_at,session_key=excluded.session_key");
            q.text(1,cache_key).text(2,context.document_key).number(3,context.page).text(4,profile_key(context)).text(5,c.block_id).number(6,1).number(7,ordinal)
                .text(8,c.source_digest).text(9,c.prompt_digest).text(10,c.text).number(11,c.text.size()+1024).number(12,impl_->cache_time()).number(13,impl_->cache_time())
                .text(14,context.model_key).text(15,context.profile_digest).text(16,context.source_digest).text(17,semantic).text(18,impl_->session).step();
        }
        Statement q(impl_->cache,"INSERT INTO attempt_chunks VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        q.text(1,data.id).text(2,c.block_id).text(3,c.id).number(4,c.begin).number(5,c.end).text(6,c.parent_id).text(7,leaf_names.at(static_cast<unsigned>(c.state)));
        if(cache_key.empty())q.null(8);else q.text(8,cache_key);
        q.number(9,ordinal++).text(10,c.source_digest).text(11,c.prompt_digest).text(12,c.plan_digest)
            .text(13,(c.state==LeafState::SourceOnly||c.state==LeafState::Complete)?std::string_view(c.text):std::string_view{})
            .number(14,c.target_tokens).number(15,c.source_tokens).number(16,c.prompt_tokens).number(17,c.output_limit).number(18,c.output_tokens)
            .number(19,c.plan).number(20,static_cast<unsigned>(c.reason)).number(21,c.split_retry).number(22,static_cast<unsigned>(c.kind)).step();
    }
    tx.commit();
}
std::optional<AttemptSnapshot> Store::attempt(const CacheContext& context){
    impl_->owns();AttemptSnapshot data;
    {Statement q(impl_->cache,"SELECT attempt_id,force_retranslate,plan_complete,status,plan_generation FROM page_attempts WHERE condition_key=? ORDER BY modified_at DESC LIMIT 1");
        q.text(1,context.condition_key());if(!q.step())return {};data.id=q.text(0);data.bypass_cache=q.number(1);data.planning_complete=q.number(2);data.state=parse<PageState>(q.text(3),page_names);data.plan=q.number(4);data.condition_key=context.condition_key();}
    Statement q(impl_->cache,"SELECT a.chunk_id,a.parent_chunk_id,a.block_id,a.source_begin,a.source_end,a.status,a.source_digest,a.prompt_digest,a.plan_digest,a.source_text,a.target_tokens,a.source_tokens,a.prompt_tokens,a.output_limit,a.output_tokens,a.plan_generation,a.reason,a.split_retry,a.block_kind,c.text_utf8 FROM attempt_chunks a LEFT JOIN completed_chunks c ON c.cache_key=a.cache_key WHERE a.attempt_id=? ORDER BY a.ordinal");
    q.text(1,data.id);
    while(q.step()){
        AttemptChunk c;c.id=q.text(0);c.parent_id=q.text(1);c.block_id=q.text(2);c.begin=q.number(3);c.end=q.number(4);c.state=parse<LeafState>(q.text(5),leaf_names);
        c.source_digest=q.text(6);c.prompt_digest=q.text(7);c.plan_digest=q.text(8);c.text=q.text(9);if(c.state==LeafState::Complete&&c.text.empty())c.text=q.text(19);
        c.target_tokens=q.number(10);c.source_tokens=q.number(11);c.prompt_tokens=q.number(12);c.output_limit=q.number(13);c.output_tokens=q.number(14);c.plan=q.number(15);
        if(q.number(16)<0||q.number(16)>static_cast<int>(FinishReason::ModelUnavailable)||q.number(18)<0||q.number(18)>static_cast<int>(BlockKind::Edited))return {};
        c.reason=static_cast<FinishReason>(q.number(16));c.split_retry=q.number(17);c.kind=static_cast<BlockKind>(q.number(18));
        if(c.state==LeafState::Complete){try{utf8_scalars(c.text);if(!valid_translation(c.text))return {};}catch(...){return {};}}
        data.chunks.push_back(std::move(c));
    }return data;
}
std::optional<std::string> Store::completed(const CacheContext& context,const PreparedChunk& chunk,bool same_session_only){
    impl_->owns();const auto key=completed_cache_key(context,chunk_semantic_key(chunk));std::optional<std::string> result;
    {Statement q(impl_->cache,"SELECT text_utf8 FROM completed_chunks WHERE cache_key=? AND source_digest=? AND prompt_ids_digest=? AND (?=0 OR session_key=?)");q.text(1,key).text(2,chunk.source_digest).text(3,chunk.prompt_ids_digest).number(4,same_session_only).text(5,impl_->session);if(q.step()){auto text=q.text(0);try{utf8_scalars(text);if(valid_translation(text))result=std::move(text);}catch(...){}}}
    if(result){Statement q(impl_->cache,"UPDATE completed_chunks SET last_used_at=? WHERE cache_key=?");q.number(1,impl_->cache_time()).text(2,key).step();}return result;
}
void Store::promote_cache(bool model,std::string_view from,std::string_view to){
    impl_->owns();if(from==to)return;Transaction tx(impl_->cache);
    std::vector<std::int64_t> rows;
    {Statement q(impl_->cache,model?"SELECT rowid FROM completed_chunks WHERE model_key=?":"SELECT rowid FROM completed_chunks WHERE document_hash=?");q.text(1,from);while(q.step())rows.push_back(q.number(0));}
    for(auto row:rows){
        CacheContext c;std::string old,semantic;
        {Statement q(impl_->cache,"SELECT cache_key,document_hash,page_index,model_key,profile_digest,page_source_digest,semantic_key FROM completed_chunks WHERE rowid=?");q.number(1,row);if(!q.step())continue;old=q.text(0);c.document_key=q.text(1);c.page=q.number(2);c.model_key=q.text(3);c.profile_digest=q.text(4);c.source_digest=q.text(5);semantic=q.text(6);}
        if(model)c.model_key=to;else c.document_key=to;const auto key=completed_cache_key(c,semantic);
        {Statement q(impl_->cache,"UPDATE attempt_chunks SET cache_key=? WHERE cache_key=?");q.text(1,key).text(2,old).step();}
        {Statement q(impl_->cache,"UPDATE OR IGNORE completed_chunks SET cache_key=?,document_hash=?,model_key=?,profile_key=? WHERE rowid=?");q.text(1,key).text(2,c.document_key).text(3,c.model_key).text(4,profile_key(c)).number(5,row).step();}
        {Statement q(impl_->cache,"DELETE FROM completed_chunks WHERE cache_key=?");q.text(1,old).step();}
    }
    rows.clear();
    {Statement q(impl_->cache,model?"SELECT rowid FROM page_attempts WHERE model_key=?":"SELECT rowid FROM page_attempts WHERE document_key=?");q.text(1,from);while(q.step())rows.push_back(q.number(0));}
    for(auto row:rows){
        CacheContext c;{Statement q(impl_->cache,"SELECT document_key,page_index,model_key,profile_digest,source_digest,override_digest FROM page_attempts WHERE rowid=?");q.number(1,row);if(!q.step())continue;c.document_key=q.text(0);c.page=q.number(1);c.model_key=q.text(2);c.profile_digest=q.text(3);c.source_digest=q.text(4);c.override_digest=q.text(5);}
        if(model)c.model_key=to;else c.document_key=to;
        Statement q(impl_->cache,"UPDATE page_attempts SET document_key=?,model_key=?,profile_key=?,condition_key=? WHERE rowid=?");q.text(1,c.document_key).text(2,c.model_key).text(3,profile_key(c)).text(4,c.condition_key()).number(5,row).step();
    }
    if(!model&&impl_->pinned==from){
        {Statement q(impl_->cache,"DELETE FROM active_documents WHERE document_key=?");q.text(1,from).step();}
        {Statement q(impl_->cache,"INSERT OR IGNORE INTO active_documents VALUES(?)");q.text(1,to).step();}impl_->pinned=to;
    }
    {Statement q(impl_->cache,"UPDATE fingerprint_promotions SET applied=1 WHERE is_model=? AND from_key=? AND to_key=?");q.number(1,model).text(2,from).text(3,to).step();}
    tx.commit();
}
std::size_t Store::for_each_complete_page(std::string_view doc,std::string_view model,std::string_view profile,const std::function<void(PageIndex,std::string_view)>& consume,std::string_view native_source_key){
    impl_->owns();std::size_t count=0;
    Statement pages(impl_->cache,"SELECT a.attempt_id,a.page_index,a.override_digest FROM page_attempts a WHERE a.document_key=? AND a.model_key=? AND a.profile_digest=? AND a.status='complete' AND a.plan_complete=1 AND a.modified_at=(SELECT MAX(b.modified_at) FROM page_attempts b WHERE b.document_key=a.document_key AND b.page_index=a.page_index AND b.model_key=a.model_key AND b.profile_digest=a.profile_digest) ORDER BY a.page_index");pages.text(1,doc).text(2,model).text(3,profile);
    while(pages.step()){
        std::string current_override;SourceRevision revision=0;
        {Statement q(impl_->state,"SELECT source_utf8,source_revision FROM source_overrides WHERE document_hash=? AND page_index=?");q.text(1,doc).number(2,pages.number(1));if(q.step()){current_override=sha256(q.text(0));revision=q.number(1);}}
        if(!native_source_key.empty()){
            Statement q(impl_->state,"SELECT source_utf8,source_revision,use_auto_source FROM pending_source_overrides WHERE provisional_key=? AND page_index=?");q.text(1,native_source_key).number(2,pages.number(1));
            if(q.step()&&static_cast<SourceRevision>(q.number(1))>=revision)current_override=q.number(2)?std::string{}:sha256(q.text(0));
        }
        if(current_override!=pages.text(2))continue;
        std::string text,prior;bool valid=true;
        Statement q(impl_->cache,"SELECT a.block_id,a.status,a.source_text,a.block_kind,c.text_utf8 FROM attempt_chunks a LEFT JOIN completed_chunks c ON c.cache_key=a.cache_key WHERE a.attempt_id=? AND a.status!='replaced' ORDER BY a.ordinal");q.text(1,pages.text(0));
        while(q.step()){
            const auto block=q.text(0),status=q.text(1);auto body=q.text(2);if(status=="complete"&&body.empty())body=q.text(4);
            if(status!="complete"&&status!="source-only"){valid=false;break;}
            try{utf8_scalars(body);if(status=="complete"&&!valid_translation(body))valid=false;}catch(...){valid=false;break;}
            if(block!=prior){if(!text.empty())text+="\n\n";prior=block;
                if(status=="source-only"&&q.number(3)==static_cast<int>(BlockKind::Table))text+="［表・原文］\n";
                if(status=="source-only"&&q.number(3)==static_cast<int>(BlockKind::Equation))text+="［数式・原文］\n";}
            text+=body;
        }if(valid){consume(static_cast<PageIndex>(pages.number(1)),text);++count;}
    }return count;
}
void Store::clear_cache(){impl_->owns();Transaction tx(impl_->cache);sql(impl_->cache,"DELETE FROM attempt_chunks;DELETE FROM page_attempts;DELETE FROM completed_chunks;DELETE FROM session_completed_chunks;DELETE FROM fingerprint_promotions");tx.commit();}
bool Store::maintain(){
    impl_->owns();
    for(;;){
        std::uint64_t bytes=0;{Statement q(impl_->cache,"SELECT (SELECT COALESCE(SUM(byte_size),0) FROM completed_chunks WHERE document_hash NOT IN(SELECT document_key FROM active_documents))+(SELECT COALESCE(SUM(length(c.source_text)+512),0) FROM attempt_chunks c JOIN page_attempts a ON a.attempt_id=c.attempt_id WHERE a.document_key NOT IN(SELECT document_key FROM active_documents))+(SELECT count(*)*512 FROM page_attempts WHERE document_key NOT IN(SELECT document_key FROM active_documents))");if(q.step())bytes=q.number(0);}
        if(bytes<=impl_->limit)break;
        std::string doc;{Statement q(impl_->cache,"SELECT document_key FROM(SELECT document_hash AS document_key,last_used_at AS used FROM completed_chunks UNION ALL SELECT document_key,modified_at FROM page_attempts) WHERE document_key NOT IN(SELECT document_key FROM active_documents) GROUP BY document_key ORDER BY MAX(used) LIMIT 1");if(q.step())doc=q.text(0);}
        if(doc.empty())break;Transaction tx(impl_->cache);
        {Statement q(impl_->cache,"DELETE FROM attempt_chunks WHERE attempt_id IN(SELECT attempt_id FROM page_attempts WHERE document_key=?)");q.text(1,doc).step();}
        {Statement q(impl_->cache,"DELETE FROM page_attempts WHERE document_key=?");q.text(1,doc).step();}
        {Statement q(impl_->cache,"DELETE FROM completed_chunks WHERE document_hash=?");q.text(1,doc).step();}tx.commit();
    }
    sql(impl_->cache,"DELETE FROM fingerprint_promotions WHERE applied=1 AND ((is_model=0 AND to_key NOT IN(SELECT document_key FROM page_attempts) AND to_key NOT IN(SELECT document_hash FROM completed_chunks) AND to_key NOT IN(SELECT document_key FROM active_documents)) OR (is_model=1 AND to_key NOT IN(SELECT model_key FROM page_attempts) AND to_key NOT IN(SELECT model_key FROM completed_chunks)))");
    // Called only while the I/O mailbox and fingerprint work are idle.
    std::int64_t before=0;{Statement q(impl_->cache,"PRAGMA freelist_count");if(q.step())before=q.number(0);}
    sql(impl_->cache,"PRAGMA wal_checkpoint(PASSIVE); PRAGMA incremental_vacuum(256)");
    Statement q(impl_->cache,"PRAGMA freelist_count");return q.step()&&q.number(0)>0&&q.number(0)<before;
}
}
