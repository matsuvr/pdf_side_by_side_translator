#include "platform/atomic_file.hpp"
#include "platform/digest.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
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
#include <sddl.h>
#else
#include <csignal>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <membership.h>
#include <sys/acl.h>
#endif
#endif

namespace fs=std::filesystem;
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
std::string bytes(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
#ifdef _WIN32
struct NativeHandle {HANDLE value=INVALID_HANDLE_VALUE;~NativeHandle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}};
struct LocalAllocation {void* value=nullptr;~LocalAllocation(){if(value)LocalFree(value);}};
bool private_staging_access(const fs::path& path){
    NativeHandle file{CreateFileW(path.c_str(),READ_CONTROL|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr)};
    require(file.value!=INVALID_HANDLE_VALUE,"open staging access controls");
    DWORD filesystem_flags=0;
    if(!GetVolumeInformationByHandleW(file.value,nullptr,0,nullptr,nullptr,&filesystem_flags,nullptr,0)){
        const auto error=GetLastError();std::cout<<"SKIP Windows ACL checks: volume capability query unavailable (error "<<error<<")\n";return false;
    }
    if(!(filesystem_flags&FILE_PERSISTENT_ACLS)){
        std::cout<<"SKIP Windows ACL checks: volume has no persistent ACLs\n";return false;
    }
    PACL acl=nullptr;PSECURITY_DESCRIPTOR raw=nullptr;
    const auto status=GetSecurityInfo(file.value,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&acl,nullptr,&raw);
    LocalAllocation descriptor{raw};require(status==ERROR_SUCCESS&&acl&&IsValidAcl(acl),"read private staging DACL");
    SECURITY_DESCRIPTOR_CONTROL control=0;DWORD revision=0;
    require(GetSecurityDescriptorControl(raw,&control,&revision)&&(control&SE_DACL_PROTECTED),"staging DACL blocks inherited grants before content is written");
    require(acl->AceCount==1,"staging DACL has no unrelated access grants");
    void* raw_entry=nullptr;require(GetAce(acl,0,&raw_entry),"read staging grant");
    const auto* entry=static_cast<const ACCESS_ALLOWED_ACE*>(raw_entry);
    require(entry->Header.AceType==ACCESS_ALLOWED_ACE_TYPE&&!(entry->Header.AceFlags&INHERITED_ACE),"staging has one explicit allow grant");
    NativeHandle token;require(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value),"read current user token");
    DWORD size=0;GetTokenInformation(token.value,TokenUser,nullptr,0,&size);require(size!=0,"read current user size");
    std::vector<std::byte> user(size);require(GetTokenInformation(token.value,TokenUser,user.data(),size,&size),"read current user SID");
    require(EqualSid(const_cast<DWORD*>(&entry->SidStart),reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid),"only the current user receives staging access");
    require((entry->Mask&GENERIC_ALL)||(entry->Mask&FILE_ALL_ACCESS)==FILE_ALL_ACCESS,"current user has full staging access");
    return true;
}
std::wstring access(const fs::path& path){
    PSECURITY_DESCRIPTOR raw=nullptr;
    constexpr auto information=OWNER_SECURITY_INFORMATION|GROUP_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION;
    require(GetNamedSecurityInfoW(path.c_str(),SE_FILE_OBJECT,information,nullptr,nullptr,nullptr,nullptr,&raw)==ERROR_SUCCESS,"read access controls");
    LocalAllocation descriptor{raw};
    // SetSecurityInfo can mark a DACL as auto-inherited even when the ACL is
    // protected and its entries are unchanged. Ignore only that bookkeeping bit.
    require(SetSecurityDescriptorControl(raw,SE_DACL_AUTO_INHERITED,0),"normalize DACL inheritance metadata");
    LPWSTR text=nullptr;
    require(ConvertSecurityDescriptorToStringSecurityDescriptorW(raw,SDDL_REVISION_1,information,&text,nullptr),"format access controls");
    LocalAllocation formatted{text};
    return std::wstring(text);
}
void distinct_access(const fs::path& path){
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;OW)(A;;GR;;;SY)",SDDL_REVISION_1,&descriptor,nullptr),"fixture DACL");
    const bool ok=SetFileSecurityW(path.c_str(),DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,descriptor);
    LocalFree(descriptor);require(ok,"set fixture DACL");
}
#elif defined(__APPLE__)
std::string access(const fs::path& path){
    auto acl=acl_get_file(path.c_str(),ACL_TYPE_EXTENDED);require(acl!=nullptr,"read ACL");
    auto text=acl_to_text(acl,nullptr);acl_free(acl);require(text!=nullptr,"format ACL");
    std::string result(text);acl_free(text);return result;
}
void distinct_access(const fs::path& path){
    auto acl=acl_init(1);require(acl!=nullptr,"fixture ACL");acl_entry_t entry;acl_permset_t permissions;
    uuid_t owner;require(mbr_uid_to_uuid(geteuid(),owner)==0,"fixture ACL owner");
    const bool ok=acl_create_entry(&acl,&entry)==0&&acl_set_tag_type(entry,ACL_EXTENDED_DENY)==0&&
        acl_set_qualifier(entry,owner)==0&&acl_get_permset(entry,&permissions)==0&&
        acl_add_perm(permissions,ACL_EXECUTE)==0&&acl_set_file(path.c_str(),ACL_TYPE_EXTENDED,acl)==0;
    acl_free(acl);require(ok,"set fixture ACL");
}
#endif
int main(){
    const auto root=fs::temp_directory_path()/("opt-atomic-"+opt::new_uuid());
    fs::create_directory(root);
    struct Cleanup {fs::path root;~Cleanup(){std::error_code ignored;fs::remove_all(root,ignored);}} cleanup{root};
    try{
#ifdef _WIN32
        bool acl_supported=false;
#endif
#ifndef _WIN32
        struct Mask {mode_t old=umask(0);~Mask(){umask(old);}} mask;
#endif
        const auto target=root/fs::path(u8"機密の書き出し.txt");fs::path temporary;
        {
            opt::AtomicFile file(target);temporary=file.temporary_path();
#ifdef _WIN32
            acl_supported=private_staging_access(temporary);
#else
            require((fs::status(temporary).permissions()&fs::perms::all)==(fs::perms::owner_read|fs::perms::owner_write),"staging is owner-only before any content is written");
#endif
            file.write([](std::ostream& out){out<<"confidential new export";});
            require(!fs::exists(target)&&bytes(temporary)=="confidential new export","writing does not publish a partial destination");
            file.publish();
        }
        require(bytes(target)=="confidential new export"&&!fs::exists(temporary),"new export publishes complete bytes without a staging orphan");
#ifndef _WIN32
        require((fs::status(target).permissions()&fs::perms::all)==(fs::perms::owner_read|fs::perms::owner_write),"new export remains owner-only even with permissive umask");
        fs::permissions(target,fs::perms::owner_read|fs::perms::owner_write|fs::perms::group_read);
#endif
#ifdef _WIN32
        std::wstring original_access;
        if(acl_supported){
            const auto initial_access=access(target);
            distinct_access(target);
            original_access=access(target);
            require(original_access!=initial_access,"comparison detects changed access entries");
        }
#elif defined(__APPLE__)
        distinct_access(target);const auto original_access=access(target);
#endif
        {
            opt::AtomicFile file(target);temporary=file.temporary_path();
            file.write([](std::ostream& out){out<<"replaced content";});file.publish();
        }
        require(bytes(target)=="replaced content"&&!fs::exists(temporary),"existing export is replaceable");
#ifndef _WIN32
        require((fs::status(target).permissions()&fs::perms::all)==(fs::perms::owner_read|fs::perms::owner_write|fs::perms::group_read),"replacement retains destination mode");
#endif
#ifdef _WIN32
        if(acl_supported)require(access(target)==original_access,"replacement retains destination ACL");
#elif defined(__APPLE__)
        require(access(target)==original_access,"replacement retains destination ACL");
#endif
        bool failed=false;
        try{
            opt::AtomicFile file(target);temporary=file.temporary_path();
            file.write([](std::ostream& out){out<<"unpublished content";});
            file.publish([]{throw std::runtime_error("destination became an input");});
        }catch(const std::runtime_error&){failed=true;}
        require(failed&&bytes(target)=="replaced content"&&!fs::exists(temporary),"failed publication leaves original bytes and removes staging");
        failed=false;
        try{
            opt::AtomicFile file(target);temporary=file.temporary_path();
            file.write([](std::ostream& out){out<<"incomplete";out.setstate(std::ios::badbit);});
        }catch(const std::runtime_error&){failed=true;}
        require(failed&&bytes(target)=="replaced content"&&!fs::exists(temporary),"write errors cannot replace the destination");
#ifdef _WIN32
        const auto encrypted_target=root/"encrypted.txt";
        {std::ofstream out(encrypted_target);out<<"encrypted original";}
        if(EncryptFileW(encrypted_target.c_str())){
            {
                opt::AtomicFile file(encrypted_target);temporary=file.temporary_path();
                const auto attributes=GetFileAttributesW(temporary.c_str());
                require(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_ENCRYPTED)!=0,"EFS applies before staging receives any document content");
                file.write([](std::ostream& out){out<<"encrypted replacement";});file.publish();
            }
            const auto attributes=GetFileAttributesW(encrypted_target.c_str());
            require(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_ENCRYPTED)!=0&&bytes(encrypted_target)=="encrypted replacement","replacement retains existing EFS encryption");
            std::cout<<"Windows EFS replacement passed\n";
        }else{const auto error=GetLastError();std::cout<<"SKIP Windows EFS: EncryptFileW unavailable on this host/volume (error "<<error<<")\n";}
#endif
#ifndef _WIN32
        // Exercise an actual OS write limit, including stdio's delayed flush.
        struct FileLimit {
            rlimit old{};struct sigaction action{};
            FileLimit(){require(getrlimit(RLIMIT_FSIZE,&old)==0,"read file limit");struct sigaction ignore{};ignore.sa_handler=SIG_IGN;sigemptyset(&ignore.sa_mask);require(sigaction(SIGXFSZ,&ignore,&action)==0,"ignore file-limit signal");auto limit=old;limit.rlim_cur=1024;require(setrlimit(RLIMIT_FSIZE,&limit)==0,"set file limit");}
            ~FileLimit(){setrlimit(RLIMIT_FSIZE,&old);sigaction(SIGXFSZ,&action,nullptr);}
        } limit;
        failed=false;
        try{
            opt::AtomicFile file(target);temporary=file.temporary_path();
            file.write([](std::ostream& out){out<<std::string(8192,'x');});file.publish();
        }catch(const std::runtime_error&){failed=true;}
        require(failed&&bytes(target)=="replaced content"&&!fs::exists(temporary),"OS write failure leaves no partial destination or staging orphan");
#endif
#ifdef _WIN32
        if(acl_supported)std::cout<<"Windows private staging DACL and retained access controls passed\n";
#else
        std::cout<<"private staging and retained POSIX mode passed\n";
#endif
        std::cout<<"atomic replacement and failed-write cleanup passed\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
