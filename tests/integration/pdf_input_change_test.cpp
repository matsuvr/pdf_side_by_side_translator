#include "pdf/file_reader.hpp"
#include "platform/digest.hpp"
#include "fpdfview.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

// Exercise real callback failure *inside* FPDF_LoadPage, after PDFium already
// parsed the cross-reference table. This catches a regression to PDFium's old
// CHECK(did_read) in CPDF_SyntaxParser::ReadStream. The pinned dependency returns
// nullptr at that site, so an external truncate is a recoverable input error.
struct Input {
    std::filesystem::path path;
    opt::PdfFileReader reader;
    unsigned calls=0,fail_on=0;
    bool armed=false,changed=false;
    explicit Input(const std::filesystem::path& source):path(source),reader(opt::FileSnapshot::open(source)){}
    static int read(void* object,unsigned long offset,unsigned char* bytes,unsigned long count) noexcept{
        auto& input=*static_cast<Input*>(object);
        try{
            if(input.armed&&++input.calls==input.fail_on){
                std::error_code ec;std::filesystem::resize_file(input.path,0,ec);
#ifdef _WIN32
                input.changed=bool(ec); // File sharing denied the attempted change.
#else
                input.changed=!ec;
#endif
            }
            return input.reader.read(offset,{reinterpret_cast<std::byte*>(bytes),count})?1:0;
        }catch(...){return 0;}
    }
};
int main(int argc,char** argv){
    if(argc!=2)return 2;
    try{
        const auto source=std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(argv[1])));
        const auto root=std::filesystem::temp_directory_path()/("opt-pdf-input-"+opt::new_uuid());
        std::filesystem::create_directory(root);
        struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}} cleanup{root};
        struct Library {Library(){FPDF_InitLibrary();}~Library(){FPDF_DestroyLibrary();}} library;
        auto run=[&](unsigned fail_on){
            const auto path=root/"input.pdf";std::filesystem::copy_file(source,path,std::filesystem::copy_options::overwrite_existing);
            Input input(path);input.fail_on=fail_on;
            FPDF_FILEACCESS access{};access.m_FileLen=static_cast<unsigned long>(input.reader.initial_signature().size);
            access.m_Param=&input;access.m_GetBlock=&Input::read;
            auto document=FPDF_LoadCustomDocument(&access,nullptr);
            if(!document)throw std::runtime_error("Cannot load input mutation fixture");
            struct Document {FPDF_DOCUMENT value;~Document(){FPDF_CloseDocument(value);}} owner{document};
            input.armed=true;auto page=FPDF_LoadPage(document,0);
#ifdef _WIN32
            if(!page)throw std::runtime_error("Read-shared PDF failed during a conflicting write");
#endif
            if(page)FPDF_ClosePage(page);
            if(!fail_on){input.reader.check();if(!input.calls)throw std::runtime_error("Fixture did not read through its block callback during page load");}
            else{
                if(!input.changed)throw std::runtime_error("Mutation injection point was not reached");
#ifdef _WIN32
                input.reader.check();
#else
                bool failed=false;try{input.reader.check();}catch(const std::exception&){failed=true;}
                if(!failed)throw std::runtime_error("Mid-parser file mutation was not reported");
#endif
            }
            return input.calls;
        };
        const auto blocks=run(0);
        for(unsigned block=1;block<=blocks;++block)run(block);
        std::cout<<"Mid-parser truncation recovered at "<<blocks<<" PDF block read positions\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
