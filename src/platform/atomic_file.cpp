#include "platform/atomic_file.hpp"
#include "platform/access_path.hpp"
#include "platform/digest.hpp"
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <streambuf>
#include <system_error>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/acl.h>
#endif
#endif

namespace opt {
namespace {
[[noreturn]] void file_error(const char* message){
#ifdef _WIN32
    throw std::system_error(GetLastError(),std::system_category(),message);
#else
    throw std::system_error(errno,std::generic_category(),message);
#endif
}
class FileBuffer final:public std::streambuf {
    std::FILE* file_;
    std::streamsize xsputn(const char* data,std::streamsize size)override{return static_cast<std::streamsize>(std::fwrite(data,1,static_cast<std::size_t>(size),file_));}
    int_type overflow(int_type value)override{return traits_type::eq_int_type(value,traits_type::eof())?traits_type::not_eof(value):(std::fputc(traits_type::to_char_type(value),file_)==EOF?traits_type::eof():value);}
    int sync()override{return std::fflush(file_);}
public:
    explicit FileBuffer(std::FILE* file):file_(file){}
};
struct FileCloser {void operator()(std::FILE* file)const noexcept{std::fclose(file);}};
#ifdef _WIN32
struct Handle {HANDLE value=INVALID_HANDLE_VALUE;~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}};
struct LocalMemory {void* value=nullptr;~LocalMemory(){if(value)LocalFree(value);}};
// Protect the staging DACL from parent-folder inheritance at CreateFile time.
HANDLE private_file(const std::filesystem::path& path,DWORD attributes){
    Handle token;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value))file_error("Export token");
    DWORD size=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&size);
    if(!size)file_error("Export owner");
    std::vector<std::byte> user(size);
    if(!GetTokenInformation(token.value,TokenUser,user.data(),size,&size))file_error("Export owner");
    EXPLICIT_ACCESSW entry{};entry.grfAccessPermissions=GENERIC_ALL;entry.grfAccessMode=SET_ACCESS;
    entry.Trustee.TrusteeForm=TRUSTEE_IS_SID;entry.Trustee.TrusteeType=TRUSTEE_IS_USER;
    entry.Trustee.ptstrName=reinterpret_cast<LPWSTR>(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid);
    PACL acl=nullptr;const auto error=SetEntriesInAclW(1,&entry,nullptr,&acl);LocalMemory owner{acl};
    if(error!=ERROR_SUCCESS)throw std::system_error(error,std::system_category(),"Private export permissions");
    SECURITY_DESCRIPTOR descriptor{};
    if(!InitializeSecurityDescriptor(&descriptor,SECURITY_DESCRIPTOR_REVISION)||!SetSecurityDescriptorDacl(&descriptor,TRUE,acl,FALSE)||
       !SetSecurityDescriptorControl(&descriptor,SE_DACL_PROTECTED,SE_DACL_PROTECTED))file_error("Private export permissions");
    SECURITY_ATTRIBUTES security{sizeof(security),&descriptor,FALSE};
    return CreateFileW(access_path(path).c_str(),GENERIC_READ|GENERIC_WRITE|WRITE_DAC|WRITE_OWNER,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,&security,CREATE_NEW,attributes,nullptr);
}
bool encrypted_file(const std::filesystem::path& path){
    const auto attributes=GetFileAttributesW(access_path(path).c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES){const auto error=GetLastError();if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return false;file_error("Read export encryption attribute");}
    return (attributes&FILE_ATTRIBUTE_ENCRYPTED)!=0;
}
#elif defined(__APPLE__)
struct FileSecurity {filesec_t value=filesec_init();~FileSecurity(){if(value)filesec_free(value);}};
struct Acl {acl_t value=acl_init(0);~Acl(){if(value)acl_free(value);}};
int private_file(const std::filesystem::path& path){
    FileSecurity security;Acl acl;acl_flagset_t flags=nullptr;mode_t mode=0600;
    if(!security.value||!acl.value||acl_get_flagset_np(acl.value,&flags)||acl_add_flag_np(flags,ACL_FLAG_NO_INHERIT)||
       filesec_set_property(security.value,FILESEC_ACL,&acl.value)||filesec_set_property(security.value,FILESEC_MODE,&mode))file_error("Private export permissions");
    return openx_np(path.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,security.value);
}
#endif
}
struct AtomicFile::Impl {
    std::filesystem::path destination,temporary;
    bool published=false;
#ifdef _WIN32
    HANDLE file=INVALID_HANDLE_VALUE;
    HANDLE cleanup=INVALID_HANDLE_VALUE;
    bool encrypted=false;
#else
    int file=-1;
#endif
    explicit Impl(std::filesystem::path target):destination(std::move(target)),temporary(destination){temporary+=".tmp-"+new_uuid();}
    ~Impl(){
#ifdef _WIN32
        if(file!=INVALID_HANDLE_VALUE){
            // A copied destination DACL must not prevent failed-export cleanup.
            if(!published){
                if(cleanup!=INVALID_HANDLE_VALUE){FILE_DISPOSITION_INFO discard{TRUE};SetFileInformationByHandle(cleanup,FileDispositionInfo,&discard,sizeof(discard));}
                else DeleteFileW(access_path(temporary).c_str());
            }
            if(cleanup!=INVALID_HANDLE_VALUE)CloseHandle(cleanup);
            CloseHandle(file);
        }
#else
        if(file>=0){if(!published)::unlink(temporary.c_str());::close(file);}
#endif
    }
    void create(){
#ifdef _WIN32
        const bool retain_encryption=encrypted_file(destination);
        file=private_file(temporary,retain_encryption?FILE_ATTRIBUTE_ENCRYPTED:FILE_ATTRIBUTE_NORMAL);
        if(file==INVALID_HANDLE_VALUE)file_error("Create export staging file");
        BY_HANDLE_FILE_INFORMATION info{};if(!GetFileInformationByHandle(file,&info))file_error("Export staging attributes");
        encrypted=(info.dwFileAttributes&FILE_ATTRIBUTE_ENCRYPTED)!=0;
        if(retain_encryption&&!encrypted)throw std::runtime_error("Existing export encryption cannot be preserved");
#elif defined(__APPLE__)
        file=private_file(temporary);if(file<0)file_error("Create export staging file");
#else
        file=::open(temporary.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0600);if(file<0)file_error("Create export staging file");
#endif
#ifndef _WIN32
        if(fchmod(file,0600))file_error("Private export mode");
#endif
    }
    std::FILE* stream(){
#ifdef _WIN32
        HANDLE duplicate=nullptr;
        if(!DuplicateHandle(GetCurrentProcess(),file,GetCurrentProcess(),&duplicate,0,FALSE,DUPLICATE_SAME_ACCESS))file_error("Export stream");
        const auto fd=_open_osfhandle(reinterpret_cast<intptr_t>(duplicate),_O_RDWR|_O_BINARY);
        if(fd<0){CloseHandle(duplicate);throw std::system_error(errno,std::generic_category(),"Export stream");}
        auto result=_fdopen(fd,"w+b");if(!result){_close(fd);throw std::system_error(errno,std::generic_category(),"Export stream");}
#else
        const auto fd=fcntl(file,F_DUPFD_CLOEXEC,0);if(fd<0)file_error("Export stream");
        auto result=fdopen(fd,"w+b");if(!result){const auto error=errno;close(fd);throw std::system_error(error,std::generic_category(),"Export stream");}
#endif
        return result;
    }
    void preserve_access(){
#ifdef _WIN32
        if(encrypted_file(destination)&&!encrypted)throw std::runtime_error("Export encryption changed while writing; retry the export");
        // SQLite opens its second handle without FILE_SHARE_DELETE. Acquire
        // DELETE access only after the writer/SQLite connection has closed.
        cleanup=ReOpenFile(file,DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,0);
        if(cleanup==INVALID_HANDLE_VALUE)file_error("Export cleanup handle");
        DWORD filesystem_flags=0;
        // FAT/exFAT has no per-file ACL to carry across a normal overwrite.
        if(GetVolumeInformationByHandleW(file,nullptr,0,nullptr,nullptr,&filesystem_flags,nullptr,0)&&
           !(filesystem_flags&FILE_PERSISTENT_ACLS))return;
        PACL acl=nullptr;PSID owner=nullptr,group=nullptr;PSECURITY_DESCRIPTOR raw=nullptr;
        constexpr auto information=OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION;
        const auto error=GetNamedSecurityInfoW(const_cast<LPWSTR>(access_path(destination).c_str()),SE_FILE_OBJECT,information,&owner,&group,&acl,nullptr,&raw);
        LocalMemory descriptor{raw};
        if(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND)return;
        if(error!=ERROR_SUCCESS)throw std::system_error(error,std::system_category(),"Read export access controls");
        SECURITY_DESCRIPTOR_CONTROL control=0;DWORD revision=0;
        if(!GetSecurityDescriptorControl(raw,&control,&revision))file_error("Read export access controls");
        const auto flags=information|((control&SE_DACL_PROTECTED)?PROTECTED_DACL_SECURITY_INFORMATION:UNPROTECTED_DACL_SECURITY_INFORMATION);
        const auto applied=SetSecurityInfo(file,SE_FILE_OBJECT,flags,owner,group,acl,nullptr);
        if(applied!=ERROR_SUCCESS)throw std::system_error(applied,std::system_category(),"Preserve export access controls");
#else
        struct stat original{};
#ifdef __APPLE__
        FileSecurity security;if(!security.value)file_error("Export access controls");
        const auto result=statx_np(destination.c_str(),&original,security.value);
#else
        const auto result=::stat(destination.c_str(),&original);
#endif
        if(result<0){if(errno==ENOENT)return;file_error("Read export access controls");}
        if(!S_ISREG(original.st_mode))throw std::runtime_error("Export destination is not a regular file");
#ifdef __APPLE__
        const mode_t mode=original.st_mode&0777;
        if(filesec_set_property(security.value,FILESEC_MODE,&mode)||fchmodx_np(file,security.value))file_error("Preserve export access controls");
#else
        struct stat staging{};if(fstat(file,&staging))file_error("Export access controls");
        if((staging.st_uid!=original.st_uid||staging.st_gid!=original.st_gid)&&fchown(file,original.st_uid,original.st_gid))file_error("Preserve export ownership");
        if(fchmod(file,original.st_mode&0777))file_error("Preserve export mode");
#endif
#endif
    }
};
AtomicFile::AtomicFile(std::filesystem::path destination):impl_(std::make_unique<Impl>(std::move(destination))){impl_->create();}
AtomicFile::~AtomicFile()=default;
const std::filesystem::path& AtomicFile::temporary_path()const{return impl_->temporary;}
void AtomicFile::write(const std::function<void(std::ostream&)>& writer){
    std::unique_ptr<std::FILE,FileCloser> file(impl_->stream());
    FileBuffer buffer(file.get());std::ostream out(&buffer);writer(out);out.flush();
    if(!out)throw std::runtime_error("Export write failed");
    if(std::fclose(file.release()))throw std::system_error(errno,std::generic_category(),"Export close failed");
}
void AtomicFile::publish(const std::function<void()>& before_replace){
    impl_->preserve_access();
#ifdef _WIN32
    if(!FlushFileBuffers(impl_->file))file_error("Flush export");
#else
    if(fsync(impl_->file))file_error("Flush export");
#endif
    if(before_replace)before_replace();
#ifdef _WIN32
    // Copy the DACL explicitly: ReplaceFileW can remove the old destination on
    // some failure paths when no backup filename is supplied.
    if(!MoveFileExW(access_path(impl_->temporary).c_str(),access_path(impl_->destination).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))file_error("Replace export");
#else
    if(::rename(impl_->temporary.c_str(),impl_->destination.c_str()))file_error("Replace export");
#endif
    impl_->published=true;
}
}
