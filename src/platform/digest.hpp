#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace opt {
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&)=delete;
    Sha256& operator=(const Sha256&)=delete;
    void update(std::span<const std::byte> bytes);
    void update(std::string_view text);
    std::string finish();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
std::string sha256(std::string_view text);
std::string new_uuid();
// Field names, types, lengths and integer byte order are part of the format.
class CanonicalKey {
public:
    CanonicalKey();
    CanonicalKey& text(std::string_view name,std::string_view value);
    CanonicalKey& number(std::string_view name,std::uint64_t value);
    CanonicalKey& signed_number(std::string_view name,std::int64_t value);
    CanonicalKey& bytes(std::string_view name,std::span<const std::byte> value);
    std::string finish(){return digest_.finish();}
private:
    Sha256 digest_;
    void field(std::string_view name,std::uint8_t type,std::span<const std::byte> value);
};
}
