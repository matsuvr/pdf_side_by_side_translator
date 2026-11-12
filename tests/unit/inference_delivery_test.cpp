#include "inference/delivery_inbox.hpp"
#include <chrono>
#include <future>
#include <iostream>

using namespace std::chrono_literals;
void check(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
int main(){try{
    opt::InferenceInbox inbox;
    auto delta=std::make_shared<opt::InferenceEvent>();
    delta->kind=opt::InferenceEvent::Kind::TextDelta;delta->delta.assign(opt::max_translation_bytes,'a');
    for(int i=0;i<4;++i)inbox.push(delta);
    auto finished=std::make_shared<opt::InferenceEvent>();
    finished->kind=opt::InferenceEvent::Kind::Finished;finished->result.text="done";
    auto pending=std::async(std::launch::async,[&]{inbox.push(finished);});
    check(pending.wait_for(50ms)==std::future_status::timeout,"Final text bypassed the delta byte budget");
    check(inbox.take().size()==4,"Unconsumed deltas were dropped");
    pending.get();
    auto batch=inbox.take();check(batch.size()==1&&batch.front()==finished,"Final result lost during backpressure");
    for(int i=0;i<64;++i)inbox.push(finished);
    auto blocked=std::async(std::launch::async,[&]{inbox.push(finished);});
    check(blocked.wait_for(50ms)==std::future_status::timeout,"Queue exceeded its event count limit");
    inbox.stop();blocked.get();
    check(inbox.take().size()==64,"Shutdown enqueued a blocked event");
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
