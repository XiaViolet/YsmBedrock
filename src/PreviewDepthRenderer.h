#pragma once
#include "ModelPreview.h"

namespace ysm {
// A private Windows graphics device. No game framebuffer or depth state is touched.
class PreviewDepthRenderer {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    PreviewDepthRenderer();
    ~PreviewDepthRenderer();
    void clearFrame();
    TextureImage render(PreviewMesh const& mesh, unsigned side);
};
}
