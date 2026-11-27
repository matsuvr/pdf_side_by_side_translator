#include "ocr/image_worker_catalog.hpp"
#include <mach-o/dyld.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <memory>
#include <system_error>
#include <thread>

extern char** environ;

namespace opt {
namespace {
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr int worker_socket=3;
constexpr char worker_argument[]="socket";
struct Request {int width,height,dpi,language;};
struct ReadingCanceled {};

void check(int status,const char* operation){
    if(status)throw std::system_error(status,std::system_category(),operation);
}

struct Socket {
    explicit Socket(int descriptor):fd(descriptor){}
    Socket(const Socket&)=delete;
    ~Socket(){close_now();}
    void close_now(){
        if(fd>=0)close(std::exchange(fd,-1));
    }
    int fd;
};

void configure_socket(int fd){
    const int yes=1;
    if(setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&yes,sizeof(yes))<0 ||
       fcntl(fd,F_SETFD,FD_CLOEXEC)<0)
        check(errno,"Configure OCR socket");
}

bool is_anonymous_socket(int fd){
    sockaddr_un local{},peer{};
    socklen_t local_size=sizeof(local),peer_size=sizeof(peer);
    int type=0;
    socklen_t type_size=sizeof(type);
    return getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&type_size)==0 && type==SOCK_STREAM &&
        getsockname(fd,reinterpret_cast<sockaddr*>(&local),&local_size)==0 &&
        getpeername(fd,reinterpret_cast<sockaddr*>(&peer),&peer_size)==0 &&
        local.sun_family==AF_UNIX && peer.sun_family==AF_UNIX &&
        local.sun_path[0]=='\0' && peer.sun_path[0]=='\0';
}

int wait_interval(std::stop_token stop,Clock::time_point deadline){
    if(stop.stop_requested())throw ReadingCanceled{};
    const auto remaining=std::chrono::ceil<std::chrono::milliseconds>(deadline-Clock::now());
    if(remaining<=0ms)throw ImageReadingTimeout{};
    return static_cast<int>(std::min(remaining,20ms).count());
}

class Channel {
public:
    Channel(int fd,std::stop_token stop,Clock::time_point deadline):fd_(fd),stop_(stop),deadline_(deadline){}

    void read(void* buffer,std::size_t size)const{
        auto* bytes=static_cast<char*>(buffer);
        transfer(POLLIN,size,[&](auto offset){return recv(fd_,bytes+offset,size-offset,MSG_DONTWAIT);});
    }
    void write(const void* buffer,std::size_t size)const{
        const auto* bytes=static_cast<const char*>(buffer);
        transfer(POLLOUT,size,[&](auto offset){return send(fd_,bytes+offset,size-offset,MSG_DONTWAIT);});
    }
private:
    template<class Transfer>
    void transfer(short events,std::size_t size,Transfer perform)const{
        for(std::size_t offset=0;offset<size;){
            const auto interval=wait_interval(stop_,deadline_);
            const auto count=perform(offset);
            if(count>0){offset+=static_cast<std::size_t>(count);continue;}
            if(count==0)throw std::runtime_error("画像を読み取れませんでした。");
            if(errno==EINTR)continue;
            if(errno!=EAGAIN && errno!=EWOULDBLOCK)check(errno,"Transfer OCR data");
            pollfd ready{fd_,events,0};
            if(poll(&ready,1,interval)<0 && errno!=EINTR)check(errno,"Wait for OCR data");
        }
    }
    int fd_;
    std::stop_token stop_;
    Clock::time_point deadline_;
};

pid_t launch_worker(int fd){
    std::vector<char> path(1024);
    auto size=static_cast<std::uint32_t>(path.size());
    if(_NSGetExecutablePath(path.data(),&size)!=0){
        path.resize(size);
        if(_NSGetExecutablePath(path.data(),&size)!=0)
            throw std::runtime_error("Locate OCR executable");
    }
    posix_spawn_file_actions_t actions;
    check(posix_spawn_file_actions_init(&actions),"Initialize OCR process actions");
    const std::unique_ptr<posix_spawn_file_actions_t,decltype(&posix_spawn_file_actions_destroy)>
        clear_actions(&actions,posix_spawn_file_actions_destroy);
    check(posix_spawn_file_actions_adddup2(&actions,fd,worker_socket),"Inherit OCR socket");

    posix_spawnattr_t attributes;
    check(posix_spawnattr_init(&attributes),"Initialize OCR process attributes");
    const std::unique_ptr<posix_spawnattr_t,decltype(&posix_spawnattr_destroy)>
        clear_attributes(&attributes,posix_spawnattr_destroy);
    check(posix_spawnattr_setflags(&attributes,POSIX_SPAWN_CLOEXEC_DEFAULT),"Limit inherited OCR descriptors");

    std::string worker_switch=image_worker_switch,argument=worker_argument;
    char* argv[]{path.data(),worker_switch.data(),argument.data(),nullptr};
    pid_t pid;
    check(posix_spawn(&pid,path.data(),&actions,&attributes,argv,environ),"Start OCR worker");
    return pid;
}

class Worker {
public:
    explicit Worker(int socket):pid_(launch_worker(socket)){}
    Worker(const Worker&)=delete;
    ~Worker(){
        if(pid_<=0)return;
        pid_t result;
        do{result=waitpid(pid_,nullptr,WNOHANG);}while(result<0 && errno==EINTR);
        if(result!=0)return;
        kill(pid_,SIGKILL);
        while(waitpid(pid_,nullptr,0)<0 && errno==EINTR){}
    }
    void wait(std::stop_token stop,Clock::time_point deadline){
        for(;;){
            const auto interval=wait_interval(stop,deadline);
            int status;
            const auto result=waitpid(pid_,&status,WNOHANG);
            if(result==pid_){
                pid_=-1;
                if(!WIFEXITED(status)||WEXITSTATUS(status)!=0)
                    throw std::runtime_error("画像を読み取れませんでした。");
                return;
            }
            if(result<0){
                if(errno==EINTR)continue;
                const auto error=errno;
                if(error==ECHILD)pid_=-1;
                check(error,"Wait for OCR worker");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(interval));
        }
    }
private:
    pid_t pid_;
};

void watch_parent(std::stop_token stop,int fd,Clock::time_point deadline){
    // After the complete page, EOF or more input ends this worker. On normal
    // completion wake poll locally and join before process teardown.
    std::stop_callback wake(stop,[fd]{shutdown(fd,SHUT_RD);});
    pollfd peer{fd,POLLIN,0};
    while(!stop.stop_requested()){
        const auto left=std::chrono::ceil<std::chrono::milliseconds>(deadline-Clock::now());
        if(left<=0ms)break;
        const auto ready=poll(&peer,1,static_cast<int>(left.count()));
        if(ready<0 && errno==EINTR)continue;
        break;
    }
    if(!stop.stop_requested())std::_Exit(EXIT_FAILURE);
}
}

ImageReadingResult read_page_image(GrayPage page,std::string_view language,std::stop_token stop,Clock::time_point deadline){
    page.validate();
    try{
        wait_interval(stop,deadline);
        const Request request{page.width,page.height,page.dpi,image_language_index(language)};
        int descriptors[2];
        if(socketpair(AF_UNIX,SOCK_STREAM,0,descriptors)<0)check(errno,"Create OCR socket");
        Socket parent(descriptors[0]),child(descriptors[1]);
        configure_socket(parent.fd);
        configure_socket(child.fd);
        Worker worker(child.fd);
        child.close_now();

        const Channel channel(parent.fd,stop,deadline);
        channel.write(&request,sizeof(request));
        if(page.stride==page.width){
            channel.write(page.pixels.data(),static_cast<std::size_t>(page.width)*page.height);
        }else{
            for(int row=0;row<page.height;++row)
                channel.write(page.pixels.data()+static_cast<std::size_t>(row)*page.stride,page.width);
        }
        ImageWorkerResult reply;
        channel.read(&reply,sizeof(reply));
        if(reply.text_size>image_reading_max_text)throw std::runtime_error("画像を読み取れませんでした。");
        std::string text(reply.text_size,'\0');
        channel.read(text.data(),text.size());
        worker.wait(stop,deadline);
        return reply.decode(std::move(text));
    }catch(const ReadingCanceled&){
        return {};
    }
}

int run_image_worker(std::string_view argument,ImageModelReader models){
    if(argument!=worker_argument||!models||!is_anonymous_socket(worker_socket))return 1;
    try{
        configure_socket(worker_socket);
        const auto deadline=Clock::now()+image_reading_timeout;
        const Channel channel(worker_socket,{},deadline);
        Request request{};
        channel.read(&request,sizeof(request));
        const auto* language=image_entry_at(image_languages,request.language);
        if(request.width<=0||request.height<=0||request.width>image_reading_max_side||
           request.height>image_reading_max_side||request.width*request.height>image_reading_max_area||request.dpi<=0)
            return 1;
        std::vector<unsigned char> pixels(static_cast<std::size_t>(request.width)*request.height);
        channel.read(pixels.data(),pixels.size());
        std::jthread parent_watch(watch_parent,worker_socket,deadline);

        const auto result=recognize_page_image({pixels,request.width,request.height,request.width,request.dpi},
            language?language->tag:std::string_view{},models,deadline);
        if(result.text.size()>image_reading_max_text)return 1;
        const ImageWorkerResult reply(result);
        channel.write(&reply,sizeof(reply));
        channel.write(result.text.data(),result.text.size());
        return 0;
    }catch(const std::exception&){
        return 1;
    }
}
}
