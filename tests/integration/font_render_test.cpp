#include "pdf/font_adapter.hpp"
#include "platform/mapped_file.hpp"
#include "core/types.hpp"
#include <fpdfview.h>
#include <fpdf_text.h>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

std::filesystem::path path(const char* s){return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s)));}
int main(int argc,char** argv){
    if(argc!=5){std::cerr<<"usage: font_render_test resource-directory fixture.pdf embedded|fallback output.ppm\n";return 2;}
    try{
        FPDF_LIBRARY_CONFIG config{};config.version=2;FPDF_InitLibraryWithConfig(&config);
        struct Library {~Library(){FPDF_DestroyLibrary();}} library;
        opt::PdfFontAdapter fonts(path(argv[1]));if(!fonts.stats().available)throw std::runtime_error("Bundled font unavailable");
        opt::MappedFile file(path(argv[2]));FPDF_FILEACCESS access{};access.m_FileLen=static_cast<unsigned long>(file.bytes().size());access.m_Param=&file;
        access.m_GetBlock=[](void* p,unsigned long at,unsigned char* out,unsigned long size){const auto bytes=static_cast<opt::MappedFile*>(p)->bytes();if(at>bytes.size()||size>bytes.size()-at)return 0;std::memcpy(out,bytes.data()+at,size);return 1;};
        auto document=FPDF_LoadCustomDocument(&access,nullptr);if(!document)throw std::runtime_error("Font fixture did not open");
        struct Document {FPDF_DOCUMENT value;~Document(){FPDF_CloseDocument(value);}} doc{document};
        auto page=FPDF_LoadPage(document,0);if(!page)throw std::runtime_error("Font page did not load");
        struct Page {FPDF_PAGE value;~Page(){FPDF_ClosePage(value);}} pg{page};
        constexpr int width=768,height=994;auto bitmap=FPDFBitmap_Create(width,height,0);if(!bitmap)throw std::runtime_error("Bitmap allocation failed");
        struct Bitmap {FPDF_BITMAP value;~Bitmap(){FPDFBitmap_Destroy(value);}} bmp{bitmap};
        FPDFBitmap_FillRect(bitmap,0,0,width,height,0xffffffff);FPDF_RenderPageBitmap(bitmap,page,0,0,width,height,0,FPDF_ANNOT);
        const auto* bytes=static_cast<const unsigned char*>(FPDFBitmap_GetBuffer(bitmap));const auto stride=FPDFBitmap_GetStride(bitmap);
        std::ofstream output(path(argv[4]),std::ios::binary);output<<"P6\n"<<width<<' '<<height<<"\n255\n";std::size_t ink=0;
        for(int y=0;y<height;++y)for(int x=0;x<width;++x){const auto* pixel=bytes+y*stride+x*4;const char rgb[]{static_cast<char>(pixel[2]),static_cast<char>(pixel[1]),static_cast<char>(pixel[0])};output.write(rgb,3);if(pixel[0]<240||pixel[1]<240||pixel[2]<240)++ink;}
        if(!output||ink<300)throw std::runtime_error("Missing rendered glyphs");
        const auto stats=fonts.stats();const bool fallback=std::string(argv[3])=="fallback";
        if(fallback&&(!stats.mapped||!stats.full_reads))throw std::runtime_error("Japanese fallback did not use the bundled font callback");
        if(!fallback&&(stats.mapped||stats.full_reads))throw std::runtime_error("Embedded font was unexpectedly replaced");
        std::cout<<"bundled_maps="<<stats.mapped<<" bundled_full_reads="<<stats.full_reads<<" table_reads="<<stats.table_reads<<" ink="<<ink<<'\n';
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
