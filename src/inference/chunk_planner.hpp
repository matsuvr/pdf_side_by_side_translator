#pragma once
#include "core/types.hpp"
#include "inference/engine.hpp"
#include <functional>
#include <optional>

namespace opt {
inline constexpr std::uint32_t page_plan_version=2;
struct ModelProfile {
    std::string id="plamo3-translation-pfml3-v1";
    std::uint32_t prompt_version=1,chunk_algorithm_version=page_plan_version,completion_version=2,engine_cache_version=2;
    std::string source_language="en",target_language="ja";
};
struct ChunkRequest {
    WorkStamp stamp;
    std::string block_id;
    std::shared_ptr<const std::string> source;
    Form form=Form::Plain;
};
struct PreparedChunk {
    WorkStamp stamp;
    std::string block_id,chunk_id;
    std::uint32_t chunk_algorithm_version=page_plan_version;
    std::size_t source_begin=0,source_end=0;
    std::string source_digest,prompt_ids_digest,plan_digest;
    PreparedInput input;
    bool source_only=false;
    std::optional<FinishReason> failure;
};
class ChunkPlanner {
public:
    using Prepare=std::function<PreparedInput(std::string_view,Form)>;
    static PreparedChunk prepare(const ChunkRequest& request,std::uint32_t context_capacity,
                                 const Prepare& prompt,const std::function<bool()>& cancelled={});
};
}
