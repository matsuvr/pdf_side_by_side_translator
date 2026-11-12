#include "platform/mapped_file.hpp"
#include "platform/access_path.hpp"
#include "platform/file_snapshot.hpp"
#include <limits>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace opt {
struct MappedFile::Impl {
    const std::byte* data=nullptr;
    std::size_t size=0;
    FileSignature original;
#ifdef _WIN32
    HANDLE file=INVALID_HANDLE_VALUE,mapping=nullptr;
    FileSignature signature() const {
        BY_HANDLE_FILE_INFORMATION s{};
        if(!GetFileInformationByHandle(file,&s))throw std::system_error(GetLastError(),std::system_category(),"File identity");
        if(s.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)throw std::runtime_error("通常のファイルを指定してください。");
        const auto timestamp=(static_cast<std::uint64_t>(s.ftLastWriteTime.dwHighDateTime)<<32)|s.ftLastWriteTime.dwLowDateTime;
        return {std::to_string(s.dwVolumeSerialNumber)+":"+std::to_string(s.nFileIndexHigh)+":"+std::to_string(s.nFileIndexLow),
            (static_cast<std::uint64_t>(s.nFileSizeHigh)<<32)|s.nFileSizeLow,
            static_cast<std::int64_t>((timestamp-116444736000000000ULL)*100)};
    }
    ~Impl(){if(data)UnmapViewOfFile(data);if(mapping)CloseHandle(mapping);if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
#else
    int file=-1;
    FileSignature signature() const {
        struct stat s{};
        if(fstat(file,&s)<0)throw std::system_error(errno,std::generic_category(),"File identity");
        if(!S_ISREG(s.st_mode))throw std::runtime_error("通常のファイルを指定してください。");
        return {std::to_string(s.st_dev)+":"+std::to_string(s.st_ino),static_cast<std::uint64_t>(s.st_size),
            static_cast<std::int64_t>(s.st_mtimespec.tv_sec)*1000000000+s.st_mtimespec.tv_nsec};
    }
    ~Impl(){if(data)munmap(const_cast<std::byte*>(data),size);if(file>=0)close(file);}
#endif
};
MappedFile::MappedFile(const std::filesystem::path& path):impl_(std::make_unique<Impl>()) {
    auto& p=*impl_;
#ifdef _WIN32
    p.file=CreateFileW(access_path(path).c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(p.file==INVALID_HANDLE_VALUE)throw std::system_error(GetLastError(),std::system_category(),"Open file");
    if(GetFileType(p.file)!=FILE_TYPE_DISK)throw std::runtime_error("通常のファイルを指定してください。");
#else
    // Reject FIFOs/devices before attempting a mapping, without waiting for a
    // writer to connect to an input merely named like a regular resource.
    p.file=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NONBLOCK);
    if(p.file<0)throw std::system_error(errno,std::generic_category(),"Open file");
#endif
    p.original=p.signature();
    if(p.original.size==0)throw std::runtime_error("空のファイルです。");
    if(p.original.size>std::numeric_limits<std::size_t>::max())throw std::runtime_error("ファイルをこのアドレス空間へ対応付けられません。");
    p.size=static_cast<std::size_t>(p.original.size);
#ifdef _WIN32
    p.mapping=CreateFileMappingW(p.file,nullptr,PAGE_READONLY,0,0,nullptr);
    if(!p.mapping)throw std::system_error(GetLastError(),std::system_category(),"Create mapping");
    p.data=static_cast<const std::byte*>(MapViewOfFile(p.mapping,FILE_MAP_READ,0,0,0));
    if(!p.data)throw std::system_error(GetLastError(),std::system_category(),"Map file");
#else
    auto result=mmap(nullptr,p.size,PROT_READ,MAP_PRIVATE,p.file,0);
    if(result==MAP_FAILED)throw std::system_error(errno,std::generic_category(),"Map file");
    p.data=static_cast<const std::byte*>(result);
#endif
}
MappedFile::~MappedFile()=default;
std::span<const std::byte> MappedFile::bytes() const noexcept{return {impl_->data,impl_->size};}
FileSignature MappedFile::signature() const{return impl_->signature();}
const FileSignature& MappedFile::initial_signature() const{return impl_->original;}
std::shared_ptr<const FileSnapshot> MappedFile::snapshot() const{
#ifdef _WIN32
    return FileSnapshot::duplicate(reinterpret_cast<std::intptr_t>(impl_->file),impl_->original);
#else
    return FileSnapshot::duplicate(impl_->file,impl_->original);
#endif
}
}
