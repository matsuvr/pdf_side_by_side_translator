#include "ui/activity.hpp"
#include "platform/cpu_usage.hpp"
#include <cassert>
#include <chrono>
#include <thread>
using namespace opt;
int main(){
    ActivityDots dots;assert(dots.text()==".");dots.advance();assert(dots.text()=="..");
    dots.advance();assert(dots.text()=="...");dots.advance();assert(dots.text()==".");
    dots.advance();dots.reset();assert(dots.text()==".");
    for(auto state:{PageState::NotStarted,PageState::WaitingForText,PageState::Queued,PageState::Running})assert(translating(state));
    for(auto state:{PageState::Complete,PageState::Partial,PageState::Failed,PageState::NoText})assert(!translating(state));
    CpuUsage cpu;
    assert(!cpu.sample(CpuTimes{100,1000}));
    assert(cpu.sample(CpuTimes{139,1100})==61);
    assert(cpu.sample(CpuTimes{239,1200})==0);
    assert(cpu.sample(CpuTimes{239,1300})==100);
    assert(!cpu.sample(CpuTimes{239,1300})); // No elapsed ticks.
    assert(!cpu.sample(CpuTimes{10,100})); // OS counter wrap/reset.
    assert(cpu.sample(CpuTimes{60,200})==50);
    assert(!cpu.sample(CpuTimes{260,300})); // Invalid idle delta.
    assert(!cpu.sample({})); // An OS failure invalidates the baseline.
    assert(!cpu.sample(CpuTimes{0,100}));
    assert(cpu.sample(CpuTimes{50,200})==50);
#if defined(_WIN32)||defined(__APPLE__)
    CpuUsage native;
    auto first=system_cpu_times();assert(first);assert(!native.sample(first));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    auto percent=native.sample(system_cpu_times());assert(percent&&*percent<=100);
#else
    assert(!system_cpu_times());
#endif
}
