#include "pdf/font_adapter.hpp"
#include "platform/mapped_file.hpp"
#include <fpdf_sysfontinfo.h>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace opt {
struct PdfFontAdapter::Impl : FPDF_SYSFONTINFO {
    FPDF_SYSFONTINFO* system=nullptr;
    std::unique_ptr<MappedFile> mapping;
    std::span<const std::byte> data;
    FontAdapterStats counts;
    char token=0;
    bool released=false,installed=false;
    static Impl& self(FPDF_SYSFONTINFO* p){return *static_cast<Impl*>(p);}
    bool bundled(void* font)const{return font==&token;}
    static bool is_bundled_face(const char* face){return face&&(std::string_view(face)=="Noto Sans CJK JP"||std::string_view(face)=="NotoSansCJKjp-Regular");}
    std::uint32_t be(std::size_t at,unsigned count)const{
        if(at>data.size()||count>data.size()-at)return 0;std::uint32_t value=0;
        for(unsigned i=0;i<count;++i)value=(value<<8)|std::to_integer<unsigned>(data[at+i]);return value;
    }
    std::span<const std::byte> table(std::uint32_t tag)const{
        if(tag==0)return data;if(data.size()<12)return {};
        const auto tables=be(4,2);if(tables>(data.size()-12)/16)return {};
        for(std::uint32_t i=0;i<tables;++i){const auto at=12+i*16;
            if(be(at,4)==tag){const auto offset=be(at+8,4),length=be(at+12,4);if(offset>data.size()||length>data.size()-offset)return {};return data.subspan(offset,length);}}
        return {};
    }
    explicit Impl(const std::filesystem::path& directory):FPDF_SYSFONTINFO{}{
        try{
#ifdef _WIN32
            const auto module=GetModuleHandleW(nullptr);const auto resource=FindResourceW(module,MAKEINTRESOURCEW(201),RT_RCDATA);
            if(resource){auto loaded=LoadResource(module,resource);auto* bytes=LockResource(loaded);if(bytes)data={static_cast<const std::byte*>(bytes),SizeofResource(module,resource)};}
#endif
            if(data.empty()&&!directory.empty()){mapping=std::make_unique<MappedFile>(directory/"NotoSansCJKjp-Regular.otf");data=mapping->bytes();}
        }catch(const std::exception&){return;}
        if(data.size()<12||table(0x636d6170).empty())return; // cmap
        counts.available=true;system=FPDF_GetDefaultSystemFontInfo();version=1;
        Release=[](FPDF_SYSFONTINFO* p){auto& s=self(p);if(s.system&&!s.released&&s.system->Release)s.system->Release(s.system);s.released=true;};
        EnumFonts=[](FPDF_SYSFONTINFO* p,void* mapper){auto& s=self(p);if(s.system&&s.system->EnumFonts)s.system->EnumFonts(s.system,mapper);FPDF_AddInstalledFont(mapper,"Noto Sans CJK JP",FXFONT_SHIFTJIS_CHARSET);};
        MapFont=[](FPDF_SYSFONTINFO* p,int weight,FPDF_BOOL italic,int charset,int pitch,const char* face,FPDF_BOOL* exact)->void*{
            auto& s=self(p);if(charset==FXFONT_SHIFTJIS_CHARSET||is_bundled_face(face)){++s.counts.mapped;if(exact)*exact=1;return &s.token;}
            return s.system&&s.system->MapFont?s.system->MapFont(s.system,weight,italic,charset,pitch,face,exact):nullptr;
        };
        GetFont=[](FPDF_SYSFONTINFO* p,const char* face)->void*{auto& s=self(p);if(is_bundled_face(face)){++s.counts.mapped;return &s.token;}return s.system&&s.system->GetFont?s.system->GetFont(s.system,face):nullptr;};
        GetFontData=[](FPDF_SYSFONTINFO* p,void* font,unsigned tag,unsigned char* buffer,unsigned long capacity)->unsigned long{
            auto& s=self(p);if(!s.bundled(font))return s.system&&s.system->GetFontData?s.system->GetFontData(s.system,font,tag,buffer,capacity):0;
            const auto bytes=s.table(tag);if(bytes.size()>std::numeric_limits<unsigned long>::max())return 0;
            if(tag==0)++s.counts.full_reads;else ++s.counts.table_reads;
            if(buffer&&capacity>=bytes.size()&&!bytes.empty())std::memcpy(buffer,bytes.data(),bytes.size());return static_cast<unsigned long>(bytes.size());
        };
        GetFaceName=[](FPDF_SYSFONTINFO* p,void* font,char* buffer,unsigned long capacity)->unsigned long{
            auto& s=self(p);if(!s.bundled(font))return s.system&&s.system->GetFaceName?s.system->GetFaceName(s.system,font,buffer,capacity):0;
            constexpr char name[]="Noto Sans CJK JP";if(buffer&&capacity>=sizeof(name))std::memcpy(buffer,name,sizeof(name));return sizeof(name);
        };
        GetFontCharset=[](FPDF_SYSFONTINFO* p,void* font)->int{auto& s=self(p);if(s.bundled(font))return FXFONT_SHIFTJIS_CHARSET;return s.system&&s.system->GetFontCharset?s.system->GetFontCharset(s.system,font):FXFONT_DEFAULT_CHARSET;};
        DeleteFont=[](FPDF_SYSFONTINFO* p,void* font){auto& s=self(p);if(!s.bundled(font)&&s.system&&s.system->DeleteFont)s.system->DeleteFont(s.system,font);};
        FPDF_SetSystemFontInfo(this);installed=true;
    }
    ~Impl(){
        if(installed)FPDF_SetSystemFontInfo(nullptr);
        if(system){if(!released&&system->Release)system->Release(system);FPDF_FreeDefaultSystemFontInfo(system);}
    }
};
PdfFontAdapter::PdfFontAdapter(const std::filesystem::path& directory):impl_(std::make_unique<Impl>(directory)){}
PdfFontAdapter::~PdfFontAdapter()=default;
FontAdapterStats PdfFontAdapter::stats()const{return impl_->counts;}
}
