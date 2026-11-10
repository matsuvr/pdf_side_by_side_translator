#pragma once
#include "platform/file_signature.hpp"
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>

namespace opt {
class MappedFile;
// Owns an open read-only file object. Path replacement cannot redirect reads.
class FileSnapshot {
public:
    static std::shared_ptr<const FileSnapshot> open(const std::filesystem::path& path);
    ~FileSnapshot();
    FileSnapshot(const FileSnapshot&)=delete;
    FileSnapshot& operator=(const FileSnapshot&)=delete;
    FileSignature signature() const;
    const FileSignature& initial_signature() const;
    std::size_t read_at(std::uint64_t offset,std::span<std::byte> output) const;
    // Caller owns and closes the stream. Hash reads never share its seek position.
    std::FILE* open_stream() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit FileSnapshot(std::unique_ptr<Impl> impl);
    static std::shared_ptr<const FileSnapshot> duplicate(std::intptr_t native,FileSignature original);
    friend class MappedFile;
};
}
