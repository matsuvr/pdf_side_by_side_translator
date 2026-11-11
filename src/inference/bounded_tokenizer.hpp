#pragma once
#include "inference/engine.hpp"
#include <algorithm>
#include <climits>
#include <limits>
#include <span>

namespace opt {
// PLaMo 3 preserves input bytes except U+EE00, which separates segments and
// emits no token. Each nonempty segment needs at least one token, and N tokens
// cover at most N * max_token_bytes other bytes. The loaded vocabulary is
// checked for user-defined tokens that could strip text or span U+EE00.
//
// Do not tokenize segments independently: PFML markers are detected BEFORE
// U+EE00 splits. Instead, shorten only runs of more than 65 U+EE00 scalars to
// 65. PFML marker bodies are limited to 64 scalars, so these runs cannot be
// inside a recognized marker; repeated-character splitting is unchanged too.
// This preserves exact token IDs while bounding even boundary-heavy input.
// llama's tokenizer remains synchronous inside this bounded single call.
template<class Encode,class Checkpoint>
std::vector<std::int32_t> tokenize_plamo3_bounded(std::string_view source,std::size_t token_limit,
                                                std::size_t max_token_bytes,Encode&& encode,
                                                Checkpoint&& checkpoint){
    if(!max_token_bytes||token_limit>INT32_MAX)throw std::invalid_argument("Invalid tokenizer budget");
    constexpr std::string_view boundary="\xee\xb8\x80"; // U+EE00
    constexpr std::size_t retained_boundaries=65;
    const auto byte_limit=token_limit>std::numeric_limits<std::size_t>::max()/max_token_bytes?
        std::numeric_limits<std::size_t>::max():token_limit*max_token_bytes;
    std::size_t effective_bytes=0,segments=0,next_checkpoint=0;
    bool in_segment=false,compacted=false;
    std::string normalized;
    const auto check_at=[&](std::size_t offset){
        if(offset>=next_checkpoint){checkpoint();next_checkpoint=offset+4096;}
    };
    checkpoint();
    for(std::size_t offset=0;offset<source.size();){
        check_at(offset);
        if(source.substr(offset,boundary.size())==boundary){
            const auto begin=offset;
            do{offset+=boundary.size();check_at(offset);}
            while(source.substr(offset,boundary.size())==boundary);
            const auto bytes=offset-begin;
            if(bytes>retained_boundaries*boundary.size()){
                if(!compacted){normalized.assign(source.substr(0,begin));compacted=true;}
                normalized.append(source.substr(begin,retained_boundaries*boundary.size()));
            }else if(compacted)normalized.append(source.substr(begin,bytes));
            in_segment=false;
        }else{
            if(++effective_bytes>byte_limit)throw InputContextLimit();
            if(!in_segment){if(++segments>token_limit)throw InputContextLimit();in_segment=true;}
            if(compacted)normalized+=source[offset];
            ++offset;
        }
    }
    checkpoint();
    if(!effective_bytes)return {};
    const std::string_view input=compacted?std::string_view(normalized):source;
    if(input.size()>INT32_MAX)throw InputContextLimit();
    std::vector<std::int32_t> tokens(token_limit);
    const auto count=encode(input,std::span(tokens));
    checkpoint();
    if(count==INT32_MIN)throw std::runtime_error("Tokenizer overflow");
    if(count<0)throw InputContextLimit();
    if(static_cast<std::size_t>(count)>token_limit)throw std::runtime_error("Tokenizer exceeded output buffer");
    tokens.resize(static_cast<std::size_t>(count));
    return tokens;
}
}
