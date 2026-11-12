#include "app/controller.hpp"
#include "product.hpp"
#include <wx/wx.h>
#include <wx/imagpng.h>
#ifdef _WIN32
#include <windows.h>
#endif

class ReaderApp final : public wxApp {
    std::unique_ptr<opt::ApplicationController> controller_;
public:
    bool OnInit() override {
#ifdef _WIN32
        // PDFium has no V8 and the CPU kernels are ahead-of-time compiled.
        PROCESS_MITIGATION_DYNAMIC_CODE_POLICY policy{};
        policy.ProhibitDynamicCode=1;
        if(!SetProcessMitigationPolicy(ProcessDynamicCodePolicy,&policy,sizeof(policy))){
            wxMessageBox(wxS("Windowsの動的コード保護を有効にできません。"),wxString::FromUTF8(opt::product::name),wxOK|wxICON_ERROR);
            return false;
        }
#endif
        // Only the embedded PNG icon is decoded through wx. Registering every
        // image codec would pull a second, unprefixed JPEG implementation into
        // the process alongside PDFium's static libjpeg-turbo.
        wxImage::AddHandler(new wxPNGHandler);
        SetAppName(wxString::FromUTF8(opt::product::name));
        SetAppDisplayName(wxString::FromUTF8(opt::product::name));
        wxString pdf;
        for(int i=1;i<argc;++i){
            const wxString argument=argv[i];
            if(!argument.StartsWith("-")&&pdf.empty())pdf=argument;
            else{wxMessageBox(wxS("起動引数: [PDF]（文書データはメモリ内だけに保持します）"),wxString::FromUTF8(opt::product::name),wxOK|wxICON_INFORMATION);return false;}
        }
        controller_=std::make_unique<opt::ApplicationController>();
        controller_->start(pdf);
        return true;
    }
    int OnExit() override {controller_.reset();return 0;}
};
wxIMPLEMENT_APP(ReaderApp);
