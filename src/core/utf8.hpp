#pragma once
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opt {
class Utf8Error final : public std::runtime_error {
public:using std::runtime_error::runtime_error;
};
// Retains an incomplete scalar between token pieces. Rejects invalid UTF-8.
class Utf8Stream {
public:
    std::string push(std::string_view bytes) {
        pending_.append(bytes);
        std::size_t i = 0;
        while (i < pending_.size()) {
            const auto lead = static_cast<unsigned char>(pending_[i]);
            const std::size_t n = lead < 0x80 ? 1 : lead >= 0xc2 && lead <= 0xdf ? 2
                : lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
            if (!n) throw Utf8Error("Invalid UTF-8 leading byte");
            const auto available = pending_.size() - i;
            for (std::size_t j = 1; j < n && j < available; ++j) {
                const auto b = static_cast<unsigned char>(pending_[i + j]);
                if ((b & 0xc0) != 0x80 || (j == 1 &&
                    ((lead == 0xe0 && b < 0xa0) || (lead == 0xed && b >= 0xa0) ||
                     (lead == 0xf0 && b < 0x90) || (lead == 0xf4 && b >= 0x90))))
                    throw Utf8Error("Invalid UTF-8 continuation");
            }
            if (available < n) break;
            i += n;
        }
        auto complete = pending_.substr(0, i);
        pending_.erase(0, i);
        return complete;
    }
    bool complete() const noexcept { return pending_.empty(); }
    void reset() noexcept { pending_.clear(); }
private:
    std::string pending_;
};
struct Utf8Scalar {std::size_t begin,end;char32_t value;};
// Validation and predicates must not materialize a mapping for every scalar.
// Checkpoints are bounded by bytes, including long runs of ASCII or whitespace.
template<class Visitor,class Checkpoint>
inline void visit_utf8(std::string_view text,Visitor&& visit,Checkpoint&& checkpoint){
    std::size_t next_checkpoint=0;
    for(std::size_t i=0;i<text.size();){
        if(i>=next_checkpoint){checkpoint();next_checkpoint=i+4096;}
        const auto first=static_cast<unsigned char>(text[i]);
        const unsigned count=first<0x80?1:first>=0xc2&&first<=0xdf?2:first>=0xe0&&first<=0xef?3:first>=0xf0&&first<=0xf4?4:0;
        if(!count||text.size()-i<count)throw Utf8Error("Incomplete or invalid UTF-8 scalar");
        char32_t value=count==1?first:first&((1U<<(7-count))-1);
        for(unsigned j=1;j<count;++j){
            const auto b=static_cast<unsigned char>(text[i+j]);
            if((b&0xc0)!=0x80)throw Utf8Error("Invalid UTF-8 continuation");
            value=(value<<6)|(b&63);
        }
        if((count==2&&value<0x80)||(count==3&&value<0x800)||(count==4&&value<0x10000)||
           value>0x10ffff||(value>=0xd800&&value<=0xdfff))throw Utf8Error("Invalid UTF-8 scalar value");
        visit(Utf8Scalar{i,i+count,value});i+=count;
    }
    checkpoint();
}
inline void validate_utf8(std::string_view text){
    visit_utf8(text,[](const Utf8Scalar&){},[]{});
}
inline std::vector<Utf8Scalar> utf8_scalars(std::string_view text){
    std::vector<Utf8Scalar> result;
    visit_utf8(text,[&](const Utf8Scalar& scalar){result.push_back(scalar);},[]{});
    return result;
}
inline bool unicode_space(char32_t value){
    return value==0x20||(value>=9&&value<=13)||value==0x85||value==0xa0||value==0x1680||
        (value>=0x2000&&value<=0x200a)||value==0x2028||value==0x2029||value==0x202f||value==0x205f||value==0x3000||value==0xfeff;
}
template<class Checkpoint>
inline bool has_usable_text(std::string_view text,Checkpoint&& checkpoint){
    bool usable=false;
    visit_utf8(text,[&](const Utf8Scalar& scalar){
        // U+EE00 is PLaMo's invisible segment boundary. C0/C1 controls and DEL
        // cannot be displayed or represented as ordinary model input.
        const bool printable=scalar.value>=0x20&&scalar.value!=0x7f&&
            !(scalar.value>=0x80&&scalar.value<=0x9f);
        usable|=printable&&!unicode_space(scalar.value)&&scalar.value!=0xfffd&&scalar.value!=0xee00;
    },std::forward<Checkpoint>(checkpoint));
    // Do not return early: an invalid suffix must still be rejected.
    return usable;
}
inline bool has_usable_text(std::string_view text){
    return has_usable_text(text,[]{});
}
// Native text widgets cannot faithfully display embedded NUL or terminal
// controls. Keep ordinary whitespace and the original generated UTF-8 intact.
inline std::size_t unsupported_text_control(std::string_view text){
    for(std::size_t i=0;i<text.size();++i){
        const auto c=static_cast<unsigned char>(text[i]);
        if((c<0x20&&c!='\t'&&c!='\n'&&c!='\r')||c==0x7f)return i;
    }
    return std::string_view::npos;
}
inline bool valid_translation(std::string_view text){
    return unsupported_text_control(text)==std::string_view::npos&&has_usable_text(text);
}
}
