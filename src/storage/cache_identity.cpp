#include "storage/cache_identity.hpp"

namespace opt {
std::string CacheContext::condition_key()const{
    return CanonicalKey{}.text("document",document_key).number("page",page).text("model",model_key)
        .text("profile",profile_digest).text("source",source_digest).text("override",override_digest).finish();
}
std::string provisional_file_key(const FileSignature& s){
    return "provisional:file:"+CanonicalKey{}.text("identity",s.identity).number("size",s.size).signed_number("mtime-ns",s.mtime_ns).finish();
}
std::string profile_digest(const ModelProfile& p,Form form,std::uint32_t capacity){
    return CanonicalKey{}.text("profile-id",p.id).number("prompt-version",p.prompt_version).number("chunk-algorithm",p.chunk_algorithm_version)
        .number("completion-version",p.completion_version).number("engine-cache-version",p.engine_cache_version)
        .text("source-language",p.source_language).text("target-language",p.target_language).number("form",static_cast<unsigned>(form))
        .number("actual-context",capacity).text("sampling","greedy-printable-bytes-v2").number("batch",512).number("micro-batch",128)
        .text("output-budget","min(remaining-context,max(512,2*source+128))-v2").finish();
}
std::string page_source_digest(const PageLayout& layout,const std::shared_ptr<const std::string>& edited){
    CanonicalKey key;key.number("layout-version",layout.layout_version).number("override-present",bool(edited));
    key.text("page-text",sha256(edited?*edited:layout.reading_text_utf8));
    return key.finish();
}
std::string chunk_semantic_key(const PreparedChunk& p){
    return CanonicalKey{}.text("plan",p.plan_digest).number("actual-output-limit",p.input.output_limit).finish();
}
std::string completed_cache_key(const CacheContext& c,std::string_view semantic){
    return CanonicalKey{}.text("document",c.document_key).number("page",c.page).text("model",c.model_key)
        .text("profile",c.profile_digest).text("page-source",c.source_digest).text("chunk",semantic).finish();
}
}
