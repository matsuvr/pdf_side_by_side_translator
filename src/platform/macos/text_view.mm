#include "platform/text_view.hpp"
#include <wx/textctrl.h>
#import <AppKit/AppKit.h>
#include <algorithm>

namespace opt {
void update_readonly_text(wxTextCtrl& control,const std::string& before,const std::string& after,bool reset){
    @autoreleasepool {
        auto* scroll=(NSScrollView*)control.GetHandle();
        auto* text=(NSTextView*)[scroll documentView];
        if(![text isKindOfClass:[NSTextView class]])return;
        auto* value=[[NSString alloc] initWithBytes:after.data() length:after.size() encoding:NSUTF8StringEncoding];
        if(!value)return;
        auto* clip=[scroll contentView];const auto origin=[clip bounds].origin;
        const auto selection=[text selectedRange];
        const bool follow=!reset&&selection.length==0&&NSMaxY([clip bounds])>=[text bounds].size.height-4;
        std::size_t common=0;
        if(!reset){while(common<std::min(before.size(),after.size())&&before[common]==after[common])++common;
            while(common<after.size()&&common>0&&(static_cast<unsigned char>(after[common])&0xc0)==0x80)--common;}
        auto* prefix=[[NSString alloc] initWithBytes:after.data() length:common encoding:NSUTF8StringEncoding];
        const auto at=[prefix length];const auto old_length=[[text string] length];
        auto* suffix=[value substringFromIndex:at];
        auto* storage=[text textStorage];[storage beginEditing];
        [storage replaceCharactersInRange:NSMakeRange(std::min(at,old_length),old_length-std::min(at,old_length)) withString:suffix];
        if([suffix length]){
            NSMutableDictionary* attributes=[NSMutableDictionary dictionary];
            if([text font])attributes[NSFontAttributeName]=[text font];
            if([text textColor])attributes[NSForegroundColorAttributeName]=[text textColor];
            [storage setAttributes:attributes range:NSMakeRange(at,[suffix length])];
        }
        [storage endEditing];
        const auto length=[value length];
        [text setSelectedRange:reset?NSMakeRange(0,0):NSMakeRange(std::min(selection.location,length),std::min(selection.length,length-std::min(selection.location,length)))];
        [[text layoutManager] ensureLayoutForTextContainer:[text textContainer]];
        if(follow)[text scrollRangeToVisible:NSMakeRange(length,0)];
        else {[clip scrollToPoint:reset?NSZeroPoint:origin];[scroll reflectScrolledClipView:clip];}
        [prefix release];[value release];
    }
}
std::size_t text_logical_anchor(wxTextCtrl& control){
    @autoreleasepool {
        auto* scroll=(NSScrollView*)control.GetHandle();auto* text=(NSTextView*)[scroll documentView];
        const auto visible=[[scroll contentView] bounds];
        const auto at=std::min([text characterIndexForInsertionAtPoint:NSMakePoint(NSMinX(visible)+3,NSMinY(visible)+1)],[[text string] length]);
        return [[[text string] substringToIndex:at] lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    }
}
void restore_text_logical_anchor(wxTextCtrl& control,std::size_t offset){
    @autoreleasepool {
        auto* scroll=(NSScrollView*)control.GetHandle();auto* text=(NSTextView*)[scroll documentView];
        auto* bytes=[[text string] dataUsingEncoding:NSUTF8StringEncoding];const auto* raw=(const unsigned char*)[bytes bytes];
        offset=std::min(offset,static_cast<std::size_t>([bytes length]));while(offset>0&&offset<[bytes length]&&(raw[offset]&0xc0)==0x80)--offset;
        auto* prefix=[[NSString alloc] initWithBytes:raw length:offset encoding:NSUTF8StringEncoding];if(!prefix)return;
        const auto at=[prefix length];[prefix release];
        if(at==0){[[scroll contentView] scrollToPoint:NSZeroPoint];[scroll reflectScrolledClipView:[scroll contentView]];return;}
        auto* manager=[text layoutManager];[manager ensureLayoutForTextContainer:[text textContainer]];
        const auto glyphs=[manager glyphRangeForCharacterRange:NSMakeRange(at<[[text string] length]?at:at-1,1) actualCharacterRange:nil];
        const auto rectangle=[manager boundingRectForGlyphRange:glyphs inTextContainer:[text textContainer]];
        auto point=[[scroll contentView] bounds].origin;point.y=rectangle.origin.y+[text textContainerOrigin].y;
        [[scroll contentView] scrollToPoint:point];[scroll reflectScrolledClipView:[scroll contentView]];
    }
}
}
