#pragma once
#include "Localization.h"
#include "ModelPack.h"

namespace ysm {
inline std::string textField(Json const& object, std::string_view key, std::string fallback={}) {
    if(!object.is_object()) return fallback;
    auto it=object.find(key);
    return it!=object.end() && it->is_string() ? it->get<std::string>() : fallback;
}
inline std::string localizedModelText(Json const& language, std::string_view locale, std::string const& key, std::string fallback={}) {
    auto value=textField(language,key,std::move(fallback));
    if(key.starts_with("metadata.authors.") && key.ends_with(".name")) return value;
    auto translated=displayLabel(locale,value);
    if(!chineseLocale(locale) && !locale.starts_with("ja") && !locale.starts_with("ko") && hasHan(translated) && key.starts_with("properties.extra_animation_buttons.") && key.ends_with(".name")) {
        auto id=key.substr(sizeof("properties.extra_animation_buttons.")-1);
        id.resize(id.size()-5);
        auto readable=readableAnimationId(id);
        if(!hasHan(readable) && !readable.empty() && readable.find_first_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ")!=std::string::npos) return readable;
    }
    return translated;
}
inline std::string declaredEmoteName(Json const& descriptor, std::string const& id) {
    auto properties=descriptor.find("properties");
    if(properties==descriptor.end() || !properties->is_object()) return {};
    auto lookup=[&](Json const& list) {return textField(list,id);};
    auto list=properties->find("extra_animation");
    auto name=list!=properties->end()?lookup(*list):std::string{};
    auto groups=properties->find("extra_animation_classify");
    if(name.empty() && groups!=properties->end() && groups->is_array()) for(auto const& group:*groups) {
        if(!group.is_object()) continue;
        auto entries=group.find("extra_animation");
        if(entries!=group.end()) name=lookup(*entries);
        if(!name.empty()) break;
    }
    auto buttons=properties->find("extra_animation_buttons");
    if(name.starts_with('#') && buttons!=properties->end() && buttons->is_array()) for(auto const& button:*buttons) {
        if(textField(button,"id")==name.substr(1)) return textField(button,"name",name.substr(1));
    }
    return name;
}
inline std::string localizedEmote(Json const& descriptor, Json const& language, std::string_view locale, std::string const& id, std::string fallback={}) {
    if(fallback.empty()) fallback=declaredEmoteName(descriptor,id);
    auto value=textField(language,"properties.extra_animation."+id,std::move(fallback));
    return displayEmote(locale,std::move(value),id);
}
}
