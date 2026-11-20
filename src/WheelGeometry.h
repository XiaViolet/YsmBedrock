#pragma once
#include "YsmUi.h"
#include <array>
#include <cmath>

namespace ysm {
struct WheelPoint { float x, y; };
constexpr int wheelSteps=12;
struct WheelSector {
    std::array<WheelPoint,wheelSteps+1> inner, outer;
};
inline WheelSector wheelSector(UiRect bounds,int slot) {
    static const auto normalized=[] {
        std::array<WheelSector,8> result{};
        constexpr float pi=3.14159265f;
        for (int i=0;i<8;++i) {
            float angle=-pi/2+i*pi/4;
            for(int n=0;n<=wheelSteps;++n) {
                float t=2.f*n/wheelSteps-1.f;
                auto point=[&](float inset,float radius) -> WheelPoint {
                    float a=angle+t*(pi/8-inset);
                    return {.5f+radius*std::cos(a),.5f+radius*std::sin(a)};
                };
                result[i].inner[n]=point(.042f,.135f);
                result[i].outer[n]=point(.012f,.5f);
            }
        }
        return result;
    }();
    auto shape=normalized[slot];
    auto transform=[&](WheelPoint& p) {p={bounds.x+p.x*bounds.w,bounds.y+p.y*bounds.h};};
    for(auto& p:shape.inner)transform(p);
    for(auto& p:shape.outer)transform(p);
    return shape;
}
inline int wheelSlotAt(UiRect rect,float x,float y) {
    if(!rect.contains(x,y))return -1;
    constexpr float pi=3.14159265f;
    auto a=std::atan2((y-rect.y)/rect.h-.5f,(x-rect.x)/rect.w-.5f);
    int slot=static_cast<int>(std::floor((a+pi/2+pi/8+2*pi)/(pi/4)))%8;
    auto sector=wheelSector(rect,slot);
    auto inside=[&](WheelPoint a,WheelPoint b) {return (b.x-a.x)*(y-a.y)-(b.y-a.y)*(x-a.x)<=0;};
    for(int n=0;n<wheelSteps;++n) {
        auto a=sector.inner[n],b=sector.inner[n+1],c=sector.outer[n+1],d=sector.outer[n];
        if(inside(a,b)&&inside(b,c)&&inside(c,d)&&inside(d,a))return slot;
    }
    return -1;
}
}
