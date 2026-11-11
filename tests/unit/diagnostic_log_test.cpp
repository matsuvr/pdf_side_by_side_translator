#include "storage/diagnostic_log.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace opt;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(){
    try{
        DiagnosticRecord record;record.code=DiagnosticCode::ChunkFinished;record.seconds=std::numeric_limits<double>::quiet_NaN();
        auto line=diagnostic_line(record);
        require(line.size()<256&&line.find("nan")==std::string::npos,"record is bounded and nonfinite times are rejected");
        require(line.find("chunk-finished backend=CPU")!=std::string::npos&&line.find("seconds=0.000")!=std::string::npos,"diagnostic contains only structured operational fields");
        record.code=static_cast<DiagnosticCode>(999);record.metal=true;record.context=std::numeric_limits<std::uint32_t>::max();record.threads=record.batch=record.micro_batch=record.page=record.outcome=record.context;record.seconds=999999999;
        line=diagnostic_line(record);require(line.size()<256&&line.find("unknown backend=Metal")!=std::string::npos,"unknown code and maximum numeric fields remain bounded");
        require(line.back()=='\n'&&line.find('\n')==line.size()-1,"one diagnostic produces one line without free-form text");
        std::cout<<"bounded in-memory structured diagnostics passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
