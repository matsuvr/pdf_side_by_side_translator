// M0 only: validates the combined static dependency stack in a native window.
#include "inference/engine.hpp"
#include "platform/font.hpp"
#include "fpdfview.h"
#include "fpdf_text.h"
#include <wx/wx.h>
#include <wx/splitter.h>
#include <wx/stdpaths.h>
#include <atomic>
#include <fstream>
#include <iterator>
#include <thread>

namespace {
struct ProbeResult {
    int width = 0, height = 0;
    std::vector<unsigned char> rgb;
    std::string text;
    bool success = false;
};
wxDEFINE_EVENT(PROBE_FINISHED, wxThreadEvent);

ProbeResult run_probe(const std::filesystem::path& pdf, const std::filesystem::path& gguf,
                      std::atomic_bool& cancelled, bool cpu) {
    ProbeResult r;
    std::ifstream stream(pdf, std::ios::binary);
    if (!stream) throw std::runtime_error("PDFを開けません。");
    std::vector<char> bytes{std::istreambuf_iterator<char>(stream), {}};
    FPDF_LIBRARY_CONFIG config{}; config.version = 2;
    FPDF_InitLibraryWithConfig(&config);
    struct Library { ~Library() { FPDF_DestroyLibrary(); } } library;
    auto document = FPDF_LoadMemDocument64(bytes.data(), bytes.size(), nullptr);
    if (!document) throw std::runtime_error("PDFを解析できません。");
    struct Document { FPDF_DOCUMENT d; ~Document() { FPDF_CloseDocument(d); } } doc{document};
    auto page = FPDF_LoadPage(document, 0);
    if (!page) throw std::runtime_error("ページを開けません。");
    struct Page { FPDF_PAGE p; ~Page() { FPDF_ClosePage(p); } } pg{page};
    auto text = FPDFText_LoadPage(page);
    if (!text) throw std::runtime_error("文字を抽出できません。");
    struct Text { FPDF_TEXTPAGE t; ~Text() { FPDFText_ClosePage(t); } } tx{text};
    r.width = 612; r.height = 792;
    std::vector<unsigned char> bgra(r.width * r.height * 4, 255);
    auto bitmap = FPDFBitmap_CreateEx(r.width,r.height,FPDFBitmap_BGRA,bgra.data(),r.width*4);
    if (!bitmap) throw std::runtime_error("Bitmapを確保できません。");
    struct Bitmap { FPDF_BITMAP b; ~Bitmap() { FPDFBitmap_Destroy(b); } } bmp{bitmap};
    FPDFBitmap_FillRect(bitmap,0,0,r.width,r.height,0xffffffff);
    FPDF_RenderPageBitmap(bitmap,page,0,0,r.width,r.height,0,FPDF_ANNOT);
    r.rgb.resize(r.width*r.height*3);
    for (std::size_t i=0,j=0;i<bgra.size();i+=4,j+=3) {
        r.rgb[j]=bgra[i+2]; r.rgb[j+1]=bgra[i+1]; r.rgb[j+2]=bgra[i];
    }
    std::string source;
    for(int i=0;i<FPDFText_CountChars(text);++i) {
        auto u=FPDFText_GetUnicode(text,i);
        if (u<128) source+=static_cast<char>(u);
    }
    opt::Engine engine(cancelled);
    try { engine.load(gguf,cpu); }
    catch (...) { if(cpu || cancelled.load()) throw; engine.load(gguf,true); }
    auto input=engine.prepare(source,opt::Form::Plain);
    auto result=engine.generate(input,[](std::string_view){});
    r.success=result.reason==opt::FinishReason::Completed;
    r.text="バックエンド: "+engine.backend()+"\n\n"+result.text;
    if(!r.success) r.text += "\n\n未完了: " + std::to_string(static_cast<int>(result.reason));
    return r;
}

class ProbeApp : public wxApp {
    wxFrame* frame_ = nullptr;
    wxStaticBitmap* bitmap_ = nullptr;
    wxTextCtrl* output_ = nullptr;
    wxTimer timer_{this};
    std::thread worker_;
    std::atomic_bool cancelled_{false}, finished_{false};
    bool closing_ = false;
    bool auto_exit_ = false;
    int result_ = 1;
public:
    bool OnInit() override {
        if (argc < 3) return false;
        auto pdf=std::filesystem::u8path(argv[1].ToUTF8().data());
        auto model=std::filesystem::u8path(argv[2].ToUTF8().data());
        const bool cpu=argc>3 && argv[3]=="cpu";
        auto_exit_=argc>4 && argv[4]=="auto-exit";
        std::string font_error;
        if (!opt::register_private_font(std::filesystem::u8path(wxStandardPaths::Get().GetResourcesDir().ToUTF8().data()),font_error))
            wxLogWarning("同梱フォントの登録に失敗しました: %s",wxString::FromUTF8(font_error));
        frame_=new wxFrame(nullptr,wxID_ANY,"Offline PDF Translator — 依存関係検証",wxDefaultPosition,wxSize(1280,860));
        auto menu=new wxMenu;
        menu->Append(wxID_EXIT,"終了\tCtrl+Q");
        auto bar=new wxMenuBar;bar->Append(menu,"ファイル");frame_->SetMenuBar(bar);
        frame_->Bind(wxEVT_MENU,[this](wxCommandEvent&){frame_->Close();},wxID_EXIT);
        auto split=new wxSplitterWindow(frame_);
        auto left=new wxScrolledWindow(split); left->SetScrollRate(10,10);
        auto ls=new wxBoxSizer(wxVERTICAL);
        bitmap_=new wxStaticBitmap(left,wxID_ANY,wxBitmap(1,1));
        ls->Add(bitmap_,0,wxALL,8); left->SetSizer(ls);
        output_=new wxTextCtrl(split,wxID_ANY,"PDF描画・モデル読込・翻訳を検証しています。",wxDefaultPosition,wxDefaultSize,wxTE_MULTILINE|wxTE_READONLY);
        auto font=output_->GetFont();font.SetFaceName("Noto Sans CJK JP");font.SetPointSize(14);output_->SetFont(font);
        split->SplitVertically(left,output_,620);split->SetSashGravity(0.5);
        frame_->CreateStatusBar();frame_->SetStatusText("M0: 静的リンク構成の検証中");
        Bind(PROBE_FINISHED,[this](wxThreadEvent& event){
            auto r=event.GetPayload<std::shared_ptr<ProbeResult>>();
            if(!closing_){
                if(!r->rgb.empty()){
                    wxImage image(r->width,r->height,r->rgb.data(),true);
                    bitmap_->SetBitmap(wxBitmap(image));bitmap_->GetParent()->FitInside();
                }
                output_->ChangeValue(wxString::FromUTF8(r->text));
                frame_->SetStatusText(r->success?"検証完了: PDF表示・日本語フォント・実GGUF翻訳":"検証失敗");
            }
            result_=r->success?0:1;
            if(auto_exit_){closing_=true;timer_.StartOnce(2000);}
        });
        frame_->Bind(wxEVT_CLOSE_WINDOW,[this](wxCloseEvent& e){
            closing_=true;
            if(!finished_.load()){
                e.Veto();closing_=true;cancelled_=true;
                frame_->SetStatusText("終了処理中…");timer_.Start(50);
            } else {if(worker_.joinable())worker_.join();frame_=nullptr;e.Skip();}
        });
        Bind(wxEVT_TIMER,[this](wxTimerEvent&){
            if(closing_ && finished_.load()){
                timer_.Stop();if(worker_.joinable())worker_.join();frame_->Destroy();frame_=nullptr;
            }
        });
        frame_->Show();
        worker_=std::thread([this,pdf,model,cpu]{
            auto r=std::make_shared<ProbeResult>();
            try {*r=run_probe(pdf,model,cancelled_,cpu);}
            catch(const std::exception& e){r->text=e.what();}
            auto event=new wxThreadEvent(PROBE_FINISHED);event->SetPayload(r);
            wxQueueEvent(this,event);finished_=true;
        });
        return true;
    }
    int OnRun() override {wxApp::OnRun();return result_;}
    int OnExit() override {cancelled_=true;if(worker_.joinable())worker_.join();return result_;}
};
}
wxIMPLEMENT_APP(ProbeApp);
