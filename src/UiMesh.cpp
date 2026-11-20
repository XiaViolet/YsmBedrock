#include "WheelGeometry.h"
#include "UiMesh.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace ysm {
std::size_t uiMeshBatches(std::size_t vertices){return (vertices+uiBatchVertices-1)/uiBatchVertices;}
std::vector<UiVertex> buildUiMesh(std::span<UiPaint const> paint) {
    std::vector<UiVertex> vertices;vertices.reserve(std::min<std::size_t>(paint.size()*18,100000));
    auto triangle=[&](UiVertex a,UiVertex b,UiVertex c){vertices.push_back(a);vertices.push_back(b);vertices.push_back(c);};
    for(auto const& item:paint) {
        auto const& r=item.rect;auto color=item.color;
        if(!item.text.empty()||!(color>>24)||r.w<=0||r.h<=0)continue;
        if(!std::isfinite(r.x)||!std::isfinite(r.y)||!std::isfinite(r.w)||!std::isfinite(r.h)||!std::isfinite(item.radius))continue;
        float radius=std::clamp(item.radius,0.f,std::min(r.w,r.h)*.5f);
        if (item.wheelSector >= 0) {
            auto sector=wheelSector(r,item.wheelSector);
            auto vertex=[&](WheelPoint p) {return UiVertex{p.x,p.y,color};};
            for(int n=0;n<wheelSteps;++n) {
                auto a=vertex(sector.inner[n]), b=vertex(sector.inner[n+1]);
                auto c=vertex(sector.outer[n+1]), d=vertex(sector.outer[n]);
                triangle(a,b,c);triangle(a,c,d);
            }
        } else if(radius<.5f) {
            UiVertex a{r.x,r.y,color},b{r.x,r.y+r.h,color},c{r.x+r.w,r.y+r.h,color},d{r.x+r.w,r.y,color};
            // Same clockwise screen winding as ScreenRenderer::fill.
            triangle(a,b,c);triangle(a,c,d);
        } else {
            constexpr float pi=3.141592653589793f;
            int segments=std::clamp(static_cast<int>(std::ceil(radius*.7f)),2,12);
            UiVertex center{r.x+r.w*.5f,r.y+r.h*.5f,color};
            std::array<std::array<float,2>,4> corners{{{r.x+radius,r.y+radius},{r.x+radius,r.y+r.h-radius},{r.x+r.w-radius,r.y+r.h-radius},{r.x+r.w-radius,r.y+radius}}};
            std::vector<UiVertex> perimeter;perimeter.reserve(4*(segments+1));
            for(int corner=0;corner<4;++corner)for(int s=0;s<=segments;++s){float angle=-pi/2-corner*pi/2-static_cast<float>(s)/segments*pi/2;
                perimeter.push_back({corners[corner][0]+std::cos(angle)*radius,corners[corner][1]+std::sin(angle)*radius,color});}
            for(std::size_t i=0;i<perimeter.size();++i)triangle(center,perimeter[i],perimeter[(i+1)%perimeter.size()]);
        }
        if(vertices.size()>uiBatchVertices*8)throw std::runtime_error("UI mesh exceeds bounded frame budget");
    }
    return vertices;
}
}
