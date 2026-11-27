#include "UiIcons.h"
#include <cassert>
#include <numeric>
#include <iostream>

int main() {
    using namespace ysm;
    unsigned tested=0;
    for(std::size_t i=1;i<uiIconNames.size();++i) {
        auto icon=static_cast<UiIcon>(i);
        assert(uiIcon(uiIconNames[i])==icon);
        for(int size:{10,14,18,24,36,64,96}) {
            auto mask=rasterizeUiIcon(icon,size);
            assert(mask.size()==static_cast<std::size_t>(size)*size);
            assert(std::count(mask.begin(),mask.end(),0)>0);
            assert(std::count_if(mask.begin(),mask.end(),[](auto a){return a>0&&a<255;})>0);
            assert(*std::max_element(mask.begin(),mask.end())>=175);
            assert(mask==rasterizeUiIcon(icon,size));
            for(int j=0;j<size;++j) {
                if(size>=36){assert(mask[j]==0);assert(mask[(size-1)*size+j]==0);assert(mask[j*size]==0);assert(mask[j*size+size-1]==0);}
            }
            if(icon==UiIcon::Close||icon==UiIcon::Plus||icon==UiIcon::Minus||icon==UiIcon::Texture) {
                for(int y=0;y<size;++y)for(int x=0;x<size;++x)assert(std::abs(int(mask[y*size+x])-int(mask[y*size+size-1-x]))<=16);
            }
            ++tested;
        }
    }
    assert(uiIcon("unknown")==UiIcon::None);
    assert(uiIconPixels(18,1.5f)==27);
    assert(uiIconPixels(18,2)==36);
    assert(uiIconPixels(18,3)==54);
    assert(uiIconPixels(18,.75f)==14);
    assert(uiIconPixels(0,1)==1);
    assert(uiIconPixels(1000,2)==512);
    assert(uiIconPixels(std::numeric_limits<float>::infinity(),2)==1);
    std::cout<<tested<<" icon/size cases passed\n";
}
