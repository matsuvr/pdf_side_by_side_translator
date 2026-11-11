#pragma once
#include "core/types.hpp"
#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace opt {
// GUI-owned source edits are session data, not a page cache. In particular a
// reset to automatic extraction is an edit too: an older asynchronous load
// must not resurrect the text that the user just removed.
class SessionSources {
    struct Entry {
        SourceRevision revision=1;
        std::shared_ptr<const std::string> text;
        bool edited=false;
    };
    using Entries=std::map<PageIndex,Entry>;
    std::map<std::string,Entries> documents_;
    std::string document_;
    SourceRevision clock_=1;
    const Entry* find(PageIndex page) const {
        const auto document=documents_.find(document_);
        if(document==documents_.end())return nullptr;
        const auto at=document->second.find(page);
        return at==document->second.end()?nullptr:&at->second;
    }
public:
    // Keep manual edits until application exit, even if an I/O mirror failed
    // and the document is closed/reopened. Identity includes file generation;
    // editing a replaced PDF must never reuse the old document's source.
    void select_document(std::string identity) {
        if(auto at=documents_.find(document_);at!=documents_.end()){
            std::erase_if(at->second,[](const auto& item){return !item.second.edited;});
            if(at->second.empty())documents_.erase(at);
        }
        document_=std::move(identity);
    }
    std::shared_ptr<const std::string> source(PageIndex page) const {
        const auto* entry=find(page);return entry?entry->text:nullptr;
    }
    SourceRevision revision(PageIndex page) const {
        const auto* entry=find(page);return entry?entry->revision:1;
    }
    void load(PageIndex page,SourceRevision revision,std::optional<std::string> text) {
        if(document_.empty())return;
        const auto* entry=find(page);
        if(entry&&entry->edited)return;
        const auto value=std::max(SourceRevision{1},revision);
        clock_=std::max(clock_,value);
        documents_[document_].insert_or_assign(page,Entry{value,text?std::make_shared<const std::string>(std::move(*text)):nullptr,false});
    }
    SourceRevision edit(PageIndex page,std::shared_ptr<const std::string> text) {
        if(document_.empty())throw std::logic_error("Source edit requires a document identity");
        auto& entry=documents_[document_][page];
        clock_=std::max(clock_,entry.revision)+1;
        entry={clock_,std::move(text),true};
        return clock_;
    }
    void evict_loaded(PageIndex page) {
        const auto document=documents_.find(document_);if(document==documents_.end())return;
        auto at=document->second.find(page);
        if(at!=document->second.end()&&!at->second.edited)document->second.erase(at);
    }
    void clear() { documents_.clear();document_.clear(); }
};
}
