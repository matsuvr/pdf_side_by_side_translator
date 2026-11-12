#pragma once
#include <string>

namespace opt {
inline void clear_secret(std::string& value) noexcept {
    // Volatile writes survive dead-store elimination, including for short strings.
    volatile char* bytes=value.data();
    for(std::size_t i=0;i<value.size();++i)bytes[i]=0;
    value.clear();
}
}
