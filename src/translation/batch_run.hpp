#pragma once
#include "core/types.hpp"
#include <map>
#include <optional>

namespace opt {
struct BatchCounts {std::uint32_t complete=0,no_text=0,partial=0,failed=0;};
// A cursor and terminal metadata only: no queued page text or prepared prompts.
class BatchRun {
public:
    BatchRun(std::uint32_t pages,std::string conditions):total_(pages),conditions_(std::move(conditions)),active_(pages>0){}
    std::optional<PageIndex> next(PageIndex current){
        if(!active_)return {};
        if(current<total_&&!terminal_.contains(current))return current;
        while(cursor_<total_&&terminal_.contains(cursor_))++cursor_;
        if(cursor_>=total_){active_=false;return {};}
        return cursor_;
    }
    bool terminal(PageIndex page,PageState state){
        if(!active_||page>=total_||terminal_.contains(page))return false;
        switch(state){case PageState::Complete:++counts_.complete;break;case PageState::NoText:++counts_.no_text;break;
        case PageState::Partial:++counts_.partial;break;case PageState::Failed:++counts_.failed;break;default:return false;}
        terminal_[page]=state;if(terminal_.size()==total_)active_=false;return true;
    }
    void stop(){active_=false;}
    bool active()const{return active_;}
    bool contains(PageIndex page)const{return terminal_.contains(page);}
    const BatchCounts& counts()const{return counts_;}
    std::uint32_t total()const{return total_;}
    const std::string& conditions()const{return conditions_;}
private:
    std::uint32_t total_=0;PageIndex cursor_=0;
    std::string conditions_;bool active_=false;
    std::map<PageIndex,PageState> terminal_;
    BatchCounts counts_;
};
}
