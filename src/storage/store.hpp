#pragma once
#include "storage/cache_identity.hpp"
#include "translation/page_attempt.hpp"
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>

namespace opt {
class StorageError final:public std::runtime_error {
    int code_;
public:
    StorageError(int code,std::string message):std::runtime_error(std::move(message)),code_(code){}
    int code()const noexcept{return code_;}
};
struct ReadingPosition {PageIndex page=0;std::optional<PdfPoint> pdf_anchor;std::size_t text_anchor=0;bool operator==(const ReadingPosition&)const=default;};
struct SavedDocument {
    std::string document_key;
    ReadingPosition position;
};
struct StoredChunk {
    std::string semantic_key;
    AttemptChunk chunk;
};
struct CacheUsage {std::uint64_t bytes=0,attempts=0,chunks=0;};
// Session-only adapter, constructed and exclusively used on the I/O owner
// thread. GUI/PDF/inference workers only exchange immutable values with it.
class Store {
public:
    // The cache budget counts database pages above the fixed empty schema.
    explicit Store(std::uint64_t cache_limit=256U*1024*1024);
    ~Store();
    Store(const Store&)=delete;Store& operator=(const Store&)=delete;
    std::vector<std::string> warnings()const;
    bool state_persistent()const;
    bool cache_persistent()const;
    std::map<std::string,std::string> settings();
    void settings(const std::map<std::string,std::string>& updates);
    std::optional<std::string> fingerprint(std::string_view path,const FileSignature& signature);
    void fingerprint(std::string_view path,const FileSignature& signature,std::string_view hash);
    SavedDocument document(std::string document_key,std::optional<PageIndex> page={});
    void save_position(std::string_view document_key,std::string_view path,const ReadingPosition& value);
    std::vector<std::string> recent_documents();
    void promote_position(std::string_view provisional_key,std::string_view document_hash);
    void promote_cache(bool model,std::string_view from,std::string_view to);
    void save_attempt(const CacheContext& context,const AttemptSnapshot& snapshot);
    std::optional<AttemptSnapshot> attempt(const CacheContext& context);
    std::optional<std::string> completed(const CacheContext& context,const PreparedChunk& chunk,bool same_session_only=false);
    void clear_cache();
    CacheUsage cache_usage()const;
    bool maintain();
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
