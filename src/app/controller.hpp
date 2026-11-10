#pragma once
#include <memory>
#include <wx/event.h>

namespace opt {
class ApplicationController final : public wxEvtHandler {
public:
    ApplicationController();
    ~ApplicationController() override;
    void start(const wxString& initial_pdf={});
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
