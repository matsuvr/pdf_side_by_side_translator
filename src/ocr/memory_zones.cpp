#include "blread.h"

namespace tesseract {
// A page supplied in memory has no companion zone file. Tesseract will
// segment the supplied page itself when this function returns false.
bool read_unlv_file(std::string&,int32_t,int32_t,BLOCK_LIST*){
    return false;
}
}
