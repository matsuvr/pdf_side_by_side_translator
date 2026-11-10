#include "platform/input_state.hpp"
#include <wx/window.h>
#import <AppKit/AppKit.h>
namespace opt {
namespace {
bool composing(NSWindow* window){id responder=[window firstResponder];return [responder conformsToProtocol:@protocol(NSTextInputClient)]&&[(id<NSTextInputClient>)responder hasMarkedText];}
}
bool input_composing(wxWindow* focused){
    if(!focused)return false;
    return composing([(NSView*)focused->GetHandle() window]);
}
struct NativeShortcutMonitor::Impl {
    id monitor=nil;
    Handler handler;
    NSWindow* window;
    Impl(wxWindow& owner,Handler callback):handler(std::move(callback)),window([(NSView*)owner.GetHandle() window]){
        auto* state=this;
        monitor=[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent*(NSEvent* event){
            if([event window]!=state->window||[[NSApp mainMenu] highlightedItem])return event;
            const auto modifiers=[event modifierFlags];auto* chars=[event charactersIgnoringModifiers];if(![chars length])return event;
            const auto character=[chars characterAtIndex:0];ShortcutKey key=ShortcutKey::Other;
            if(character=='\r'||character==3)key=ShortcutKey::Enter;else if(character=='.')key=ShortcutKey::Period;
            else if(character==',')key=ShortcutKey::Comma;else if(character=='g'||character=='G')key=ShortcutKey::G;
            else if(character==NSF6FunctionKey)key=ShortcutKey::F6;else if(character==27)key=ShortcutKey::Escape;
            if(key==ShortcutKey::Other)return event;
            ShortcutContext context;context.composing=composing(state->window);context.primary=(modifiers&NSEventModifierFlagCommand)!=0;
            context.shift=(modifiers&NSEventModifierFlagShift)!=0;context.alt=(modifiers&(NSEventModifierFlagOption|NSEventModifierFlagControl))!=0;
            return state->handler(key,context)?nil:event;
        }];
    }
    ~Impl(){if(monitor)[NSEvent removeMonitor:monitor];}
};
NativeShortcutMonitor::NativeShortcutMonitor(wxWindow& window,Handler handler):impl_(std::make_unique<Impl>(window,std::move(handler))){}
NativeShortcutMonitor::~NativeShortcutMonitor()=default;
}
