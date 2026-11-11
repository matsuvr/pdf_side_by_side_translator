#pragma once
#include "pdf/pdf_service.hpp"
#include <algorithm>
#include <deque>
#include <mutex>

namespace opt {
struct PdfDeliveryBatch {
    std::deque<std::shared_ptr<const PdfEvent>> events;
    bool lost_layout=false,lost_raster=false;
    std::size_t raster_bytes=0,layout_bytes=0;
};
// One pending wake; bounds raw pixels and pending layouts independently. Dropped
// deliveries are acknowledged so callers retry missing data instead of hanging.
class PdfInbox {
    std::mutex mutex_;
    PdfDeliveryBatch pending_;
    bool wake_=false;
    static std::size_t raster(const std::shared_ptr<const PdfEvent>& e){return e->pixels?e->pixels->bgra.capacity():0;}
    static std::size_t layout(const std::shared_ptr<const PdfEvent>& e){return e->layout?layout_memory_bytes(*e->layout):0;}
public:
    static constexpr std::size_t raster_budget=24U*1024*1024,layout_budget=32U*1024*1024;
    bool push(std::shared_ptr<const PdfEvent> event){
        // Never make a single-payload exception to a byte budget. Replacing an
        // oversized layout with a small result also avoids an endless retry.
        if(layout(event)>layout_budget){
            auto limited=std::make_shared<PdfEvent>(*event);auto page=std::make_shared<PageLayout>();page->page=event->page;
            page->availability=PageLayout::Availability::ResourceLimited;limited->layout=std::move(page);
            limited->hit.reset();limited->code="PdfTextBudget";limited->message=LayoutResourceLimit{}.what();event=std::move(limited);
        }
        if(raster(event)>raster_budget){
            auto failed=std::make_shared<PdfEvent>(*event);failed->pixels.reset();failed->kind=PdfEvent::Kind::Failed;
            failed->code="PdfPage";failed->message="描画用メモリの予算を超えました。表示倍率を変更して再試行できます。";event=std::move(failed);
        }
        std::lock_guard lock(mutex_);auto& q=pending_.events;
        auto remove=[&](auto at,bool dropped){
            if(dropped){pending_.lost_raster|=bool((*at)->pixels);pending_.lost_layout|=bool((*at)->layout)||(*at)->kind==PdfEvent::Kind::SearchResult;}
            pending_.raster_bytes-=raster(*at);pending_.layout_bytes-=layout(*at);return q.erase(at);
        };
        for(auto i=q.begin();i!=q.end();){
            if((*i)->document!=event->document||((*i)->kind==event->kind&&(*i)->page==event->page&&(!event->pixels||(*i)->key==event->key)))i=remove(i,false);else ++i;
        }
        const auto pixels=raster(event),glyphs=layout(event);
        while(!q.empty()&&(q.size()>=64||pending_.raster_bytes+pixels>raster_budget||(glyphs&&pending_.layout_bytes+glyphs>layout_budget))){
            auto victim=q.end();
            if(pending_.raster_bytes+pixels>raster_budget)victim=std::find_if(q.begin(),q.end(),[](const auto& e){return bool(e->pixels);});
            if(victim==q.end()&&pending_.layout_bytes+glyphs>layout_budget)victim=std::find_if(q.begin(),q.end(),[](const auto& e){return bool(e->layout);});
            if(victim==q.end()&&q.size()<64)break;
            if(victim==q.end())victim=std::find_if(q.begin(),q.end(),[](const auto& e){return bool(e->pixels);});
            if(victim==q.end())victim=q.begin();remove(victim,true);
        }
        pending_.raster_bytes+=pixels;pending_.layout_bytes+=glyphs;q.push_back(std::move(event));
        const bool notify=!wake_;wake_=true;return notify;
    }
    PdfDeliveryBatch take(){std::lock_guard lock(mutex_);auto result=std::move(pending_);pending_={};wake_=false;return result;}
};
}
