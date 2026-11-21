#include "BoneVisual.h"
#include <cassert>

int main() {
    constexpr auto material = "entity_alphatest.skinning";
    ysm::BoneVisual lamp;
    for (int level = -1; level <= 15; ++level) {
        lamp.glow = level;
        assert(ysm::supportsArkeGlowLayer(lamp, material) == (level == 15));
    }
    assert(ysm::supportsArkeGlowLayer(lamp, "entity_alphatest_one_sided.skinning"));
    assert(!ysm::supportsArkeGlowLayer(lamp, "entity_alphatest_change_color.skinning"));
    assert(!ysm::supportsArkeGlowLayer(lamp, "entity_glint.skinning"));
    lamp.alpha = 0;
    assert(!ysm::supportsArkeGlowLayer(lamp, material));
    lamp.alpha = 128;
    assert(!ysm::supportsArkeGlowLayer(lamp, material));
    lamp.alpha = 255;
    lamp.color = {255, 196, 127};
    assert(!ysm::supportsArkeGlowLayer(lamp, material));
}
