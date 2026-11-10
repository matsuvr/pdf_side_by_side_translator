#include "core/types.hpp"
#include <cmath>
#include <stdexcept>

namespace opt {
Affine2D Affine2D::inverse() const {
    const double det=a*d-b*c;
    if(!std::isfinite(det)||det==0)throw std::runtime_error("Singular coordinate transform");
    return {d/det,-b/det,-c/det,a/det,(c*ty-d*tx)/det,(b*tx-a*ty)/det};
}
std::string encode_utf8(char32_t c) {
    if(c>0x10ffff||(c>=0xd800&&c<=0xdfff))c=0xfffd;
    std::string out;
    if(c<0x80)out+=static_cast<char>(c);
    else if(c<0x800){out+=static_cast<char>(0xc0|(c>>6));out+=static_cast<char>(0x80|(c&63));}
    else if(c<0x10000){out+=static_cast<char>(0xe0|(c>>12));out+=static_cast<char>(0x80|((c>>6)&63));out+=static_cast<char>(0x80|(c&63));}
    else {out+=static_cast<char>(0xf0|(c>>18));out+=static_cast<char>(0x80|((c>>12)&63));out+=static_cast<char>(0x80|((c>>6)&63));out+=static_cast<char>(0x80|(c&63));}
    return out;
}
}
