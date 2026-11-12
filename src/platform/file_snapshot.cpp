#include "platform/file_snapshot.hpp"
#include "platform/access_path.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace opt {
struct FileSnapshot::Impl {
    FileSignature original;
#ifdef _WIN32
    HANDLE file=INVALID_HANDLE_VALUE;
    ~Impl(){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
    FileSignature signature() const {
        BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(file,&info))throw std::system_error(GetLastError(),std::system_category(),"File identity");
        if(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)throw std::runtime_error("通常のファイルを指定してください。");
        const auto timestamp=(static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime;
        return {std::to_string(info.dwVolumeSerialNumber)+":"+std::to_string(info.nFileIndexHigh)+":"+std::to_string(info.nFileIndexLow),
            (static_cast<std::uint64_t>(info.nFileSizeHigh)<<32)|info.nFileSizeLow,
            static_cast<std::int64_t>((timestamp-116444736000000000ULL)*100)};
    }
#else
    int file=-1;
    ~Impl(){if(file>=0)close(file);}
    FileSignature signature() const {
        struct stat info{};
        if(fstat(file,&info)<0)throw std::system_error(errno,std::generic_category(),"File identity");
        if(!S_ISREG(info.st_mode))throw std::runtime_error("通常のファイルを指定してください。");
        return {std::to_string(info.st_dev)+":"+std::to_string(info.st_ino),static_cast<std::uint64_t>(info.st_size),
            static_cast<std::int64_t>(info.st_mtimespec.tv_sec)*1000000000+info.st_mtimespec.tv_nsec};
    }
#endif
};
FileSnapshot::FileSnapshot(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
FileSnapshot::~FileSnapshot()=default;
std::shared_ptr<const FileSnapshot> FileSnapshot::open(const std::filesystem::path& path){
    auto impl=std::make_unique<Impl>();
#ifdef _WIN32
    impl->file=CreateFileW(access_path(path).c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OVERLAPPED|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(impl->file==INVALID_HANDLE_VALUE)throw std::system_error(GetLastError(),std::system_category(),"Open file");
    if(GetFileType(impl->file)!=FILE_TYPE_DISK)throw std::runtime_error("通常のファイルを指定してください。");
#else
    // A PDF/model needs seekable bytes and a stable length. Opening a FIFO
    // without O_NONBLOCK can wait forever before fstat can reject it.
    impl->file=::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NONBLOCK);
    if(impl->file<0)throw std::system_error(errno,std::generic_category(),"Open file");
#endif
    impl->original=impl->signature();
    return std::shared_ptr<const FileSnapshot>(new FileSnapshot(std::move(impl)));
}
std::shared_ptr<const FileSnapshot> FileSnapshot::duplicate(std::intptr_t native,FileSignature original){
    auto impl=std::make_unique<Impl>();
#ifdef _WIN32
    impl->file=ReOpenFile(reinterpret_cast<HANDLE>(native),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,FILE_FLAG_OVERLAPPED);
    if(impl->file==INVALID_HANDLE_VALUE)throw std::system_error(GetLastError(),std::system_category(),"Duplicate file object");
#else
    impl->file=fcntl(static_cast<int>(native),F_DUPFD_CLOEXEC,0);
    if(impl->file<0)throw std::system_error(errno,std::generic_category(),"Duplicate file object");
#endif
    impl->original=std::move(original);
    return std::shared_ptr<const FileSnapshot>(new FileSnapshot(std::move(impl)));
}
FileSignature FileSnapshot::signature() const{return impl_->signature();}
const FileSignature& FileSnapshot::initial_signature() const{return impl_->original;}
std::size_t FileSnapshot::read_at(std::uint64_t offset,std::span<std::byte> output) const{
    if(output.empty())return 0;
    if(offset>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))throw std::overflow_error("File offset overflow");
    const auto count=std::min<std::size_t>(output.size(),1024*1024);
#ifdef _WIN32
    OVERLAPPED operation{};operation.Offset=static_cast<DWORD>(offset);operation.OffsetHigh=static_cast<DWORD>(offset>>32);
    DWORD received=0;
    if(!ReadFile(impl_->file,output.data(),static_cast<DWORD>(count),&received,&operation)){
        auto error=GetLastError();if(error==ERROR_HANDLE_EOF)return 0;
        if(error!=ERROR_IO_PENDING)throw std::system_error(error,std::system_category(),"Read file snapshot");
        if(!GetOverlappedResult(impl_->file,&operation,&received,TRUE))throw std::system_error(GetLastError(),std::system_category(),"Read file snapshot");
    }
    return received;
#else
    ssize_t received;
    do {received=pread(impl_->file,output.data(),count,static_cast<off_t>(offset));}while(received<0&&errno==EINTR);
    if(received<0)throw std::system_error(errno,std::generic_category(),"Read file snapshot");
    return static_cast<std::size_t>(received);
#endif
}
std::FILE* FileSnapshot::open_stream() const{
#ifdef _WIN32
    auto handle=ReOpenFile(impl_->file,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,0);
    if(handle==INVALID_HANDLE_VALUE)throw std::system_error(GetLastError(),std::system_category(),"Open model stream");
    const int descriptor=_open_osfhandle(reinterpret_cast<intptr_t>(handle),_O_RDONLY|_O_BINARY);
    if(descriptor<0){CloseHandle(handle);throw std::system_error(errno,std::generic_category(),"Open model stream");}
    auto stream=_fdopen(descriptor,"rb");
    if(!stream){_close(descriptor);throw std::system_error(errno,std::generic_category(),"Open model stream");}
#else
    const int descriptor=fcntl(impl_->file,F_DUPFD_CLOEXEC,0);
    if(descriptor<0)throw std::system_error(errno,std::generic_category(),"Open model stream");
    auto stream=fdopen(descriptor,"rb");
    if(!stream){close(descriptor);throw std::system_error(errno,std::generic_category(),"Open model stream");}
#endif
    return stream;
}
}
