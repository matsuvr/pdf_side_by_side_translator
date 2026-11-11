#include "app/session_sources.hpp"
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
}

int main(){
    opt::SessionSources sources;
    sources.select_document("native:file-generation-one");
    sources.load(0,2,"old source");
    const auto revision=sources.edit(0,std::make_shared<const std::string>("user edit"));
    // The GUI can move through an arbitrary number of pages while the I/O
    // owner is blocked. Neither eviction nor a delayed reply may lose an edit.
    for(opt::PageIndex page=1;page<100;++page){
        sources.load(page,1,"disposable source");
        sources.evict_loaded(page-1);
    }
    sources.load(0,2,"old source");
    require(sources.source(0)&&*sources.source(0)=="user edit","edit survives eviction and a delayed load");
    require(sources.revision(0)==revision,"edit revision survives eviction");
    require(!sources.source(1),"evicted page is not retained as a source");

    sources.select_document({});
    require(!sources.source(0),"closed document hides its sources");
    sources.select_document("native:different-document");
    sources.load(0,1,"other document");
    sources.select_document("native:file-generation-one");
    sources.load(0,2,"old source from failed I/O mirror");
    require(*sources.source(0)=="user edit"&&sources.revision(0)==revision,"edited source survives an I/O failure and reopen");
    sources.select_document("native:file-generation-two");
    require(!sources.source(0),"a new document generation cannot reuse old source");
    sources.select_document("native:file-generation-one");

    const auto reset_revision=sources.edit(0,nullptr);
    sources.evict_loaded(0);
    sources.load(0,revision,"user edit");
    require(!sources.source(0)&&sources.revision(0)==reset_revision,"reset to automatic survives a delayed load");
    // An intentionally empty override also differs from automatic extraction.
    sources.edit(0,std::make_shared<const std::string>());
    sources.evict_loaded(0);
    require(sources.source(0)&&sources.source(0)->empty(),"an intentionally empty edit is retained");
    sources.clear();
    require(!sources.source(0)&&sources.revision(0)==1,"clear removes every cached source");
    sources.select_document("native:file-generation-one");
    sources.load(0,reset_revision,"session reload");
    require(*sources.source(0)=="session reload","sources load again after clear");
    std::cout<<"session sources passed\n";
}
