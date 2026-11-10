#include "storage/diagnostic_log.hpp"
#include "platform/digest.hpp"
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace opt;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(){const auto root=std::filesystem::temp_directory_path()/("opt-log-"+new_uuid());
    try{
        DiagnosticRecord record;record.code=DiagnosticCode::ChunkFinished;record.seconds=std::numeric_limits<double>::quiet_NaN();
        require(diagnostic_line(record).size()<256&&diagnostic_line(record).find("nan")==std::string::npos,"record is bounded and nonfinite times are rejected");
        append_diagnostic(root,record);
        for(int generation=0;generation<4;++generation){
            {std::ofstream fill(root/"diagnostic.log",std::ios::binary|std::ios::trunc);fill<<std::string(diagnostic_log_limit-2,'x');}
            append_diagnostic(root,record);
        }
        require(std::filesystem::file_size(root/"diagnostic.log")<256,"new generation contains one complete record");
        require(std::filesystem::exists(root/"diagnostic.log.1")&&std::filesystem::exists(root/"diagnostic.log.2")&&!std::filesystem::exists(root/"diagnostic.log.3"),"only two historical generations retained");
        std::uintmax_t total=0;for(const auto& f:std::filesystem::directory_iterator(root))total+=f.file_size();
        require(total<=3*diagnostic_log_limit,"diagnostic disk storage stays bounded");
        std::filesystem::remove_all(root);std::cout<<"bounded structured diagnostics and three-generation rotation passed\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
