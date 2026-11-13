#include "platform/cpu_usage.hpp"
#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/host_info.h>
#endif
namespace opt {
std::optional<CpuTimes> system_cpu_times() noexcept {
#if defined(_WIN32)
    FILETIME idle{},kernel{},user{};
    if(!GetSystemTimes(&idle,&kernel,&user))return {};
    const auto ticks=[](FILETIME t){return (std::uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;};
    return CpuTimes{ticks(idle),ticks(kernel)+ticks(user)};
#elif defined(__APPLE__)
    host_cpu_load_info_data_t info{};mach_msg_type_number_t count=HOST_CPU_LOAD_INFO_COUNT;
    const auto host=mach_host_self();
    const auto result=host_statistics(host,HOST_CPU_LOAD_INFO,reinterpret_cast<host_info_t>(&info),&count);
    mach_port_deallocate(mach_task_self(),host);
    if(result!=KERN_SUCCESS||count!=HOST_CPU_LOAD_INFO_COUNT)return {};
    std::uint64_t total=0;for(auto tick:info.cpu_ticks)total+=tick;
    return CpuTimes{info.cpu_ticks[CPU_STATE_IDLE],total};
#else
    return {};
#endif
}
}
