#pragma once
namespace opt {
enum class ShortcutKey { Other, Enter, Period, Comma, G, F6, Escape };
enum class ShortcutAction { Native, Translate, TranslateAll, Stop, ModelSettings, FocusPage, CycleFocus, CloseSearch };
struct ShortcutContext {bool modal=false,composing=false,primary=false,shift=false,alt=false,search_open=false;};
inline ShortcutAction frame_shortcut(ShortcutKey key,const ShortcutContext& state){
    if(state.modal||state.composing||state.alt)return ShortcutAction::Native;
    if(state.primary){
        switch(key){case ShortcutKey::Enter:return state.shift?ShortcutAction::TranslateAll:ShortcutAction::Translate;
        case ShortcutKey::Period:return ShortcutAction::Stop;case ShortcutKey::Comma:return ShortcutAction::ModelSettings;
        case ShortcutKey::G:if(!state.shift)return ShortcutAction::FocusPage;break;default:break;}
    }else{
        if(key==ShortcutKey::F6)return ShortcutAction::CycleFocus;
        if(key==ShortcutKey::Escape&&state.search_open)return ShortcutAction::CloseSearch;
    }
    return ShortcutAction::Native;
}
}
