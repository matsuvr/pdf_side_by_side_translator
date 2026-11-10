#pragma once
#include "core/types.hpp"
#include "inference/engine.hpp"
#include <functional>
#include <limits>
#include <optional>

namespace opt {
struct ModelProfile {
    std::string id="plamo3-translation-pfml3-v1";
    std::uint32_t prompt_version=1,chunk_algorithm_version=1,completion_version=2,engine_cache_version=2;
    std::string source_language="en",target_language="ja";
};
struct ChunkRequest {
    WorkStamp stamp;
    std::string block_id;
    std::shared_ptr<const std::string> source;
    std::size_t begin=0,end=std::numeric_limits<std::size_t>::max();
    std::uint32_t target_source_tokens=1024;
    Form form=Form::Plain;
};
struct PreparedChunk {
    WorkStamp stamp;
    std::string block_id,chunk_id;
    std::uint32_t chunk_algorithm_version=1;
    std::size_t source_begin=0,source_end=0;
    std::string source_digest,prompt_ids_digest,plan_digest;
    PreparedInput input;
    bool source_only=false;
    std::optional<FinishReason> failure;
};
class ChunkPlanner {
public:
    using Prepare=std::function<PreparedInput(std::string_view,Form)>;
    using Count=std::function<std::size_t(std::string_view)>;
    static PreparedChunk prepare(const ChunkRequest& request,std::uint32_t context_capacity,
                                 const Prepare& prompt,const Count& count);
};
// Interior boundary nearest the middle; zero when the source is one cluster.
std::size_t source_split_boundary(std::string_view source);
}
