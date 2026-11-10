#pragma once
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
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
inline std::vector<Utf8Scalar> utf8_scalars(std::string_view text){
    std::vector<Utf8Scalar> result;
    for(std::size_t i=0;i<text.size();){
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
        result.push_back({i,i+count,value});i+=count;
    }
    return result;
}
inline bool unicode_space(char32_t value){
    return value==0x20||(value>=9&&value<=13)||value==0x85||value==0xa0||value==0x1680||
        (value>=0x2000&&value<=0x200a)||value==0x2028||value==0x2029||value==0x202f||value==0x205f||value==0x3000||value==0xfeff;
}
inline bool has_usable_text(std::string_view text){
    for(const auto scalar:utf8_scalars(text))if(!unicode_space(scalar.value)&&scalar.value!=0xfffd)return true;
    return false;
}
}
