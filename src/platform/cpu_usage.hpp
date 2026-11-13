#pragma once
#include <cstdint>
#include <optional>

namespace opt {
// Aggregate OS CPU time across all processors. Idle is included in total.
struct CpuTimes { std::uint64_t idle=0,total=0; };
std::optional<CpuTimes> system_cpu_times() noexcept;
class CpuUsage {
    std::optional<CpuTimes> previous_;
public:
    std::optional<unsigned> sample(std::optional<CpuTimes> now) noexcept {
        const auto before=previous_;previous_=now;
        if(!now||!before||now->total<=before->total||now->idle<before->idle)return {};
        const auto total=now->total-before->total,idle=now->idle-before->idle;
        if(idle>total)return {};
        return static_cast<unsigned>(100.0L*(total-idle)/total+0.5L);
    }
};
}
