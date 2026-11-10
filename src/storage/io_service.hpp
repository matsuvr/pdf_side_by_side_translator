#pragma once
#include "storage/store.hpp"
#include "platform/file_snapshot.hpp"
#include <atomic>

namespace opt {
struct IoEvent {
    enum class Kind { Initialized, DocumentLoaded, PageLoaded, Fingerprint, OverrideSaved, AttemptLoaded, AttemptSaved, CacheResult, Exported, CacheCleared, Error };
    Kind kind=Kind::Error;
    DocumentEpoch document=0;
    ModelEpoch model=0;
    std::uint64_t request=0;
    WorkStamp stamp;
    bool model_fingerprint=false,persistent=false;
    std::string from_key,key,message,detail;
    std::map<std::string,std::string> settings;
    std::vector<std::string> recent_paths,warnings;
    std::optional<SavedDocument> saved_document;
    std::optional<AttemptSnapshot> attempt;
    std::optional<std::string> cached_text;
};
class IoService {
public:
    using Delivery=std::function<void(std::shared_ptr<const IoEvent>)>;
    IoService(std::filesystem::path state_directory,std::filesystem::path cache_directory,Delivery delivery);
    ~IoService();
    bool open_document(DocumentEpoch document,std::string provisional_cache_key,std::filesystem::path path,std::shared_ptr<const FileSnapshot> file);
    bool close_document();
    bool read_page(DocumentEpoch document,std::string document_key,std::string native_provisional,PageIndex page);
    bool track_model(ModelEpoch model,std::string provisional_cache_key,std::filesystem::path path,std::shared_ptr<const FileSnapshot> file);
    bool settings(std::map<std::string,std::string> updates);
    bool save_override(DocumentEpoch document,std::string native_provisional,std::string document_key,std::filesystem::path path,SavedOverride value);
    bool save_position(std::string document_key,std::filesystem::path path,ReadingPosition position);
    bool save_attempt(CacheContext context,AttemptSnapshot data);
    bool read_attempt(CacheContext context,std::uint64_t request,DocumentEpoch document);
    bool lookup(CacheContext context,std::shared_ptr<const PreparedChunk> chunk);
    bool export_current(std::filesystem::path path,PageIndex page,std::string text,PageState state);
    bool export_complete(std::filesystem::path path,CacheContext context);
    bool clear_cache();
    void stop()noexcept;
    bool done()const noexcept;
    void join();
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
