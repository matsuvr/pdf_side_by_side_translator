#pragma once
#include "platform/file_snapshot.hpp"
#include <algorithm>
#include <functional>
#include <stdexcept>

namespace opt {
// PDFium's block callback must not throw through its C API. Reads use the
// already-open file object, not its pathname, and never expose an mmap of a
// truncatable POSIX file to the parser.
class PdfFileReader {
public:
    enum class Failure { None, Changed, Read, Cancelled };
    explicit PdfFileReader(std::shared_ptr<const FileSnapshot> file,
                           std::function<bool()> cancelled={})
        :file_(std::move(file)),cancelled_(std::move(cancelled)){}
    const std::shared_ptr<const FileSnapshot>& snapshot() const noexcept{return file_;}
    const FileSignature& initial_signature() const{return file_->initial_signature();}
    Failure failure() const noexcept{return failure_;}
    bool read(std::uint64_t offset,std::span<std::byte> bytes) noexcept{
        try{
            if(failure_!=Failure::None)return false;
            if(cancelled_&&cancelled_()){failure_=Failure::Cancelled;return false;}
            if(file_->signature()!=initial_signature()){failure_=Failure::Changed;return false;}
            if(offset>initial_signature().size||bytes.size()>initial_signature().size-offset){failure_=Failure::Read;return false;}
            while(!bytes.empty()){
                if(cancelled_&&cancelled_()){failure_=Failure::Cancelled;return false;}
                const auto count=file_->read_at(offset,bytes);
                if(!count){failure_=Failure::Changed;return false;}
                offset+=count;bytes=bytes.subspan(count);
            }
            if(file_->signature()!=initial_signature()){failure_=Failure::Changed;return false;}
            return true;
        }catch(...){failure_=Failure::Read;return false;}
    }
    void check() const{
        if(failure_==Failure::Cancelled)throw std::runtime_error("PDFの読み込みを中断しました。");
        if(failure_==Failure::Changed||file_->signature()!=initial_signature())
            throw std::runtime_error("PDFが外部で変更されました。再度開いてください。");
        if(failure_==Failure::Read)throw std::runtime_error("PDFの読み取りに失敗しました。再度開いてください。");
    }
private:
    std::shared_ptr<const FileSnapshot> file_;
    std::function<bool()> cancelled_;
    Failure failure_=Failure::None;
};
}
