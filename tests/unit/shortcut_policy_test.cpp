#include "ui/shortcut_policy.hpp"
#include <stdexcept>
#include <iostream>
using namespace opt;
void require(bool v,const char* why){if(!v)throw std::runtime_error(why);}
int main(){try{
    ShortcutContext s;s.primary=true;require(frame_shortcut(ShortcutKey::Enter,s)==ShortcutAction::Native,"primary Enter stays native");s.shift=true;require(frame_shortcut(ShortcutKey::Enter,s)==ShortcutAction::Native,"primary Shift Enter stays native");
    for(bool modal:{false,true})for(bool composing:{false,true})if(modal||composing){s.modal=modal;s.composing=composing;
        for(auto key:{ShortcutKey::Enter,ShortcutKey::Comma,ShortcutKey::G,ShortcutKey::F6,ShortcutKey::Escape})require(frame_shortcut(key,s)==ShortcutAction::Native,"IME and modal input must stay native");}
    s={};s.search_open=true;require(frame_shortcut(ShortcutKey::Escape,s)==ShortcutAction::CloseSearch,"Escape closes search");s.search_open=false;require(frame_shortcut(ShortcutKey::Escape,s)==ShortcutAction::Native,"Escape never stops translation");
    require(frame_shortcut(ShortcutKey::Enter,s)==ShortcutAction::Native,"bare Enter stays with its text field");require(frame_shortcut(ShortcutKey::Other,s)==ShortcutAction::Native,"navigation keys stay native");
    s.primary=true;require(frame_shortcut(ShortcutKey::G,s)==ShortcutAction::FocusPage,"primary G focuses page number");
    std::cout<<"IME/modal shortcut isolation and native key priorities passed\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
