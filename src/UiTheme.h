#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
namespace ysm {
struct UiTheme { std::uint32_t ink, muted, accent, edge, panel, card, selected, subtle, sidebar, hover, stage, onAccent; };
inline constexpr std::array<UiTheme,4> uiThemes{{
    {0xffedf1f7,0xffaeb9c9,0xff8bb9ff,0xff344052,0xff171d27,0xff222b38,0xff304866,0xff293442,0xff111722,0xff2b394c,0xff1c2838,0xff111b2a},
    {0xff19283b,0xff52647a,0xff245cb1,0xffd5dfea,0xfff6f8fc,0xffffffff,0xffdce9fc,0xffedf2f8,0xffe9eff7,0xffe2eaf6,0xffe5edf7,0xffffffff},
    {0xffe4f5ed,0xffa3c1b4,0xff79d4b0,0xff314d43,0xff172820,0xff20382e,0xff305a49,0xff263e34,0xff13241c,0xff2d493c,0xff1b3228,0xff10281e},
    {0xfff5eaf5,0xffcbb3cf,0xffefb0df,0xff543c5b,0xff281e30,0xff392b43,0xff614361,0xff44334d,0xff211829,0xff4c3655,0xff34253f,0xff2b1930}
}};
inline constexpr auto const& uiTheme(int id) { return uiThemes[static_cast<unsigned>(std::clamp(id,0,3))]; }
inline constexpr std::uint32_t themeColor(std::uint32_t c, UiTheme const& t) {
    auto alpha=c&0xff000000u; auto rgb=c&0xffffffu; std::uint32_t out=c;
    switch(rgb) {
    case 0x243e37: out=t.ink; break;
    case 0x82938a: case 0x93aa98: case 0xbecbbb: out=t.muted; break;
    case 0x247968: out=t.accent; break;
    case 0xe0e7df: case 0xc8d7c9: case 0xb9cdb7: case 0xcbded2: out=t.edge; break;
    case 0xfafbf7: out=t.panel; break;
    case 0xffffff: out=t.card; break;
    case 0xfefefe: out=t.onAccent; break;
    case 0xe6f2e9: case 0xc5e5d5: case 0xd7eadb: case 0xedf6ee: out=t.selected; break;
    case 0xf0f4ed: case 0xd8e0d5: out=t.subtle; break;
    case 0xf0f3eb: out=t.sidebar; break;
    case 0xeaf0e7: case 0xe7eddf: out=t.hover; break;
    case 0xeaf1e5: out=t.stage; break;
    case 0xa9cdbc: case 0xbdd8c9: case 0x90cbb8: case 0x8fbea6: case 0x93bfaa: out=t.accent; break;
    case 0x0d241c: out=0xff090d14; break;
    default: break;
    }
    return alpha | (out&0xffffffu);
}
inline constexpr int previewLimit(int quality) { return quality<=0?320:quality==1?512:768; }
}
