#pragma once
#include "storage/cache_identity.hpp"
#include "translation/page_attempt.hpp"
#include <filesystem>
#include <map>
#include <optional>

namespace opt {
struct SavedOverride {PageIndex page=0;SourceRevision revision=0;std::optional<std::string> source;};
struct ReadingPosition {PageIndex page=0;std::optional<PdfPoint> pdf_anchor;std::size_t text_anchor=0;bool operator==(const ReadingPosition&)const=default;};
struct SavedDocument {
    std::string document_key;
    ReadingPosition position;
    std::vector<SavedOverride> overrides;
};
struct StoredChunk {
    std::string semantic_key;
    AttemptChunk chunk;
};
// Synchronous disk adapter, constructed and exclusively used on the I/O owner
// thread. GUI/PDF/inference workers only exchange immutable values with it.
class Store {
public:
    Store(std::filesystem::path state_directory,std::filesystem::path cache_directory,std::uint64_t cache_limit=256U*1024*1024);
    ~Store();
    Store(const Store&)=delete;Store& operator=(const Store&)=delete;
    std::vector<std::string> warnings()const;
    bool state_persistent()const;
    bool cache_persistent()const;
    std::map<std::string,std::string> settings();
    void settings(const std::map<std::string,std::string>& updates);
    std::optional<std::string> fingerprint(std::string_view path,const FileSignature& signature);
    void fingerprint(std::string_view path,const FileSignature& signature,std::string_view hash);
    SavedDocument document(std::string document_key,std::string_view provisional_key,bool only_reading_page=false,std::optional<PageIndex> page={});
    void save_override(std::string_view provisional_key,std::string_view document_key,std::string_view path,const SavedOverride& value);
    void promote_overrides(std::string_view provisional_key,std::string_view document_hash);
    void save_position(std::string_view document_key,std::string_view path,const ReadingPosition& value);
    std::vector<std::string> recent_documents();
    void pin_document(std::string_view document_key);
    void prepare_cache_promotion(bool model,std::string_view from,std::string_view to);
    std::optional<std::string> promoted_key(bool model,std::string_view from);
    void promote_cache(bool model,std::string_view from,std::string_view to);
    void save_attempt(const CacheContext& context,const AttemptSnapshot& snapshot);
    std::optional<AttemptSnapshot> attempt(const CacheContext& context);
    std::optional<std::string> completed(const CacheContext& context,const PreparedChunk& chunk,bool same_session_only=false);
    std::size_t for_each_complete_page(std::string_view document_key,std::string_view model_key,std::string_view profile,
        const std::function<void(PageIndex,std::string_view)>& consume,std::string_view native_source_key={});
    void clear_cache();
    bool maintain();
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
