#include "storage/legacy_data.hpp"
#include "platform/digest.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs=std::filesystem;
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
void write(const fs::path& path,std::string_view value="old application data"){
    std::ofstream out(path,std::ios::binary);out<<value;require(static_cast<bool>(out),"fixture write");
}
std::string read(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
int main(){
    const auto root=fs::temp_directory_path()/fs::path("opt-legacy-"+opt::new_uuid());
    try{
        fs::create_directories(root);
        require(!opt::discard_legacy_data({}).error,"empty cleanup target is a no-op");
        const auto absent=root/"absent";
        require(!opt::discard_legacy_data(absent).error&&!fs::exists(absent),"cleanup never creates missing storage");

        const auto old=root/"old",cache=old/"Cache",logs=old/"logs";
        fs::create_directories(cache);fs::create_directories(logs);
        const auto corrupt="cache.sqlite3.corrupt-"+opt::new_uuid();
        for(const auto suffix:{"","-wal","-shm","-journal"}){
            write(old/(std::string("state.sqlite3")+suffix));
            write(cache/(std::string("cache.sqlite3")+suffix));
            write(cache/(corrupt+suffix));
        }
        write(old/"storage.lock");
        for(const auto name:{"diagnostic.log","diagnostic.log.1","diagnostic.log.2"})write(logs/name);
        require(!opt::discard_legacy_data(old).error&&!fs::exists(old),"all recognized legacy files and empty folders discarded without reading databases");
        require(!opt::discard_legacy_data(old).error&&!fs::exists(old),"discard is idempotent");

        fs::create_directories(cache);fs::create_directories(logs);
        const fs::path preserved[]={old/u8"原稿.pdf",old/"model.gguf",old/"translator-state-backup.sqlite3",cache/"export.txt",cache/"cache.sqlite3.corrupt-not-a-generated-id",cache/(corrupt+".pdf"),logs/"notes.txt"};
        for(const auto& file:preserved)write(file,"keep user file");
        fs::create_directory(old/"state.sqlite3");write(old/"state.sqlite3"/"notes.txt","keep nested file");
        write(cache/"cache.sqlite3");write(logs/"diagnostic.log");
        require(!opt::discard_legacy_data(old).error,"unrecognized files do not make cleanup fail");
        for(const auto& file:preserved)require(read(file)=="keep user file","PDFs, GGUF, exports and unknown files preserved byte-for-byte");
        require(read(old/"state.sqlite3"/"notes.txt")=="keep nested file","cleanup never descends into an unexpected directory");
        require(!fs::exists(cache/"cache.sqlite3")&&!fs::exists(logs/"diagnostic.log"),"known files removed alongside unknown files");

        const auto outside=root/"outside",linked=root/"linked";fs::create_directory(outside);write(outside/"state.sqlite3","outside state");write(outside/"cache.sqlite3","outside cache");write(outside/"diagnostic.log","outside log");
        std::error_code link_error;fs::create_directory_symlink(outside,linked,link_error);
        if(!link_error){
            require(!opt::discard_legacy_data(linked).error&&fs::is_symlink(linked),"a legacy directory alias is not traversed");
            const auto alias_parent=root/"aliases";fs::create_directory(alias_parent);
            fs::create_directory_symlink(outside,alias_parent/"Cache");fs::create_directory_symlink(outside,alias_parent/"logs");
            fs::create_symlink(outside/"state.sqlite3",alias_parent/"state.sqlite3");
            require(!opt::discard_legacy_data(alias_parent).error,"folder aliases do not prevent normal cleanup");
            require(!fs::exists(alias_parent/"state.sqlite3")&&fs::is_symlink(alias_parent/"Cache")&&fs::is_symlink(alias_parent/"logs"),"known file alias removed without following folder aliases");
            require(read(outside/"state.sqlite3")=="outside state"&&read(outside/"cache.sqlite3")=="outside cache"&&read(outside/"diagnostic.log")=="outside log","no symlink target contents deleted");
        }
#ifndef _WIN32
        if(geteuid()!=0){
            const auto locked=root/"locked";fs::create_directory(locked);write(locked/"state.sqlite3");
            fs::permissions(locked,fs::perms::owner_read|fs::perms::owner_exec);
            const auto failed=opt::discard_legacy_data(locked);
            fs::permissions(locked,fs::perms::owner_all);
            require(failed.error&&failed.failed_path==locked/"state.sqlite3"&&fs::exists(locked/"state.sqlite3"),"real removal failure is reported with its path");
            require(!opt::discard_legacy_data(locked).error&&!fs::exists(locked),"cleanup retries after a real failure");
        }
#endif
        fs::remove_all(root);
        std::cout<<"legacy file discard, no creation, idempotence, retained exports and non-traversed aliases passed\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<" (fixtures at "<<root<<")\n";return 1;}
}
