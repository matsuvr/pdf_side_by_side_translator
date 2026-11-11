#include "app/session_sources.hpp"
#include <cassert>
#include <iostream>

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
    assert(sources.source(0)&&*sources.source(0)=="user edit");
    assert(sources.revision(0)==revision);
    assert(!sources.source(1));

    sources.select_document({});
    assert(!sources.source(0));
    sources.select_document("native:different-document");
    sources.load(0,1,"other document");
    sources.select_document("native:file-generation-one");
    sources.load(0,2,"old source from failed I/O mirror");
    assert(*sources.source(0)=="user edit"&&sources.revision(0)==revision);
    sources.select_document("native:file-generation-two");
    assert(!sources.source(0));
    sources.select_document("native:file-generation-one");

    const auto reset_revision=sources.edit(0,nullptr);
    sources.evict_loaded(0);
    sources.load(0,revision,"user edit");
    assert(!sources.source(0)&&sources.revision(0)==reset_revision);
    // An intentionally empty override also differs from automatic extraction.
    sources.edit(0,std::make_shared<const std::string>());
    sources.evict_loaded(0);
    assert(sources.source(0)&&sources.source(0)->empty());
    sources.clear();
    assert(!sources.source(0)&&sources.revision(0)==1);
    sources.select_document("native:file-generation-one");
    sources.load(0,reset_revision,"session reload");
    assert(*sources.source(0)=="session reload");
    std::cout<<"session sources passed\n";
}
