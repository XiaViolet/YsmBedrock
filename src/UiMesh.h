#pragma once
#include "YsmUi.h"
#include <atomic>
#include <span>

namespace ysm {
struct UiVertex { float x,y; std::uint32_t color; };
// Keep one native submission per bounded chunk, rather than one submission
// per scanline or icon pixel. Triangle boundaries also stay chunk aligned.
inline constexpr std::size_t uiBatchVertices = 24576;
std::vector<UiVertex> buildUiMesh(std::span<UiPaint const> paint);
std::size_t uiMeshBatches(std::size_t vertices);
std::uint64_t uiShapeKey(std::span<UiPaint const> paint);
class UiFrameGate {
    std::atomic<std::uint64_t> frame{1}, submitted{0};
public:
    void begin() { frame.fetch_add(1,std::memory_order_release); }
    bool claim() {auto current=frame.load(std::memory_order_acquire);return submitted.exchange(current,std::memory_order_acq_rel)!=current;}
};
}
