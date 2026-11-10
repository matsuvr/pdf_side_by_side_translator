#include "translation/batch_run.hpp"
#include <iostream>
#include <stdexcept>
using namespace opt;
void require(bool v,const char* why){if(!v)throw std::runtime_error(why);}
int main(){try{
    BatchRun run(1000000,"snapshot-profile");require(run.next(500)==500,"current page first without preloading million pages");
    require(run.terminal(500,PageState::Complete),"completion recorded");require(run.next(500)==0,"physical cursor begins after priority");
    require(run.terminal(0,PageState::Partial),"partial terminal");require(!run.terminal(0,PageState::Complete),"same run never retries or recounts partial");
    require(run.next(700)==700,"new current page interrupts cursor priority");run.terminal(700,PageState::NoText);
    require(run.next(500)==1,"cursor retained across priority page");run.terminal(1,PageState::Failed);require(run.next(500)==2,"failed page skipped");
    require(run.counts().complete==1&&run.counts().partial==1&&run.counts().no_text==1&&run.counts().failed==1,"terminal categories distinct");
    run.stop();require(!run.next(900),"stop prevents automatic resume");
    BatchRun retry(3,"snapshot-profile");retry.terminal(0,PageState::Complete);require(retry.next(0)==1,"new run retries previously incomplete pages");retry.terminal(1,PageState::Partial);retry.terminal(2,PageState::NoText);require(!retry.active(),"run ends after all terminal categories");
    std::cout<<"lazy batch cursor, priority, terminal counts and stop passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
