#pragma once
#include <string>
#include <string_view>

namespace opt {
// Rich Edit's native paragraph delimiter is one CR. Canonicalize only the
// display representation; generated/cache/export bytes retain their newlines.
inline std::string rich_edit_text(std::string_view source){
    std::string value;value.reserve(source.size());
    for(std::size_t i=0;i<source.size();++i){
        if(source[i]=='\r'){
            value+='\r';if(i+1<source.size()&&source[i+1]=='\n')++i;
        }else value+=source[i]=='\n'?'\r':source[i];
    }
    return value;
}
}
