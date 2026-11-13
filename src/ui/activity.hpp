#pragma once
#include "core/types.hpp"
#include <string>
namespace opt {
inline bool translating(PageState state) noexcept {
    return state==PageState::NotStarted||state==PageState::WaitingForText||state==PageState::Queued||state==PageState::Running;
}
class ActivityDots {
    unsigned phase_=0;
public:
    void reset() noexcept {phase_=0;}
    void advance() noexcept {phase_=(phase_+1)%3;}
    std::string text() const {return std::string(phase_+1,'.');}
};
}
