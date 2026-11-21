#include "WheelGeometry.h"
#include "YsmUi.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace ysm {
namespace {
constexpr std::uint32_t ink=0xff243e37,muted=0xff82938a,teal=0xff247968,edge=0xffe0e7df;
constexpr std::uint32_t paper=0xfffafbf7,white=0xffffffff,wash=0xffe6f2e9,soft=0xfff0f4ed;
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
    void text(float x,float y,float w,std::string value,std::uint32_t color=ink,float scale=1,bool center=false){scene.paint.push_back({{x,y,w,14*scale},color,std::move(value),scale,center});}
    void line(float x,float y,float xx,float yy,std::uint32_t color,float weight=1.4f){
        auto distance=std::hypot(xx-x,yy-y);auto steps=std::max(1,static_cast<int>(distance));
        for(int i=0;i<=steps;++i){auto f=static_cast<float>(i)/steps;box({x+(xx-x)*f-weight/2,y+(yy-y)*f-weight/2,weight,weight},color,weight/2);}
    }
    void circle(float x,float y,float r,std::uint32_t color){box({x-r,y-r,r*2,r*2},color,r);}
    void icon(std::string const& id,float x,float y,float size,std::uint32_t color){
        auto l=[&](float a,float b,float c,float d){line(x+a*size,y+b*size,x+c*size,y+d*size,color,std::max(1.f,size*.075f));};
        if(id=="close"){l(.25f,.25f,.75f,.75f);l(.75f,.25f,.25f,.75f);}
        else if(id=="check"){l(.17f,.52f,.40f,.75f);l(.40f,.75f,.84f,.24f);}
        else if(id=="prev"||id=="next"){auto s=id=="prev"?1.f:-1.f;float c=.5f;l(c+s*.15f,.2f,c-s*.15f,.5f);l(c-s*.15f,.5f,c+s*.15f,.8f);}
        else if(id=="search"){border({x+.12f*size,y+.1f*size,size*.57f,size*.57f},color,white,size*.3f,1.5f);l(.61f,.61f,.87f,.87f);}
        else if(id=="model"){l(.5f,.07f,.9f,.3f);l(.9f,.3f,.9f,.73f);l(.9f,.73f,.5f,.96f);l(.5f,.96f,.1f,.73f);l(.1f,.73f,.1f,.3f);l(.1f,.3f,.5f,.07f);l(.1f,.3f,.5f,.54f);l(.5f,.54f,.9f,.3f);l(.5f,.54f,.5f,.96f);}
        else if(id=="texture"){for(int a=0;a<2;++a)for(int b=0;b<2;++b)border({x+size*(.1f+a*.48f),y+size*(.1f+b*.48f),size*.32f,size*.32f},color,color,1,0);}
        else if(id=="play"){l(.28f,.12f,.86f,.5f);l(.86f,.5f,.28f,.88f);l(.28f,.88f,.28f,.12f);}
        else if(id=="wheel"||id=="settings"||id=="info"){
            border({x+.08f*size,y+.08f*size,size*.84f,size*.84f},color,paper,size*.42f,1.4f);
            if(id=="info"){circle(x+.5f*size,y+.32f*size,size*.045f,color);l(.5f,.46f,.5f,.73f);}
            else if(id=="settings"){border({x+.35f*size,y+.35f*size,size*.3f,size*.3f},color,paper,size*.15f,1.2f);for(int i=0;i<4;++i){auto a=i*1.5707963f;l(.5f+std::cos(a)*.32f,.5f+std::sin(a)*.32f,.5f+std::cos(a)*.50f,.5f+std::sin(a)*.50f);}}
            else for(int i=0;i<8;++i){auto a=i*.78539816f;l(.5f+std::cos(a)*.18f,.5f+std::sin(a)*.18f,.5f+std::cos(a)*.39f,.5f+std::sin(a)*.39f);}
        } else if(id=="star") {
            float lastX=.5f,lastY=.06f;for(int i=1;i<=10;++i){float a=-1.5707963f+i*.6283185f,r=i%2?.19f:.44f;float nx=.5f+std::cos(a)*r,ny=.5f+std::sin(a)*r;l(lastX,lastY,nx,ny);lastX=nx;lastY=ny;}
        } else if(id=="reload"){l(.8f,.28f,.66f,.13f);l(.66f,.13f,.3f,.13f);l(.3f,.13f,.12f,.36f);l(.12f,.36f,.12f,.7f);l(.12f,.7f,.36f,.88f);l(.36f,.88f,.72f,.85f);l(.72f,.85f,.88f,.66f);l(.8f,.1f,.8f,.35f);l(.8f,.35f,.55f,.35f);}
        else if(id=="stop")box({x+size*.23f,y+size*.23f,size*.54f,size*.54f},color,2);
        else if(id=="minus")l(.2f,.5f,.8f,.5f);
        else if(id=="plus"){l(.2f,.5f,.8f,.5f);l(.5f,.2f,.5f,.8f);}
    }
};
}

UiScene YsmUi::draw(float width,float height, float hotbarTop) {
    previewArea.reset();
    UiScene scene;hits.clear();ranges.clear();if(!isOpen()||width<=0||height<=0)return scene;
    constexpr float W=920,H=540;
    // HUD and menu use logical GUI pixels. Account for the highlighted slot before
    // computing the shared transform for drawing, hover, dragging and button hits.
    bool validBar = std::isfinite(hotbarTop) && hotbarTop >= height*.5f && hotbarTop < height;
    float contentHeight = std::max(1.f, std::min(validBar ? hotbarTop : height, height - 24.f));
    auto scale=std::min(width/(W+32),contentHeight/(H+28));if(!std::isfinite(scale)||scale<=0)return scene;
    auto ox=(width-W*scale)/2,oy=(contentHeight-H*scale)/2;
    auto mx=(mouseX-ox)/scale,my=(mouseY-oy)/scale;
    Painter p{scene};
    auto hit=[&](UiRect r,std::string id){hits.push_back({r,std::move(id)});};
    auto focus=[&](UiRect r,std::string const& id,float radius=6){if(focusId==id)p.border({r.x-2,r.y-2,r.w+4,r.h+4},teal,0x00247968,radius+2,1);};
    auto button=[&](UiRect r,std::string label,std::string id,bool active=false,bool primary=false,std::string icon={}) {
        bool hover=r.contains(mx,my);auto fill=primary?teal:active?wash:hover?0xffeaf0e7:white;
        focus(r,id);p.border(r,active?0xffa9cdbc:edge,fill,6,primary?0.f:1.f);
        auto color=primary?white:active?teal:ink;
        if(!icon.empty())p.icon(icon,r.x+10,r.y+(r.h-14)/2,14,color);
        p.text(r.x+(icon.empty()?10:32),r.y+(r.h-9)/2,r.w-(icon.empty()?20:42),fit(label,r.w-(icon.empty()?20:42),.95f),color,.95f);
        hit(r,std::move(id));
    };
    auto iconButton=[&](UiRect r,std::string icon,std::string id,bool active=false) {
        bool hover=r.contains(mx,my);focus(r,id,r.h/2);p.border(r,active?0xffbdd8c9:hover?0xffc8d7c9:edge,active?wash:white,r.h/2);
        p.icon(icon,r.x+(r.w-14)/2,r.y+(r.h-14)/2,14,active?teal:muted);hit(r,std::move(id));
    };
    auto toggle=[&](UiRect r,std::string id,bool on,std::optional<UiRect> row={}) {
        focus(r,id,r.h/2);p.box(r,on?teal:0xffd8e0d5,r.h/2);p.circle(r.x+(on?r.w-r.h/2:r.h/2),r.y+r.h/2,r.h/2-3,white);hit(row.value_or(r),std::move(id));
    };
    auto slider=[&](UiRect r,std::string route,double minimum,double maximum,double step,bool setting=false) {
        auto v=setting?data.settings.value(route,0.0):data.values.value(route,0.0);auto value=maximum>minimum?std::clamp((v-minimum)/(maximum-minimum),0.0,1.0):0;
        auto id="range:"+route;focus(r,id);p.box({r.x,r.y+r.h/2-2,r.w,4},edge,2);p.box({r.x,r.y+r.h/2-2,static_cast<float>(r.w*value),4},teal,2);
        p.circle(r.x+static_cast<float>(r.w*value),r.y+r.h/2,7,0x20243e37);p.circle(r.x+static_cast<float>(r.w*value),r.y+r.h/2-1,5,white);p.circle(r.x+static_cast<float>(r.w*value),r.y+r.h/2-1,3,teal);
        hit(r,id);ranges.push_back({r,id,std::move(route),minimum,maximum,step,setting});
    };
    auto chip=[&](float x,float y,std::string label,std::uint32_t fill=wash,std::uint32_t color=teal,float cw=64){p.box({x,y,cw,21},fill,10);p.text(x,y+6,cw,label,color,.8f,true);};
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
    auto modelName=candidate ? str(language,"metadata.name",candidate->name) : local("metadata.name",str(metadata,"name",data.source.empty()?"尚未选择模型":data.source));
    std::string title=page==Page::Models?"模型库":page==Page::Textures?"模型贴图":page==Page::Preview?"动作预览":page==Page::Info?"模型信息":page==Page::Settings?"本地设置":page==Page::Forms?"模型设置":page==Page::Players?"联机玩家":page==Page::Import?"导入模型":"动作转盘";
    if (isWheel()) {
        p.text(210,11,500,"动作转盘",white,1.9f,true);
        auto control=[&](UiRect r,std::string label,std::string id,bool active=false) {
            auto hovered=r.contains(mx,my);
            focus(r,id);
            p.border(r,active ? 0xff90cbb8 : edge,active ? wash : hovered ? soft : white,7);
            p.text(r.x+3,r.y+(r.h-10)/2,r.w-6,fit(label,r.w-6),active ? teal : ink,1,true);
            hit(r,std::move(id));
        };
        // All preview and control rectangles are disjoint, even at narrow aspect ratios.
        if (data.enabled) {
            // Give the portrait a full-height stage and use spare left margin on wide screens.
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
        p.circle(cx,cy+4,radius+3,0x2807140c);
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
                control({cx+dx*(radius*.42f)-17,cy+dy*(radius*.42f)-9,34,18},"设置","form:"+e->form,editing);
        }
        p.circle(cx,cy,42,paper);
        p.icon("wheel",cx-12,cy-27,24,teal);
        control({cx-37,cy+3,74,22},data.settings.value("movement_lock",false)?"动作锁：开":"动作锁：关","setting:movement_lock");
        constexpr float sx=602, sw=313;
        std::string path="根目录";
        for (std::size_t n=1;n<categories.size();++n) {
            auto const& id=categories[n].first;
            auto parent=props(data.descriptor).value("extra_animation",Json::object());
            if(n>1) for(auto const& c:props(data.descriptor).value("extra_animation_classify",Json::array()))
                if(str(c,"id")==categories[n-1].first){parent=c.value("extra_animation",Json::object());break;}
            path+=" / "+local("properties.extra_animation.#"+id,str(parent,("#"+id).c_str(),id));
        }
        float cardBottom=page==Page::Forms ? 478.f : 280.f;
        p.box({sx-5,46,sw+14,cardBottom-38},0x2808140c,12);
        p.border({sx-8,40,sw+16,cardBottom-36},edge,paper,12);
        p.box({sx+1,45,sw-2,58},white,8);
        p.text(sx+45,55,sw-90,fit(path,sw-90),ink,1,true);
        p.text(sx+45,79,sw-90,std::to_string(wheelPage+1)+" / "+std::to_string(pages),teal,1.15f,true);
        iconButton({sx+9,57,32,32},"prev","wheel_prev");
        iconButton({sx+sw-41,57,32,32},"next","wheel_next");
        control({sx,108,sw,31},"返回","back");
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
                // The range control has room below it for the author's option legend.
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
                        control({sx+sw-71,y,64,29},on?"已开启":"已关闭","value:"+key+":toggle",on);
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
                    control({sx,390,90,30},"上一项","prev");
                    p.text(sx+96,400,sw-192,std::to_string(formPage+1)+" / "+std::to_string(forms.size()),muted,.95f,true);
                    control({sx+sw-90,390,90,30},"下一项","next");
                }
            } else p.text(sx+12,163,sw-24,"作者未提供设置项",muted,1);
        } else {
            std::string heading="选择装饰或动作", detail="点击扇区，或按对应数字键 1–8。";
            if(wheelHovered>=0 && wheelPage*8+wheelHovered<entries.size()) {
                auto const& e=entries[wheelPage*8+wheelHovered];
                heading=e.label;detail=e.description;
                if(detail.empty()) detail=e.key=="#return" ? "返回上一级" : e.key.starts_with('#') ? "打开分组"
                    : !e.form.empty() ? "在右侧调整装饰" : "播放动作并关闭转盘";
            }
            p.text(sx+12,160,sw-24,fit(heading,sw-24,1.15f),teal,1.15f);
            auto lines=wrap(detail,sw-24,.9f);
            for(std::size_t i=0;i<std::min<std::size_t>(3,lines.size());++i)p.text(sx+12,186+i*14,sw-24,lines[i],muted,.9f);
        }
        float actionY=cardBottom-38;
        button({sx,actionY,151,32},"停止当前动作","stop",false,false,"stop");
        button({sx+157,actionY,156,32},"动作预览","preview",false,true,"play");
        iconButton({W-28,3,28,28},"close","close");
    } else {
        p.box({2,7,W,H},0x3008140c,16);p.box({0,0,W,H},paper,14);
        p.box({0,0,143,H},0xfff0f3eb,14);p.box({132,0,11,H},0xfff0f3eb);p.box({143,0,1,H},edge);
        p.box({20,25,29,29},teal,8);p.icon("model",26,31,17,white);p.text(59,29,70,"YSM",ink,1.75f);p.text(22,67,100,"模型工坊",muted,.9f);
        float navY=112;
        auto nav=[&](std::string label,std::string id,std::string icon,Page target) {
            UiRect r{12,navY,119,37};bool active=page==target,hover=r.contains(mx,my);focus(r,id);if(active||hover)p.box(r,active?wash:0xffe7eddf,7);
            if(active)p.box({12,navY+10,3,17},teal,1.5f);p.icon(icon,25,navY+10,17,active?teal:muted);p.text(52,navY+14,70,label,active?teal:ink,1);
            hit(r,std::move(id));navY+=45;
        };
        nav("模型库","models","model",Page::Models);nav("贴图","textures","texture",Page::Textures);nav("动作预览","preview","play",Page::Preview);nav("动作转盘","roulette","wheel",Page::Roulette);nav("模型信息","info","info",Page::Info);nav("本地设置","settings","settings",Page::Settings);
        nav("联机玩家","players","model",Page::Players);
        p.box({20,424,103,1},edge);p.circle(27,447,3,data.enabled?teal:muted);p.text(38,443,85,data.enabled?"模型已启用":"原版外观",muted,.8f);
        button({19,474,105,30},data.enabled?"恢复原版":"启用模型","toggle",false,false);
        p.text(169,29,610,title,ink,1.85f);p.text(170,60,600,page==Page::Import?"选择本机的模型包或源文件夹。":page==Page::Models?"点选模型即可预览。":page==Page::Textures?"换一张贴图，换一种心情。":page==Page::Preview?"查看模型自带的动作与外观设置。":page==Page::Info?"每一个模型，都有创作者的故事。":page==Page::Settings?"调整自己的观看与操作体验。":"由模型作者提供的外观与动作选项。",muted,.9f);
        iconButton({866,24,30,30},"close","close");
        p.box({169,490,727,1},edge);hint(171,507,"Tab","切换焦点",29);hint(320,507,"Enter","选择",39);hint(451,507,"Esc","返回",28);
        if(data.loading || !data.browseLoading.empty())chip(726,507,"正在加载…",wash,teal,167);
        else if(!data.message.empty())p.text(618,513,270,fit(data.message,270,.8f),muted,.8f);
        else p.text(698,513,190,"本地模型 · 随时切换",muted,.8f);
        constexpr float top=96,cx=171,cw=725;
        auto pager=[&](std::size_t& current,std::size_t total,float px,float yy,float ww) {
            total=std::max<std::size_t>(1,total);current%=total;
            iconButton({px,yy,29,27},"prev","prev");iconButton({px+ww-29,yy,29,27},"next","next");p.text(px+38,yy+9,ww-76,std::to_string(current+1)+" / "+std::to_string(total),muted,.82f,true);
        };
        auto modelStage=[&](UiRect r) {
            p.box(r,0xffeaf1e5,10);p.box({r.x+14,r.y+14,75,20},0xcfffffff,10);p.text(r.x+14,r.y+20,75,page==Page::Models?"模型预览":"当前模型",teal,.75f,true);
            preview({r.x+12,r.y+47,r.w-24,r.h-126});
            if (libraryPage && !data.browseLoading.empty()) {
                p.text(r.x+18,r.y+r.h/2,r.w-36,"正在读取模型…",muted,.9f,true);
                return;
            }
            p.text(r.x+18,r.y+r.h-66,r.w-36,fit(modelName,r.w-36,1.12f),ink,1.12f);
            if (page==Page::Models && candidate) {
                auto src=data.browsing->source;
                if(src.starts_with("server:")){
                    auto width=(r.w-40)*.5f;
                    button({r.x+16,r.y+r.h-43,width,31},"穿戴","wear",false,true);
                    button({r.x+24+width,r.y+r.h-43,width,31},"收藏到本地","room:collect:"+src.substr(7));
                }else button({r.x+16,r.y+r.h-43,r.w-32,31},"穿戴此模型","wear",false,true);
            }
            else p.text(r.x+18,r.y+r.h-40,r.w-36,fit(local("metadata.tips",str(metadata,"tips","陪你开始一段新的冒险")),r.w-36,.8f),muted,.8f);
        };
        if(page==Page::Models) {
            constexpr float listW=453;
            UiRect searchRect{cx,top,277,32};focus(searchRect,"search");p.border(searchRect,searchFocused?0xff93bfaa:edge,white,7);p.icon("search",cx+11,top+9,14,muted);
            p.text(cx+33,top+12,232,fit(search.empty()?"搜索名称（可粘贴中文）":search,232,.9f),search.empty()?muted:ink,.9f);hit(searchRect,"search");
            button({cx+288,top,105,32},favoritesOnly?"已收藏":"全部模型","favorite_filter",favoritesOnly,false,"star");iconButton({cx+409,top,34,32},"reload","reload");
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
                auto name=str(strings,"metadata.name",str(e,"name",source));
                auto author=str(strings,"metadata.authors.0.name",str(e,"author",str(e,"kind")=="folder"?"源模型文件夹":"YSM 模型包"));
                if(!query.empty()&&lower(source+"\n"+name+"\n"+author+"\n"+str(strings,"metadata.tips")).find(query)==std::string::npos)continue;
                entries.push_back({&e,std::move(name),std::move(author)});
            }
            p.text(cx,top+49,300,"本地模型",ink,.9f);p.text(cx+360,top+49,83,std::to_string(entries.size())+" 个模型",muted,.8f);
            constexpr std::size_t perPage=4;auto pages=std::max<std::size_t>(1,(entries.size()+perPage-1)/perPage);listPage%=pages;
            for(std::size_t n=0;n<perPage&&listPage*perPage+n<entries.size();++n) {
                auto const& card=entries[listPage*perPage+n];auto const& e=*card.row;
                auto source=str(e,"source");bool selected=source==(data.browseLoading.empty()?(candidate?candidate->source:data.source):data.browseLoading),starred=std::find(favorites.begin(),favorites.end(),source)!=favorites.end();
                auto display=card.name;if(display.ends_with(".zip"))display.resize(display.size()-4);
                UiRect r{cx+static_cast<float>(n%2)*232,top+72+static_cast<float>(n/2)*131,221,119};auto id="select:"+source;
                bool hover=r.contains(mx,my);focus(r,id,9);p.border(r,selected?0xff8fbea6:hover?0xffb9cdb7:edge,selected?0xffedf6ee:white,9,selected?1.5f:1.f);hit(r,id);
                p.box({r.x+13,r.y+13,38,38},selected?0xffd7eadb:soft,9);p.icon("model",r.x+23,r.y+23,18,selected?teal:0xff93aa98);
                if(source==data.source && data.enabled)chip(r.x+60,r.y+21,"已穿戴",wash,teal,58);
                else if(selected)chip(r.x+60,r.y+21,"预览中",wash,teal,58);
                iconButton({r.x+r.w-38,r.y+15,25,25},"star","favorite:"+source,starred);
                p.text(r.x+14,r.y+66,r.w-28,fit(display,r.w-28,1.05f),ink,1.05f);
                p.text(r.x+14,r.y+92,r.w-28,fit(str(e,"status")=="invalid"?"读取失败 · 可重新选择":card.author,r.w-28,.76f),str(e,"status")=="invalid"?0xffa27a49:muted,.76f);
            }
            if(entries.empty()){p.icon("model",cx+195,top+157,38,0xffbecbbb);p.text(cx,top+211,listW,"没有匹配的本地模型",muted,1,true);}
            button({cx,439,104,28},"导入模型","import");
            pager(listPage,pages,cx+128,439,197);
            modelStage({650,top,246,378});
        } else if(page==Page::Import) {
            button({cx,top+20,221,40},"选择模型包","import_file",false,true);
            p.text(cx,top+73,cw,"支持 .zip 和 .ysm",muted,.9f);
            button({cx,top+124,221,40},"选择源文件夹","import_folder");
            p.text(cx,top+177,cw,"选择包含完整模型资源的文件夹",muted,.9f);
            button({cx,top+220,221,40},"打开模型文件夹","open_models_folder");
            button({cx,top+306,90,31},"返回","back");
        } else if(page==Page::Textures) {
            modelStage({cx,top,292,378});float lx=485,lw=411;
            p.text(lx,top+4,lw,"可用贴图",ink,1.1f);p.text(lx,top+27,lw,"选择后应用到当前模型",muted,.85f);
            constexpr std::size_t rows=5;auto pages=std::max<std::size_t>(1,(data.textures.size()+rows-1)/rows);texturePage%=pages;
            for(std::size_t n=0;n<rows&&texturePage*rows+n<data.textures.size();++n){auto const& t=data.textures[texturePage*rows+n];UiRect r{lx,top+57+n*51,lw,43};button(r,local("files.player.texture."+t,t),"texture:"+t,t==data.texture,false,"texture");if(t==data.texture)p.icon("check",r.x+r.w-30,r.y+14,14,teal);}
            pager(texturePage,pages,lx+102,439,207);
        } else if(page==Page::Preview) {
            scene.animatePreview=true;
            p.box({cx,top,328,378},0xffeaf1e5,10);chip(cx+15,top+15,"动作展示",white,teal,72);preview({cx+15,top+46,298,264});
            iconButton({cx+16,top+329,33,31},"prev","rotate_left");iconButton({cx+58,top+329,33,31},"next","rotate_right");button({cx+121,top+329,86,31},"复位预览","reset_preview");iconButton({cx+226,top+329,33,31},"minus","zoom_out");iconButton({cx+268,top+329,33,31},"plus","zoom_in");
            float lx=521,lw=375;std::vector<std::pair<std::string,std::string>> choices{{"自动动作","automatic"}};
            if (!candidate && data.browseLoading.empty()) for(auto const& b:buttons(data.descriptor))choices.emplace_back(str(b,"name",str(b,"id")),"form:"+str(b,"id"));
            if (data.browseLoading.empty()) for(auto const& c:clips)choices.emplace_back(str(language,("properties.extra_animation."+c).c_str(),c),"clip:"+c);
            p.text(lx,top+5,lw,candidate ? modelName : "模型设置与动作",ink,1.1f);p.text(lx,top+30,lw,"保留模型作者的原始动作",muted,.85f);
            constexpr std::size_t rows=7;auto pages=std::max<std::size_t>(1,(choices.size()+rows-1)/rows);previewPage%=pages;
            for(std::size_t n=0;n<rows&&previewPage*rows+n<choices.size();++n){auto const& [label,id]=choices[previewPage*rows+n];button({lx,top+61+n*38,lw,31},label,id,id=="clip:"+previewClip,false,id.starts_with("form:")?"settings":"play");}
            pager(previewPage,pages,lx+89,439,197);
        } else if(page==Page::Players) {
            button({cx,top,135,30},"共享文件夹","room:folder");
            button({cx+145,top,130,30},"房间规则","room:settings");
            button({cx+285,top,130,30},"重读共享设置","room:reload");
            auto active=data.room.value("active",false);
            auto host=data.room.value("host",false);
            if(host){
                button({cx+424,top,167,30},data.room.value("auto_receive",true)?"自动接收 · 开":"自动接收 · 关","room:toggle-auto");
                button({cx+600,top,125,30},observersOpen?"返回玩家":"客端显示回报","room_observers",observersOpen);
            }
            else p.text(cx+430,top+9,287,active?"已连接房主":"等待兼容的房间连接",muted,.85f);
            auto offers=data.room.value("offers",Json::array());
            auto denied=host?data.room.value("denied",Json::array()):Json::array();
            auto reports=data.room.value("observations",Json::array());
            bool showReports=host && observersOpen;
            constexpr std::size_t rows=5;
            auto count=showReports?reports.size():data.players.size()+denied.size()+offers.size();
            auto pages=std::max<std::size_t>(1,(count+rows-1)/rows);
            playerPage%=pages;
            std::string notice=str(data.room,"message");
            if(showReports)notice=count?"由各客端回报该玩家的模型显示状态。":"等待客端加入并接收玩家模型。";
            if(notice.empty() && !count)notice="房主开放的模型会自动下载。";
            auto noticeWidth=active?cw-275:cw;
            p.text(cx,top+45,noticeWidth,fit(notice,noticeWidth,.85f),muted,.85f);
            if(active){
                auto speed="本机上传 "+std::to_string(data.room.value("upload_bps",0u)/1024)+" KiB/s · 下载 "
                    +std::to_string(data.room.value("download_bps",0u)/1024)+" KiB/s";
                p.text(cx+cw-268,top+45,268,speed,ink,.85f);
                auto uploadTarget=str(data.room,"upload_retry");
                if(!uploadTarget.empty())button({cx,437,145,29},"重新上传我的模型","room:retry-upload:"+uploadTarget);
                auto limit="本机限速 "+std::to_string(data.room.value("rate_limit",0u)/1024)+" KiB/s";
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
                        button({cx+cw-274,y+6,80,30},"本场拒绝","room:deny-once:"+key);
                        button({cx+cw-190,y+6,88,30},"长期拒绝","room:deny:"+key);
                    }
                    if(peer.value("retry",false))button({cx+cw-(host?369:187),y+6,87,30},"重新下载","room:retry:"+str(peer,"stream")+"|"+str(peer,"id"));
                    button({cx+cw-96,y+6,84,30},hidden?"本机显示":"本机屏蔽","peer:"+str(peer,"id"),hidden);
                }else if(index<data.players.size()+denied.size()){
                    auto const& model=denied[index-data.players.size()];
                    auto key=str(model,"key"),title=str(model,"title");
                    bool temporary=model.value("temporary",false);
                    auto label=std::string(temporary?"本场已拒绝 · ":"长期已拒绝 · ")+(title.empty()?std::string{}:title+" · ")+key.substr(7,12);
                    p.text(cx+16,y+13,cw-255,fit(label,cw-255,1),ink,1);
                    if(temporary)button({cx+cw-244,y+6,102,30},"改为长期拒绝","room:deny:"+key);
                    button({cx+cw-137,y+6,121,30},"重新允许",std::string(temporary?"room:allow-once:":"room:allow:")+key);
                }else{
                    auto const& offer=offers[index-data.players.size()-denied.size()];
                    p.text(cx+16,y+13,480,fit(str(offer,"title"),480,1),ink,1);
                    button({cx+cw-137,y+6,121,30},"接收分享","room:offer:"+str(offer,"key"));
                }
            }
            if(pages>1)pager(playerPage,pages,cx+260,439,205);
        } else if(page==Page::Settings) {
            struct Setting {char const* label;char const* description;char const* key;};
            Setting controls[]={{"使用原版外观","关闭自己的 YSM 模型替换","disable_self_model"},{"使用原版手臂","仅恢复第一人称手臂","disable_self_hands"},{"屏蔽投射物模型","恢复箭等投射物的原版外观","disable_projectile_model"},{"屏蔽载具模型","恢复船与矿车的原版外观","disable_vehicle_model"},{"动作提示","显示转盘动作名称","print_animation_roulette_msg"},{"移动时保留动作","关闭后，走动、跳跃或潜行会停止额外动作","movement_lock"},{"屏蔽其他玩家","恢复其他玩家的原版外观","disable_other_models"},{"角色小窗","在游戏画面显示自己的动作","show_hud_avatar"}};
            for(std::size_t i=0;i<std::size(controls);++i){auto const& c=controls[i];UiRect r{cx+static_cast<float>(i%2)*371,top+static_cast<float>(i/2)*55,354,49};auto id="setting:"+std::string(c.key);p.border(r,edge,white,9);p.text(r.x+16,r.y+8,240,c.label,ink,1.02f);p.text(r.x+16,r.y+28,254,c.description,muted,.78f);toggle({r.x+r.w-56,r.y+13,40,21},id,data.settings.value(c.key,false),r);}
            auto option = [&](float x, char const* label, char const* key, bool initial) {
                p.text(x+12,top+365,165,label,ink,.95f);
                toggle({x+177,top+357,40,21},"setting:"+std::string(key),data.settings.value(key,initial),UiRect{x,top+355,229,28});
            };
            option(cx, "原生网格预览", "native_menu_preview", true);
            option(cx+247, "鼠标兼容模式", "relative_menu_pointer", false);
            option(cx+494, "Arke 附加发光层", "arke_extra_emissive_layer", false);
            p.box({cx,top+224,cw,130},white,9);
            p.text(cx+16,top+234,96,"模型音量",ink,1);
            p.text(cx+367,top+234,168,"纸娃娃帧率  "+std::to_string(data.settings.value("hud_fps",30)),ink,.95f);
            slider({cx+510,top+226,182,24},"hud_fps",5,30,1,true);
            slider({cx+112,top+226,218,24},"sound_volume",0,100,1,true);
            if (data.settings.value("show_hud_avatar",false)) {
                p.text(cx+16,top+271,95,"左右位置",ink,.92f);slider({cx+113,top+262,219,26},"hud_x",0,100,1,true);
                p.text(cx+385,top+271,95,"上下位置",ink,.92f);slider({cx+482,top+262,210,26},"hud_y",0,100,1,true);
                p.text(cx+16,top+314,95,"小窗大小",ink,.92f);slider({cx+113,top+305,219,26},"hud_size",0,100,1,true);
                p.text(cx+385,top+314,95,"小窗角度",ink,.92f);slider({cx+482,top+305,210,26},"hud_yaw",0,100,1,true);
            }
        } else if(page==Page::Info) {
            std::vector<std::pair<std::string,std::string>> lines;
            auto add=[&](std::string const& text){for(auto const& row:wrap(text,665,.96f))lines.emplace_back(row,"");};
            add("模型："+modelName);auto tips=local("metadata.tips",str(metadata,"tips"));if(!tips.empty())add(tips);
            auto license=metadata.value("license",Json::object());add("许可："+(license.is_string()?license.get<std::string>():str(license,"type","未声明")));
            std::size_t authorIndex=0;for(auto const& author:metadata.value("authors",Json::array())){
                auto key="metadata.authors."+std::to_string(authorIndex++);add(local(key+".name",str(author,"name"))+"  ·  "+local(key+".role",str(author,"role")));
                auto comment=local(key+".comment",str(author,"comment"));if(!comment.empty())add(comment);
                auto contacts=author.value("contact",Json::object());if(contacts.is_object())for(auto const& [label,link]:contacts.items())if(link.is_string())lines.emplace_back("打开 "+label,link.get<std::string>());
            }
            auto links=metadata.value("link",Json::object());if(links.is_object())for(auto const& [label,link]:links.items())if(link.is_string())lines.emplace_back("打开 "+label,link.get<std::string>());
            p.border({cx,top,cw,378},edge,white,10);constexpr std::size_t rows=9;auto pages=std::max<std::size_t>(1,(lines.size()+rows-1)/rows);infoPage%=pages;
            for(std::size_t n=0;n<rows&&infoPage*rows+n<lines.size();++n){auto const& [label,link]=lines[infoPage*rows+n];if(link.empty())p.text(cx+22,top+24+n*33,cw-44,label,n==0?ink:muted,.96f);else button({cx+22,top+15+n*33,cw-44,28},label,"link:"+link,false,false,"info");}
            pager(infoPage,pages,cx+264,439,197);

        }
    }
    // Flat vector cursor and subtle shadow, in the same coordinates as hits.
    scene.overlayStart=scene.paint.size();
    if(mouseX>=0&&mouseY>=0){p.line(mx+1,my+2,mx+1,my+17,0x50243e37,2);p.line(mx+1,my+2,mx+12,my+12,0x50243e37,2);p.line(mx,my,mx,my+15,white,2.4f);p.line(mx,my,mx+10,my+10,white,2.4f);p.line(mx,my,mx,my+12,teal,1);p.line(mx,my,mx+8,my+8,teal,1);}
    auto transform=[&](UiRect& r){r.x=ox+r.x*scale;r.y=oy+r.y*scale;r.w*=scale;r.h*=scale;};
    for(auto& paint:scene.paint){transform(paint.rect);paint.scale*=scale;paint.radius*=scale;}
    for(auto& h:hits)transform(h.rect);for(auto& r:ranges)transform(r.rect);
    if(scene.preview){transform(*scene.preview);previewArea=scene.preview;}
    return scene;
}
}
