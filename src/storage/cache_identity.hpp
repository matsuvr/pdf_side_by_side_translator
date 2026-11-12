#pragma once
#include "inference/chunk_planner.hpp"
#include "platform/digest.hpp"
#include "platform/file_signature.hpp"

namespace opt {
struct CacheContext {
    std::string document_key,model_key,profile_digest,source_digest;
    PageIndex page=0;
    std::string condition_key()const;
};
std::string provisional_file_key(const FileSignature& signature);
std::string profile_digest(const ModelProfile& profile,Form form,std::uint32_t context_capacity);
std::string page_source_digest(const PageLayout& layout);
std::string chunk_semantic_key(const PreparedChunk& chunk);
std::string completed_cache_key(const CacheContext& context,std::string_view semantic_key);
}
