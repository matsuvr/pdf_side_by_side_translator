#include "pdf/file_reader.hpp"
#include "platform/mapped_file.hpp"
#include "platform/digest.hpp"
#include <array>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(int argc,char** argv){try{
    if(argc==2){
        const auto path=std::filesystem::u8path(argv[1]);
        opt::MappedFile mapped(path);
        for(auto snapshot:{opt::FileSnapshot::open(path),mapped.snapshot()}){
            opt::PdfFileReader reader(snapshot);std::array<std::byte,4> bytes{};
            check(reader.read((std::uint64_t{1}<<32)+4096,bytes)&&
                  std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size())=="xref",
                  "Large file read truncated a 64-bit offset");
        }
        return 0;
    }
    const auto root=std::filesystem::temp_directory_path()/("opt-pdf-reader-"+opt::new_uuid());
    std::filesystem::create_directory(root);
    struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}} cleanup{root};
    const auto path=root/"input.pdf",old=root/"original.pdf";
    const std::string content(2*1024*1024,'a');
    auto write=[&]{std::ofstream out(path,std::ios::binary|std::ios::trunc);out.write(content.data(),content.size());};
    write();
    opt::PdfFileReader reader(opt::FileSnapshot::open(path));
    std::array<std::byte,16> bytes{};
    check(reader.read(100,bytes)&&static_cast<char>(bytes[0])=='a',"Initial callback read failed");
#ifdef _WIN32
    std::error_code blocked;
    std::filesystem::resize_file(path,8,blocked);
    check(bool(blocked)&&reader.read(4096,bytes),"Open PDF must remain readable and deny truncation");
    check(!std::fstream(path,std::ios::in|std::ios::out),"Opened active PDF for in-place writes");
#else
    std::filesystem::rename(path,old);{std::ofstream out(path);out<<"replacement";}
    check(reader.read(100,bytes)&&static_cast<char>(bytes[0])=='a',"Path replacement redirected the callback");
    // The same underlying file is truncated after it has already been read.
    // This is a normal false/error result, never a SIGBUS from a stale mmap.
    std::filesystem::resize_file(old,8);
    check(!reader.read(4096,bytes)&&reader.failure()==opt::PdfFileReader::Failure::Changed,"External truncation was not a recoverable read failure");
    bool reported=false;try{reader.check();}catch(const std::exception&){reported=true;}
    check(reported,"Changed input was not surfaced to the PDF worker");
    write();std::size_t callbacks=0;
    opt::PdfFileReader racing(opt::FileSnapshot::open(path),[&]{if(++callbacks==3)std::filesystem::resize_file(path,32);return false;});
    std::vector<std::byte> large(content.size());
    check(!racing.read(0,large)&&racing.failure()==opt::PdfFileReader::Failure::Changed,"Truncation between block reads bypassed the identity precheck");
    write();
#endif
    opt::PdfFileReader cancelled(opt::FileSnapshot::open(path),[]{return true;});
    check(!cancelled.read(0,bytes)&&cancelled.failure()==opt::PdfFileReader::Failure::Cancelled,"Read cancellation was ignored");
    opt::PdfFileReader throwing(opt::FileSnapshot::open(path),[]{throw std::runtime_error("failure");return false;});
    check(!throwing.read(0,bytes),"C callback allowed an exception to escape");
    opt::PdfFileReader overflow(opt::FileSnapshot::open(path));
    check(!overflow.read(std::numeric_limits<std::uint64_t>::max(),bytes),"Offset arithmetic overflow was accepted");
#ifndef _WIN32
    // POSIX filesystems create an unallocated hole; only the sentinel is stored.
    // Avoid creating a multi-GiB non-sparse file on Windows test filesystems.
    const auto sparse=root/"large.pdf";
    constexpr std::uint64_t offset=(std::uint64_t{1}<<32)+4096;
    {std::ofstream out(sparse,std::ios::binary);out.seekp(static_cast<std::streamoff>(offset));out<<"sentinel";check(bool(out),"Cannot create sparse large-offset fixture");}
    opt::PdfFileReader wide(opt::FileSnapshot::open(sparse));
    std::array<std::byte,8> sentinel{};
    check(wide.read(offset,sentinel)&&std::string(reinterpret_cast<const char*>(sentinel.data()),sentinel.size())=="sentinel","PDF reader truncated an offset above 4 GiB");
#endif
    std::cout<<"Safe PDF callback: truncation, mid-read changes, replacement, cancellation, exceptions and wide offsets passed\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
