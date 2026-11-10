#include "translation/page_attempt.hpp"
#include <iostream>
#include <stdexcept>

using namespace opt;
void require(bool test,const char* why){if(!test)throw std::runtime_error(why);}
std::shared_ptr<PreparedChunk> prepare(const ChunkRequest& r){
    return std::make_shared<PreparedChunk>(ChunkPlanner::prepare(r,8192,[](std::string_view s,Form){
        PreparedInput p;p.tokens.assign(s.size(),42);p.source_tokens=static_cast<std::uint32_t>(s.size());p.output_limit=512;return p;
    },[](std::string_view s){return s.size();}));
}
int main(){try{
    WorkStamp stamp{1,0,2,3,4};
    const auto source=std::make_shared<const std::string>("First sentence. Second sentence.");
    std::vector<AttemptBlock> blocks{{"paragraph",BlockKind::Paragraph,source,false},
        {"table",BlockKind::Table,std::make_shared<const std::string>("1 | 2 | 3"),true}};
    PageAttempt a(stamp,"conditions",blocks,Form::Plain,true);
    require(!a.may_read_cache(),"forced attempt must bypass prior cache");
    auto r=a.next_request();require(r.has_value(),"first leaf planned");auto p=prepare(*r);require(a.prepared(p),"prepared accepted");
    const auto old_stamp=p->stamp;
    require(a.delta(old_stamp,"old partial"),"active delta accepted");
    require(a.finished(old_stamp,{FinishReason::OutputLimit,"old partial",512}),"limit accepted");
    require(a.snapshot().plan==2,"retry gets new plan generation");
    require(a.display_text().find("old partial")==std::string::npos,"superseded partial removed");
    require(!a.delta(old_stamp,"stale"),"old plan delta rejected");
    r=a.next_request();require(r&&r->end<source->size(),"retry has a smaller hard range");p=prepare(*r);a.prepared(p);
    a.finished(p->stamp,{FinishReason::Completed,"前半",2});
    require(a.state()!=PageState::Complete,"untranslated suffix prevents completion");
    auto saved=a.snapshot();const auto attempt_id=saved.id;
    a.interrupt();
    PageAttempt resumed(stamp,"conditions",blocks,Form::Plain,false,saved);
    require(resumed.snapshot().id==attempt_id&&!resumed.may_read_cache(),"resume preserves forced cache bypass");
    r=resumed.next_request();require(r&&r->begin>0,"own completed prefix retained");p=prepare(*r);resumed.prepared(p);
    const auto current=p->stamp;
    WorkStamp stale=current;stale.document++;require(!resumed.delta(stale,"bad"),"different document rejected");
    resumed.finished(current,{FinishReason::OutputLimit,"途中",512});
    require(resumed.state()==PageState::Partial,"second output limit becomes terminal partial");
    require(!resumed.next_request(),"failed leaf not retried within run");
    require(resumed.display_text().find("前半")!=std::string::npos&&resumed.display_text().find("表・原文")!=std::string::npos,"partial and source table retained");
    resumed.resume();r=resumed.next_request();require(r.has_value(),"explicit resume retries failure");p=prepare(*r);resumed.prepared(p);
    resumed.finished(p->stamp,{FinishReason::Completed,"後半",2});
    require(resumed.state()==PageState::Complete,"all coverage complete");
    const auto text=resumed.display_text();require(text.find("前半後半")!=std::string::npos&&text.find("途中")==std::string::npos,"retry replaces partial exactly once");
    auto corrupt=resumed.snapshot();corrupt.chunks.erase(corrupt.chunks.begin()+1);
    PageAttempt rejected(stamp,"conditions",blocks,Form::Plain,false,corrupt);
    require(rejected.snapshot().id!=corrupt.id&&rejected.state()!=PageState::Complete,"manifest missing coverage rejected");
    PageAttempt different(stamp,"new-style",blocks,Form::Polite,false,resumed.snapshot());
    require(different.snapshot().id!=attempt_id,"changed condition creates new attempt");
    PageAttempt empty(stamp,"none",{{"blank",BlockKind::Paragraph,std::make_shared<const std::string>(" \n"),false}},Form::Plain);
    require(empty.state()==PageState::NoText&&!empty.next_request(),"whitespace never invokes inference");
    require(source_split_boundary("👩‍👩‍👧‍👦")==0,"one ZWJ cluster not split for retry");
    PageAttempt control(stamp,"control",{blocks.front()},Form::Plain);
    r=control.next_request();p=prepare(*r);control.prepared(p);
    const std::string unsafe("prefix\0hidden",13);
    require(!control.delta(p->stamp,unsafe),"control bytes cannot enter native stream");
    control.finished(p->stamp,{FinishReason::Completed,unsafe,3});
    require(control.state()==PageState::Partial&&control.snapshot().chunks.front().text=="prefix"&&control.snapshot().chunks.front().reason==FinishReason::InvalidModelOutput,"NUL output cannot become a completed chunk");
    std::cout<<"attempt coverage, forced resume, split replacement and stale events passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
