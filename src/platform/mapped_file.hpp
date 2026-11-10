#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace opt {
struct FileSignature {
    std::string identity;
    std::uint64_t size=0;
    std::int64_t mtime_ns=0;
    bool operator==(const FileSignature&) const = default;
};
// A read-only mapping and its open file object have one shared lifetime.
class MappedFile {
public:
    explicit MappedFile(const std::filesystem::path& path);
    ~MappedFile();
    MappedFile(const MappedFile&)=delete;
    MappedFile& operator=(const MappedFile&)=delete;
    std::span<const std::byte> bytes() const noexcept;
    FileSignature signature() const;
    const FileSignature& initial_signature() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
