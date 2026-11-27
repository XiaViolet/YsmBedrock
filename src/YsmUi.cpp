#include "WheelGeometry.h"
#include "YsmUi.h"
#include "ModelLocalization.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace ysm {
namespace {
std::string str(Json const& j, char const* key, std::string fallback = {}) {
    auto p = j.find(key); return p != j.end() && p->is_string() ? p->get<std::string>() : fallback;
}
std::string lower(std::string s) { for (auto& c : s) if (static_cast<unsigned char>(c)<128) c=static_cast<char>(std::tolower(c)); return s; }
std::string number(double n) { std::ostringstream s; s << std::setprecision(4) << n; return s.str(); }
Json const& properties(Json const& descriptor) {
    static const Json empty=Json::object();
    auto it=descriptor.find("properties");
    return it!=descriptor.end() && it->is_object() ? *it : empty;
}
Json const& buttons(Json const& descriptor) {
    static const Json empty=Json::array();
    auto const& info=properties(descriptor);
    auto it=info.find("extra_animation_buttons");
    return it!=info.end() && it->is_array() ? *it : empty;
}
void eraseUtf8(std::string& s) { if(s.empty())return; auto i=s.size()-1; while(i && (static_cast<unsigned char>(s[i])&0xc0)==0x80)--i; s.erase(i); }
std::string fit(std::string const& s, std::size_t columns) {
    std::size_t i=0,n=0; for(;i<s.size();++i) if((static_cast<unsigned char>(s[i])&0xc0)!=0x80) {
        auto width=static_cast<unsigned char>(s[i])>=128?2u:1u;
        if(n+width>columns)break; n+=width;
    }
    return i<s.size()?s.substr(0,i)+"…":s;
}
}
ModelUiBindings::ModelUiBindings(Json const& descriptor) {
    auto const& definitions=buttons(descriptor);
    if(definitions.size()>256) throw std::runtime_error("Too many model UI buttons");
    for(auto const& button:definitions) {
        auto id=str(button,"id");
        if(id.empty() || id.size()>128) throw std::runtime_error("Invalid model UI button id");
        auto list=button.value("config_forms",Json::array());
        if(!list.is_array() || list.size()>16384) throw std::runtime_error("Invalid model UI forms");
        std::size_t index=0;
        for(auto const& form:list) {
            Form binding; binding.type=str(form,"type"); auto value=str(form,"value");
            auto labels=form.value("labels",Json::object());
            bool actionOnly=value.empty()&&binding.type=="radio"&&labels.is_object()&&!labels.empty();
            if((binding.type!="checkbox"&&binding.type!="radio"&&binding.type!="range") || (value.empty()&&!actionOnly))
                throw std::runtime_error("Invalid model UI form type or expression");
            binding.read=MolangProgram(actionOnly?"0":value);
            binding.write=MolangProgram(actionOnly?"return;":"t.value=args;"+value+"=t.value;");
            binding.min=form.value("min",0.0); binding.max=form.value("max",100.0); binding.step=form.value("step",1.0);
            if(!std::isfinite(binding.min)||!std::isfinite(binding.max)||!std::isfinite(binding.step)
                ||binding.max<binding.min||binding.step<0) throw std::runtime_error("Invalid model UI range");
            if(!labels.is_object() || labels.size()>1024) throw std::runtime_error("Invalid model UI labels");
            for(auto const& [name,program]:labels.items()) {
                if(!program.is_string())throw std::runtime_error("Invalid model UI label program");
                binding.labels.emplace_back(program.get<std::string>());
            }
            formVariables.merge(binding.write.assignedModelVariables());
            for (auto const& label : binding.labels) formVariables.merge(label.assignedModelVariables());
            if(!forms.emplace(id+"/"+std::to_string(index++),std::move(binding)).second)
                throw std::runtime_error("Duplicate model UI route");
        }
    }
}
std::map<std::string,float> ModelUiBindings::snapshotScalars(MolangContext const& context) const {
    std::map<std::string,float> result;
    for (auto const& name : formVariables) {
        if (name.size() > 96) continue;
        auto it = context.variables.find(name);
        if (it == context.variables.end()) continue;
        auto const& value = it->second;
        if (!value.present() || value.isArray() || value.isRecord() || value.isString()) continue;
        auto number = static_cast<float>(value.number());
        if (std::isfinite(number)) result.emplace(name, number);
        if (result.size() == 128) break;
    }
    return result;
}
bool ModelUiBindings::applyScalars(MolangContext& context, std::map<std::string,float> const& previous,
    std::map<std::string,float> const& next) const {
    bool changed = false;
    for (auto const& [name, value] : previous)
        if (!next.contains(name) && formVariables.contains(name)) changed |= context.variables.erase(name) != 0;
    for (auto const& [name, value] : next) {
        if (!formVariables.contains(name) || !std::isfinite(value)) continue;
        auto current = context.variables.find(name);
        MolangValue restored(static_cast<double>(value));
        if (current == context.variables.end() || !(current->second == restored)) {
            context.writeVariable(name, std::move(restored));
            changed = true;
        }
    }
    return changed;
}
Json ModelUiBindings::observe(MolangContext const& context) const {
    Json values=Json::object();
    for(auto const& [route,form]:forms) { auto observer=context.forObservation(); auto value=form.read.evaluate(observer).number(); values[route]=std::isfinite(value)?value:0; }
    return values;
}
bool ModelUiBindings::act(MolangContext& context,std::string const& route,double value,int label) const {
    auto found=forms.find(route); if(found==forms.end() || !std::isfinite(value) || context.observation)return false;
    auto const& form=found->second;
    if(label>=0 && (form.type!="radio" || static_cast<std::size_t>(label)>=form.labels.size()))return false;
     
    MolangDeferredScope deferred(context,this); MolangActionScope actions(context,true);
    auto previous=context.localVariables.find("temp.value");
    std::optional<MolangValue> saved=previous==context.localVariables.end()?std::nullopt:std::optional(previous->second);
    if(label>=0) form.labels[label].invoke(context);
    else {
        if(form.type=="checkbox")value=value!=0?1:0;
        else if(form.type=="range") {
             
            value=std::clamp(value,form.min,form.max);
            if(form.step>0)value=std::clamp(form.min+std::round((value-form.min)/form.step)*form.step,form.min,form.max);
        }
        form.write.invoke(context,MolangValue(static_cast<double>(static_cast<float>(value))));
    }
    if(saved)context.localVariables["temp.value"]=*saved; else context.localVariables.erase("temp.value");
    return true;
}
std::set<std::string> ModelUiBindings::roamingFields() const {
    std::set<std::string> result;
    for(auto const& [route,form]:forms) {
        auto add=[&](MolangProgram const& p){auto fields=p.roamingFields();result.insert(fields.begin(),fields.end());};
        add(form.read);add(form.write);for(auto const& p:form.labels)add(p);
    } return result;
}
Json YsmUi::defaultSettings() { return {{"sound_volume",100},{"disable_self_model",false},{"disable_self_hands",false},
    {"disable_projectile_model",false},{"disable_vehicle_model",false},{"print_animation_roulette_msg",true},
    {"movement_lock",false},{"disable_other_models",false},{"show_hud_avatar",false},{"arke_extra_emissive_layer",false},{"native_menu_preview",true},{"relative_menu_pointer",false},
    {"menu_theme",0},{"preview_quality",1},{"preview_fps",60},{"hud_fps",60},{"hud_x",12},{"hud_y",82},{"hud_size",40},{"hud_yaw",50},{"favorites",Json::array()},
    {"hidden_players",Json::array()}}; }
void YsmUi::update(UiSnapshot value) {
    if (value == data) return;
    if (!sceneDirty && page!=Page::Players && page!=Page::Forms && page!=Page::Roulette &&
        value.generation==data.generation && value.source==data.source && value.texture==data.texture &&
        value.message==data.message && value.locale==data.locale && value.descriptor==data.descriptor &&
        value.language==data.language && value.catalog==data.catalog && value.settings==data.settings &&
        value.textures==data.textures && value.clips==data.clips && value.browsing==data.browsing &&
        value.browseLoading==data.browseLoading && value.enabled==data.enabled && value.loading==data.loading &&
        value.debugOverlay==data.debugOverlay) {
        data.players=std::move(value.players);data.room=std::move(value.room);data.values=std::move(value.values);return;
    }
    sceneDirty = true;
    if(value.generation!=data.generation || value.source!=data.source) {
        categories={{"",0}};formButton.clear();formPage=texturePage=previewPage=infoPage=0;previewClip.clear();
        previewCamera={}; previewArea.reset(); previewDrag.reset();
        if(page==Page::Forms)page=Page::Roulette;
         
         
        commands.clear(); hits.clear(); ranges.clear(); dragging.reset(); focusId.clear();
    }
    if (data.browsing != value.browsing || data.browseLoading != value.browseLoading) {
        previewClip.clear(); previewPage = 0; previewCamera = {}; previewDrag.reset(); hits.clear();
    }
    data=std::move(value);
}
void YsmUi::open(bool wheel) { sceneDirty=true; page=wheel?Page::Roulette:Page::Models;wheelHovered=-1;previewClip.clear();previewCamera={};searchFocused=false;hits.clear(); ranges.clear(); dragging.reset(); previewArea.reset(); previewDrag.reset(); focusId.clear(); }
void YsmUi::close() {sceneDirty=true;page=Page::Closed;searchFocused=false;hits.clear(); ranges.clear(); dragging.reset(); previewArea.reset(); previewDrag.reset(); focusId.clear();}
bool YsmUi::blocksMovement() const { return isOpen() && !isWheel(); }
std::string YsmUi::local(std::string const& key,std::string fallback) const {return localizedModelText(data.language,data.locale,key,std::move(fallback));}
void YsmUi::command(std::string kind,std::string key,double value,int label) {
    if(!commands.empty() && label<0 && (kind=="form"||kind=="setting") && commands.back().kind==kind && commands.back().key==key && commands.back().label<0)
        commands.back().value=value;
    else if(commands.size()<256)commands.push_back({std::move(kind),std::move(key),value,label,data.generation});
}
std::vector<YsmUi::WheelEntry> YsmUi::wheelEntries() const {
    auto const& p=properties(data.descriptor);auto list=p.value("extra_animation",Json::object());
    if(!categories.back().first.empty()) for(auto const& c:p.value("extra_animation_classify",Json::array()))
        if(str(c,"id")==categories.back().first){list=c.value("extra_animation",Json::object());break;}
    std::vector<WheelEntry> result;
    if(list.is_object())for(auto const& [key,label]:list.items())if(label.is_string()) {
        std::string text=label.get<std::string>();
        std::string form;
        if(text.starts_with('#'))for(auto const& b:buttons(data.descriptor))if(str(b,"id")==text.substr(1)){form=str(b,"id");text=str(b,"name",key);break;}
        auto translationKey = "properties.extra_animation." + key;
        result.push_back({key,localizedEmote(data.descriptor,data.language,data.locale,key,std::move(text)),local(translationKey+".desc",{}),form});
    }return result;
}
void YsmUi::chooseWheel(std::size_t slot) {
    auto entries=wheelEntries();auto index=categories.back().second*8+slot;if(index>=entries.size())return;
    auto const& e=entries[index];
    if(e.key=="#return"){page=Page::Roulette;formButton.clear();back();return;}
    if(e.key.starts_with('#')) {
        auto id=e.key.substr(1);for(auto const& c:properties(data.descriptor).value("extra_animation_classify",Json::array()))
            if(str(c,"id")==id){categories.emplace_back(id,0);page=Page::Roulette;formButton.clear();hits.clear();focusId.clear();return;}
        return;
    }
    if (!e.form.empty() && std::find(data.clips.begin(),data.clips.end(),e.key)==data.clips.end()) {
        activate("form:"+e.form); return;
    }
    command("extra",e.key);close();
}
void YsmUi::back() {
    searchFocused=false;hits.clear(); ranges.clear(); dragging.reset(); focusId.clear();
    previewArea.reset(); previewDrag.reset();
    if(page==Page::Forms){page=Page::Roulette;return;}
    if(page==Page::Appearance){page=Page::Settings;return;}
    if(page==Page::Roulette && categories.size()>1){categories.pop_back();return;}
    if(page!=Page::Models && page!=Page::Roulette){page=Page::Models;return;}
    close();
}
void YsmUi::activate(std::string const& id) {
    dragging.reset();
    previewArea.reset(); previewDrag.reset();
    searchFocused = id == "search";
    if (searchFocused) return;
    if(id=="close"){close();return;}if(id=="back"){back();return;}
    if(id=="models"){page=Page::Models;return;}if(id=="textures"){page=Page::Textures;return;}
    if(id=="info"){page=Page::Info;return;}if(id=="settings"){page=Page::Settings;return;}
    if(id=="appearance"){page=Page::Appearance;return;}
    if(id.starts_with("theme:") || id.starts_with("quality:")) {
        bool theme=id.starts_with("theme:"); auto key=theme?"menu_theme":"preview_quality";
        auto value=std::clamp(std::stoi(id.substr(theme?6:8)),0,theme?3:2);
        data.settings[key]=value;command("setting",key,value);return;
    }
    if(id=="players"){page=Page::Players;return;}
    if(id=="preview"){page=Page::Preview;return;}if(id=="roulette"){page=Page::Roulette;return;}
    if(id=="favorite_filter"){favoritesOnly=!favoritesOnly;listPage=0;return;}
    if(id=="reload"){command("reload",data.source);return;}
    if(id=="toggle"){command("toggle");return;}
    if(id=="stop"){command("stop");return;}
    if(id=="automatic"){previewClip.clear();return;}
    if(id=="reset_preview"){previewCamera={};return;}
    if(id=="rotate_left"){previewCamera.yaw-=15;return;}if(id=="rotate_right"){previewCamera.yaw+=15;return;}
    if(id=="zoom_in"){previewCamera.zoom=std::min(2.f,previewCamera.zoom+.1f);return;}if(id=="zoom_out"){previewCamera.zoom=std::max(.5f,previewCamera.zoom-.1f);return;}
    if (id=="wheel_prev" || id=="wheel_next") {
        auto& index=categories.back().second;
        if (id=="wheel_next") ++index; else if (index) --index;
        page=Page::Roulette;formButton.clear();focusId.clear();hits.clear();
        return;
    }
    if(id=="prev"||id=="next") {
        if(page==Page::Forms)radioPage=0;
        auto* p=page==Page::Roulette?&categories.back().second:page==Page::Textures?&texturePage:page==Page::Forms?&formPage:page==Page::Info?&infoPage:page==Page::Preview?&previewPage:page==Page::Players?&playerPage:&listPage;
        if(id=="next")++*p;else if(*p)--*p;return;
    }
    if(id.starts_with("select:")){command("browse",id.substr(7));return;}
    if(id=="import"){page=Page::Import;return;}
    if(id=="open_models_folder"){command(id);return;}
    if(id=="import_file" || id=="import_folder"){page=Page::Models;command(id);return;}
    if(id=="wear" && data.browsing && data.browseLoading.empty()){command("wear",data.browsing->source);return;}
    if(id.starts_with("texture:")){command("texture",id.substr(8));return;}
    if(id.starts_with("favorite:")){command("favorite",id.substr(9));return;}
    if(id=="room_observers"){observersOpen=!observersOpen;playerPage=0;return;}
    if(id.starts_with("room:")){command("room",id.substr(5));return;}
    if(id.starts_with("peer:")){command("peer_visibility",id.substr(5));return;}
    if(id.starts_with("clip:")){previewClip=id.substr(5);return;}
    if(id.starts_with("form:")){formButton=id.substr(5);formPage=radioPage=0;page=Page::Forms;focusId.clear();hits.clear();ranges.clear();return;}
    if(id.starts_with("wheel:")){chooseWheel(std::stoul(id.substr(6)));return;}
    if(id.starts_with("setting:")) {auto key=id.substr(8);auto value=!data.settings.value(key,false);data.settings[key]=value;command("setting",key,value);return;}
    if(id=="radio_prev"||id=="radio_next"){if(id=="radio_next")++radioPage;else if(radioPage)--radioPage;return;}
    if(id=="volume_down"||id=="volume_up"){command("setting","sound_volume",std::clamp(data.settings.value("sound_volume",100)+(id=="volume_up"?10:-10),0,100));return;}
    if(id.starts_with("link:")){command("link",id.substr(5));return;}
    if(id.starts_with("value:")) {
        auto spec=id.substr(6);auto sep=spec.rfind(':');auto route=spec.substr(0,sep);auto direction=spec.substr(sep+1);
        auto const& defs=buttons(data.descriptor);auto slash=route.rfind('/');
        for(auto const& b:defs)if(str(b,"id")==route.substr(0,slash)) {
            auto const& f=b.at("config_forms").at(std::stoul(route.substr(slash+1)));auto value=data.values.value(route,0.0);
            if(direction=="toggle")command("form",route,value==0?1:0);
            else if(direction.starts_with("label"))command("form",route,0,std::stoi(direction.substr(5)));
            else {auto step=f.value("step",1.0);if(step==0)step=(f.value("max",100.0)-f.value("min",0.0))/100;command("form",route,value+(direction=="up"?1:-1)*step);}
        }return;
    }
}
bool YsmUi::key(int key,bool down,bool control,std::string text,bool shift) {
    sceneDirty = true;
    if(!isOpen())return false;
    if(!down)return false;  
    if(key==27){back();return true;}
    if(searchFocused && key==9)searchFocused=false;
    if(searchFocused) {
        if(key==13){searchFocused=false;return true;}
        if(key==8){eraseUtf8(search);listPage=0;return true;}
        if(control&&key==65){search.clear();listPage=0;return true;}
        if(!text.empty()&&search.size()+text.size()<=256){search+=text;listPage=0;}
        return true;
    }
    if(key==90&&isWheel()){close();return true;}
    if(key==9 || key==38 || key==40) {
        if(hits.empty())return true;
        auto p=std::find_if(hits.begin(),hits.end(),[&](auto const& h){return h.id==focusId;});
        auto delta=key==38||(key==9&&shift)?-1:1;
        auto n=p==hits.end()?(delta<0?0:-1):static_cast<int>(p-hits.begin());
        n=(n+delta+static_cast<int>(hits.size()))%static_cast<int>(hits.size());
        focusId=hits[n].id;searchFocused=false;return true;
    }
    if((key==37||key==39)&&!focusId.empty())for(auto const& range:ranges)if(range.id==focusId) {
        auto value=range.setting?data.settings.value(range.route,0.0):data.values.value(range.route,0.0);
        setRange(range,value+(key==39?1:-1)*(range.step>0?range.step:(range.max-range.min)/100));return true;
    }
    if((key==13||key==32)&&!focusId.empty()) {activate(focusId);return true;}
    if(key>=49&&key<=56&&isWheel()){chooseWheel(key-49);return true;}
    if(key==33){activate("prev");return true;}if(key==34){activate("next");return true;}
    if(key==13&&isWheel()&&wheelHovered>=0){chooseWheel(wheelHovered);return true;}
    if(isWheel()&&!blocksMovement()&&(key==87||key==65||key==83||key==68||key==32||key==16||key==17))return false;
    return true;
}
void YsmUi::cancelPointer() {
    sceneDirty = true;
    mouseX = mouseY = -1;
    wheelHovered = -1;
    previewDrag.reset();
    dragging.reset();
}
bool YsmUi::mouse(int button,int value,float x,float y) {
    if(!isOpen())return false;
    if (button == 0 && x == mouseX && y == mouseY && !dragging && !previewDrag) return true;
    if (button != 0 || dragging || previewDrag) sceneDirty = true;
    else {
        if (isWheel() && wheelSlotAt(wheelBounds,x,y)!=wheelHovered) sceneDirty=true;
        if (!sceneDirty) for(auto const& hit:hits) if(hit.rect.contains(x,y)!=hit.rect.contains(mouseX,mouseY)){sceneDirty=true;break;}
    }
    mouseX=x;mouseY=y;
    if (previewDrag && (button == 0 || button == previewDrag->button)) {
        auto& drag = *previewDrag;
        auto dx = x - drag.x, dy = y - drag.y;
        if (drag.button == 1) {
            previewCamera.yaw = std::remainder(previewCamera.yaw + 1.5f * dx, 360.f);
            previewCamera.pitch = std::clamp(previewCamera.pitch - dy, -90.f, 90.f);
        } else {
            previewCamera.panX = std::clamp(previewCamera.panX + dx / drag.side, -1.f, 1.f);
            previewCamera.panY = std::clamp(previewCamera.panY + dy / drag.side, -1.f, 1.f);
        }
        drag.x=x; drag.y=y;
        if (button != 0 && !value) previewDrag.reset();
        return true;
    }
    if(dragging) {
        if(button==0||button==1) {
            auto range=*dragging;
            setRange(range,range.min+std::clamp((x-range.rect.x)/range.rect.w,0.f,1.f)*(range.max-range.min));
            if(button==1&&!value)dragging.reset();
            return true;
        }
    }
    if(isWheel()) wheelHovered=wheelSlotAt(wheelBounds,x,y);
    if (previewArea && previewArea->contains(x,y) && value != 0) {
        if (button == 4) {
            previewCamera.zoom = std::clamp(previewCamera.zoom * (value > 0 ? 1.07f : .93f), .5f, 2.f);
            return true;
        }
        if (button == 1 || button == 2) {
            previewDrag = PreviewDrag{button, x, y, std::min(previewArea->w, previewArea->h)};
            searchFocused = false;
            return true;
        }
    }
    if(button==4&&value){
        if(isWheel() && wheelBounds.contains(x,y)) activate(value>0?"wheel_prev":"wheel_next");
        else activate(value>0?"prev":"next");
        return true;
    }
    if(button==1&&value!=0) {
        for(auto const& range:ranges)if(range.rect.contains(x,y)&&range.max>range.min){dragging=range;focusId=range.id;setRange(range,range.min+std::clamp((x-range.rect.x)/range.rect.w,0.f,1.f)*(range.max-range.min));return true;}
        for(auto i=hits.rbegin();i!=hits.rend();++i)if(i->rect.contains(x,y)){auto id=i->id;focusId=id;activate(id);return true;}
        if(isWheel()&&wheelHovered>=0){chooseWheel(wheelHovered);return true;}
        searchFocused=false;
    }
    return true;
}
void YsmUi::setRange(Range const& range,double value) {
    value=std::clamp(value,range.min,range.max);
    if(range.step>0)value=std::clamp(range.min+std::round((value-range.min)/range.step)*range.step,range.min,range.max);
    auto& target=range.setting?data.settings:data.values;
    if(target.contains(range.route) && target.value(range.route,0.)==value)return;
    target[range.route]=value;
    command(range.setting?"setting":"form",range.route,value);
}
std::vector<UiCommand> YsmUi::takeCommands() {std::vector<UiCommand> out(commands.begin(),commands.end());commands.clear();return out;}
Json YsmUi::report() const {return {{"open",isOpen()},{"page",static_cast<int>(page)},{"source",data.source},{"generation",data.generation},{"category",categories.back().first},{"wheel_page",categories.back().second},{"search",search},{"form_button",formButton},{"blocks_movement",blocksMovement()},
    {"preview_camera",{previewCamera.yaw,previewCamera.pitch,previewCamera.zoom,previewCamera.panX,previewCamera.panY}}};}

}
