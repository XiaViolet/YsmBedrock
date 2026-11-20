#include "UiTextAtlas.h"
#include <Windows.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <stdexcept>

namespace ysm {
namespace {
std::wstring wide(std::string const& text) {
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0);
    if(!n)return {};std::wstring result(n,0);MultiByteToWideChar(CP_UTF8,0,text.data(),static_cast<int>(text.size()),result.data(),n);return result;
}
struct FontContext {
    HDC dc=CreateCompatibleDC(nullptr);
    HGDIOBJ originalFont=dc?GetCurrentObject(dc,OBJ_FONT):nullptr;
    std::map<int,HFONT> fonts;
    ~FontContext(){if(dc&&originalFont)SelectObject(dc,originalFont);for(auto const& [n,font]:fonts)DeleteObject(font);if(dc)DeleteDC(dc);}
    void font(int pixels) {
        if(!dc)throw std::runtime_error("Cannot create UI font context");
        auto [i,inserted]=fonts.try_emplace(pixels,nullptr);
        if(inserted)i->second=CreateFontW(-pixels,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_TT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Microsoft YaHei UI");
        if(!i->second)throw std::runtime_error("Cannot create smooth UI font");SelectObject(dc,i->second);
    }
};
struct Label {UiRect source;std::vector<unsigned char> rgba;int width,height,x=0,y=0;};
}
std::uint64_t uiTextKey(std::span<UiPaint const> paint,float density) {
    std::uint64_t hash=1469598103934665603ull;auto byte=[&](unsigned char v){hash^=v;hash*=1099511628211ull;};
    auto word=[&](std::uint32_t v){for(int i=0;i<4;++i)byte(static_cast<unsigned char>(v>>(i*8)));};word(std::bit_cast<std::uint32_t>(density));
    for(auto const& p:paint)if(!p.text.empty()) {for(auto c:p.text)byte(static_cast<unsigned char>(c));byte(0);word(p.color);
        for(float f:{p.rect.x,p.rect.y,p.rect.w,p.rect.h,p.scale})word(std::bit_cast<std::uint32_t>(f));byte(p.centered);}
    return hash;
}
UiTextAtlas rasterizeUiText(std::span<UiPaint const> paint,float density) {
    UiTextAtlas result;result.pixelDensity=std::clamp(density,.5f,12.f);density=result.pixelDensity;
    FontContext fonts;std::vector<Label> labels;constexpr int atlasWidth=2048,atlasLimit=4096;
    for(auto const& p:paint) {
        if(p.text.empty()||p.rect.w<=0||!(p.color>>24))continue;
        if(labels.size()>=512)throw std::runtime_error("UI text label budget exceeded");
        auto text=wide(p.text);if(text.empty())continue;
        int pixels=std::clamp(static_cast<int>(std::round(p.scale*10*density)),8,128);fonts.font(pixels);
        SIZE extent{};if(!GetTextExtentPoint32W(fonts.dc,text.data(),static_cast<int>(text.size()),&extent))continue;
        int width=std::clamp(static_cast<int>(std::ceil(std::min(p.rect.w*density,static_cast<float>(extent.cx+2)))),1,atlasWidth-4);
        int height=std::clamp(static_cast<int>(std::ceil(p.rect.h*density)),pixels+2,256);
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=width;info.bmiHeader.biHeight=-height;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        void* bits=nullptr;HBITMAP bitmap=CreateDIBSection(fonts.dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        if(!bitmap||!bits){if(bitmap)DeleteObject(bitmap);throw std::runtime_error("Cannot rasterize UI label");}
        auto original=SelectObject(fonts.dc,bitmap);std::fill_n(static_cast<unsigned char*>(bits),width*height*4,0);
        SetTextColor(fonts.dc,RGB(255,255,255));SetBkColor(fonts.dc,RGB(0,0,0));SetBkMode(fonts.dc,OPAQUE);
        RECT rect{0,0,width,height};DrawTextW(fonts.dc,text.data(),static_cast<int>(text.size()),&rect,DT_LEFT|DT_TOP|DT_SINGLELINE|DT_NOPREFIX|DT_END_ELLIPSIS);
        GdiFlush();Label label;label.width=width;label.height=height;label.source={p.rect.x,p.rect.y,width/density,height/density};
        if(p.centered)label.source.x+=(p.rect.w-label.source.w)/2;
        label.rgba.resize(width*height*4);auto* source=static_cast<unsigned char const*>(bits);
        for(std::size_t i=0;i<label.rgba.size();i+=4){auto alpha=std::max({source[i],source[i+1],source[i+2]});
            label.rgba[i]=static_cast<unsigned char>((p.color>>16)&255);label.rgba[i+1]=static_cast<unsigned char>((p.color>>8)&255);label.rgba[i+2]=static_cast<unsigned char>(p.color&255);
            label.rgba[i+3]=static_cast<unsigned char>(alpha*((p.color>>24)&255)/255);}
        SelectObject(fonts.dc,original);DeleteObject(bitmap);labels.push_back(std::move(label));
    }
    int x=2,y=2,rowHeight=0;for(auto& label:labels){if(x+label.width+2>atlasWidth){x=2;y+=rowHeight+2;rowHeight=0;}
        label.x=x;label.y=y;x+=label.width+2;rowHeight=std::max(rowHeight,label.height);}
    int height=std::max(4,y+rowHeight+2);if(height>atlasLimit)throw std::runtime_error("UI font atlas budget exceeded");
    result.image.width=atlasWidth;result.image.height=height;result.image.format="rgba";result.image.rgba.resize(static_cast<std::size_t>(atlasWidth)*height*4);
    for(auto const& label:labels){for(int row=0;row<label.height;++row)std::copy_n(label.rgba.data()+static_cast<std::size_t>(row)*label.width*4,label.width*4,
        result.image.rgba.data()+(static_cast<std::size_t>(label.y+row)*atlasWidth+label.x)*4);
        result.quads.push_back({label.source,{static_cast<float>(label.x)/atlasWidth,static_cast<float>(label.y)/height,static_cast<float>(label.width)/atlasWidth,static_cast<float>(label.height)/height}});}
    return result;
}
}
