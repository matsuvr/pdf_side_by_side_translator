#pragma once
#include "platform/file_signature.hpp"
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <sys/stat.h>
#endif

namespace opt {
// Keep the native fields for repeated comparisons on an open file. Format the
// identity only when a FileSignature is needed by callers or cache keys.
struct OpenFileStatus {
#ifdef _WIN32
    DWORD volume=0,index_high=0,index_low=0;
#else
    dev_t device=0;
    ino_t inode=0;
#endif
    std::uint64_t size=0;
    std::int64_t mtime_ns=0;

    bool operator==(const OpenFileStatus&) const = default;

    FileSignature signature() const {
#ifdef _WIN32
        return {std::to_string(volume)+":"+std::to_string(index_high)+":"+std::to_string(index_low),size,mtime_ns};
#else
        return {std::to_string(device)+":"+std::to_string(inode),size,mtime_ns};
#endif
    }
};

#ifdef _WIN32
inline OpenFileStatus open_file_status(HANDLE file) {
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(file,&info))throw std::system_error(GetLastError(),std::system_category(),"File identity");
    if(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)throw std::runtime_error("通常のファイルを指定してください。");
    const auto timestamp=(static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime;
    return {info.dwVolumeSerialNumber,info.nFileIndexHigh,info.nFileIndexLow,
        (static_cast<std::uint64_t>(info.nFileSizeHigh)<<32)|info.nFileSizeLow,
        static_cast<std::int64_t>((timestamp-116444736000000000ULL)*100)};
}
#else
inline OpenFileStatus open_file_status(int file) {
    struct stat info{};
    if(fstat(file,&info)<0)throw std::system_error(errno,std::generic_category(),"File identity");
    if(!S_ISREG(info.st_mode))throw std::runtime_error("通常のファイルを指定してください。");
    return {info.st_dev,info.st_ino,static_cast<std::uint64_t>(info.st_size),
        static_cast<std::int64_t>(info.st_mtimespec.tv_sec)*1000000000+info.st_mtimespec.tv_nsec};
}
#endif
}
