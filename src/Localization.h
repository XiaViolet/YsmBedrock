#pragma once
#include <atomic>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace ysm {
inline bool chineseLocale(std::string_view locale) {
    return locale.size() >= 2 && (locale[0] == 'z' || locale[0] == 'Z') &&
        (locale[1] == 'h' || locale[1] == 'H') && (locale.size() == 2 || locale[2] == '_' || locale[2] == '-');
}
inline std::atomic_bool gameChinese{false};
inline void setGameLanguage(std::string_view locale) { gameChinese.store(chineseLocale(locale), std::memory_order_relaxed); }
inline bool chineseGameLanguage() { return gameChinese.load(std::memory_order_relaxed); }
template<class Char> inline Char const* uiText(std::string_view locale, Char const* zh, Char const* en) {
    return chineseLocale(locale) ? zh : en;
}
template<class Char> inline Char const* gameText(Char const* zh, Char const* en) { return chineseGameLanguage() ? zh : en; }
inline bool hasHan(std::string_view text) {
    for (std::size_t i=0;i+2<text.size();++i) {
        auto a=static_cast<unsigned char>(text[i]), b=static_cast<unsigned char>(text[i+1]), c=static_cast<unsigned char>(text[i+2]);
        if ((a&0xf0)!=0xe0 || (b&0xc0)!=0x80 || (c&0xc0)!=0x80) continue;
        auto cp=((a&15)<<12)|((b&63)<<6)|(c&63);
        if ((cp>=0x3400 && cp<=0x9fff)||(cp>=0xf900&&cp<=0xfaff)) return true;
    }
    return false;
}
inline std::string englishModelLabel(std::string_view label) {
    if (!hasHan(label)) return std::string(label);
    std::string clean;
    for(std::size_t i=0;i<label.size();) {
        if(label.substr(i).starts_with("§") && i+2<label.size() && static_cast<unsigned char>(label[i+2])<128){i+=3;continue;}
        clean+=label[i++];
    }
    label=clean;
    static constexpr std::pair<std::string_view,std::string_view> words[] = {
        {"切换表情","Change Expression"},{"切换发型","Change Hairstyle"},{"切换颜色","Change Color"},{"点击切换","Click to switch"},
        {"点击播放","Click to play"},{"点击开启","Click to enable"},{"点击关闭","Click to disable"},{"是否显示","Show"},
        {"默认服装","Default Outfit"},{"原版皮肤","Vanilla Skin"},{"随机动作","Random Emote"},{"随机表情","Random Expression"},
        {"头发颜色","Hair Color"},{"眼睛颜色","Eye Color"},{"头发样式","Hairstyle"},{"衣服颜色","Outfit Color"},
        {"特殊表情","Special Expressions"},{"普通表情","Normal Expressions"},{"脸部表情","Facial Expressions"},{"表情管理","Expressions"},
        {"脸部","Face"},{"眉毛","Eyebrows"},{"睫毛","Eyelashes"},{"眼泪","Tears"},{"汗水","Sweat"},{"腮红","Blush"},
        {"瞳孔","Pupils"},{"刘海","Bangs"},{"双马尾","Twin Tails"},{"单马尾","Ponytail"},{"马尾","Ponytail"},
        {"蝴蝶结","Bow"},{"发卡","Hair Clip"},{"发夹","Hair Clip"},{"耳机","Headphones"},{"耳环","Earrings"},
        {"水手服","Sailor Uniform"},{"校服","School Uniform"},{"女仆装","Maid Outfit"},{"睡衣","Pajamas"},{"泳衣","Swimsuit"},
        {"夏装","Summer Outfit"},{"冬装","Winter Outfit"},{"礼服","Formal Outfit"},{"外套","Jacket"},{"上衣","Top"},
        {"裤子","Pants"},{"短裤","Shorts"},{"丝袜","Stockings"},{"长袜","Long Socks"},{"靴子","Boots"},{"领带","Tie"},
        {"魔法棒","Magic Wand"},{"法杖","Staff"},{"雨伞","Umbrella"},{"扇子","Fan"},{"麦克风","Microphone"},{"吉他","Guitar"},
        {"跑酷","Parkour"},{"鼓励","Encourage"},{"撒娇","Act Cute"},{"卖萌","Act Cute"},{"叹气","Sigh"},{"鼓脸","Pout"},
        {"托腮","Rest Chin"},{"摇尾巴","Wag Tail"},{"摆手","Wave"},{"牵手","Hold Hands"},{"扶额","Facepalm"},
        {"双手叉腰","Hands on Hips"},{"伸手","Reach Out"},{"举高高","Lift Up"},{"仰卧","Lie on Back"},{"侧卧","Lie on Side"},
        {"盘腿坐","Sit Cross-Legged"},{"盘腿","Cross-Legged"},{"踢腿","Kick"},{"拉伸","Stretch"},{"原地转圈","Spin in Place"},
        {"连续播放","Loop"},{"循环播放","Loop"},{"播放一次","Play Once"},{"暂停","Pause"},{"继续","Resume"},{"播放","Play"},
        {"互动","Interaction"},{"互动动作","Interactions"},{"快捷动作","Quick Emotes"},{"特殊","Special"},{"休息","Rest"},
        {"动作速度","Animation Speed"},{"模型大小","Model Size"},{"模型高度","Model Height"},{"音效音量","Sound Volume"},
        {"强度","Intensity"},{"亮度","Brightness"},{"透明","Transparent"},{"不透明","Opaque"},{"隐藏全部","Hide All"},
        {"显示全部","Show All"},{"不显示","Hide"},{"无","None"},{"有","Yes"},{"是","Yes"},{"否","No"},
        {"开启后","When enabled"},{"关闭后","When disabled"},{"长按","Hold"},{"按下","Press"},{"点击","Click"},
        {"哔哩哔哩","Bilibili"},{"官方网站","Official Website"},{"下载地址","Download"},{"交流群","Community Group"},
        {"連結","Link"},{"链接","Link"},{"视频","Video"},{"展示","Showcase"},{"教程","Tutorial"},
        {"頭髮","Hair"},{"衣裝","Outfit"},{"換裝","Change Outfit"},{"換髮型","Change Hairstyle"},{"臉紅","Blush"},
        {"跳躍","Jump"},{"跳舞","Dance"},{"坐下","Sit"},{"躺下","Lie Down"},{"預設","Default"},{"關閉","Off"},
        {"返回上一级","Back"},{"返回","Back"},{"动作转盘","Emote Wheel"},{"额外动作","Emotes"},{"特殊动作","Special Emotes"},
        {"动作设置","Emote Settings"},{"外观设置","Appearance Settings"},{"模型设置","Model Settings"},{"装饰设置","Accessory Settings"},
        {"表情设置","Expression Settings"},{"服装切换","Change Outfit"},{"切换服装","Change Outfit"},{"切换皮肤","Change Skin"},
        {"默认动作","Default Animation"},{"默认表情","Default Expression"},{"恢复默认","Reset to Default"},{"重置","Reset"},
        {"停止动作","Stop Emote"},{"停止","Stop"},{"取消","Cancel"},{"确认","Confirm"},{"设置","Settings"},
        {"打招呼","Greet"},{"招手","Wave"},{"挥手","Wave"},{"挥挥手","Wave"},{"你好","Hello"},{"再见","Goodbye"},
        {"跳舞","Dance"},{"舞蹈","Dance"},{"跳一支舞","Dance"},{"鞠躬","Bow"},{"敬礼","Salute"},{"鼓掌","Clap"},{"拍手","Clap"},
        {"比心","Heart Gesture"},{"爱心","Heart"},{"飞吻","Blow a Kiss"},{"亲吻","Kiss"},{"拥抱","Hug"},{"抱抱","Hug"},
        {"坐下","Sit"},{"坐姿","Sitting"},{"站立","Stand"},{"起立","Stand Up"},{"躺下","Lie Down"},{"躺平","Lie Down"},
        {"睡觉","Sleep"},{"睡眠","Sleep"},{"趴下","Lie on Stomach"},{"跪下","Kneel"},{"蹲下","Crouch"},{"潜行","Sneak"},
        {"走路","Walk"},{"行走","Walk"},{"跑步","Run"},{"奔跑","Run"},{"跳跃","Jump"},{"游泳","Swim"},{"飞行","Fly"},
        {"待机","Idle"},{"闲置","Idle"},{"攻击","Attack"},{"防御","Defend"},{"格挡","Block"},{"举手","Raise Hand"},
        {"点头","Nod"},{"摇头","Shake Head"},{"转圈","Spin"},{"旋转","Spin"},{"伸懒腰","Stretch"},{"打哈欠","Yawn"},
        {"拍照","Take Photo"},{"自拍","Selfie"},{"吃东西","Eat"},{"吃饭","Eat"},{"喝水","Drink Water"},{"喝茶","Drink Tea"},
        {"高兴","Happy"},{"开心","Happy"},{"微笑","Smile"},{"笑脸","Smile"},{"大笑","Laugh"},{"笑","Laugh"},
        {"哭泣","Cry"},{"哭","Cry"},{"悲伤","Sad"},{"伤心","Sad"},{"生气","Angry"},{"愤怒","Angry"},
        {"害羞","Shy"},{"脸红","Blush"},{"惊讶","Surprised"},{"疑惑","Confused"},{"无语","Speechless"},{"思考","Think"},
        {"闭眼","Close Eyes"},{"眨眼","Blink"},{"睁眼","Open Eyes"},{"张嘴","Open Mouth"},{"吐舌","Stick Out Tongue"},
        {"装饰","Accessories"},{"饰品","Accessories"},{"配饰","Accessories"},{"外观","Appearance"},{"表情","Expressions"},
        {"服装","Outfit"},{"衣服","Clothes"},{"皮肤","Skin"},{"贴图","Texture"},{"纹理","Texture"},{"帽子","Hat"},
        {"眼镜","Glasses"},{"头发","Hair"},{"发型","Hairstyle"},{"耳朵","Ears"},{"尾巴","Tail"},{"翅膀","Wings"},
        {"背包","Backpack"},{"围巾","Scarf"},{"口罩","Mask"},{"面具","Mask"},{"披风","Cape"},{"斗篷","Cloak"},
        {"武器","Weapon"},{"道具","Prop"},{"光环","Halo"},{"特效","Effects"},{"粒子","Particles"},{"声音","Sound"},
        {"音量","Volume"},{"速度","Speed"},{"大小","Size"},{"高度","Height"},{"颜色","Color"},{"眼睛","Eyes"},
        {"左手","Left Hand"},{"右手","Right Hand"},{"双手","Both Hands"},{"头部","Head"},{"身体","Body"},{"腿部","Legs"},
        {"开启","On"},{"关闭","Off"},{"显示","Show"},{"隐藏","Hide"},{"启用","Enable"},{"禁用","Disable"},
        {"默认","Default"},{"普通","Normal"},{"自定义","Custom"},{"自动","Automatic"},{"随机","Random"},{"全部","All"},
        {"其他","Other"},{"更多","More"},{"动作","Emotes"},{"动画","Animation"},{"模型","Model"},
        {"红色","Red"},{"蓝色","Blue"},{"绿色","Green"},{"黄色","Yellow"},{"白色","White"},{"黑色","Black"},
        {"粉色","Pink"},{"紫色","Purple"},{"橙色","Orange"},{"灰色","Gray"},{"棕色","Brown"},
        {"挥舞","Wave"},{"握手","Handshake"},{"胜利","Victory"},{"欢呼","Cheer"},{"庆祝","Celebrate"},
        {"摇摆","Sway"},{"摇晃","Shake"},{"扭腰","Hip Sway"},{"摆姿势","Pose"},{"姿势","Pose"},{"耍帅","Strike a Pose"},
        {"倒地","Fall Down"},{"摔倒","Fall Down"},{"翻滚","Roll"},{"翻跟头","Somersault"},{"后空翻","Backflip"},
        {"摸头","Pat Head"},{"揉眼睛","Rub Eyes"},{"叉腰","Hands on Hips"},{"抱胸","Cross Arms"},{"抱头","Hold Head"},
        {"拒绝","Refuse"},{"同意","Agree"},{"点赞","Thumbs Up"},{"竖大拇指","Thumbs Up"},{"剪刀手","Peace Sign"},
        {"卖萌","Act Cute"},{"可爱","Cute"},{"晕倒","Faint"},{"眩晕","Dizzy"},{"恐惧","Scared"},{"惊恐","Terrified"},
        {"流泪","Tears"},{"眨眨眼","Blink"},{"单眼眨眼","Wink"},{"嘲笑","Mock"},{"坏笑","Smirk"},{"得意","Smug"},
        {"脱下","Remove"},{"穿上","Wear"},{"切换","Switch"},{"开关","Toggle"},{"选择","Select"},{"调整","Adjust"},
        {"头饰","Headwear"},{"发饰","Hair Accessory"},{"裙子","Skirt"},{"袜子","Socks"},{"鞋子","Shoes"},
        {"蝴蝶结","Bow"},{"手套","Gloves"},{"猫耳","Cat Ears"},{"兽耳","Animal Ears"},{"长发","Long Hair"},{"短发","Short Hair"},
        {"眼罩","Eye Patch"},{"项链","Necklace"},{"发光","Glow"},{"透明度","Opacity"},{"音效","Sound Effects"},
        {"正面","Front"},{"背面","Back"},{"左侧","Left"},{"右侧","Right"},{"第一人称","First Person"},{"第三人称","Third Person"},
        {"简体中文","Simplified Chinese"},{"繁體中文","Traditional Chinese"},{"動作","Emotes"},{"動畫","Animation"},
        {"揮手","Wave"},{"擁抱","Hug"},{"睡覺","Sleep"},{"開啟","On"},{"關閉","Off"},
        {"顯示","Show"},{"隱藏","Hide"},{"設定","Settings"},{"飾品","Accessories"},{"髮型","Hairstyle"},{"顏色","Color"},
        {"紅色","Red"},{"藍色","Blue"},{"綠色","Green"},{"黃色","Yellow"},
        {"酒狐","Wine Fox"},{"魔法酒狐","Magical Wine Fox"},{"基础款","Basic"},{"原版","Vanilla"},
        {"作者","Author"},{"建模","Modeling"},{"动画制作","Animation"},{"材质","Texture"},{"程序","Programming"},
        {"一","1"},{"二","2"},{"三","3"},{"四","4"},{"五","5"},{"六","6"},{"七","7"},{"八","8"},{"九","9"},{"十","10"}
    };
    for (auto const& [zh,en]:words) if(label==zh) return std::string(en);
    std::string result;
    for (std::size_t i=0;i<label.size();) {
        std::string_view best, translated;
        for (auto const& [zh,en]:words) if(zh.size()>best.size() && label.substr(i).starts_with(zh)) {best=zh;translated=en;}
        if (!best.empty()) {
            if(!result.empty() && result.back()!=' ' && result.back()!='(' && result.back()!='[' && result.back()!='-') result+=' ';
            result+=translated; i+=best.size();
            if(i<label.size() && static_cast<unsigned char>(label[i])>=128) result+=' ';
        } else {
            auto c=static_cast<unsigned char>(label[i]);
            auto n=c<128?1u:(c&0xe0)==0xc0?2u:(c&0xf0)==0xe0?3u:4u;
            result+=label.substr(i,n); i+=n;
        }
    }
    if(hasHan(result)) return std::string(label);
    while(!result.empty() && result.back()==' ')result.pop_back();
    return result;
}
inline std::string readableAnimationId(std::string_view id) {
    if(id=="#return") return "Back";
    if(id.starts_with('#')) id.remove_prefix(1);
    if(id.starts_with("animation.")) id.remove_prefix(10);
    if(id.starts_with("extra") && id.size()>5 && id.find_first_not_of("0123456789",5)==std::string_view::npos) {
        if(id.size()<=9) return "Emote " + std::to_string(std::stoi(std::string(id.substr(5)))+1);
        return "Emote " + std::string(id.substr(5));
    }
    std::string result;
    for(std::size_t i=0;i<id.size();++i) {
        auto c=static_cast<unsigned char>(id[i]);
        if(c=='_'||c=='.'||c=='-'||c=='/') {if(!result.empty()&&result.back()!=' ')result+=' ';continue;}
        if(i && c>='A' && c<='Z' && id[i-1]>='a' && id[i-1]<='z') result+=' ';
        if((result.empty()||result.back()==' ') && c>='a'&&c<='z') c=static_cast<unsigned char>(c-'a'+'A');
        result+=static_cast<char>(c);
    }
    return result;
}
inline std::string displayLabel(std::string_view locale, std::string value) {
    if(chineseLocale(locale) || locale.starts_with("ja") || locale.starts_with("ko")) return value;
    return englishModelLabel(value);
}
inline std::string displayEmote(std::string_view locale, std::string value, std::string_view id) {
    if(chineseLocale(locale)) return value.empty()?std::string(id):value;
    if(!value.empty() && value!=id && !value.starts_with('#')) {
        auto translated=displayLabel(locale,value);
        if(!hasHan(translated) || locale.starts_with("ja") || locale.starts_with("ko")) return translated;
        auto readable=readableAnimationId(id);
        if(!hasHan(readable) && !readable.empty()) return readable;
        return id.starts_with('#') ? "Emote Group" : "Emote";
    }
    auto readable=displayLabel(locale,readableAnimationId(id));
    return hasHan(readable) && !locale.starts_with("ja") && !locale.starts_with("ko") ? (id.starts_with('#') ? "Emote Group" : "Emote") : readable;
}
}
