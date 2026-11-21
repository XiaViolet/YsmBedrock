#pragma once
#include "UiTouchQueue.h"
#include "YsmUi.h"
namespace ysm {
inline bool deliverMenuTouches(std::vector<MenuTouchSample> const& samples, YsmUi& ui, UiPointer& pointer,
                              float width, float height) {
    bool delivered = false;
    for (auto const& sample : samples) {
        if (!ui.isOpen()) break;
        if (sample.phase == TouchPhase::Cancel) {
            ui.cancelPointer();
            pointer.reset();
        } else if (auto p = pointer.contact(sample.fraction,1,1,width,height)) {
            ui.mouse(sample.phase == TouchPhase::Move ? 0 : 1, sample.phase == TouchPhase::Down, p->x, p->y);
            delivered = true;
        }
    }
    return delivered;
}
}
