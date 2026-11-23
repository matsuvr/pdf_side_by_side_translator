#include "ocr/image_reading.hpp"
#include "image_reading_assets.hpp"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <type_traits>

namespace opt {
using Clock=std::chrono::steady_clock;
namespace {
using Handle=std::unique_ptr<void,decltype(&CloseHandle)>;
struct SharedPage {
    int width,height,dpi;
    int language; // -1 is automatic on input, uncertain on output.
    std::uint32_t text_size;
    char text[image_reading_max_text];
    unsigned char pixels[image_reading_max_area];
};
void check_windows(bool success,const char* operation){
    if(!success)throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),operation);
}
auto map_page(HANDLE mapping){
    auto* page=static_cast<SharedPage*>(MapViewOfFile(mapping,FILE_MAP_WRITE,0,0,sizeof(SharedPage)));
    check_windows(page!=nullptr,"Map OCR page");
    return std::unique_ptr<SharedPage,decltype(&UnmapViewOfFile)>(page,UnmapViewOfFile);
}
int language_index(std::string_view tag){
    if(tag.empty())return -1;
    for(std::size_t i=0;i<std::size(image_languages);++i)
        if(image_languages[i].tag==tag)return static_cast<int>(i);
    throw std::invalid_argument("Unsupported OCR language");
}
const ImageLanguage* language_at(int index){
    if(index==-1)return nullptr;
    if(index<0||static_cast<std::size_t>(index)>=std::size(image_languages))
        throw std::invalid_argument("Invalid OCR language index");
    return &image_languages[index];
}
Handle launch_worker(HANDLE mapping,HANDLE job){
    SIZE_T bytes=0;
    InitializeProcThreadAttributeList(nullptr,2,0,&bytes);
    std::vector<unsigned char> buffer(bytes);
    auto* list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
    check_windows(InitializeProcThreadAttributeList(list,2,0,&bytes),"Initialize OCR process attributes");
    std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>,decltype(&DeleteProcThreadAttributeList)> attributes(list,DeleteProcThreadAttributeList);
    check_windows(UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,&mapping,sizeof(mapping),nullptr,nullptr),"Inherit OCR page");
    check_windows(UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_JOB_LIST,&job,sizeof(job),nullptr,nullptr),"Assign OCR job");
    std::wstring executable(32768,L'\0');
    const auto size=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
    check_windows(size && size<executable.size(),"Locate OCR executable");
    executable.resize(size);
    std::wstring command=L"\""+executable+L"\" "+std::wstring(image_worker_switch,image_worker_switch+sizeof(image_worker_switch)-1)
        +L" "+std::to_wstring(reinterpret_cast<std::uintptr_t>(mapping));
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb=sizeof(startup);startup.lpAttributeList=list;
    PROCESS_INFORMATION child{};
    check_windows(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,
        EXTENDED_STARTUPINFO_PRESENT|CREATE_NO_WINDOW,nullptr,nullptr,&startup.StartupInfo,&child),"Start OCR worker");
    CloseHandle(child.hThread);
    return Handle(child.hProcess,CloseHandle);
}
}

ImageReadingResult read_page_image(GrayPage page,std::string_view language,std::stop_token stop,Clock::time_point deadline){
    page.validate();
    if(stop.stop_requested())return {};
    const auto requested=language_index(language);
    SECURITY_ATTRIBUTES inherit{sizeof(inherit),nullptr,TRUE};
    Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE,&inherit,PAGE_READWRITE,0,sizeof(SharedPage),nullptr),CloseHandle);
    check_windows(bool(mapping),"Create OCR page");
    auto shared=map_page(mapping.get());
    shared->width=page.width;shared->height=page.height;shared->dpi=page.dpi;shared->language=requested;
    for(int row=0;row<page.height;++row)
        std::memcpy(shared->pixels+row*page.width,page.pixels.data()+std::size_t(row)*page.stride,page.width);

    Handle canceled(CreateEventW(nullptr,TRUE,FALSE,nullptr),CloseHandle);
    check_windows(bool(canceled),"Create OCR stop event");
    std::stop_callback signal(stop,[&]{SetEvent(canceled.get());});
    Handle job(CreateJobObjectW(nullptr,nullptr),CloseHandle);
    check_windows(bool(job),"Create OCR job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    check_windows(SetInformationJobObject(job.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"Bound OCR worker lifetime");
    if(stop.stop_requested())return {};
    if(Clock::now()>=deadline)throw ImageReadingTimeout{};
    auto process=launch_worker(mapping.get(),job.get());
    HANDLE waits[]{canceled.get(),process.get()};
    const auto remaining=std::chrono::ceil<std::chrono::milliseconds>(deadline-Clock::now()).count();
    const auto wait=WaitForMultipleObjects(2,waits,FALSE,static_cast<DWORD>(std::clamp<long long>(remaining,0,INFINITE-1)));
    // Closing the job stops even uninterruptible Tesseract initialization, OSD
    // and layout calls. The child never holds a window or an input file handle.
    if(wait==WAIT_OBJECT_0||stop.stop_requested())return {};
    if(wait==WAIT_TIMEOUT)throw ImageReadingTimeout{};
    check_windows(wait==WAIT_OBJECT_0+1,"Wait for OCR worker");
    DWORD status;
    check_windows(GetExitCodeProcess(process.get(),&status),"Read OCR exit status");
    if(status!=0||shared->text_size>sizeof(shared->text))throw std::runtime_error("画像を読み取れませんでした。");
    const auto* result=language_at(shared->language);
    return {std::string(shared->text,shared->text_size),result?std::string(result->label):std::string{},
        !result,result?result->tag:std::string_view{}};
}

int run_image_worker(std::string_view handle,ImageModelReader models){
    try{
        std::uintptr_t value=0;
        const auto [end,error]=std::from_chars(handle.data(),handle.data()+handle.size(),value);
        if(error!=std::errc{}||end!=handle.data()+handle.size()||!value||!models)return 1;
        Handle mapping(reinterpret_cast<HANDLE>(value),CloseHandle);
        auto shared=map_page(mapping.get());
        // The worker may publish results only into an anonymous section, even
        // if another launcher supplies the internal command-line arguments.
        wchar_t mapped_name[MAX_PATH];
        if(K32GetMappedFileNameW(GetCurrentProcess(),shared.get(),mapped_name,MAX_PATH)!=0 ||
           GetLastError()!=ERROR_FILE_INVALID)return 1;
        const auto* requested=language_at(shared->language);
        GrayPage page{shared->pixels,shared->width,shared->height,shared->width,shared->dpi};
        auto result=recognize_page_image(page,requested?requested->tag:std::string_view{},models,Clock::now()+image_reading_timeout);
        if(result.text.size()>sizeof(shared->text))return 1;
        std::memcpy(shared->text,result.text.data(),result.text.size());
        shared->text_size=static_cast<std::uint32_t>(result.text.size());
        shared->language=language_index(result.language_tag);
        return 0;
    }catch(const std::exception&){return 1;}
}
}
