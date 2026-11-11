#include "storage/legacy_data.hpp"
#include "platform/access_path.hpp"
#include <string_view>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace opt {
namespace {
bool absent(const std::error_code& error){return error==std::errc::no_such_file_or_directory||error==std::errc::not_a_directory;}
bool database_file(std::string_view name,std::string_view database){
    if(!name.starts_with(database))return false;
    const auto suffix=name.substr(database.size());
    return suffix.empty()||suffix=="-wal"||suffix=="-shm"||suffix=="-journal";
}
bool cache_file(std::string_view name){
    if(database_file(name,"cache.sqlite3"))return true;
    constexpr std::string_view prefix="cache.sqlite3.corrupt-";
    if(!name.starts_with(prefix))return false;
    const auto tail=name.substr(prefix.size());
    if(tail.size()<36)return false;
    // The old recovery path used new_uuid(), not arbitrary suffixes.
    for(std::size_t i=0;i<36;++i){
        const auto c=tail[i];
        if(i==8||i==13||i==18||i==23){if(c!='-')return false;}
        else if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
    }
    return database_file(tail,tail.substr(0,36));
}
struct Cleanup {
    LegacyCleanupResult result;
    void remember(const std::filesystem::path& path,const std::error_code& error){
        if(error&&!absent(error)&&!result.error)result={path,error};
    }
    bool directory(const std::filesystem::path& path){
        std::error_code error;
        const auto status=std::filesystem::symlink_status(access_path(path),error);
        if(error){remember(path,error);return false;}
        if(!std::filesystem::is_directory(status))return false;
#ifdef _WIN32
        // Junctions are directory aliases too; do not traverse their targets.
        const auto attributes=GetFileAttributesW(access_path(path).c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES){remember(path,{static_cast<int>(GetLastError()),std::system_category()});return false;}
        if(attributes&FILE_ATTRIBUTE_REPARSE_POINT)return false;
#endif
        return true;
    }
    void file(const std::filesystem::path& path){
        std::error_code error;
        const auto status=std::filesystem::symlink_status(access_path(path),error);
        if(error){remember(path,error);return;}
        // An unexpected directory at a known filename is not an old DB/log.
        if(!std::filesystem::is_regular_file(status)&&!std::filesystem::is_symlink(status))return;
        std::filesystem::remove(access_path(path),error);remember(path,error);
    }
    void empty_directory(const std::filesystem::path& path){
        std::error_code error;std::filesystem::remove(access_path(path),error);
        if(error!=std::errc::directory_not_empty)remember(path,error);
    }
    template<class Owned> void files(const std::filesystem::path& path,Owned owned){
        std::error_code error;
        std::filesystem::directory_iterator iterator(access_path(path),error),end;
        if(error){remember(path,error);return;}
        while(iterator!=end){
            const auto entry=iterator->path();
            const auto name=entry.filename().u8string();
            if(owned(std::string_view(reinterpret_cast<const char*>(name.data()),name.size())))file(entry);
            iterator.increment(error);
            if(error){remember(path,error);return;}
        }
    }
};
}
LegacyCleanupResult discard_legacy_data(const std::filesystem::path& directory){
    Cleanup cleanup;
    if(directory.empty()||!cleanup.directory(directory))return cleanup.result;
    cleanup.files(directory,[](std::string_view name){return name=="storage.lock"||database_file(name,"state.sqlite3");});
    const auto cache=directory/"Cache",logs=directory/"logs";
    if(cleanup.directory(cache)){cleanup.files(cache,cache_file);cleanup.empty_directory(cache);}
    if(cleanup.directory(logs)){
        cleanup.files(logs,[](std::string_view name){return name=="diagnostic.log"||name=="diagnostic.log.1"||name=="diagnostic.log.2";});
        cleanup.empty_directory(logs);
    }
    cleanup.empty_directory(directory);
    return cleanup.result;
}
}
