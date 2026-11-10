#include "platform/digest.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#else
#include <CommonCrypto/CommonDigest.h>
#include <uuid/uuid.h>
#endif

namespace opt {
namespace {
std::string hex(std::span<const unsigned char> bytes){
    constexpr char digits[]="0123456789abcdef";std::string result;result.reserve(bytes.size()*2);
    for(auto b:bytes){result+=digits[b>>4];result+=digits[b&15];}return result;
}
std::array<std::byte,8> little_endian(std::uint64_t value){
    std::array<std::byte,8> bytes{};for(unsigned i=0;i<8;++i)bytes[i]=static_cast<std::byte>((value>>(i*8))&255);return bytes;
}
}
struct Sha256::Impl {
    bool finished=false;
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    BCRYPT_HASH_HANDLE hash=nullptr;
    std::vector<unsigned char> object;
    Impl(){
        try{
            if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("SHA-256 provider unavailable");
            DWORD size=0,returned=0;
            if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&returned,0)<0)throw std::runtime_error("SHA-256 object size unavailable");
            object.resize(size);
            if(BCryptCreateHash(algorithm,&hash,object.data(),size,nullptr,0,0)<0)throw std::runtime_error("Cannot create SHA-256");
        }catch(...){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);throw;}
    }
    ~Impl(){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}
#else
    CC_SHA256_CTX context{};
    Impl(){if(CC_SHA256_Init(&context)!=1)throw std::runtime_error("Cannot create SHA-256");}
#endif
};
Sha256::Sha256():impl_(std::make_unique<Impl>()){}
Sha256::~Sha256()=default;
void Sha256::update(std::span<const std::byte> bytes){
    if(impl_->finished)throw std::logic_error("Digest already finalized");
    while(!bytes.empty()){
        const auto count=std::min<std::size_t>(bytes.size(),1024*1024);
#ifdef _WIN32
        if(BCryptHashData(impl_->hash,const_cast<PUCHAR>(reinterpret_cast<const unsigned char*>(bytes.data())),static_cast<ULONG>(count),0)<0)throw std::runtime_error("SHA-256 update failed");
#else
        if(CC_SHA256_Update(&impl_->context,bytes.data(),static_cast<CC_LONG>(count))!=1)throw std::runtime_error("SHA-256 update failed");
#endif
        bytes=bytes.subspan(count);
    }
}
void Sha256::update(std::string_view text){update(std::as_bytes(std::span(text.data(),text.size())));}
std::string Sha256::finish(){
    if(impl_->finished)throw std::logic_error("Digest already finalized");
    std::array<unsigned char,32> value{};
#ifdef _WIN32
    if(BCryptFinishHash(impl_->hash,value.data(),static_cast<ULONG>(value.size()),0)<0)throw std::runtime_error("SHA-256 finalization failed");
#else
    if(CC_SHA256_Final(value.data(),&impl_->context)!=1)throw std::runtime_error("SHA-256 finalization failed");
#endif
    impl_->finished=true;return hex(value);
}
std::string sha256(std::string_view text){Sha256 digest;digest.update(text);return digest.finish();}
std::string new_uuid(){
    std::array<unsigned char,16> bytes{};
#ifdef _WIN32
    if(BCryptGenRandom(nullptr,bytes.data(),static_cast<ULONG>(bytes.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)throw std::runtime_error("OS random source unavailable");
#else
    uuid_generate_random(bytes.data());
#endif
    bytes[6]=static_cast<unsigned char>((bytes[6]&15)|0x40);bytes[8]=static_cast<unsigned char>((bytes[8]&63)|0x80);
    const auto raw=hex(bytes);return raw.substr(0,8)+'-'+raw.substr(8,4)+'-'+raw.substr(12,4)+'-'+raw.substr(16,4)+'-'+raw.substr(20);
}
CanonicalKey::CanonicalKey(){digest_.update(std::string_view("OPT-canonical-key\0v1",20));}
void CanonicalKey::field(std::string_view name,std::uint8_t type,std::span<const std::byte> value){
    digest_.update(little_endian(name.size()));digest_.update(name);
    const std::array<std::byte,1> tag{static_cast<std::byte>(type)};digest_.update(tag);
    digest_.update(little_endian(value.size()));digest_.update(value);
}
CanonicalKey& CanonicalKey::text(std::string_view name,std::string_view value){field(name,1,std::as_bytes(std::span(value.data(),value.size())));return *this;}
CanonicalKey& CanonicalKey::number(std::string_view name,std::uint64_t value){field(name,2,little_endian(value));return *this;}
CanonicalKey& CanonicalKey::signed_number(std::string_view name,std::int64_t value){field(name,3,little_endian(std::bit_cast<std::uint64_t>(value)));return *this;}
CanonicalKey& CanonicalKey::bytes(std::string_view name,std::span<const std::byte> value){field(name,4,value);return *this;}
}
