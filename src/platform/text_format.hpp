#pragma once
#include "core/utf8.hpp"
#include <optional>
#include <string>
#include <string_view>

namespace opt {
// Rich Edit's native paragraph delimiter is one CR. Canonicalize only the
// display representation; generated and memory-cached bytes retain their newlines.
inline std::string rich_edit_text(std::string_view source){
    std::string value;value.reserve(source.size());
    for(std::size_t i=0;i<source.size();++i){
        if(source[i]=='\r'){
            value+='\r';if(i+1<source.size()&&source[i+1]=='\n')++i;
        }else value+=source[i]=='\n'?'\r':source[i];
    }
    return value;
}
inline std::string_view appended_utf8_suffix(std::string_view before,std::string_view after){
    if(before.empty()||before.size()>=after.size()||
       (static_cast<unsigned char>(after[before.size()])&0xc0)==0x80||!after.starts_with(before))return {};
    return after.substr(before.size());
}

inline std::optional<std::size_t> utf16_units(std::string_view text,bool collapse_crlf=false){
    std::size_t units=0;
    try{
        visit_utf8(text,[&](const Utf8Scalar& scalar){
            if(collapse_crlf&&scalar.value=='\n'&&scalar.begin>0&&text[scalar.begin-1]=='\r')return;
            units+=scalar.value<0x10000?1:2;
        },[]{});
    }catch(const Utf8Error&){
        return {};
    }
    return units;
}
}
