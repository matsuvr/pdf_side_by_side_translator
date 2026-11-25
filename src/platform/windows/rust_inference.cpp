#include "inference/engine.hpp"
#include "inference/bounded_tokenizer.hpp"
#include "core/utf8.hpp"
#include "platform/file_snapshot.hpp"
#include "platform/digest.hpp"
#include "platform/windows/plamo-rs/ffi.h"
#include <algorithm>
#include <chrono>
#include <climits>
#include <cerrno>
#include <cstdlib>
#include <thread>

namespace opt {
namespace {
constexpr std::uint32_t context=32768;
constexpr std::string_view model_sha="aa290f1609fc0ad49811060a8499a773ababd088b9d21dc3e907c287f4fe2478";
const std::vector<std::int32_t> stops{1,2,16};
unsigned inference_threads(unsigned hardware){
    const auto fallback=hardware<=1?1U:std::min(8U,hardware-1);
    const char* configured=std::getenv("OPT_INFERENCE_THREADS");
    if(!configured||!*configured)return fallback;
    char* end=nullptr;errno=0;const auto value=std::strtoul(configured,&end,10);
    if(errno!=0||end==configured||*end!='\0'||value==0)return fallback;
    return static_cast<unsigned>(std::min<unsigned long>(value,std::min(256U,std::max(1U,hardware))));
}
struct ReadState {
    const FileSnapshot& file;
    const std::atomic_bool& cancel;
    Sha256 hash;
    static std::size_t read(void* user,std::uint64_t offset,std::uint8_t* dst,std::size_t count) noexcept {
        auto& s=*static_cast<ReadState*>(user);
        try {
            if(s.cancel.load())return 0;
            const auto bytes=std::span(reinterpret_cast<std::byte*>(dst),count);
            const auto n=s.file.read_at(offset,bytes);
            s.hash.update(bytes.first(n));
            if(offset+n==s.file.initial_signature().size && s.hash.finish()!=model_sha)return 0;
            return n;
        } catch(...){return 0;}
    }
};
}
struct Engine::Impl {
    std::atomic_bool& cancelled;
    OptPlamo* model=nullptr;
    std::shared_ptr<const FileSnapshot> file;
    unsigned threads=0;
    std::vector<std::int32_t> prefix,plain,easy;
    explicit Impl(std::atomic_bool& c):cancelled(c){}
    void clear(){opt_plamo_close(model);model=nullptr;file.reset();prefix.clear();plain.clear();easy.clear();threads=0;}
    ~Impl(){clear();}
};
Engine::Engine(std::atomic_bool& c):impl_(std::make_unique<Impl>(c)){}
Engine::~Engine()=default;
void Engine::unload(){impl_->clear();}
void Engine::load(const std::filesystem::path& path,bool){
    unload();
    auto& p=*impl_;
    try {
        p.file=FileSnapshot::open(path);
        const auto size=p.file->initial_signature().size;
        if(size!=516613376ULL)throw std::runtime_error("P4X86 v7専用翻訳モデルのサイズが一致しません。");
        const auto hw=std::thread::hardware_concurrency();
        p.threads=inference_threads(hw);
        ReadState state{*p.file,p.cancelled,{}};
        p.model=opt_plamo_open(static_cast<std::size_t>(size),p.threads,ReadState::read,&state);
        if(p.cancelled.load())throw PreparationCancelled();
        if(!p.model)throw std::runtime_error("PLaMo 3 Translate 0.8B x86 S4 v7モデルを読み込めません。");
        if(!p.file->matches_initial_signature())throw std::runtime_error("翻訳モデルが読込中に変更されました。");
        p.prefix=tokenize("<|plamo:tag|>dataset<|plamo:msg|>translation<|plamo:tag|>input<|plamo:msg|>",true,true);
        const std::string output="<|plamo:tag|>output<|plamo:key|>lang<|plamo:val|>";
        p.plain=tokenize(output+"ja<|plamo:key|>form<|plamo:val|>plain<|plamo:msg|>",false,true);
        p.easy=tokenize(output+"ja-easy<|plamo:msg|>",false,true);
    }catch(...){unload();throw;}
}
std::vector<std::int32_t> Engine::tokenize(std::string_view text,bool bos,bool special)const{
    const auto& p=*impl_;
    if(!p.model||text.size()>8*1024*1024)throw std::runtime_error("Invalid tokenizer input");
    if(text.empty())return bos?std::vector<std::int32_t>{1}:std::vector<std::int32_t>{};
    std::vector<std::int32_t> ids(std::min<std::size_t>(text.size()+1,4096));
    auto encode=[&]{return opt_plamo_tokenize(p.model,reinterpret_cast<const std::uint8_t*>(text.data()),text.size(),
        special,bos,ids.data(),ids.size());};
    auto n=encode();
    if(n==INT32_MIN)throw std::runtime_error("Tokenization failed");
    if(n<0){ids.resize(static_cast<std::size_t>(-n));n=encode();}
    if(n<0||static_cast<std::size_t>(n)>ids.size())throw std::runtime_error("Tokenization failed");
    ids.resize(n);return ids;
}
PreparedInput Engine::prepare(std::string_view source,Form form)const{
    const auto& p=*impl_;
    const auto checkpoint=[&]{if(p.cancelled.load())throw PreparationCancelled();};
    checkpoint();
    if(!p.model)throw std::runtime_error("Model unavailable");
    const auto& tail=form==Form::Easy?p.easy:p.plain;
    const auto overhead=p.prefix.size()+tail.size();
    const auto limit=context-overhead-1;
    auto body=tokenize_plamo3_bounded(source,limit,opt_plamo_max_piece(p.model),
        [&](std::string_view part,std::span<std::int32_t> out){
            return opt_plamo_tokenize(p.model,reinterpret_cast<const std::uint8_t*>(part.data()),part.size(),0,0,out.data(),out.size());
        },checkpoint);
    PreparedInput result;
    result.stop_ids=stops;
    result.source_tokens=static_cast<std::uint32_t>(body.size());
    result.output_limit=static_cast<std::uint32_t>(std::min<std::size_t>(context,std::max<std::size_t>(512,2*body.size()+128)));
    result.tokens=p.prefix;
    result.tokens.insert(result.tokens.end(),body.begin(),body.end());
    result.tokens.insert(result.tokens.end(),tail.begin(),tail.end());
    checkpoint();return result;
}
std::uint32_t Engine::capacity()const{return impl_->model?context:0;}
std::string Engine::backend()const{return "CPU (PLaMo S4 v7 / Rust)";}
std::shared_ptr<const FileSnapshot> Engine::file_snapshot()const{return impl_->file;}
InferenceRuntime Engine::runtime()const{
    if(!impl_->model)return {};
    return {context,32,32,static_cast<std::int32_t>(impl_->threads),static_cast<std::int32_t>(impl_->threads)};
}
GenerationResult Engine::generate(const PreparedInput& input,const std::function<void(std::string_view)>& delta){
    auto& p=*impl_;
    GenerationResult result;
    if(!p.model||!p.file->matches_initial_signature())return result;
    if(input.stop_ids!=stops){result.reason=FinishReason::ModelInputUnsupported;return result;}
    if(input.tokens.empty()||input.tokens.size()>context||input.output_limit>context-input.tokens.size()){
        result.reason=FinishReason::ContextLimit;return result;
    }
    struct State{
        Impl& p;
        GenerationResult& result;
        const std::function<void(std::string_view)>& delta;
        Utf8Stream decoder;
        std::string pending;
        std::chrono::steady_clock::time_point last=std::chrono::steady_clock::now();
        FinishReason error=FinishReason::InvalidModelOutput;
        static std::int32_t cancel(void* ptr)noexcept{return static_cast<State*>(ptr)->p.cancelled.load()?1:0;}
        static std::int32_t accept(void* ptr,const std::uint8_t* bytes,std::size_t n)noexcept{
            const auto& decoder=static_cast<const State*>(ptr)->decoder;
            if(!bytes)return decoder.complete();
            try{
                const std::string_view piece(reinterpret_cast<const char*>(bytes),n);
                return unsupported_text_control(piece)==std::string_view::npos&&decoder.accepts(piece);
            }catch(...){return 0;}
        }
        static std::int32_t piece(void* ptr,const std::uint8_t* bytes,std::size_t n)noexcept{
            auto& s=*static_cast<State*>(ptr);
            try{
                auto text=s.decoder.push({reinterpret_cast<const char*>(bytes),n});
                if(text.size()>max_translation_bytes-s.result.text.size()){s.error=FinishReason::OutputLimit;return 1;}
                if(unsupported_text_control(text)!=std::string::npos)return 1;
                s.result.text+=text;s.pending+=text;++s.result.output_tokens;
                const auto now=std::chrono::steady_clock::now();
                if(now-s.last>=std::chrono::milliseconds(50)){
                    if(!s.pending.empty()){s.delta(s.pending);s.pending.clear();}
                    s.last=now;
                    if(!s.p.file->matches_initial_signature()){s.error=FinishReason::ModelUnavailable;return 1;}
                }
                return 0;
            }catch(const Utf8Error&){s.error=FinishReason::InvalidEncoding;return 1;}
            catch(...){s.error=FinishReason::DecodeError;return 1;}
        }
    } state{p,result,delta};
    switch(opt_plamo_generate(p.model,input.tokens.data(),input.tokens.size(),input.output_limit,
                              State::cancel,State::accept,State::piece,&state)){
        case 0:result.reason=!state.decoder.complete()?FinishReason::InvalidEncoding:
            has_usable_text(result.text)?FinishReason::Completed:FinishReason::EmptyOutput;break;
        case 1:result.reason=FinishReason::Cancelled;break;
        case 2:result.reason=FinishReason::OutputLimit;break;
        case 3:result.reason=FinishReason::ContextLimit;break;
        case 4:result.reason=state.error;break;
        default:result.reason=FinishReason::DecodeError;break;
    }
    if(!state.pending.empty())delta(state.pending);
    if(p.cancelled.load())result.reason=FinishReason::Cancelled;
    if(!p.file->matches_initial_signature())result.reason=FinishReason::ModelUnavailable;
    return result;
}
}
