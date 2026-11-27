#include "ModelLocalization.h"
#include "WheelGeometry.h"
#include "YsmUi.h"
#include "UiTheme.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace ysm {
namespace {
constexpr std::uint32_t ink=0xff243e37,muted=0xff82938a,teal=0xff247968,edge=0xffe0e7df;
constexpr std::uint32_t paper=0xfffafbf7,white=0xffffffff,wash=0xffe6f2e9,soft=0xfff0f4ed, onAccent=0xfffefefe;
std::string str(Json const& j,char const* key,std::string fallback={}){auto p=j.find(key);return p!=j.end()&&p->is_string()?p->get<std::string>():fallback;}
Json const& props(Json const& descriptor) {
    static const Json empty=Json::object();
    auto it=descriptor.find("properties");
    return it!=descriptor.end() && it->is_object() ? *it : empty;
}
Json const& buttons(Json const& descriptor) {
    static const Json empty=Json::array();
    auto const& info=props(descriptor);
    auto it=info.find("extra_animation_buttons");
    return it!=info.end() && it->is_array() ? *it : empty;
}
std::string lower(std::string s){for(auto& c:s)if(c>='A'&&c<='Z')c=static_cast<char>(c+32);return s;}
std::string number(double value){std::ostringstream s;s<<std::setprecision(4)<<value;return s.str();}
std::size_t stride(unsigned char c){return c<128?1:(c&0xe0)==0xc0?2:(c&0xf0)==0xe0?3:4;}
std::string plain(std::string const& value){
    std::string out;for(std::size_t i=0;i<value.size();){
        if(value.compare(i,2,"§")==0){i+=2;if(i<value.size())i+=stride(static_cast<unsigned char>(value[i]));continue;}
        auto n=std::min(stride(static_cast<unsigned char>(value[i])),value.size()-i);out.append(value,i,n);i+=n;
    }return out;
}
float glyph(unsigned char c){return c>=128?10.f:c==' '||c=='.'||c==','||c==':'||c=='i'||c=='l'?3.f:6.f;}
std::string fit(std::string const& input,float width,float scale=1){
    auto value=plain(input);float used=0;std::size_t i=0;
    while(i<value.size()){auto c=static_cast<unsigned char>(value[i]);if(value[i]=='\n'||used+(glyph(c)+8)*scale>width)break;used+=glyph(c)*scale;i+=std::min(stride(c),value.size()-i);}
    return i<value.size()?value.substr(0,i)+"…":value;
}
std::vector<std::string> wrap(std::string const& input,float width,float scale=1){
    auto value=plain(input);std::vector<std::string> out;std::string row;float used=0;
    for(std::size_t i=0;i<value.size();){auto c=static_cast<unsigned char>(value[i]);auto n=std::min(stride(c),value.size()-i);
        if(value[i]=='\n'){out.push_back(std::move(row));row.clear();used=0;++i;continue;}
        if(used+glyph(c)*scale>width&&!row.empty()){out.push_back(std::move(row));row.clear();used=0;}
        row.append(value,i,n);used+=glyph(c)*scale;i+=n;
    }if(!row.empty())out.push_back(std::move(row));return out;
}
struct Painter {
    UiScene& scene;
    void box(UiRect r,std::uint32_t color,float radius=0){if(r.w>0&&r.h>0)scene.paint.push_back({r,color,{},1,false,radius});}
    void border(UiRect r,std::uint32_t stroke,std::uint32_t fill,float radius=7,float weight=1){box(r,stroke,radius);box({r.x+weight,r.y+weight,r.w-weight*2,r.h-weight*2},fill,std::max(0.f,radius-weight));}
    void text(float x,float y,float w,std::string value,std::uint32_t color=ink,float scale=1,bool center=false){
        float estimated=0; for(std::size_t i=0;i<value.size();) { auto c=static_cast<unsigned char>(value[i]); estimated+=glyph(c);i+=std::min(stride(c),value.size()-i); }
        if(estimated*scale>w && w>0) scale=std::max(scale*.88f,w/estimated);
        if(estimated*scale>w && w>0) value=fit(value,w,scale);
        scene.paint.push_back({{x,y,w,16*scale},color,std::move(value),scale,center});}
    void line(float x,float y,float xx,float yy,std::uint32_t color,float weight=1.4f){
        scene.paint.push_back({{x,y,weight,weight},color,{},1,false,0,-1,true,xx,yy});
    }
    void circle(float x,float y,float r,std::uint32_t color){box({x-r,y-r,r*2,r*2},color,r);}
    void icon(std::string const& id,float x,float y,float size,std::uint32_t color){
        auto kind=uiIcon(id);
        if(kind==UiIcon::None||size<=0)return;
        UiPaint paint{{x,y,size,size},color,{}};
        paint.icon=kind;
        scene.paint.push_back(std::move(paint));
    }
};
}

UiScene const& YsmUi::draw(float width,float height,float hotbarTop) {
    if (sceneDirty || width != cachedWidth || height != cachedHeight || hotbarTop != cachedHotbar) {
        cachedScene = buildScene(width, height, hotbarTop);
        cachedScene.revision = ++sceneRevision;
        cursorX = cursorY = -2;
        cachedWidth = width; cachedHeight = height; cachedHotbar = hotbarTop;
        sceneDirty = false;
    }
    if (mouseX!=cursorX || mouseY!=cursorY) {
        cursorX=mouseX; cursorY=mouseY; cachedScene.cursor.clear();
        if(isOpen() && mouseX>=0 && mouseY>=0) {
            auto scale=std::min(width/952.f,height/568.f);
            auto line=[&](float x,float y,float ex,float ey,std::uint32_t color,float weight){cachedScene.cursor.push_back({{mouseX+x*scale,mouseY+y*scale,weight*scale,weight*scale},color,{},1,false,0,-1,true,mouseX+ex*scale,mouseY+ey*scale});};
            line(1,2,1,17,0x90000000,3);line(1,2,12,12,0x90000000,3);
            line(0,0,0,15,0xffffffff,2.4f);line(0,0,10,10,0xffffffff,2.4f);
            line(0,0,0,12,0xff142033,1);line(0,0,8,8,0xff142033,1);
        }
    }
    return cachedScene;
}
UiScene YsmUi::buildScene(float width,float height, float hotbarTop) {
    previewArea.reset();
    UiScene scene;hits.clear();ranges.clear();if(!isOpen()||width<=0||height<=0)return scene;
    constexpr float W=920,H=540;
     
     
    bool validBar = std::isfinite(hotbarTop) && hotbarTop >= height*.5f && hotbarTop < height;
    float contentHeight = std::max(1.f, std::min(validBar ? hotbarTop : height, height - 24.f));
    auto scale=std::min(width/(W+32),contentHeight/(H+28));if(!std::isfinite(scale)||scale<=0)return scene;
    auto ox=(width-W*scale)/2,oy=(contentHeight-H*scale)/2;
    auto mx=(mouseX-ox)/scale,my=(mouseY-oy)/scale;
    Painter p{scene};
    auto tr = [&](char const* zh, char const* en) { return uiText(data.locale, zh, en); };
    auto hit=[&](UiRect r,std::string id){hits.push_back({r,std::move(id)});};
    auto focus=[&](UiRect r,std::string const& id,float radius=6){if(focusId==id)p.border({r.x-2,r.y-2,r.w+4,r.h+4},teal,0x00247968,radius+2,1);};
    auto button=[&](UiRect r,std::string label,std::string id,bool active=false,bool primary=false,std::string icon={}) {
        bool hover=r.contains(mx,my);auto fill=primary?teal:active?wash:hover?0xffeaf0e7:white;
        focus(r,id);p.border(r,active?0xffa9cdbc:edge,fill,6,primary?0.f:1.f);
        auto color=primary?onAccent:active?teal:ink;
        if(!icon.empty())p.icon(icon,r.x+9,r.y+(r.h-18)/2,18,color);
        p.text(r.x+(icon.empty()?10:32),r.y+(r.h-13)/2,r.w-(icon.empty()?20:42),label,color,.95f);
        hit(r,std::move(id));
    };
    auto iconButton=[&](UiRect r,std::string icon,std::string id,bool active=false) {
        bool hover=r.contains(mx,my);focus(r,id,r.h/2);p.border(r,active?0xffbdd8c9:hover?0xffc8d7c9:edge,active?wash:white,r.h/2);
        auto size=std::clamp(std::min(r.w,r.h)-8.f,12.f,20.f);
        p.icon(icon,r.x+(r.w-size)/2,r.y+(r.h-size)/2,size,active?teal:ink);hit(r,std::move(id));
    };
    auto toggle=[&](UiRect r,std::string id,bool on,std::optional<UiRect> row={}) {
        focus(r,id,r.h/2);p.box(r,on?teal:0xffd8e0d5,r.h/2);p.circle(r.x+(on?r.w-r.h/2:r.h/2),r.y+r.h/2,r.h/2-3,on?onAccent:white);hit(row.value_or(r),std::move(id));
    };
    auto slider=[&](UiRect r,std::string route,double minimum,double maximum,double step,bool setting=false) {
        auto v=setting?data.settings.value(route,0.0):data.values.value(route,0.0);auto value=maximum>minimum?std::clamp((v-minimum)/(maximum-minimum),0.0,1.0):0;
        auto id="range:"+route;focus(r,id);p.box({r.x,r.y+r.h/2-2,r.w,4},edge,2);p.box({r.x,r.y+r.h/2-2,static_cast<float>(r.w*value),4},teal,2);
        p.circle(r.x+static_cast<float>(r.w*value),r.y+r.h/2,7,0x20243e37);p.circle(r.x+static_cast<float>(r.w*value),r.y+r.h/2-1,5,white);p.circle(r.x+static_cast<float>(r.w*value),r.y+r.h/2-1,3,teal);
        hit(r,id);ranges.push_back({r,id,std::move(route),minimum,maximum,step,setting});
    };
    auto chip=[&](float x,float y,std::string label,std::uint32_t fill=wash,std::uint32_t color=teal,float cw=64){p.box({x,y,cw,21},fill,10);p.text(x,y+5,cw,label,color,.8f,true);};
    auto hint=[&](float x,float y,std::string key,std::string label,float kw=23){p.border({x,y,kw,20},edge,white,4);p.text(x,y+6,kw,key,muted,.72f,true);p.text(x+kw+7,y+6,90,label,muted,.78f);};
    bool libraryPage = page == Page::Models || page == Page::Preview;
    auto const* candidate = libraryPage ? data.browsing.get() : nullptr;
    auto const& clips = candidate ? candidate->clips : data.clips;
    auto const& language = candidate ? modelLanguage(candidate->languages, data.locale) : data.language;
    auto preview=[&](UiRect r) {
        if (libraryPage && !data.browseLoading.empty()) return;
        scene.preview=r;scene.previewCamera=previewCamera;scene.previewClip=libraryPage ? previewClip : std::string{};
        scene.libraryPreview = candidate != nullptr;
        if (page == Page::Models && (candidate ? candidate->fixedRotation : props(data.descriptor).value("disable_preview_rotation", false))) {
            scene.previewCamera.yaw = 0;
            scene.previewCamera.pitch = 0;
        }
        if (libraryPage && scene.previewClip.empty()) {
            auto authored = candidate ? candidate->animation : str(props(data.descriptor),"preview_animation");
            if (!authored.empty() && std::find(clips.begin(),clips.end(),authored)!=clips.end()) scene.previewClip=std::move(authored);
        }
        if (libraryPage) scene.animatePreview=true;
        p.box({r.x+10,r.y+r.h-16,r.w-20,1},0xffd4e2d4);
    };
    p.box({-ox/scale,-oy/scale,width/scale,height/scale},0x850d241c);
    auto metadata=data.descriptor.value("metadata",Json::object());
    auto modelName=candidate ? localizedModelText(language,data.locale,"metadata.name",candidate->name) : local("metadata.name",str(metadata,"name",data.source.empty()?tr("尚未选择模型", "No model selected"):data.source));
    std::string title=page==Page::Models?tr("模型库", "Model Library"):page==Page::Textures?tr("模型贴图", "Model Textures"):page==Page::Preview?tr("动作预览", "Emote Preview"):page==Page::Info?tr("模型信息", "Model Info"):page==Page::Appearance?tr("外观与性能", "Appearance & Performance"):page==Page::Settings?tr("本地设置", "Local Settings"):page==Page::Forms?tr("模型设置", "Model Settings"):page==Page::Players?tr("联机玩家", "Players"):page==Page::Import?tr("导入模型", "Import Model"):tr("动作转盘", "Emote Wheel");
    if (isWheel()) {
        p.text(210,11,500,tr("动作转盘", "Emote Wheel"),0xfffdfdfd,1.9f,true);
        auto control=[&](UiRect r,std::string label,std::string id,bool active=false) {
            auto hovered=r.contains(mx,my);
            focus(r,id);
            p.border(r,active ? 0xff90cbb8 : edge,active ? wash : hovered ? soft : white,7);
            if(label=="−"||label=="+"||label=="‹"||label=="›") {
                auto size=std::min(18.f,r.h-4.f);
                p.icon(label=="−"?"minus":label=="+"?"plus":label=="‹"?"prev":"next",r.x+(r.w-size)/2,r.y+(r.h-size)/2,size,active?teal:ink);
            } else p.text(r.x+3,r.y+(r.h-14)/2,r.w-6,label,active ? teal : ink,1,true);
            hit(r,std::move(id));
        };
         
        if (data.enabled) {
             
            float left=std::max(-48.f,8.f-ox/scale);
            scene.preview=UiRect{left,66,258.f-left,400};
            scene.previewCamera=previewCamera;
            scene.animatePreview=true;
            scene.livePlayerPreview=true;
        }
        constexpr float cx=425,cy=268,radius=160,pi=3.14159265f;
        UiRect ring{cx-radius,cy-radius,2*radius,2*radius};
        wheelBounds={ox+ring.x*scale,oy+ring.y*scale,ring.w*scale,ring.h*scale};
        wheelHovered=wheelSlotAt(wheelBounds,mouseX,mouseY);
        auto entries=wheelEntries();
        auto pages=std::max<std::size_t>(1,(entries.size()+7)/8);
        auto& wheelPage=categories.back().second;
        wheelPage%=pages;
        p.circle(cx,cy+4,radius+3,0x40000000);
        p.circle(cx,cy,radius+2,0xffcbded2);
        for (int i=0;i<8;++i) {
            auto index=wheelPage*8+i;
            auto const* e=index<entries.size()?&entries[index]:nullptr;
            bool editing=e && page==Page::Forms && e->form==formButton;
            bool highlighted=editing || (e && i==wheelHovered);
            auto color=highlighted ? 0xffc5e5d5 : e ? paper : soft;
            scene.paint.push_back({ring,color,{},1,false,0,i});
            if (!e) continue;
            float a=-pi/2+i*pi/4, dx=std::cos(a), dy=std::sin(a);
            float x=cx+dx*(radius*.65f), y=cy+dy*(radius*.65f);
            auto lines=wrap(e->label,91,1.03f);
            float top=y-(lines.size()>1?12.f:5.f);
            for(std::size_t n=0;n<std::min<std::size_t>(2,lines.size());++n)
                p.text(x-46,top+n*13,92,n==1&&lines.size()>2?fit(lines[n]+"…",91,1.03f):lines[n],highlighted ? teal : ink,1.03f,true);
            p.text(cx+dx*(radius*.86f)-16,cy+dy*(radius*.86f)-4,32,std::to_string(i+1),highlighted ? teal : muted,.78f,true);
            if (!e->form.empty() && (e->key.starts_with('#') || std::find(data.clips.begin(),data.clips.end(),e->key)!=data.clips.end()))
                control({cx+dx*(radius*.42f)-17,cy+dy*(radius*.42f)-9,34,18},tr("设置", "Edit"),"edit_wheel:"+std::to_string(i),editing);
        }
        p.circle(cx,cy,42,paper);
        p.icon("wheel",cx-12,cy-27,24,teal);
        control({cx-37,cy+3,74,22},data.settings.value("movement_lock",false)?tr("动作锁：开", "Lock: On"):tr("动作锁：关", "Lock: Off"),"setting:movement_lock");
        constexpr float sx=602, sw=313;
        std::string path=tr("根目录", "Root");
        for (std::size_t n=1;n<categories.size();++n) {
            auto const& id=categories[n].first;
            auto parent=props(data.descriptor).value("extra_animation",Json::object());
            if(n>1) for(auto const& c:props(data.descriptor).value("extra_animation_classify",Json::array()))
                if(str(c,"id")==categories[n-1].first){parent=c.value("extra_animation",Json::object());break;}
            path+=" / "+localizedEmote(data.descriptor,data.language,data.locale,"#"+id,str(parent,("#"+id).c_str(),id));
        }
        float cardBottom=page==Page::Forms ? 478.f : 280.f;
        p.box({sx-5,46,sw+14,cardBottom-38},0x40000000,12);
        p.border({sx-8,40,sw+16,cardBottom-36},edge,paper,12);
        p.box({sx+1,45,sw-2,58},white,8);
        p.text(sx+45,55,sw-90,fit(path,sw-90),ink,1,true);
        p.text(sx+45,79,sw-90,std::to_string(wheelPage+1)+" / "+std::to_string(pages),teal,1.15f,true);
        iconButton({sx+9,57,32,32},"prev","wheel_prev");
        iconButton({sx+sw-41,57,32,32},"next","wheel_next");
        control({sx,108,sw,31},tr("返回", "Back"),"back");
        if (page==Page::Forms) {
            Json forms=Json::array();
            for (auto const& b:buttons(data.descriptor)) if (str(b,"id")==formButton) {
                forms=b.value("config_forms",Json::array());break;
            }
            p.box({sx,145,sw,240},white,8);
            if (!forms.empty()) {
                formPage%=forms.size();
                auto route=formButton+"/"+std::to_string(formPage);
                auto const& form=forms[formPage];
                auto base="properties.extra_animation_buttons."+formButton+".config_forms."+std::to_string(formPage);
                auto type=str(form,"type");
                auto value=data.values.value(route,0.);
                p.text(sx+10,157,sw-20,fit(local(base+".title",str(form,"title")),sw-20,1.18f),ink,1.18f);
                auto desc=wrap(local(base+".description",str(form,"description")),sw-20,.88f);
                 
                auto descTop=type=="range" ? 310.f : 179.f;
                auto maxLines=type=="range" ? 5u : 2u;
                for(std::size_t n=0;n<std::min<std::size_t>(maxLines,desc.size());++n)
                    p.text(sx+10,descTop+n*13,sw-20,desc[n],muted,.88f);
                if (type=="checkbox") {
                    for(std::size_t i=formPage;i<std::min(forms.size(),formPage+4);++i) {
                        if(str(forms[i],"type")!="checkbox")break;
                        auto key=formButton+"/"+std::to_string(i);
                        bool on=data.values.value(key,0.)!=0;
                        float y=212+static_cast<float>(i-formPage)*32;
                        auto label=local("properties.extra_animation_buttons."+formButton+".config_forms."+std::to_string(i)+".title",str(forms[i],"title"));
                        p.box({sx+7,y,sw-14,29},soft,6);
                        p.text(sx+15,y+9,sw-92,fit(label,sw-92),ink,1);
                        control({sx+sw-71,y,64,29},on?tr("已开启", "On"):tr("已关闭", "Off"),"value:"+key+":toggle",on);
                        hits.back().rect={sx+7,y,sw-14,29};
                    }
                } else if (type=="range") {
                    auto lo=form.value("min",0.),hi=form.value("max",100.),step=form.value("step",1.);
                    p.text(sx+42,200,sw-84,number(value),teal,1.35f,true);
                    control({sx+10,190,32,32},"−","value:"+route+":down");
                    control({sx+sw-42,190,32,32},"+","value:"+route+":up");
                    slider({sx+20,230,sw-40,32},route,lo,hi,step);
                    p.text(sx+14,267,65,number(lo),muted,.85f);
                    p.text(sx+sw-65,267,52,number(hi),muted,.85f,true);
                } else if (type=="radio") {
                    auto labels=form.value("labels",Json::object());
                    constexpr std::size_t perPage=8;
                    auto count=std::max<std::size_t>(1,(labels.size()+perPage-1)/perPage);
                    radioPage%=count;std::size_t i=0;
                    for(auto const& [label,program]:labels.items()){
                        auto index=i++;if(index/perPage!=radioPage)continue;
                        auto slot=index%perPage;
                        control({sx+7+151*float(slot%2),210+31*float(slot/2),147,27},
                                local(base+".labels."+std::to_string(index),label),"value:"+route+":label"+std::to_string(index),
                                !str(form,"value").empty() && std::round(value)==static_cast<double>(index));
                    }
                    if(count>1){
                        control({sx+9,338,39,23},"‹","radio_prev");
                        p.text(sx+65,345,sw-130,std::to_string(radioPage+1)+" / "+std::to_string(count),muted,.9f,true);
                        control({sx+sw-48,338,39,23},"›","radio_next");
                    }
                }
                if(forms.size()>1){
                    control({sx,390,90,30},tr("上一项", "Previous"),"prev");
                    p.text(sx+96,400,sw-192,std::to_string(formPage+1)+" / "+std::to_string(forms.size()),muted,.95f,true);
                    control({sx+sw-90,390,90,30},tr("下一项", "Next"),"next");
                }
            } else p.text(sx+12,163,sw-24,tr("作者未提供设置项", "No settings provided by the author"),muted,1);
        } else {
            std::string heading=tr("选择装饰或动作", "Choose an emote or accessory"), detail=tr("点击扇区，或按对应数字键 1–8。", "Click a sector or press 1–8.");
            if(wheelHovered>=0 && wheelPage*8+wheelHovered<entries.size()) {
                auto const& e=entries[wheelPage*8+wheelHovered];
                heading=e.label;detail=e.description;
                if (detail.empty() && !e.form.empty() && std::find(data.clips.begin(),data.clips.end(),e.key)!=data.clips.end())
                    detail=tr("点击播放；点设置或右键调整后播放。", "Click to play. Use Edit or right-click to configure and play.");
                if(detail.empty()) detail=e.key=="#return" ? tr("返回上一级", "Return to parent group") : e.key.starts_with('#') ? tr("打开分组", "Open group")
                    : !e.form.empty() ? tr("在右侧调整装饰", "Adjust accessories on the right") : tr("播放动作并关闭转盘", "Play emote and close wheel");
            }
            p.text(sx+12,160,sw-24,fit(heading,sw-24,1.15f),teal,1.15f);
            auto lines=wrap(detail,sw-24,.9f);
            for(std::size_t i=0;i<std::min<std::size_t>(3,lines.size());++i)p.text(sx+12,186+i*14,sw-24,lines[i],muted,.9f);
        }
        float actionY=cardBottom-38;
        button({sx,actionY,151,32},tr("停止当前动作", "Stop Emote"),"stop",false,false,"stop");
        if (page == Page::Forms && !formAction.empty())
            button({sx+157,actionY,156,32},tr("执行此动作", "Play this emote"),"play_form",true,true,"play");
        else
            button({sx+157,actionY,156,32},tr("动作预览", "Emote Preview"),"preview",false,true,"play");
        iconButton({W-28,3,28,28},"close","close");
    } else {
        p.box({2,7,W,H},0x58000000,16);p.box({0,0,W,H},paper,14);
        p.box({0,0,143,H},0xfff0f3eb,14);p.box({132,0,11,H},0xfff0f3eb);p.box({143,0,1,H},edge);
        p.box({20,25,29,29},teal,8);p.icon("model",25,30,19,onAccent);p.text(59,29,70,"YSM",ink,1.75f);p.text(22,67,100,tr("模型工坊", "Model Workshop"),muted,.9f);
        float navY=112;
        auto nav=[&](std::string label,std::string id,std::string icon,Page target) {
            UiRect r{12,navY,119,37};bool active=page==target||(target==Page::Settings&&page==Page::Appearance),hover=r.contains(mx,my);focus(r,id);if(active||hover)p.box(r,active?wash:0xffe7eddf,7);
            if(active)p.box({12,navY+10,3,17},teal,1.5f);p.icon(icon,24,navY+9,19,active?teal:muted);p.text(52,navY+11,76,label,active?teal:ink,1);
            hit(r,std::move(id));navY+=45;
        };
        nav(tr("模型库", "Models"),"models","model",Page::Models);nav(tr("贴图", "Textures"),"textures","texture",Page::Textures);nav(tr("动作预览", "Preview"),"preview","play",Page::Preview);nav(tr("动作转盘", "Emotes"),"roulette","wheel",Page::Roulette);nav(tr("模型信息", "Model Info"),"info","info",Page::Info);nav(tr("本地设置", "Settings"),"settings","settings",Page::Settings);
        nav(tr("联机玩家", "Players"),"players","model",Page::Players);
        p.box({20,424,103,1},edge);p.circle(27,447,3,data.enabled?teal:muted);p.text(38,443,85,data.enabled?tr("模型已启用", "Model enabled"):tr("原版外观", "Vanilla appearance"),muted,.8f);
        button({19,474,105,30},data.enabled?tr("恢复原版", "Use Vanilla"):tr("启用模型", "Enable Model"),"toggle",false,false);
        p.text(169,29,610,title,ink,1.85f);p.text(170,60,page==Page::Settings?530.f:600.f,page==Page::Import?tr("选择本机的模型包或源文件夹。", "Choose a local model pack or source folder."):page==Page::Models?tr("点选模型即可预览。", "Select a model to preview it."):page==Page::Textures?tr("换一张贴图，换一种心情。", "Choose a new texture for your model."):page==Page::Preview?tr("查看模型自带的动作与外观设置。", "Preview the model's animations and appearance settings."):page==Page::Info?tr("每一个模型，都有创作者的故事。", "Learn about the model and its creators."):page==Page::Appearance?tr("自定义主题和预览性能。", "Customize the theme and preview performance."):page==Page::Settings?tr("调整自己的观看与操作体验。", "Customize your controls and visuals."):tr("由模型作者提供的外观与动作选项。", "Appearance and animation options provided by the author."),muted,.9f);
        iconButton({866,24,30,30},"close","close");
        p.box({169,490,727,1},edge);hint(171,507,"Tab",tr("切换焦点", "Change focus"),29);hint(320,507,"Enter",tr("选择", "Select"),39);hint(451,507,"Esc",tr("返回", "Back"),28);
        if(data.loading || !data.browseLoading.empty())chip(726,507,tr("正在加载…", "Loading…"),wash,teal,167);
        else if(!data.message.empty())p.text(618,513,270,fit(displayLabel(data.locale,data.message),270,.8f),muted,.8f);
        else p.text(698,513,190,tr("本地模型 · 随时切换", "Local models · switch anytime"),muted,.8f);
        constexpr float top=96,cx=171,cw=725;
        auto pager=[&](std::size_t& current,std::size_t total,float px,float yy,float ww) {
            total=std::max<std::size_t>(1,total);current%=total;
            iconButton({px,yy,29,27},"prev","prev");iconButton({px+ww-29,yy,29,27},"next","next");p.text(px+38,yy+9,ww-76,std::to_string(current+1)+" / "+std::to_string(total),muted,.82f,true);
        };
        auto modelStage=[&](UiRect r) {
            p.box(r,0xffeaf1e5,10);p.box({r.x+14,r.y+14,75,20},0xcfffffff,10);p.text(r.x+14,r.y+20,75,page==Page::Models?tr("模型预览", "Preview"):tr("当前模型", "Current model"),teal,.75f,true);
            preview({r.x+12,r.y+47,r.w-24,r.h-126});
            if (libraryPage && !data.browseLoading.empty()) {
                p.text(r.x+18,r.y+r.h/2,r.w-36,tr("正在读取模型…", "Loading model…"),muted,.9f,true);
                return;
            }
            p.text(r.x+18,r.y+r.h-66,r.w-36,fit(modelName,r.w-36,1.12f),ink,1.12f);
            if (page==Page::Models && candidate) {
                auto src=data.browsing->source;
                if(src.starts_with("server:")){
                    auto width=(r.w-40)*.5f;
                    button({r.x+16,r.y+r.h-43,width,31},tr("穿戴", "Equip"),"wear",false,true);
                    button({r.x+24+width,r.y+r.h-43,width,31},tr("收藏到本地", "Save Locally"),"room:collect:"+src.substr(7));
                }else button({r.x+16,r.y+r.h-43,r.w-32,31},tr("穿戴此模型", "Equip Model"),"wear",false,true);
            }
            else p.text(r.x+18,r.y+r.h-40,r.w-36,fit(local("metadata.tips",str(metadata,"tips",tr("陪你开始一段新的冒险", "Start a new adventure together"))),r.w-36,.8f),muted,.8f);
        };
        if(page==Page::Models) {
            constexpr float listW=453;
            UiRect searchRect{cx,top,277,32};focus(searchRect,"search");p.border(searchRect,searchFocused?0xff93bfaa:edge,white,7);p.icon("search",cx+9,top+7,18,muted);
            p.text(cx+33,top+12,232,fit(search.empty()?tr("搜索名称（可粘贴中文）", "Search models"):search,232,.9f),search.empty()?muted:ink,.9f);hit(searchRect,"search");
            button({cx+288,top,105,32},favoritesOnly?tr("已收藏", "Favorites"):tr("全部模型", "All Models"),"favorite_filter",favoritesOnly,false,"star");iconButton({cx+409,top,34,32},"reload","reload");
            struct Card { Json const* row; std::string name, author; };
            auto favorites=data.settings.value("favorites",Json::array());std::vector<Card> entries;
            auto query=lower(search);
            for(auto const& e:data.catalog){
                auto source=str(e,"source");
                bool starred=std::find(favorites.begin(),favorites.end(),source)!=favorites.end();
                if(favoritesOnly&&!starred)continue;
                static Json const empty=Json::object();
                auto languages=e.find("languages");
                auto const& strings=languages!=e.end()?modelLanguage(*languages,data.locale):source==data.source?data.language:empty;
                auto name=localizedModelText(strings,data.locale,"metadata.name",str(e,"name",source));
                auto author=str(e,"kind")=="server" ? std::string(tr("服务器", "Server")) : localizedModelText(strings,data.locale,"metadata.authors.0.name",str(e,"author",str(e,"kind")=="folder"?tr("源模型文件夹", "Source folder"):tr("YSM 模型包", "YSM model pack")));
                if(!query.empty()&&lower(source+"\n"+name+"\n"+str(e,"name")+"\n"+author+"\n"+str(strings,"metadata.tips")).find(query)==std::string::npos)continue;
                entries.push_back({&e,std::move(name),std::move(author)});
            }
            p.text(cx,top+49,300,tr("本地模型", "Local Models"),ink,.9f);p.text(cx+360,top+49,83,std::to_string(entries.size())+tr(" 个模型", " models"),muted,.8f);
            constexpr std::size_t perPage=4;auto pages=std::max<std::size_t>(1,(entries.size()+perPage-1)/perPage);listPage%=pages;
            for(std::size_t n=0;n<perPage&&listPage*perPage+n<entries.size();++n) {
                auto const& card=entries[listPage*perPage+n];auto const& e=*card.row;
                auto source=str(e,"source");bool selected=source==(data.browseLoading.empty()?(candidate?candidate->source:data.source):data.browseLoading),starred=std::find(favorites.begin(),favorites.end(),source)!=favorites.end();
                auto display=card.name;if(display.ends_with(".zip"))display.resize(display.size()-4);
                UiRect r{cx+static_cast<float>(n%2)*232,top+72+static_cast<float>(n/2)*131,221,119};auto id="select:"+source;
                bool hover=r.contains(mx,my);focus(r,id,9);p.border(r,selected?0xff8fbea6:hover?0xffb9cdb7:edge,selected?0xffedf6ee:white,9,selected?1.5f:1.f);hit(r,id);
                p.box({r.x+13,r.y+13,38,38},selected?0xffd7eadb:soft,9);p.icon("model",r.x+23,r.y+23,18,selected?teal:0xff93aa98);
                if(source==data.source && data.enabled)chip(r.x+60,r.y+21,tr("已穿戴", "Equipped"),wash,teal,58);
                else if(selected)chip(r.x+60,r.y+21,tr("预览中", "Preview"),wash,teal,58);
                iconButton({r.x+r.w-38,r.y+15,25,25},"star","favorite:"+source,starred);
                p.text(r.x+14,r.y+66,r.w-28,fit(display,r.w-28,1.05f),ink,1.05f);
                p.text(r.x+14,r.y+92,r.w-28,fit(str(e,"status")=="invalid"?tr("读取失败 · 可重新选择", "Load failed · select again"):card.author,r.w-28,.76f),str(e,"status")=="invalid"?0xffa27a49:muted,.76f);
            }
            if(entries.empty()){p.icon("model",cx+195,top+157,38,0xffbecbbb);p.text(cx,top+211,listW,tr("没有匹配的本地模型", "No matching local models"),muted,1,true);}
            button({cx,439,104,28},tr("导入模型", "Import Model"),"import");
            pager(listPage,pages,cx+128,439,197);
            modelStage({650,top,246,378});
        } else if(page==Page::Import) {
            button({cx,top+20,221,40},tr("选择模型包", "Choose Model Pack"),"import_file",false,true);
            p.text(cx,top+73,cw,tr("支持 .zip 和 .ysm", "Supports .zip and .ysm"),muted,.9f);
            button({cx,top+124,221,40},tr("选择源文件夹", "Choose Source Folder"),"import_folder");
            p.text(cx,top+177,cw,tr("选择包含完整模型资源的文件夹", "Choose a folder containing all model resources"),muted,.9f);
            button({cx,top+220,221,40},tr("打开模型文件夹", "Open Models Folder"),"open_models_folder");
            button({cx,top+306,90,31},tr("返回", "Back"),"back");
        } else if(page==Page::Textures) {
            modelStage({cx,top,292,378});float lx=485,lw=411;
            p.text(lx,top+4,lw,tr("可用贴图", "Available Textures"),ink,1.1f);p.text(lx,top+27,lw,tr("选择后应用到当前模型", "Select to apply to the current model"),muted,.85f);
            constexpr std::size_t rows=5;auto pages=std::max<std::size_t>(1,(data.textures.size()+rows-1)/rows);texturePage%=pages;
            for(std::size_t n=0;n<rows&&texturePage*rows+n<data.textures.size();++n){auto const& t=data.textures[texturePage*rows+n];UiRect r{lx,top+57+n*51,lw,43};button(r,local("files.player.texture."+t,t),"texture:"+t,t==data.texture,false,"texture");if(t==data.texture)p.icon("check",r.x+r.w-31,r.y+12,18,teal);}
            pager(texturePage,pages,lx+102,439,207);
        } else if(page==Page::Preview) {
            scene.animatePreview=true;
            p.box({cx,top,328,378},0xffeaf1e5,10);chip(cx+15,top+15,tr("动作展示", "Emotes"),white,teal,72);preview({cx+15,top+46,298,264});
            iconButton({cx+16,top+329,33,31},"prev","rotate_left");iconButton({cx+58,top+329,33,31},"next","rotate_right");button({cx+121,top+329,86,31},tr("复位预览", "Reset View"),"reset_preview");iconButton({cx+226,top+329,33,31},"minus","zoom_out");iconButton({cx+268,top+329,33,31},"plus","zoom_in");
            float lx=521,lw=375;
            auto const& forms=buttons(data.descriptor);
            auto formCount=(!candidate && data.browseLoading.empty())?forms.size():0;
            auto clipCount=data.browseLoading.empty()?clips.size():0;
            auto count=1+formCount+clipCount;
            p.text(lx,top+5,lw,candidate ? modelName : tr("模型设置与动作", "Model Settings and Emotes"),ink,1.1f);p.text(lx,top+30,lw,tr("保留模型作者的原始动作", "Preview the author's original animations"),muted,.85f);
            constexpr std::size_t rows=7;auto pages=std::max<std::size_t>(1,(count+rows-1)/rows);previewPage%=pages;
            for(std::size_t n=0;n<rows&&previewPage*rows+n<count;++n){
                auto index=previewPage*rows+n;
                std::string label,id;
                if(index==0){label=tr("自动动作", "Automatic");id="automatic";}
                else if(index<=formCount){auto const& b=forms.at(index-1);auto name=str(b,"id");label=local("properties.extra_animation_buttons."+name+".name",str(b,"name",name));id="form:"+name;}
                else {auto const& c=clips[index-1-formCount];label=localizedEmote(candidate ? candidate->descriptor : data.descriptor,language,data.locale,c);id="clip:"+c;}
                button({lx,top+61+n*38,lw,31},label,id,id=="clip:"+previewClip,false,id.starts_with("form:")?"settings":"play");
            }
            pager(previewPage,pages,lx+89,439,197);
        } else if(page==Page::Players) {
            button({cx,top,135,30},tr("共享文件夹", "Shared Folder"),"room:folder");
            button({cx+145,top,130,30},tr("房间规则", "Room Rules"),"room:settings");
            button({cx+285,top,130,30},tr("重读共享设置", "Reload Sharing"),"room:reload");
            auto active=data.room.value("active",false);
            auto host=data.room.value("host",false);
            if(host){
                button({cx+424,top,167,30},data.room.value("auto_receive",true)?tr("自动接收 · 开", "Auto Receive: On"):tr("自动接收 · 关", "Auto Receive: Off"),"room:toggle-auto");
                button({cx+600,top,125,30},observersOpen?tr("返回玩家", "Back to Players"):tr("客端显示回报", "Client Reports"),"room_observers",observersOpen);
            }
            else p.text(cx+430,top+9,287,active?tr("已连接房主", "Connected to host"):tr("等待兼容的房间连接", "Waiting for a compatible room"),muted,.85f);
            auto offers=data.room.value("offers",Json::array());
            auto denied=host?data.room.value("denied",Json::array()):Json::array();
            auto reports=data.room.value("observations",Json::array());
            bool showReports=host && observersOpen;
            constexpr std::size_t rows=5;
            auto count=showReports?reports.size():data.players.size()+denied.size()+offers.size();
            auto pages=std::max<std::size_t>(1,(count+rows-1)/rows);
            playerPage%=pages;
            std::string notice=str(data.room,"message");
            if(showReports)notice=count?tr("由各客端回报该玩家的模型显示状态。", "Each client reports how this player's model is displayed."):tr("等待客端加入并接收玩家模型。", "Waiting for clients to join and receive models.");
            if(notice.empty() && !count)notice=tr("房主开放的模型会自动下载。", "Models shared by the host download automatically.");
            auto noticeWidth=active?cw-275:cw;
            p.text(cx,top+45,noticeWidth,fit(notice,noticeWidth,.85f),muted,.85f);
            if(active){
                auto speed=tr("本机上传 ", "Up ")+std::to_string(data.room.value("upload_bps",0u)/1024)+tr(" KiB/s · 下载 ", " KiB/s · Down ")
                    +std::to_string(data.room.value("download_bps",0u)/1024)+" KiB/s";
                p.text(cx+cw-268,top+45,268,speed,ink,.85f);
                auto uploadTarget=str(data.room,"upload_retry");
                if(!uploadTarget.empty())button({cx,437,145,29},tr("重新上传我的模型", "Retry My Upload"),"room:retry-upload:"+uploadTarget);
                auto limit=tr("本机限速 ", "Rate limit: ")+std::to_string(data.room.value("rate_limit",0u)/1024)+" KiB/s";
                button({cx+cw-160,437,160,29},limit,"room:cycle-rate");
            }
            for(std::size_t i=0;i<rows && playerPage*rows+i<count;++i){
                auto index=playerPage*rows+i;auto y=top+68+i*48;
                p.border({cx,y,cw,42},edge,white,7);
                if(showReports){
                    p.text(cx+16,y+13,cw-32,fit(reports.at(index).get<std::string>(),cw-32,1),ink,1);
                }else if(index<data.players.size()){
                    auto const& peer=data.players[index];auto hidden=peer.value("hidden",false);
                    auto width=host?cw-392:cw-223;
                    auto rates=str(peer,"rates");
                    float rateWidth=rates.empty()?0:165;
                    p.text(cx+16,y+4,width-rateWidth,fit(str(peer,"name"),width-rateWidth,.96f),ink,.96f);
                    if(!rates.empty())p.text(cx+16+width-rateWidth,y+5,rateWidth,rates,muted,.80f);
                    p.text(cx+16,y+23,width,fit(str(peer,"state"),width,.87f),muted,.87f);
                    auto key=str(peer,"key");
                    if(host && !key.empty()){
                        button({cx+cw-274,y+6,80,30},tr("本场拒绝", "Deny Once"),"room:deny-once:"+key);
                        button({cx+cw-190,y+6,88,30},tr("长期拒绝", "Deny Always"),"room:deny:"+key);
                    }
                    if(peer.value("retry",false))button({cx+cw-(host?369:187),y+6,87,30},tr("重新下载", "Retry"),"room:retry:"+str(peer,"stream")+"|"+str(peer,"id"));
                    button({cx+cw-96,y+6,84,30},hidden?tr("本机显示", "Show"):tr("本机屏蔽", "Hide"),"peer:"+str(peer,"id"),hidden);
                }else if(index<data.players.size()+denied.size()){
                    auto const& model=denied[index-data.players.size()];
                    auto key=str(model,"key"),title=str(model,"title");
                    bool temporary=model.value("temporary",false);
                    auto label=std::string(temporary?tr("本场已拒绝 · ", "Denied this session · "):tr("长期已拒绝 · ", "Always denied · "))+(title.empty()?std::string{}:title+" · ")+key.substr(7,12);
                    p.text(cx+16,y+13,cw-255,fit(label,cw-255,1),ink,1);
                    if(temporary)button({cx+cw-244,y+6,102,30},tr("改为长期拒绝", "Deny Always"),"room:deny:"+key);
                    button({cx+cw-137,y+6,121,30},tr("重新允许", "Allow Again"),std::string(temporary?"room:allow-once:":"room:allow:")+key);
                }else{
                    auto const& offer=offers[index-data.players.size()-denied.size()];
                    p.text(cx+16,y+13,480,fit(str(offer,"title"),480,1),ink,1);
                    button({cx+cw-137,y+6,121,30},tr("接收分享", "Accept Share"),"room:offer:"+str(offer,"key"));
                }
            }
            if(pages>1)pager(playerPage,pages,cx+260,439,205);
        } else if(page==Page::Appearance) {
            p.text(cx,top,420,tr("界面主题", "Menu theme"),ink,1.25f);
            auto selected=data.settings.value("menu_theme",0);
            std::array<char const*,4> names{tr("深蓝", "Midnight"),tr("浅色", "Light"),tr("森林", "Forest"),tr("紫罗兰", "Violet")};
            for(int i=0;i<4;++i){
                float x=cx+i*184.f;
                button({x,top+30,174,78},names[i],"theme:"+std::to_string(i),selected==i);
                auto const& t=uiTheme(i);
                for(int n=0;n<3;++n){auto color=n==0?t.panel:n==1?t.card:t.accent;
                    p.box({x+12+n*27.f,top+42,22,13},color,3);scene.paint.back().rawColor=true;}
            }
            p.text(cx,top+132,cw,tr("预览画质", "Preview quality"),ink,1.25f);
            auto quality=data.settings.value("preview_quality",1);
            button({cx,top+163,228,40},tr("流畅 · 320", "Performance · 320"),"quality:0",quality==0);
            button({cx+244,top+163,228,40},tr("均衡 · 512", "Balanced · 512"),"quality:1",quality==1);
            button({cx+488,top+163,228,40},tr("高清 · 768", "Sharp · 768"),"quality:2",quality==2);
            p.text(cx,top+216,cw,tr("分辨率上限越低，复杂模型的预览越流畅。", "Lower resolution caps reduce the cost of complex previews."),muted,.95f);
            p.border({cx,top+252,cw,67},edge,white,8);
            p.text(cx+14,top+267,300,tr("预览帧率", "Preview FPS: ")+std::to_string(data.settings.value("preview_fps",60)),ink,1.05f);
            slider({cx+320,top+268,374,25},"preview_fps",15,60,5,true);
            p.text(cx+14,top+292,680,tr("只限制菜单预览，不限制游戏帧率。", "Limits the preview only, not Minecraft's frame rate."),muted,.85f);
            button({cx,439,130,29},tr("返回设置", "Back to Settings"),"back");
        } else if(page==Page::Settings) {
            button({712,54,143,29},tr("外观与性能", "Appearance"),"appearance",false,true);
            struct Setting {char const* label;char const* description;char const* key;};
            Setting controls[]={{tr("使用原版外观", "Vanilla Appearance"),tr("关闭自己的 YSM 模型替换", "Disable your own YSM model"),"disable_self_model"},{tr("使用原版手臂", "Vanilla Hands"),tr("仅恢复第一人称手臂", "Restore first-person arms only"),"disable_self_hands"},{tr("屏蔽投射物模型", "Vanilla Projectiles"),tr("恢复箭等投射物的原版外观", "Restore vanilla arrows and other projectiles"),"disable_projectile_model"},{tr("屏蔽载具模型", "Vanilla Vehicles"),tr("恢复船与矿车的原版外观", "Restore vanilla boats and minecarts"),"disable_vehicle_model"},{tr("动作提示", "Emote Notifications"),tr("显示转盘动作名称", "Show the selected emote name"),"print_animation_roulette_msg"},{tr("移动时保留动作", "Keep Emote While Moving"),tr("关闭后，走动、跳跃或潜行会停止额外动作", "Off: walking, jumping or sneaking stops emotes"),"movement_lock"},{tr("屏蔽其他玩家", "Hide Other Models"),tr("恢复其他玩家的原版外观", "Restore other players' vanilla appearance"),"disable_other_models"},{tr("角色小窗", "HUD Avatar"),tr("在游戏画面显示自己的动作", "Show your model on the game HUD"),"show_hud_avatar"}};
            for(std::size_t i=0;i<std::size(controls);++i){auto const& c=controls[i];UiRect r{cx+static_cast<float>(i%2)*371,top+static_cast<float>(i/2)*55,354,49};auto id="setting:"+std::string(c.key);p.border(r,edge,white,9);p.text(r.x+16,r.y+8,240,c.label,ink,1.02f);p.text(r.x+16,r.y+28,254,c.description,muted,.78f);toggle({r.x+r.w-56,r.y+13,40,21},id,data.settings.value(c.key,false),r);}
            auto option = [&](float x, char const* label, char const* key, bool initial) {
                p.text(x+12,top+365,165,label,ink,.95f);
                toggle({x+177,top+357,40,21},"setting:"+std::string(key),data.settings.value(key,initial),UiRect{x,top+355,229,28});
            };
            option(cx, tr("原生网格预览", "Native Mesh Preview"), "native_menu_preview", true);
            option(cx+247, tr("鼠标兼容模式", "Mouse Compatibility"), "relative_menu_pointer", false);
            option(cx+494, tr("Arke 附加发光层", "Arke Emissive Layer"), "arke_extra_emissive_layer", false);
            p.box({cx,top+224,cw,130},white,9);
            p.text(cx+16,top+234,96,tr("模型音量", "Model Volume"),ink,1);
            p.text(cx+367,top+234,168,tr("纸娃娃帧率  ", "Avatar FPS: ")+std::to_string(data.settings.value("hud_fps",60)),ink,.95f);
            slider({cx+510,top+226,182,24},"hud_fps",5,60,1,true);
            slider({cx+112,top+226,218,24},"sound_volume",0,100,1,true);
            if (data.settings.value("show_hud_avatar",false)) {
                p.text(cx+16,top+271,95,tr("左右位置", "Horizontal"),ink,.92f);slider({cx+113,top+262,219,26},"hud_x",0,100,1,true);
                p.text(cx+385,top+271,95,tr("上下位置", "Vertical"),ink,.92f);slider({cx+482,top+262,210,26},"hud_y",0,100,1,true);
                p.text(cx+16,top+314,95,tr("小窗大小", "Avatar Size"),ink,.92f);slider({cx+113,top+305,219,26},"hud_size",0,100,1,true);
                p.text(cx+385,top+314,95,tr("小窗角度", "Avatar Angle"),ink,.92f);slider({cx+482,top+305,210,26},"hud_yaw",0,100,1,true);
            }
        } else if(page==Page::Info) {
            std::vector<std::pair<std::string,std::string>> lines;
            auto add=[&](std::string const& text){for(auto const& row:wrap(text,665,.96f))lines.emplace_back(row,"");};
            add(tr("模型：", "Model: ")+modelName);auto tips=local("metadata.tips",str(metadata,"tips"));if(!tips.empty())add(tips);
            auto license=metadata.value("license",Json::object());add(tr("许可：", "License: ")+(license.is_string()?license.get<std::string>():str(license,"type",tr("未声明", "Unspecified"))));
            std::size_t authorIndex=0;for(auto const& author:metadata.value("authors",Json::array())){
                auto key="metadata.authors."+std::to_string(authorIndex++);add(local(key+".name",str(author,"name"))+"  ·  "+local(key+".role",str(author,"role")));
                auto comment=local(key+".comment",str(author,"comment"));if(!comment.empty())add(comment);
                auto contacts=author.value("contact",Json::object());if(contacts.is_object())for(auto const& [label,link]:contacts.items())if(link.is_string())lines.emplace_back(tr("打开 ", "Open ")+displayLabel(data.locale,label),link.get<std::string>());
            }
            auto links=metadata.value("link",Json::object());if(links.is_object())for(auto const& [label,link]:links.items())if(link.is_string())lines.emplace_back(tr("打开 ", "Open ")+displayLabel(data.locale,label),link.get<std::string>());
            p.border({cx,top,cw,378},edge,white,10);constexpr std::size_t rows=9;auto pages=std::max<std::size_t>(1,(lines.size()+rows-1)/rows);infoPage%=pages;
            for(std::size_t n=0;n<rows&&infoPage*rows+n<lines.size();++n){auto const& [label,link]=lines[infoPage*rows+n];if(link.empty())p.text(cx+22,top+24+n*33,cw-44,label,n==0?ink:muted,.96f);else button({cx+22,top+15+n*33,cw-44,28},label,"link:"+link,false,false,"info");}
            pager(infoPage,pages,cx+264,439,197);

        }
    }
     
    scene.overlayStart=scene.paint.size();
    auto transform=[&](UiRect& r){r.x=ox+r.x*scale;r.y=oy+r.y*scale;r.w*=scale;r.h*=scale;};
    auto const& theme=uiTheme(data.settings.value("menu_theme",0));
    for(auto& paint:scene.paint){if(!paint.rawColor)paint.color=themeColor(paint.color,theme);transform(paint.rect);paint.scale*=scale;paint.radius*=scale;
        if(paint.line){paint.endX=ox+paint.endX*scale;paint.endY=oy+paint.endY*scale;}}
    for(auto& h:hits)transform(h.rect);for(auto& r:ranges)transform(r.rect);
    if(scene.preview){transform(*scene.preview);previewArea=scene.preview;}
    return scene;
}
}
