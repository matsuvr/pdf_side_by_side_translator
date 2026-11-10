#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include "platform/file_signature.hpp"

namespace opt {
class FileSnapshot;
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
    std::shared_ptr<const FileSnapshot> snapshot() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
