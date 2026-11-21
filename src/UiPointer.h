#pragma once
#include <algorithm>
#include <cmath>
#include <optional>

namespace ysm {
struct UiPoint { float x, y; };
// Physical client pixels and native GUI units are separate coordinate spaces.
// A button event need not carry a position: retain the last motion instead of
// replacing it with its often-zero x/y fields.
class UiPointer {
    // Position is a fraction of the client area so a resize preserves the
    // virtual cursor's position. Only motion events contribute deltas.
    std::optional<UiPoint> position, lastFrame, lastGamePosition;
    UiPoint pending{};
    unsigned stalledFrames = 0;
    bool moved = false, relative = false, touch = false;
    bool gameMoved = false, useGamePosition = false;

    void advance(UiPoint delta) {
        UiPoint p = position.value_or(UiPoint{.5f, .5f});
        position = UiPoint{std::clamp(p.x + delta.x, 0.f, 1.f), std::clamp(p.y + delta.y, 0.f, 1.f)};
    }
    std::optional<UiPoint> gui(float width, float height) const {
        if (!position || !(width > 0 && height > 0)) return {};
        return UiPoint{position->x * width, position->y * height};
    }
public:
    void reset() { *this = UiPointer{}; }
    bool usesRelativeMotion() const { return relative; }
    bool usesTouch() const { return touch; }
    char const* source() const { return touch ? "touch" : relative ? "relative" : useGamePosition ? "game_absolute" : "absolute"; }
    std::optional<UiPoint> contact(UiPoint pixel, float width, float height, float guiWidth, float guiHeight) {
        if (!(width > 0 && height > 0)) return {};
        reset();
        touch = true;
        position = UiPoint{pixel.x / width, pixel.y / height};
        return gui(guiWidth, guiHeight);
    }
    std::optional<UiPoint> feed(int button, float eventX, float eventY,
        std::optional<UiPoint> windowPosition, float clientWidth, float clientHeight,
        float guiWidth, float guiHeight, UiPoint delta = {}, bool forceRelative = false) {
        if (!(clientWidth > 0 && clientHeight > 0)) return {};
        touch = false;
        if (useGamePosition && button == 0 && (delta.x || delta.y)) {
            useGamePosition = false;
            relative = true;
        }
        if (button == 0 && eventX >= 0 && eventX < clientWidth && eventY >= 0 && eventY < clientHeight &&
            (eventX != 0 || eventY != 0)) {
            UiPoint game{eventX / clientWidth, eventY / clientHeight};
            gameMoved |= lastGamePosition && (lastGamePosition->x != game.x || lastGamePosition->y != game.y);
            lastGamePosition = game;
            if (forceRelative && gameMoved && !delta.x && !delta.y && !relative) useGamePosition = true;
            if (useGamePosition) position = game;
        }
        if (!relative && !useGamePosition) {
            if (windowPosition) position = UiPoint{windowPosition->x / clientWidth, windowPosition->y / clientHeight};
            else if (button == 0 && std::isfinite(eventX) && std::isfinite(eventY))
                position = UiPoint{eventX / clientWidth, eventY / clientHeight};
        }
        if (button == 0 && (delta.x || delta.y)) {
            delta = {delta.x / clientWidth, delta.y / clientHeight};
            relative |= forceRelative;
            if (relative) { useGamePosition = false; advance(delta); }
            else {
                pending.x += delta.x;
                pending.y += delta.y;
                moved = true;
            }
        }
        return gui(guiWidth, guiHeight);
    }
    std::optional<UiPoint> frame(UiPoint window, float width, float height, float guiWidth, float guiHeight) {
        if (!(width > 0 && height > 0)) return {};
        if (!relative && !touch) {
            // Compare once per frame. High-polling mice can deliver several
            // events before Windows publishes a new absolute position.
            bool inside = window.x > 0 && window.y > 0 && window.x < width - 1 && window.y < height - 1;
            if (!lastFrame || lastFrame->x != window.x || lastFrame->y != window.y) {
                stalledFrames = 0;
                pending = {};
                useGamePosition = false;
            } else if (!useGamePosition && (gameMoved || (inside && moved))) {
                if (++stalledFrames >= 2) {
                    if (inside && moved) relative = true;
                    else useGamePosition = true;
                }
            } else if (!inside) {
                stalledFrames = 0;
                pending = {};
            }
            if (useGamePosition) position = lastGamePosition;
            else position = UiPoint{window.x / width, window.y / height};
            if (relative) advance(pending);
            lastFrame = window;
            moved = gameMoved = false;
        }
        return gui(guiWidth, guiHeight);
    }
};
}
