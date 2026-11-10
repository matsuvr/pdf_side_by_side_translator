#pragma once
namespace opt {
enum class ShortcutKey { Other, Enter, Comma, G, F6, Escape };
enum class ShortcutAction { Native, ModelSettings, FocusPage, CycleFocus, CloseSearch };
struct ShortcutContext {bool modal=false,composing=false,primary=false,shift=false,alt=false,search_open=false;};
inline ShortcutAction frame_shortcut(ShortcutKey key,const ShortcutContext& state){
    if(state.modal||state.composing||state.alt)return ShortcutAction::Native;
    if(state.primary){
        switch(key){case ShortcutKey::Comma:return ShortcutAction::ModelSettings;
        case ShortcutKey::G:if(!state.shift)return ShortcutAction::FocusPage;break;default:break;}
    }else{
        if(key==ShortcutKey::F6)return ShortcutAction::CycleFocus;
        if(key==ShortcutKey::Escape&&state.search_open)return ShortcutAction::CloseSearch;
    }
    return ShortcutAction::Native;
}
}
