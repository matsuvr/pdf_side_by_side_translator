#include "platform/text_view.hpp"
#include "platform/text_format.hpp"
#include <wx/textctrl.h>
#import <AppKit/AppKit.h>
#import <CoreText/CoreText.h>
#include <algorithm>

namespace opt {
namespace {
NSString* language_tag(std::string_view language){
    return language.empty()?nil:[[[NSString alloc] initWithBytes:language.data() length:language.size()
        encoding:NSUTF8StringEncoding] autorelease];
}
}
void set_reader_font(wxTextCtrl& control,int points,bool bundled_font,std::string_view language){
    @autoreleasepool {
        wxFontInfo font(points);
        if(language=="ja")font.FaceName(bundled_font?"Noto Sans CJK JP":"Hiragino Sans");
        const wxFont face(font);
        control.SetFont(face);
        auto* view=(NSTextView*)[(NSScrollView*)control.GetHandle() documentView];
        auto* storage=[view textStorage];
        const auto range=NSMakeRange(0,[storage length]);
        [storage beginEditing];
        [storage addAttribute:NSFontAttributeName value:face.OSXGetNSFont() range:range];
        auto* key=(NSString*)kCTLanguageAttributeName;
        if(auto* tag=language_tag(language))[storage addAttribute:key value:tag range:range];
        else [storage removeAttribute:key range:range];
        [storage endEditing];
    }
}
void style_reader_quotes(wxTextCtrl& control,std::span<const ReaderQuote> quotes,std::string_view language){
    if(quotes.empty())return;
    @autoreleasepool {
        auto* view=(NSTextView*)[(NSScrollView*)control.GetHandle() documentView];
        auto* storage=[view textStorage];
        auto* tag=language_tag(language);
        if(!tag)return;
        auto* base=[NSFont systemFontOfSize:control.GetFont().GetFractionalPointSize()];
        [storage beginEditing];
        for(const auto& quote:quotes){
            const auto range=NSMakeRange(quote.begin,quote.end-quote.begin);
            auto font=CTFontCreateForStringWithLanguage((CTFontRef)base,(CFStringRef)[storage string],
                CFRangeMake(range.location,range.length),(CFStringRef)tag);
            [storage addAttribute:(NSString*)kCTLanguageAttributeName value:tag range:range];
            [storage addAttribute:NSFontAttributeName value:font?(NSFont*)font:base range:range];
            if(font)CFRelease(font);
        }
        [storage endEditing];
    }
}
bool update_readonly_text(wxTextCtrl& control,const std::string& before,const std::string& after,bool reset,
                          std::string_view language){
    @autoreleasepool {
        auto* scroll=(NSScrollView*)control.GetHandle();
        auto* text=(NSTextView*)[scroll documentView];
        if(![text isKindOfClass:[NSTextView class]])return false;
        NSString* suffix=nil;
        std::size_t at=0,length=0;
        const auto tail=reset?std::string_view{}:appended_utf8_suffix(before,after);
        constexpr std::string_view bom="\xef\xbb\xbf";
        // Decode BOMs at their original offsets in the complete string.
        if(!tail.empty()&&!std::string_view(before).starts_with(bom)&&!tail.starts_with(bom)){
            if(const auto units=utf16_units(before)){
                suffix=[[[NSString alloc] initWithBytes:tail.data() length:tail.size()
                    encoding:NSUTF8StringEncoding] autorelease];
                if(!suffix)return false;
                at=*units;
                length=at+[suffix length];
            }
        }
        if(!suffix){
            auto* value=[[[NSString alloc] initWithBytes:after.data() length:after.size() encoding:NSUTF8StringEncoding] autorelease];
            if(!value)return false;
            std::size_t common=0;
            if(!reset){while(common<std::min(before.size(),after.size())&&before[common]==after[common])++common;
                while(common<after.size()&&common>0&&(static_cast<unsigned char>(after[common])&0xc0)==0x80)--common;}
            auto* prefix=[[NSString alloc] initWithBytes:after.data() length:common encoding:NSUTF8StringEncoding];
            at=[prefix length];
            length=[value length];
            suffix=[value substringFromIndex:at];
            [prefix release];
        }
        auto* clip=[scroll contentView];const auto origin=[clip bounds].origin;
        const auto selection=[text selectedRange];
        const bool follow=!reset&&selection.length==0&&NSMaxY([clip bounds])>=[text bounds].size.height-4;
        const auto old_length=[[text string] length];
        auto* storage=[text textStorage];[storage beginEditing];
        [storage replaceCharactersInRange:NSMakeRange(std::min(at,old_length),old_length-std::min(at,old_length)) withString:suffix];
        if([suffix length]){
            NSMutableDictionary* attributes=[NSMutableDictionary dictionary];
            attributes[NSFontAttributeName]=control.GetFont().OSXGetNSFont();
            if(auto* tag=language_tag(language))attributes[(NSString*)kCTLanguageAttributeName]=tag;
            if([text textColor])attributes[NSForegroundColorAttributeName]=[text textColor];
            [storage setAttributes:attributes range:NSMakeRange(at,[suffix length])];
        }
        [storage endEditing];
        [text setSelectedRange:reset?NSMakeRange(0,0):NSMakeRange(std::min(selection.location,length),std::min(selection.length,length-std::min(selection.location,length)))];
        [[text layoutManager] ensureLayoutForTextContainer:[text textContainer]];
        if(follow)[text scrollRangeToVisible:NSMakeRange(length,0)];
        else {[clip scrollToPoint:reset?NSZeroPoint:origin];[scroll reflectScrolledClipView:clip];}
        return !tail.empty();
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
