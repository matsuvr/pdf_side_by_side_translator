#pragma once
#include "pdf/pdf_service.hpp"
#ifdef _WIN32
namespace opt {
// The Windows UI only owns the IPC client. PdfService (PDFium) is instantiated
// inside the restricted worker, never by ApplicationController.
class RestrictedPdfService {
public:
    using Delivery=PdfService::Delivery;
    explicit RestrictedPdfService(Delivery,std::filesystem::path font_resource_directory={},LayoutOptions={});
    ~RestrictedPdfService();
    RestrictedPdfService(const RestrictedPdfService&)=delete;
    RestrictedPdfService& operator=(const RestrictedPdfService&)=delete;
    void open(DocumentEpoch,const std::filesystem::path&,std::string_view password={});
    void close(DocumentEpoch);
    void page_info(DocumentEpoch,PageIndex);
    void viewport(ViewRequest);
    bool render_page_image(DocumentEpoch,PageIndex,int,std::uint64_t);
    bool extract(DocumentEpoch,PageIndex);
    void search(PdfSearchRequest);
    void cancel_search(DocumentEpoch,std::uint64_t);
    void stop()noexcept;
    bool done()const noexcept;
    std::uint32_t worker_process_id()const noexcept; // Read-only process identity for diagnostics/tests.
    void join();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
using ApplicationPdfService=RestrictedPdfService;
inline constexpr char pdf_worker_switch[]="--internal-pdf-worker";
// Worker mode accepts only the three handles established by the launcher.
int run_pdf_worker(std::string_view input,std::string_view output,std::string_view file);
}
#else
namespace opt {using ApplicationPdfService=PdfService;}
#endif
