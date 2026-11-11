#include "pdf/delivery_inbox.hpp"
#include <iostream>
#include <stdexcept>
using namespace opt;
void require(bool v,const char* why){if(!v)throw std::runtime_error(why);}
int main(){try{
    PdfInbox inbox;
    auto pixels=std::make_shared<RasterBuffer>();pixels->bgra.resize(1024*1024);
    for(unsigned i=0;i<100;++i){auto e=std::make_shared<PdfEvent>();e->kind=PdfEvent::Kind::TileReady;e->document=1;e->page=i;e->pixels=pixels;e->key.page=i;inbox.push(e);}
    auto batch=inbox.take();require(batch.raster_bytes<=PdfInbox::raster_budget&&batch.events.size()<=64&&batch.lost_raster,"pixel deliveries bounded and missing data acknowledged");
    auto layout=std::make_shared<PageLayout>();layout->reading_text_utf8.resize(1024*1024,'x');
    for(unsigned i=0;i<100;++i){auto e=std::make_shared<PdfEvent>();e->kind=PdfEvent::Kind::Extracted;e->document=1;e->page=i;e->layout=layout;inbox.push(e);}
    batch=inbox.take();require(batch.layout_bytes<=PdfInbox::layout_budget&&batch.events.size()<=64&&batch.lost_layout,"pending geometry bounded and extraction requests can retry");
    auto first=std::make_shared<PdfEvent>();first->kind=PdfEvent::Kind::Opened;first->document=1;require(inbox.push(first),"empty inbox schedules one wake");
    auto second=std::make_shared<PdfEvent>(*first);second->document=2;require(!inbox.push(second),"replacement keeps single pending wake");
    batch=inbox.take();require(batch.events.size()==1&&batch.events.front()->document==2,"old document payload removed");
    auto oversized=std::make_shared<PageLayout>();oversized->reading_text_utf8.resize(PdfInbox::layout_budget+1,'x');
    auto excessive=std::make_shared<PdfEvent>();excessive->kind=PdfEvent::Kind::Extracted;excessive->document=3;excessive->layout=oversized;
    inbox.push(excessive);batch=inbox.take();
    require(batch.layout_bytes<=PdfInbox::layout_budget&&batch.events.size()==1,"One oversized page escaped the inbox budget");
    require(batch.events.front()->layout&&batch.events.front()->layout->availability==PageLayout::Availability::ResourceLimited&&!batch.lost_layout,"Oversized page should allow manual text instead of endless extraction retries");
    auto huge_pixels=std::make_shared<RasterBuffer>();huge_pixels->bgra.resize(PdfInbox::raster_budget+1);
    excessive=std::make_shared<PdfEvent>();excessive->kind=PdfEvent::Kind::TileReady;excessive->document=3;excessive->pixels=huge_pixels;
    inbox.push(excessive);batch=inbox.take();
    require(batch.raster_bytes==0&&batch.events.front()->kind==PdfEvent::Kind::Failed,"One oversized raster escaped the inbox budget");
    require(pdf_render_flags==(0x001|0x200),"Tile identity lost PDFium's limited image cache flag");
    std::cout<<"bounded pixels/layout deliveries, retry acknowledgements and single wake passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
