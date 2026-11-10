#pragma once
#include <cstdint>
#include <string>
namespace opt {
struct FileSignature {
    std::string identity;
    std::uint64_t size=0;
    std::int64_t mtime_ns=0;
    bool operator==(const FileSignature&) const = default;
};
}
