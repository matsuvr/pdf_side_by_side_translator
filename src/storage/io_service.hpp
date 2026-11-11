#pragma once
#include "storage/store.hpp"
#include "platform/file_snapshot.hpp"
#include "storage/diagnostic_log.hpp"
#include <atomic>

namespace opt {
struct IoEvent {
    enum class Kind { Initialized, DocumentLoaded, PageLoaded, Fingerprint, OverrideSaved, AttemptLoaded, AttemptSaved, CacheResult, Exported, CacheCleared, Error };
    Kind kind=Kind::Error;
    DocumentEpoch document=0;
    ModelEpoch model=0;
    std::uint64_t request=0;
    WorkStamp stamp;
    bool model_fingerprint=false,persistent=false,export_operation=false;
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
    // The optional hook is a test-only fault-injection seam, never a product
    // setting. It runs on the I/O owner immediately before an accepted job.
    // The optional legacy directory is only a discard target. No files are
    // imported or created there; the current Store always starts in memory.
    IoService(Delivery delivery,std::function<void()> before_job={},std::filesystem::path legacy_directory={});
    ~IoService();
    // Path registration is synchronous and does not access the filesystem. It
    // protects selected inputs while PDF/model workers are still opening them.
    void protect_document(std::filesystem::path path);
    void protect_model(std::filesystem::path path);
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
    bool export_current(std::filesystem::path path,PageIndex page,std::string text,PageState state,Form form=Form::Plain);
    bool export_complete(std::filesystem::path path,CacheContext context,Form form=Form::Plain);
    bool clear_cache();
    bool export_state(std::filesystem::path destination);
    bool diagnostic(DiagnosticRecord record);
    // Accepted queued and executing exports. Zero is published only after
    // their success/error events have been delivered; the Store stays usable
    // until stop(), so a failed export can be retried without losing the session.
    std::size_t exports_pending()const noexcept;
    void stop()noexcept;
    bool done()const noexcept;
    void join();
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
