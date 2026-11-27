#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <initializer_list>
#include <string_view>
#include <vector>

namespace ysm {
enum class UiIcon : std::uint8_t { None, Close, Check, Prev, Next, Search, Model, Texture, Play, Wheel, Settings, Info, Star, Reload, Stop, Minus, Plus };
inline constexpr std::array<std::string_view,17> uiIconNames{"", "close", "check", "prev", "next", "search", "model", "texture", "play", "wheel", "settings", "info", "star", "reload", "stop", "minus", "plus"};
inline UiIcon uiIcon(std::string_view name) {
    auto found=std::find(uiIconNames.begin(),uiIconNames.end(),name);
    return found==uiIconNames.end()?UiIcon::None:static_cast<UiIcon>(found-uiIconNames.begin());
}
inline int uiIconPixels(float size,float density) {
    auto pixels=size*density;
    return std::isfinite(pixels)?static_cast<int>(std::round(std::clamp(pixels,1.f,512.f))):1;
}
inline std::vector<unsigned char> rasterizeUiIcon(UiIcon icon,int pixels) {
    pixels=std::clamp(pixels,1,512);
    struct Point {float x,y;};
    struct Segment {Point a,b;float dx,dy,invLength,r;};
    struct Circle {Point center;float radius,stroke;};
    struct Square {Point center;float half,radius;};
    std::vector<Segment> segments;
    std::vector<Circle> circles;
    std::vector<Square> squares;
    constexpr float pi=3.14159265358979323846f;
    const float stroke=std::clamp(1.65f/static_cast<float>(pixels),.085f,.15f)*.5f;
    auto line=[&](Point a,Point b){float dx=b.x-a.x,dy=b.y-a.y;float len=dx*dx+dy*dy;segments.push_back({a,b,dx,dy,len>0?1.f/len:0.f,stroke});};
    auto path=[&](std::initializer_list<Point> points){if(points.size()<2)return;auto a=points.begin();for(auto b=a+1;b!=points.end();++a,++b)line(*a,*b);};
    auto polar=[&](float x,float y,float r,float a){return Point{x+r*std::cos(a),y+r*std::sin(a)};};
    auto ring=[&](float x,float y,float r){circles.push_back({{x,y},r,stroke});};
    auto dot=[&](float x,float y,float r){circles.push_back({{x,y},r,-1});};
    auto arc=[&](float x,float y,float r,float start,float end){int steps=std::max(12,static_cast<int>(std::ceil(std::abs(end-start)*8)));auto prev=polar(x,y,r,start);for(int i=1;i<=steps;++i){auto next=polar(x,y,r,start+(end-start)*i/steps);line(prev,next);prev=next;}};
    switch(icon) {
    case UiIcon::Close: path({{.25f,.25f},{.75f,.75f}});path({{.75f,.25f},{.25f,.75f}});break;
    case UiIcon::Check: path({{.17f,.52f},{.40f,.75f},{.84f,.24f}});break;
    case UiIcon::Prev: path({{.61f,.20f},{.32f,.50f},{.61f,.80f}});break;
    case UiIcon::Next: path({{.39f,.20f},{.68f,.50f},{.39f,.80f}});break;
    case UiIcon::Search: ring(.42f,.42f,.25f);path({{.60f,.60f},{.84f,.84f}});break;
    case UiIcon::Model: path({{.50f,.10f},{.86f,.30f},{.86f,.72f},{.50f,.92f},{.14f,.72f},{.14f,.30f},{.50f,.10f}});path({{.14f,.30f},{.50f,.51f},{.86f,.30f}});path({{.50f,.51f},{.50f,.92f}});break;
    case UiIcon::Texture: for(int y=0;y<2;++y)for(int x=0;x<2;++x)squares.push_back({{.30f+x*.40f,.30f+y*.40f},.145f,.04f});break;
    case UiIcon::Play: path({{.29f,.16f},{.82f,.50f},{.29f,.84f},{.29f,.16f}});break;
    case UiIcon::Wheel: ring(.50f,.50f,.37f);ring(.50f,.50f,.12f);for(int i=0;i<8;++i){float a=i*pi/4;line(polar(.5f,.5f,.22f,a),polar(.5f,.5f,.37f,a));}break;
    case UiIcon::Settings: {ring(.50f,.50f,.14f);Point first{},previous{};for(int i=0;i<32;++i){float a=-pi/2+(i-.5f)*pi/16;float r=i%4==0||i%4==3?.32f:.42f;auto next=polar(.5f,.5f,r,a);if(i==0)first=next;else line(previous,next);previous=next;}line(previous,first);break;}
    case UiIcon::Info: ring(.50f,.50f,.37f);dot(.50f,.31f,stroke*1.1f);path({{.50f,.46f},{.50f,.70f}});break;
    case UiIcon::Star: {auto previous=polar(.5f,.52f,.40f,-pi/2);for(int i=1;i<=10;++i){auto next=polar(.5f,.52f,i%2?.19f:.40f,-pi/2+i*pi/5);line(previous,next);previous=next;}break;}
    case UiIcon::Reload: arc(.50f,.50f,.33f,.15f*pi,1.80f*pi);path({{.77f,.14f},{.77f,.35f},{.56f,.35f}});break;
    case UiIcon::Stop: squares.push_back({{.50f,.50f},.27f,.055f});break;
    case UiIcon::Minus: path({{.22f,.50f},{.78f,.50f}});break;
    case UiIcon::Plus: path({{.22f,.50f},{.78f,.50f}});path({{.50f,.22f},{.50f,.78f}});break;
    default: break;
    }
    auto distance=[&](float x,float y){
        float d=std::numeric_limits<float>::infinity();
        for(auto const& s:segments){float t=std::clamp(((x-s.a.x)*s.dx+(y-s.a.y)*s.dy)*s.invLength,0.f,1.f);float dx=x-s.a.x-t*s.dx,dy=y-s.a.y-t*s.dy;d=std::min(d,std::sqrt(dx*dx+dy*dy)-s.r);}
        for(auto const& c:circles){float dx=x-c.center.x,dy=y-c.center.y;float r=std::sqrt(dx*dx+dy*dy)-c.radius;d=std::min(d,c.stroke<0?r:std::abs(r)-c.stroke);}
        for(auto const& s:squares){float dx=std::abs(x-s.center.x)-s.half+s.radius,dy=std::abs(y-s.center.y)-s.half+s.radius;float ox=std::max(dx,0.f),oy=std::max(dy,0.f);d=std::min(d,std::sqrt(ox*ox+oy*oy)+std::min(std::max(dx,dy),0.f)-s.radius);}
        return d;
    };
    std::vector<unsigned char> alpha(static_cast<std::size_t>(pixels)*pixels);
    float inv=1.f/pixels;
    for(int y=0;y<pixels;++y)for(int x=0;x<pixels;++x){
        auto d=distance((x+.5f)*inv,(y+.5f)*inv);
        auto& a=alpha[static_cast<std::size_t>(y)*pixels+x];
        if(d<=-.708f*inv)a=255;
        else if(d<.708f*inv){int count=0;for(int sy=0;sy<4;++sy)for(int sx=0;sx<4;++sx)count+=distance((x+(sx+.5f)/4)*inv,(y+(sy+.5f)/4)*inv)<=0; a=static_cast<unsigned char>((count*255+8)/16);}
    }
    return alpha;
}
}
