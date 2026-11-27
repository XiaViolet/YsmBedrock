#include "Localization.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>

int main() {
    using namespace ysm;
    assert(!chineseLocale("en_US"));
    assert(chineseLocale("zh-CN"));
    assert(chineseLocale("ZH_tw"));
    assert(!chineseLocale("ja_JP"));
    setGameLanguage("zh_CN");
    assert(std::string(gameText("返回","Back"))=="返回");
    setGameLanguage("en_US");
    assert(std::string(gameText("返回","Back"))=="Back");
    assert(std::wstring(gameText(L"返回",L"Back"))==L"Back");
    assert(std::string(uiText("fr_FR","返回","Back"))=="Back");
    assert(displayEmote("en_us","招手","extra0")=="Wave");
    assert(displayEmote("en_us","跳舞","extra1")=="Dance");
    assert(displayEmote("en_us","作者自定义未翻译动作","extra2")=="Emote 3");
    assert(displayEmote("en_us","作者自定义未翻译动作","happy_dance")=="Happy Dance");
    assert(displayEmote("en_us","作者自定义未翻译动作","未知动作")=="Emote");
    assert(displayEmote("en_us","Author's English Name","extra0")=="Author's English Name");
    assert(displayEmote("zh_cn","招手","extra0")=="招手");
    assert(displayEmote("ja_jp","手を振る","extra0")=="手を振る");
    assert(displayEmote("en_us","","#return")=="Back");
    assert(displayLabel("en_us","显示帽子")=="Show Hat");
    assert(displayLabel("en_us","红色")=="Red");
    assert(displayLabel("en_us","揮手")=="Wave");
    assert(displayLabel("en_us","未翻译的作者原文")=="未翻译的作者原文");
    assert(displayLabel("fr_fr","Bonjour")=="Bonjour");
    assert(readableAnimationId("animation.player.happyDance")=="Player Happy Dance");
    std::cout << "Localization regression passed (22 checks)\n";
}
