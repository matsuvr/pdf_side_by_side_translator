#include "platform/file_snapshot.hpp"
#include "platform/mapped_file.hpp"
#include "platform/digest.hpp"
#include <array>
#include <cerrno>
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifndef _WIN32
#include <chrono>
#include <thread>
#include <csignal>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
std::string hash_file(const opt::FileSnapshot& file){
    opt::Sha256 digest;std::array<std::byte,65536> bytes{};std::uint64_t offset=0;
    while(offset<file.initial_signature().size){
        const auto read=file.read_at(offset,bytes);if(!read)throw std::runtime_error("Unexpected end of snapshot");
        digest.update(std::span(bytes).first(read));offset+=read;
    }
    return digest.finish();
}
void reject_special_file(const std::filesystem::path& path){
    bool rejected=false;try{opt::FileSnapshot::open(path);}catch(const std::exception&){rejected=true;}
    check(rejected,"Non-regular input was accepted as a seekable snapshot");
    rejected=false;try{opt::MappedFile mapping(path);}catch(const std::exception&){rejected=true;}
    check(rejected,"Non-regular input was accepted as a mapped file");
}
#ifndef _WIN32
void reject_fifo_without_waiting(const std::filesystem::path& path){
    check(mkfifo(path.c_str(),0600)==0,"Cannot create FIFO regression fixture");
    // A regressed blocking open must fail this test rather than hang CTest.
    const auto child=fork();check(child>=0,"Cannot fork FIFO input probe");
    if(child==0){try{reject_special_file(path);_exit(0);}catch(...){_exit(1);}}
    int status=0;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    for(;;){
        const auto done=waitpid(child,&status,WNOHANG);
        if(done==child){check(WIFEXITED(status)&&WEXITSTATUS(status)==0,"FIFO was not rejected as a non-regular input");return;}
        if(done<0&&errno==EINTR)continue;
        if(done<0||std::chrono::steady_clock::now()>=deadline){
            kill(child,SIGKILL);while(waitpid(child,&status,0)<0&&errno==EINTR){}
            check(false,"Opening a FIFO waited for a writer and blocked the file owner");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
#endif
int main(){
    try{
        const auto root=std::filesystem::temp_directory_path()/("opt-file-test-"+opt::new_uuid());
        check(std::filesystem::create_directory(root),"Cannot create isolated fixture directory");
        struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}} cleanup{root};
        reject_special_file(root);
#ifndef _WIN32
        reject_fifo_without_waiting(root/"input.pdf");
        reject_special_file("/dev/null");
#endif
        const auto path=root/std::filesystem::path(u8"原文 と モデル.gguf");
        const auto old=root/std::filesystem::path(u8"退避.gguf");
        const std::string original="original 日本語 "+std::string(2*1024*1024,'x');
        {std::ofstream out(path,std::ios::binary);out.write(original.data(),static_cast<std::streamsize>(original.size()));}
        const auto timestamp=std::filesystem::last_write_time(path);
        auto file=opt::FileSnapshot::open(path);opt::MappedFile mapping(path);auto mapped_file=mapping.snapshot();
        check(file->initial_signature()==mapped_file->initial_signature(),"Mapping and hash do not identify the same file object");
        const auto alias=root/"regular-file-alias.gguf";std::error_code link_error;std::filesystem::create_symlink(path,alias,link_error);
        if(!link_error){
            const auto aliased=opt::FileSnapshot::open(alias);opt::MappedFile aliased_mapping(alias);
            check(aliased->initial_signature()==file->initial_signature()&&aliased_mapping.initial_signature()==file->initial_signature(),"Symlinks to regular files stopped working");
        }
        std::unique_ptr<std::FILE,decltype(&std::fclose)> stream(file->open_stream(),std::fclose);
        check(std::setvbuf(stream.get(),nullptr,_IONBF,0)==0,"Cannot configure test stream");
        std::array<char,4> first{},second{};check(std::fread(first.data(),1,4,stream.get())==4,"Cannot read model stream");
        std::array<std::byte,128> scratch{};file->read_at(1000,scratch);
        check(std::fread(second.data(),1,4,stream.get())==4,"Cannot continue model stream");
        check(std::string(first.data(),4)==original.substr(0,4)&&std::string(second.data(),4)==original.substr(4,4),"Hash reads changed the model stream seek position");
        std::filesystem::rename(path,old);
        {std::ofstream out(path,std::ios::binary);const std::string replacement(original.size(),'y');out.write(replacement.data(),static_cast<std::streamsize>(replacement.size()));}
        std::filesystem::last_write_time(path,timestamp);
        auto replaced=opt::FileSnapshot::open(path);
        check(replaced->initial_signature().identity!=file->initial_signature().identity,"Path replacement reused the old file identity");
        check(hash_file(*file)==opt::sha256(original)&&hash_file(*mapped_file)==opt::sha256(original),"Hashing was redirected to a same-named replacement file");
        check(hash_file(*replaced)!=opt::sha256(original),"Replacement content was not distinguished");
        check(mapping.bytes().size()==original.size()&&static_cast<char>(mapping.bytes()[0])=='o',"Mapping stopped referring to its original file object");
        std::cout<<"Native Unicode paths, regular-file aliases, nonblocking special-file rejection, independent streams and path replacement passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
