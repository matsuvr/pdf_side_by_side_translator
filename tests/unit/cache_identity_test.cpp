#include "storage/cache_identity.hpp"
#include "translation/page_attempt.hpp"
#include <iostream>
#include <stdexcept>
using namespace opt;
void require(bool v,const char* why){if(!v)throw std::runtime_error(why);}
int main(){try{
    ModelProfile p;const auto baseline=profile_digest(p,Form::Plain,8192);
    require(profile_digest(p,Form::Easy,8192)!=baseline,"form included");require(profile_digest(p,Form::Plain,4096)!=baseline,"actual context included");
    require(profile_digest(p,Form::Easy,8192)!=profile_digest(p,static_cast<Form>(1),8192),"easy mode cannot reuse former polite translations");
    require(form_setting(Form::Easy)=="easy"&&form_from_setting("easy")==Form::Easy,"easy setting round trips");
    require(form_from_setting("polite")==Form::Easy,"former secondary style migrates to easy mode");
    require(form_from_setting("plain")==Form::Plain&&form_from_setting("")==Form::Plain,"standard style remains the default");
    auto q=p;q.prompt_version++;require(profile_digest(q,Form::Plain,8192)!=baseline,"prompt version included");
    q=p;q.chunk_algorithm_version=1;require(profile_digest(q,Form::Plain,8192)!=baseline,"old block translations excluded from page cache");
    q=p;q.completion_version++;require(profile_digest(q,Form::Plain,8192)!=baseline,"completion rules included");
    q=p;q.engine_cache_version++;require(profile_digest(q,Form::Plain,8192)!=baseline,"engine compatibility included");
    q=p;q.target_language="fr";require(profile_digest(q,Form::Plain,8192)!=baseline,"target language included");
    CacheContext c{"sha256:document","sha256:model",baseline,"source-page",0};PreparedChunk chunk;
    chunk.plan_digest="actual-source-range-and-prompt-ids";chunk.input.output_limit=512;
    const auto key=completed_cache_key(c,chunk_semantic_key(chunk));
    auto changed=c;changed.document_key="sha256:other-document";require(completed_cache_key(changed,chunk_semantic_key(chunk))!=key,"document contents included");
    changed=c;changed.model_key="sha256:other-model";require(completed_cache_key(changed,chunk_semantic_key(chunk))!=key,"model contents included");
    changed=c;changed.page++;require(completed_cache_key(changed,chunk_semantic_key(chunk))!=key,"physical page included");
    changed=c;changed.source_digest="edited-page";require(completed_cache_key(changed,chunk_semantic_key(chunk))!=key,"page source override included");
    auto other=chunk;other.input.output_limit++;require(completed_cache_key(c,chunk_semantic_key(other))!=key,"actual generation bound included");
    other=chunk;other.stamp={19,8,22,4,5,"new-attempt",6,100};other.chunk_id="new-execution-uuid";
    require(completed_cache_key(c,chunk_semantic_key(other))==key,"session epochs and execution UUIDs excluded");
    FileSignature file{"native-object",123,456};const auto native=provisional_file_key(file);file.identity="replaced-object";
    require(provisional_file_key(file)!=native,"same size and mtime do not hide file replacement");
    PageLayout layout;layout.reading_text_utf8="Text";
    const auto original=page_source_digest(layout);require(page_source_digest(layout,std::make_shared<const std::string>("Text"))!=original,"presence of explicit override included even when text matches");
    layout.reading_text_utf8+="\n\nMore text";require(page_source_digest(layout)!=original,"page digest includes later paragraphs");
    std::cout<<"semantic cache identity and provisional namespaces passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
