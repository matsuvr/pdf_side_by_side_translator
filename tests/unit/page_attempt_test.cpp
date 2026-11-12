#include "translation/page_attempt.hpp"
#include <iostream>
#include <stdexcept>

using namespace opt;
void require(bool test,const char* why){if(!test)throw std::runtime_error(why);}
std::shared_ptr<PreparedChunk> prepare(const ChunkRequest& r){
    return std::make_shared<PreparedChunk>(ChunkPlanner::prepare(r,8192,[](std::string_view s,Form){
        PreparedInput p;p.tokens.assign(s.size(),42);p.source_tokens=static_cast<std::uint32_t>(s.size());p.output_limit=512;return p;
    }));
}
int main(){try{
    WorkStamp stamp{1,0,2,3,4};
    PageLayout layout;
    layout.reading_text_utf8="Heading\n\nThe device has a switch.\n\nIt controls all three sensors.\n\nA | B | C\n1 | 2 | 3";
    for(auto kind:{BlockKind::Heading,BlockKind::Paragraph,BlockKind::Paragraph,BlockKind::Table}){
        TextBlock block;block.kind=kind;layout.blocks.push_back(std::move(block));
    }
    const auto source=PageAttempt::source(layout);
    PageAttempt a(stamp,"conditions",source,Form::Plain);
    auto r=a.next_request();
    require(r&&*r->source==layout.reading_text_utf8,"one request must cover all paragraphs and structured text in reading order");
    auto p=prepare(*r);
    require(a.prepared(p)&&!a.next_request(),"page has only one active request");
    const auto old_stamp=p->stamp;
    require(a.delta(old_stamp,"途中"),"active delta accepted");
    require(a.finished(old_stamp,{FinishReason::OutputLimit,"途中",512}),"limit accepted");
    require(a.state()==PageState::Partial&&!a.next_request(),"output limit silently split the page");
    require(a.display_text().find("途中")!=std::string::npos&&!a.delta(old_stamp,"stale"),"partial output or stale delta handling changed");
    PageAttempt resumed(stamp,"conditions",source,Form::Plain,a.snapshot());
    require(resumed.snapshot().id==a.snapshot().id,"resume preserves the interrupted attempt");
    r=resumed.next_request();require(r&&*r->source==*source,"resume must retry the whole page");
    require(!resumed.delta(old_stamp,"stale"),"restored attempt accepted a callback from the prior generation");
    p=prepare(*r);resumed.prepared(p);
    WorkStamp stale=p->stamp;stale.document++;require(!resumed.delta(stale,"bad"),"different document rejected");
    resumed.finished(p->stamp,{FinishReason::Completed,"完訳",2});
    require(resumed.state()==PageState::Complete&&resumed.display_text()=="完訳"&&!resumed.next_request(),"page translation must replace partial exactly once");
    auto corrupt=resumed.snapshot();corrupt.chunks.front().end--;
    PageAttempt rejected(stamp,"conditions",source,Form::Plain,corrupt);
    require(rejected.snapshot().id!=corrupt.id&&rejected.state()!=PageState::Complete,"manifest missing coverage rejected");
    PageAttempt easy(stamp,"new-style",PageAttempt::source(layout),Form::Easy,resumed.snapshot());
    r=easy.next_request();require(r&&*r->source==layout.reading_text_utf8&&r->form==Form::Easy,"style changes preserve the extracted page input");
    PageLayout blank;blank.reading_text_utf8=" \n";
    PageAttempt empty(stamp,"none",PageAttempt::source(blank),Form::Plain);
    require(empty.state()==PageState::NoText&&!empty.next_request(),"blank PDF text is not sent to inference");
    PageAttempt completed(stamp,"conditions",source,Form::Plain,resumed.snapshot());
    require(completed.state()==PageState::Complete&&!completed.next_request()&&completed.display_text()=="完訳","revisiting a completed page reuses its translation");
    PageAttempt control(stamp,"control",source,Form::Plain);
    r=control.next_request();p=prepare(*r);control.prepared(p);
    const std::string unsafe("prefix\0hidden",13);
    require(!control.delta(p->stamp,unsafe),"control bytes cannot enter native stream");
    require(!control.delta(p->stamp,"\xff"),"invalid UTF-8 reached the GUI");
    control.finished(p->stamp,{FinishReason::Completed,unsafe,3});
    require(control.state()==PageState::Partial&&control.snapshot().chunks.front().text=="prefix"&&control.snapshot().chunks.front().reason==FinishReason::InvalidModelOutput,"NUL output cannot become a completed chunk");
    PageAttempt invalid(stamp,"invalid",source,Form::Plain);
    r=invalid.next_request();p=prepare(*r);invalid.prepared(p);invalid.delta(p->stamp,"prefix");
    invalid.finished(p->stamp,{FinishReason::Cancelled,"\xff",3});
    require(invalid.snapshot().chunks.front().reason==FinishReason::InvalidEncoding&&invalid.snapshot().chunks.front().text=="prefix","invalid partial result replaced a valid prefix");
    std::cout<<"extracted page input, completion reuse and stale events passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
