#include "app/controller.hpp"
#include "product.hpp"
#include <wx/wx.h>
#include <wx/imagpng.h>
#ifdef OPT_PAGE_IMAGE_READING
#include "ocr/image_reading.hpp"
#include <optional>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

class ReaderApp final : public wxApp {
    std::unique_ptr<opt::ApplicationController> controller_;
#if defined(OPT_PAGE_IMAGE_READING) && defined(_WIN32)
    std::optional<int> worker_status_;
#endif
public:
    bool OnInit() override {
#ifdef _WIN32
        // ProhibitDynamicCode is defense in depth, not a functional requirement:
        // PDFium has no V8 and the CPU kernels are ahead-of-time compiled. Keep
        // the other process mitigations and start even when this policy cannot
        // be applied on the host.
        PROCESS_MITIGATION_DYNAMIC_CODE_POLICY policy{};
        policy.ProhibitDynamicCode=1;
        (void)SetProcessMitigationPolicy(ProcessDynamicCodePolicy,&policy,sizeof(policy));
#endif
#if defined(OPT_PAGE_IMAGE_READING) && defined(_WIN32)
        if(argc==3 && wxString(argv[1])==wxString::FromAscii(opt::image_worker_switch)){
            worker_status_=opt::run_image_worker(wxString(argv[2]).ToStdString(),opt::read_bundled_image_model);
            return true;
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
            if(argument==wxS("--help")||argument==wxS("-h")){
                wxMessageBox(wxS("起動引数: [PDF]\n\nPDFを指定せずに起動した場合は、アプリの「PDFを開く」から選べます。"),
                    wxString::FromUTF8(opt::product::name),wxOK|wxICON_INFORMATION);
                return false;
            }
            // Unknown switches (including Finder's -psn_* argument on macOS) are
            // ignored. A second path is ignored; the first argument wins.
            if(argument.StartsWith("-"))continue;
            if(pdf.empty())pdf=argument;
        }
        controller_=std::make_unique<opt::ApplicationController>();
        controller_->start(pdf);
        return true;
    }
#if defined(OPT_PAGE_IMAGE_READING) && defined(_WIN32)
    int OnRun() override {return worker_status_?*worker_status_:wxApp::OnRun();}
#endif
    int OnExit() override {controller_.reset();return 0;}
};
#if defined(OPT_PAGE_IMAGE_READING) && defined(__APPLE__)
wxIMPLEMENT_APP_NO_MAIN(ReaderApp);
int main(int argc,char** argv){
    // The worker has no windows and must avoid wx/AppKit initialization.
    if(argc==3 && std::string_view(argv[1])==opt::image_worker_switch)
        return opt::run_image_worker(argv[2],opt::read_bundled_image_model);
    return wxEntry(argc,argv);
}
#else
wxIMPLEMENT_APP(ReaderApp);
#endif
