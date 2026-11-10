#pragma once
#include "inference/chunk_planner.hpp"
#include <optional>

namespace opt {
enum class LeafState { Pending, Planning, Running, Complete, SourceOnly, Partial, Failed, Replaced };
struct AttemptChunk {
    std::string id,parent_id,block_id,source_digest,prompt_digest,plan_digest,text;
    std::size_t begin=0,end=0;
    std::uint32_t target_tokens=0,source_tokens=0,prompt_tokens=0,output_limit=0,output_tokens=0;
    PlanGeneration plan=1;
    LeafState state=LeafState::Pending;
    FinishReason reason=FinishReason::Cancelled;
    BlockKind kind=BlockKind::Paragraph;
    bool split_retry=false;
};
struct AttemptSnapshot {
    std::string id,condition_key;
    bool bypass_cache=false,planning_complete=false;
    PlanGeneration plan=1;
    PageState state=PageState::NotStarted;
    std::vector<AttemptChunk> chunks;
};
// GUI-owned state machine. It owns no llama/PDF/DB handles and has at most one
// prepared prompt. A translatable page has one entry in the persisted manifest.
class PageAttempt {
public:
    PageAttempt(WorkStamp base,std::string condition_key,std::shared_ptr<const std::string> source,
                Form form,bool force=false,std::optional<AttemptSnapshot> resume={},bool resume_unfinished=true);
    std::optional<ChunkRequest> next_request();
    bool prepared(std::shared_ptr<const PreparedChunk> chunk);
    bool delta(const WorkStamp& stamp,std::string_view text);
    bool finished(const WorkStamp& stamp,const GenerationResult& result);
    bool preparation_failed(const WorkStamp& stamp,FinishReason reason);
    void interrupt();
    void resume();
    bool accepts(const WorkStamp& stamp) const;
    bool may_read_cache() const noexcept {return !data_.bypass_cache;}
    const WorkStamp& active_stamp() const noexcept {return active_stamp_;}
    std::shared_ptr<const PreparedChunk> active_prepared() const {return prepared_;}
    const AttemptSnapshot& snapshot() const noexcept {return data_;}
    void promote_condition_key(std::string key){data_.condition_key=std::move(key);}
    PageState state() const noexcept {return data_.state;}
    std::string display_text() const;
    static std::shared_ptr<const std::string> source(const PageLayout& layout,std::shared_ptr<const std::string> edited={});
private:
    WorkStamp base_,active_stamp_;
    AttemptSnapshot data_;
    std::shared_ptr<const std::string> source_;
    Form form_;
    JobId next_job_=0;
    bool active_=false;
    std::shared_ptr<const PreparedChunk> prepared_;
    void settle();
};
std::string_view finish_reason_label(FinishReason reason);
}
