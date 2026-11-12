#pragma once
#include "inference/inference_service.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

namespace opt {
class InferenceInbox {
    std::mutex mutex_;
    std::condition_variable space_;
    bool stopped_=false;
    std::size_t text_bytes_=0;
    static constexpr std::size_t text_budget=4*max_translation_bytes;
    std::deque<std::shared_ptr<const InferenceEvent>> queue_;
public:
    void push(std::shared_ptr<const InferenceEvent> event){
        if(event->delta.size()>max_translation_bytes||event->result.text.size()>max_translation_bytes)
            throw std::length_error("Inference output exceeds delivery budget");
        const auto bytes=event->delta.size()+event->result.text.size();
        std::unique_lock lock(mutex_);
        space_.wait(lock,[&]{return stopped_||(queue_.size()<64&&bytes<=text_budget-text_bytes_);});
        if(stopped_)return;
        queue_.push_back(std::move(event));text_bytes_+=bytes;
    }
    auto take(){
        std::lock_guard lock(mutex_);auto batch=std::exchange(queue_,{});
        text_bytes_=0;space_.notify_one();return batch;
    }
    void stop(){std::lock_guard lock(mutex_);stopped_=true;space_.notify_one();}
};
}
