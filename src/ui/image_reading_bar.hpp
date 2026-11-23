#pragma once
#include "ocr/image_reading.hpp"
#include <cstdint>
#include <functional>
#include <wx/panel.h>

class wxButton;
class wxCheckBox;
class wxStaticText;

namespace opt {
class ImageReadingBar final : public wxPanel {
public:
    ImageReadingBar(wxWindow* parent,std::function<void(bool)> toggle,
                    std::function<void(const ImageLanguage*)> correct);
    void update(bool enabled,const ImageLanguage* manual,const ImageReadingResult& result,bool failed);
private:
    void arrange();
    void choose_language();

    wxButton* word_;
    wxStaticText* suffix_;
    wxCheckBox* toggle_;
    wxString toggle_text_;
    std::function<void(const ImageLanguage*)> correct_;
    const ImageLanguage* current_=nullptr;
    const ImageLanguage* manual_=nullptr;
    ImageAlternatives alternatives_{};
    std::uint64_t revision_=0;
};
}
