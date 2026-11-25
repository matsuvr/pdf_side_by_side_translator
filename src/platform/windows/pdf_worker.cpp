#include "pdf/restricted_pdf_service.hpp"
#include "pdf/worker_protocol.hpp"
#include "platform/file_snapshot.hpp"
#include "platform/clear_secret.hpp"
#include <windows.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>

namespace opt {
namespace {
using namespace pdf_wire;
using Handle=std::unique_ptr<void,decltype(&CloseHandle)>;
void check(bool ok,const char* message){
    if(!ok)throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),message);
}
Handle handle(HANDLE h){return Handle(h,CloseHandle);}
bool read_exact(HANDLE h,unsigned char* to,std::size_t size){
    while(size){
        DWORD n=0;const auto want=static_cast<DWORD>(std::min<std::size_t>(size,65536));
        if(!ReadFile(h,to,want,&n,nullptr)||!n)return false;
        to+=n;size-=n;
    }
    return true;
}
void write_exact(HANDLE h,const unsigned char* from,std::size_t size){
    while(size){
        DWORD n=0;const auto want=static_cast<DWORD>(std::min<std::size_t>(size,65536));
        check(WriteFile(h,from,want,&n,nullptr)&&n>0,"Write PDF worker pipe");from+=n;size-=n;
    }
}
void erase(Bytes& bytes)noexcept{
    volatile unsigned char* data=bytes.data();for(std::size_t i=0;i<bytes.size();++i)data[i]=0;
}
struct Wipe { Bytes& bytes;~Wipe(){erase(bytes);} };
void send(HANDLE h,Bytes bytes){
    Wipe wipe{bytes};
    if(bytes.empty()||bytes.size()>max_frame)throw std::runtime_error("Invalid PDF worker frame size");
    unsigned char length[8];for(unsigned i=0;i<8;++i)length[i]=static_cast<unsigned char>(static_cast<std::uint64_t>(bytes.size())>>(8*i));
    write_exact(h,length,sizeof(length));write_exact(h,bytes.data(),bytes.size());
}
std::optional<Bytes> receive(HANDLE h,std::size_t limit){
    unsigned char length[8];if(!read_exact(h,length,sizeof(length)))return {};
    std::uint64_t n=0;for(unsigned i=0;i<8;++i)n|=static_cast<std::uint64_t>(length[i])<<(8*i);
    if(!n||n>limit)throw std::runtime_error("Invalid PDF worker frame size");
    Bytes bytes(static_cast<std::size_t>(n));if(!read_exact(h,bytes.data(),bytes.size())){erase(bytes);throw std::runtime_error("Incomplete PDF worker frame");}
    return bytes;
}
Handle restricted_token(){
    HANDLE raw=nullptr;
    check(OpenProcessToken(GetCurrentProcess(),TOKEN_DUPLICATE|TOKEN_QUERY|TOKEN_ASSIGN_PRIMARY|TOKEN_ADJUST_DEFAULT,&raw),"Open process token");
    auto primary=handle(raw);
    // Drop optional privileges and turn elevated administrative groups into
    // deny-only groups. This does not grant the caller any new privileges.
    raw=nullptr;
    check(CreateRestrictedToken(primary.get(),DISABLE_MAX_PRIVILEGE|LUA_TOKEN,0,nullptr,0,nullptr,0,nullptr,&raw),"Restrict PDF process token");
    auto token=handle(raw);
    SID_IDENTIFIER_AUTHORITY authority=SECURITY_MANDATORY_LABEL_AUTHORITY;PSID low=nullptr;
    check(AllocateAndInitializeSid(&authority,1,SECURITY_MANDATORY_LOW_RID,0,0,0,0,0,0,0,&low),"Create low integrity SID");
    std::unique_ptr<void,decltype(&FreeSid)> sid(low,FreeSid);
    TOKEN_MANDATORY_LABEL label{};label.Label.Sid=low;label.Label.Attributes=SE_GROUP_INTEGRITY;
    check(SetTokenInformation(token.get(),TokenIntegrityLevel,&label,sizeof(label)+GetLengthSid(low)),"Lower PDF worker integrity");
    return token;
}
bool low_integrity_process(){
    HANDLE raw=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw))return false;
    auto token=handle(raw);DWORD size=0;
    GetTokenInformation(token.get(),TokenIntegrityLevel,nullptr,0,&size);
    if(size<sizeof(TOKEN_MANDATORY_LABEL)||size>65536)return false;
    std::vector<unsigned char> bytes(size);
    if(!GetTokenInformation(token.get(),TokenIntegrityLevel,bytes.data(),size,&size))return false;
    auto sid=reinterpret_cast<TOKEN_MANDATORY_LABEL*>(bytes.data())->Label.Sid;
    if(!IsValidSid(sid)||!*GetSidSubAuthorityCount(sid))return false;
    return *GetSidSubAuthority(sid,*GetSidSubAuthorityCount(sid)-1)<=SECURITY_MANDATORY_LOW_RID;
}
Handle make_job(){
    auto job=handle(CreateJobObjectW(nullptr,nullptr));check(bool(job),"Create PDF worker job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE|JOB_OBJECT_LIMIT_ACTIVE_PROCESS|JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.BasicLimitInformation.ActiveProcessLimit=1;
    // Cap committed memory (not virtual address space). The input read-only
    // file mapping may address PDFs larger than 4 GiB without copying them.
    limits.ProcessMemoryLimit=1024ULL*1024*1024;
    check(SetInformationJobObject(job.get(),JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"Bound PDF worker resources");
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
    ui.UIRestrictionsClass=JOB_OBJECT_UILIMIT_HANDLES|JOB_OBJECT_UILIMIT_READCLIPBOARD|JOB_OBJECT_UILIMIT_WRITECLIPBOARD|
        JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS|JOB_OBJECT_UILIMIT_GLOBALATOMS|JOB_OBJECT_UILIMIT_DISPLAYSETTINGS|JOB_OBJECT_UILIMIT_EXITWINDOWS|JOB_OBJECT_UILIMIT_DESKTOP;
    check(SetInformationJobObject(job.get(),JobObjectBasicUIRestrictions,&ui,sizeof(ui)),"Restrict PDF worker UI");
    return job;
}
std::pair<Handle,Handle> pipe_pair(bool child_reads){
    SECURITY_ATTRIBUTES inherit{sizeof(inherit),nullptr,TRUE};HANDLE rd=nullptr,wr=nullptr;
    check(CreatePipe(&rd,&wr,&inherit,65536),"Create PDF worker pipe");
    auto reader=handle(rd),writer=handle(wr);
    check(SetHandleInformation(child_reads?writer.get():reader.get(),HANDLE_FLAG_INHERIT,0),"Restrict PDF worker handle inheritance");
    return {std::move(reader),std::move(writer)};
}
struct Process {
    Handle job=handle(nullptr),process=handle(nullptr),input=handle(nullptr),output=handle(nullptr);
    std::thread reader;
    std::atomic_bool cancelled{false},exited{false},open_result{false};
    const std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
    std::uint64_t serial=0;
    std::shared_ptr<const FileSnapshot> snapshot;
    DocumentEpoch epoch=0;
    ~Process(){terminate();if(reader.joinable())reader.join();}
    void terminate()noexcept{
        cancelled=true;if(job&&!TerminateJobObject(job.get(),1)&&process)TerminateProcess(process.get(),1);
    }
};
std::shared_ptr<Process> spawn_pdf(DocumentEpoch epoch,std::uint64_t serial,std::shared_ptr<const FileSnapshot> snapshot){
    auto result=std::make_shared<Process>();result->epoch=epoch;result->serial=serial;result->snapshot=std::move(snapshot);
    auto [child_in,parent_in]=pipe_pair(true);auto [parent_out,child_out]=pipe_pair(false);
    auto file=handle(reinterpret_cast<HANDLE>(result->snapshot->duplicate_read_handle(true)));
    result->job=make_job();auto token=restricted_token();
    SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,2,0,&bytes);
    if(!bytes)throw std::runtime_error("Cannot allocate PDF worker attributes");
    std::vector<unsigned char> backing(bytes);
    auto* list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(backing.data());
    check(InitializeProcThreadAttributeList(list,2,0,&bytes),"Initialise PDF worker attributes");
    std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>,decltype(&DeleteProcThreadAttributeList)> attributes(list,DeleteProcThreadAttributeList);
    HANDLE inherited[]{child_in.get(),child_out.get(),file.get()};HANDLE job=result->job.get();
    check(UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr),"Allow only PDF worker handles");
    check(UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_JOB_LIST,&job,sizeof(job),nullptr,nullptr),"Assign PDF worker job");
    std::wstring executable(32768,L'\0');
    const auto size=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
    check(size&&size<executable.size(),"Locate PDF worker executable");executable.resize(size);
    std::wstring command=L"\""+executable+L"\" "+std::wstring(pdf_worker_switch,pdf_worker_switch+sizeof(pdf_worker_switch)-1);
    for(auto h:inherited)command+=L" "+std::to_wstring(reinterpret_cast<std::uintptr_t>(h));
    STARTUPINFOEXW startup{};startup.StartupInfo.cb=sizeof(startup);startup.lpAttributeList=list;
    // The child has no need for application environment variables or a user
    // working directory. Keep only the OS root needed by system DLL loading.
    std::wstring windows(32768,L'\0');const auto path_length=GetWindowsDirectoryW(windows.data(),static_cast<UINT>(windows.size()));
    check(path_length&&path_length<windows.size(),"Locate Windows directory");windows.resize(path_length);
    std::wstring environment=L"SystemRoot="+windows;environment.push_back(L'\0');
    environment+=L"WINDIR="+windows;environment.push_back(L'\0');environment.push_back(L'\0');
    PROCESS_INFORMATION child{};
    check(CreateProcessAsUserW(token.get(),executable.c_str(),command.data(),nullptr,nullptr,TRUE,EXTENDED_STARTUPINFO_PRESENT|CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT,
          environment.data(),windows.c_str(),&startup.StartupInfo,&child),"Start restricted PDF worker");
    CloseHandle(child.hThread);result->process=handle(child.hProcess);
    result->input=std::move(parent_in);result->output=std::move(parent_out);
    return result;
}
HANDLE parse_handle(std::string_view text){
    std::uintptr_t n=0;auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),n);
    if(error!=std::errc{}||end!=text.data()+text.size()||!n||n==UINTPTR_MAX)throw std::runtime_error("Invalid PDF worker handle");
    return reinterpret_cast<HANDLE>(n);
}
bool pipe_handle(HANDLE value){return GetFileType(value)==FILE_TYPE_PIPE;}
PdfEvent failure(DocumentEpoch epoch,std::string code){
    PdfEvent e;e.kind=PdfEvent::Kind::Failed;e.document=epoch;e.code=std::move(code);e.message="PDF処理プロセスを利用できません。PDFを開き直してください。";return e;
}
}
int run_pdf_worker(std::string_view input,std::string_view output,std::string_view file){
    try{
        if(!low_integrity_process())return 1;
        PROCESS_MITIGATION_DYNAMIC_CODE_POLICY policy{};policy.ProhibitDynamicCode=1;
        if(!SetProcessMitigationPolicy(ProcessDynamicCodePolicy,&policy,sizeof(policy))){
            policy={};
            if(!GetProcessMitigationPolicy(GetCurrentProcess(),ProcessDynamicCodePolicy,&policy,sizeof(policy))||
               !policy.ProhibitDynamicCode||policy.AllowThreadOptOut||policy.AllowRemoteDowngrade)return 1;
        }
        const auto in_handle=parse_handle(input),out_handle=parse_handle(output),file_handle=parse_handle(file);
        if(in_handle==out_handle||in_handle==file_handle||out_handle==file_handle||!pipe_handle(in_handle)||!pipe_handle(out_handle))return 1;
        auto in=handle(in_handle),out=handle(out_handle);
        check(SetHandleInformation(in.get(),HANDLE_FLAG_INHERIT,0)&&SetHandleInformation(out.get(),HANDLE_FLAG_INHERIT,0),"Close worker pipe inheritance");
        auto snapshot=FileSnapshot::take_read_handle(reinterpret_cast<std::intptr_t>(file_handle));
        auto first=receive(in.get(),max_command);if(!first)return 1;
        Wipe wipe{*first};auto request=decode_command(*first);if(request.op!=Operation::Open)return 1;
        const auto epoch=request.document;
        std::atomic_bool failed{false};
        std::mutex writer;
        auto send_event=[&](auto event){
            std::lock_guard lock(writer);
            if(failed.load())return;
            try{
                Bytes bytes;
                try{bytes=encode(*event);}
                catch(...){
                    // A legitimate page can reach the separate IPC budget.
                    // Convert that to a small page result, rather than losing
                    // the entire document or passing an oversized payload.
                    PdfEvent limited;limited.document=event->document;limited.page=event->page;
                    limited.message="PDF処理結果が通信容量の上限に達しました。";
                    if(event->kind==PdfEvent::Kind::Extracted||event->kind==PdfEvent::Kind::SearchResult){
                        limited.kind=event->kind;limited.query_generation=event->query_generation;limited.code="PdfTextBudget";
                        auto layout=std::make_shared<PageLayout>();layout->page=event->page;layout->availability=PageLayout::Availability::ResourceLimited;limited.layout=std::move(layout);
                    }else if(event->kind==PdfEvent::Kind::PageImage){
                        limited.kind=event->kind;limited.image_generation=event->image_generation;
                    }else{
                        limited.kind=PdfEvent::Kind::Failed;limited.code=event->kind==PdfEvent::Kind::Opened?"PdfOpen":"PdfPage";
                    }
                    bytes=encode(limited);
                }
                send(out.get(),std::move(bytes));
            }catch(...){failed=true;TerminateProcess(GetCurrentProcess(),1);}
        };
        PdfService service(send_event);
        service.open_snapshot(epoch,snapshot,request.password);
        clear_secret(request.password);erase(*first);
        while(!failed.load()){
            auto frame=receive(in.get(),max_command);if(!frame)break;
            Wipe wipe_frame{*frame};auto c=decode_command(*frame);
            if(c.document!=epoch||c.op==Operation::Open)throw std::runtime_error("Unexpected PDF worker command");
            switch(c.op){
            case Operation::Page:service.page_info(c.document,c.page);break;
            case Operation::View:service.viewport(std::move(c.view));break;
            case Operation::Extract:
                if(!service.extract(c.document,c.page)){
                    PdfEvent e;e.kind=PdfEvent::Kind::ExtractionDeferred;e.document=c.document;e.page=c.page;
                    send_event(std::make_shared<const PdfEvent>(std::move(e)));
                }
                break;
            case Operation::Image:
                if(!service.render_page_image(c.document,c.page,c.rotation,c.generation)){
                    PdfEvent e;e.kind=PdfEvent::Kind::PageImage;e.document=c.document;e.page=c.page;e.image_generation=c.generation;e.message="ページ画像の要求を中断しました。";
                    send_event(std::make_shared<const PdfEvent>(std::move(e)));
                }
                break;
            case Operation::Search:service.search(std::move(c.search));break;
            case Operation::Cancel:service.cancel_search(c.document,c.generation);break;
            case Operation::Open:break;
            }
        }
        service.stop();service.join();return failed.load()?1:0;
    }catch(...){return 1;}
}
struct RestrictedPdfService::Impl {
    struct Opening {DocumentEpoch epoch;std::uint64_t serial;std::filesystem::path path;std::string password;~Opening(){clear_secret(password);}};
    Delivery delivery;
    std::mutex mutex;std::condition_variable cv;
    std::unique_ptr<Opening> opening;
    std::deque<Command> queue;
    std::set<PageIndex> extraction;
    std::shared_ptr<Process> current;
    std::atomic<DocumentEpoch> wanted{0};
    std::atomic<std::uint64_t> wanted_serial{0};
    std::atomic_bool stopping{false},finished{false};
    PageIndex displayed=0;std::atomic<std::uint64_t> query_generation{0};
    std::thread manager;
    explicit Impl(Delivery cb):delivery(std::move(cb)),manager([this]{run();}){}
    ~Impl(){stop();if(manager.joinable())manager.join();}
    void emit(PdfEvent event,std::uint64_t serial){if(wanted.load()==event.document&&wanted_serial.load()==serial&&!stopping.load())delivery(std::make_shared<const PdfEvent>(std::move(event)));}
    void stop()noexcept{
        std::lock_guard lock(mutex);stopping=true;if(current)current->terminate();cv.notify_all();
    }
    void read(std::shared_ptr<Process> p){
        try{
            std::uint32_t count=0;
            while(auto bytes=receive(p->output.get(),max_frame)){
                Wipe wipe{*bytes};auto event=decode_event(*bytes);
                if(event.document!=p->epoch)throw std::runtime_error("Wrong PDF worker epoch");
                if(event.kind==PdfEvent::Kind::Opened||event.kind==PdfEvent::Kind::PasswordRequired||
                   (event.kind==PdfEvent::Kind::Failed&&event.code=="PdfOpen"))p->open_result=true;
                if(event.kind==PdfEvent::Kind::Opened){
                    if(count)throw std::runtime_error("Duplicate PDF worker open");
                    count=event.page_count;event.file_snapshot=p->snapshot;event.file_signature=p->snapshot->initial_signature();
                }else if(event.kind!=PdfEvent::Kind::Failed&&event.kind!=PdfEvent::Kind::PasswordRequired&&(!count||event.page>=count))
                    throw std::runtime_error("Invalid PDF worker page");
                {
                    std::lock_guard lock(mutex);
                    if(current.get()!=p.get()||p->cancelled.load())continue;
                    if(event.kind==PdfEvent::Kind::Extracted||event.kind==PdfEvent::Kind::ExtractionDeferred||
                       (event.kind==PdfEvent::Kind::Failed&&event.code=="PdfPage"))extraction.erase(event.page);
                }
                if(!p->cancelled.load())emit(std::move(event),p->serial);
            }
        }catch(...){}
        if(!p->cancelled.load()&&!stopping.load()){
            p->terminate();try{emit(failure(p->epoch,"PdfService"),p->serial);}catch(...){}
        }
        p->exited=true;cv.notify_all();
    }
    void end(std::shared_ptr<Process> p){
        if(p){p->terminate();if(p->reader.joinable())p->reader.join();}
    }
    bool enqueue(Command command,bool replace){
        if(stopping.load()||command.document!=wanted.load()||(current&&current->exited.load()))return false;
        if(replace){
            std::erase_if(queue,[&](const auto& c){return c.op==command.op||
                ((command.op==Operation::Search||command.op==Operation::Cancel)&&(c.op==Operation::Search||c.op==Operation::Cancel));});
        }
        if(queue.size()>=128)return false;
        queue.push_back(std::move(command));cv.notify_one();return true;
    }
    void run()noexcept{
        try{
            while(!stopping.load()){
                std::unique_ptr<Opening> next;std::optional<Command> command;std::shared_ptr<Process> p;bool timed_out=false;
                {
                    std::unique_lock lock(mutex);
                    cv.wait_for(lock,std::chrono::milliseconds(250),[&]{return stopping.load()||opening||(current&&current->exited.load())||(!queue.empty()&&current);});
                    if(stopping.load())break;
                    if(opening)next=std::move(opening);
                    p=current;
                    timed_out=p&&!p->open_result.load()&&std::chrono::steady_clock::now()-p->started>std::chrono::seconds(60);
                    if(next||(p&&(p->exited.load()||timed_out))){current.reset();if(!next){queue.clear();extraction.clear();}}
                    else if(p&&!queue.empty()){command=std::move(queue.front());queue.pop_front();}
                }
                if(next){
                    end(std::move(p));
                    if(next->path.empty()){
                        PdfEvent e;e.kind=PdfEvent::Kind::Closed;e.document=next->epoch;emit(std::move(e),next->serial);continue;
                    }
                    try{
                        auto snapshot=FileSnapshot::open(next->path);
                        auto started=spawn_pdf(next->epoch,next->serial,snapshot);
                        {
                            std::lock_guard lock(mutex);
                            if(stopping.load()||wanted_serial.load()!=next->serial)continue;
                            current=started;
                        }
                        started->reader=std::thread([this,started]{read(started);});
                        Command c;c.op=Operation::Open;c.document=next->epoch;c.password=next->password;clear_secret(next->password);
                        send(started->input.get(),encode(c));clear_secret(c.password);
                    }catch(...){
                        std::shared_ptr<Process> failed;
                        {std::lock_guard lock(mutex);if(current&&current->serial==next->serial)failed=std::exchange(current,{});}
                        end(std::move(failed));emit(failure(next->epoch,"PdfOpen"),next->serial);
                    }
                }else if(timed_out&&p){
                    PdfEvent e=failure(p->epoch,"PdfOpen");e.message="PDFを開く処理が時間上限に達しました。";
                    auto serial=p->serial;end(std::move(p));emit(std::move(e),serial);
                }else if(p&&p->exited.load())end(std::move(p));
                else if(command&&p){
                    try{
                        // Cached layouts are already decoded and validated on
                        // this side. Search them locally without re-parsing.
                        const auto& q=command->search;
                        if(command->op==Operation::Search&&q.query.size()>32768){
                            PdfEvent e=failure(q.document,"PdfSearch");e.page=q.page;e.query_generation=q.generation;e.message="検索語が長すぎます。";emit(std::move(e),p->serial);continue;
                        }
                        if(command->op==Operation::Search&&q.cached_layout&&q.cached_layout->page==q.page&&q.cached_layout->layout_version==PageLayout::current_version){
                            const auto cancelled=[&]{return stopping.load()||wanted.load()!=q.document||query_generation.load()!=q.generation;};
                            if(!cancelled()){
                                PdfEvent e;e.kind=PdfEvent::Kind::SearchResult;e.document=q.document;e.page=q.page;e.query_generation=q.generation;e.layout=q.cached_layout;
                                try{e.hit=q.backward?find_text_backward(*e.layout,q.query,q.start,q.match_case,cancelled):find_text(*e.layout,q.query,q.start,q.match_case,cancelled);}
                                catch(const LayoutCancelled&){continue;}
                                if(!cancelled())emit(std::move(e),p->serial);
                            }
                        }else send(p->input.get(),encode(*command));
                    }catch(...){
                        p->terminate();emit(failure(p->epoch,"PdfService"),p->serial);
                        {std::lock_guard lock(mutex);if(current.get()==p.get())current.reset();}
                        end(std::move(p));
                    }
                }
            }
        }catch(...){try{emit(failure(wanted.load(),"PdfService"),wanted_serial.load());}catch(...){}}
        std::shared_ptr<Process> p;{std::lock_guard lock(mutex);p=std::exchange(current,{});opening.reset();queue.clear();}
        end(std::move(p));finished=true;
    }
};
RestrictedPdfService::RestrictedPdfService(Delivery cb,std::filesystem::path,LayoutOptions):impl_(std::make_unique<Impl>(std::move(cb))){}
RestrictedPdfService::~RestrictedPdfService()=default;
void RestrictedPdfService::open(DocumentEpoch epoch,const std::filesystem::path& path,std::string_view password){
    std::lock_guard lock(impl_->mutex);if(impl_->stopping.load())return;
    impl_->wanted=epoch;++impl_->wanted_serial;impl_->query_generation=0;impl_->displayed=0;impl_->queue.clear();impl_->extraction.clear();impl_->opening=std::make_unique<Impl::Opening>();
    impl_->opening->epoch=epoch;impl_->opening->serial=impl_->wanted_serial.load();impl_->opening->path=path;impl_->opening->password=password;
    if(impl_->current)impl_->current->terminate();impl_->cv.notify_all();
}
void RestrictedPdfService::close(DocumentEpoch epoch){open(epoch,{});}
void RestrictedPdfService::page_info(DocumentEpoch epoch,PageIndex page){
    std::lock_guard lock(impl_->mutex);Command c;c.op=Operation::Page;c.document=epoch;c.page=page;
    if(impl_->enqueue(std::move(c),true)){impl_->displayed=page;std::erase_if(impl_->queue,[&](const auto& q){return q.op==Operation::Image&&q.page!=page;});}
}
void RestrictedPdfService::viewport(ViewRequest view){
    std::lock_guard lock(impl_->mutex);if(view.tiles.size()>64)view.tiles.resize(64);Command c;c.op=Operation::View;c.document=view.document;c.page=view.page;c.view=std::move(view);
    const auto page=c.page;if(impl_->enqueue(std::move(c),true)){impl_->displayed=page;std::erase_if(impl_->queue,[&](const auto& q){return q.op==Operation::Image&&q.page!=page;});}
}
bool RestrictedPdfService::render_page_image(DocumentEpoch epoch,PageIndex page,int rotation,std::uint64_t generation){
    std::lock_guard lock(impl_->mutex);if(page!=impl_->displayed||rotation<0||rotation>3)return false;
    Command c;c.op=Operation::Image;c.document=epoch;c.page=page;c.rotation=rotation;c.generation=generation;return impl_->enqueue(std::move(c),true);
}
bool RestrictedPdfService::extract(DocumentEpoch epoch,PageIndex page){
    std::lock_guard lock(impl_->mutex);if(epoch!=impl_->wanted.load()||impl_->stopping.load())return false;
    if(impl_->extraction.contains(page))return true;if(impl_->extraction.size()>=64)return false;
    Command c;c.op=Operation::Extract;c.document=epoch;c.page=page;
    if(!impl_->enqueue(std::move(c),false))return false;impl_->extraction.insert(page);return true;
}
void RestrictedPdfService::search(PdfSearchRequest search){
    std::lock_guard lock(impl_->mutex);if(search.document!=impl_->wanted.load())return;Command c;c.op=Operation::Search;c.document=search.document;c.page=search.page;c.search=std::move(search);
    // Cached layouts remain local. The wire format contains only a search
    // query and scalar coordinates/generation, never pointers or layout data.
    impl_->query_generation=c.search.generation;impl_->enqueue(std::move(c),true);
}
void RestrictedPdfService::cancel_search(DocumentEpoch epoch,std::uint64_t generation){
    std::lock_guard lock(impl_->mutex);if(epoch!=impl_->wanted.load())return;impl_->query_generation=generation;Command c;c.op=Operation::Cancel;c.document=epoch;c.generation=generation;impl_->enqueue(std::move(c),true);
}
void RestrictedPdfService::stop()noexcept{impl_->stop();}
bool RestrictedPdfService::done()const noexcept{return impl_->finished.load();}
std::uint32_t RestrictedPdfService::worker_process_id()const noexcept{
    std::lock_guard lock(impl_->mutex);return impl_->current&&impl_->current->process?GetProcessId(impl_->current->process.get()):0;
}
void RestrictedPdfService::join(){if(impl_->manager.joinable())impl_->manager.join();}
}
