#include "PreviewDepthRenderer.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>

int main() {
    try {
        ysm::PreviewDepthRenderer renderer;
        for (unsigned i = 0; i < 100; ++i) {
            auto side = i % 2 ? 256u : 224u;
            std::array<unsigned char,4> color{static_cast<unsigned char>(i + 20), 180, 40, 255};
            auto skin = std::make_shared<ysm::TextureImage>();
            skin->width = skin->height = 1;
            skin->rgba.assign(color.begin(), color.end());
            ysm::PreviewMesh mesh;
            mesh.skin = skin;
            mesh.vertices = {{0,0,.5f,.5f,.5f,0xffffffff}, {1,0,.5f,.5f,.5f,0xffffffff},
                             {0,1,.5f,.5f,.5f,0xffffffff}};
            auto image = renderer.render(mesh, side);
            auto pixel = image.rgba.begin() + 4 * (side / 4 * side + side / 4);
            if (image.width != side || image.height != side || !std::equal(color.begin(), color.end(), pixel)
                || image.rgba.back() != 0) throw std::runtime_error("Wrong preview size, color or background");
            renderer.clearFrame();
            renderer.clearFrame();
        }
        std::puts("PASS 100 hardware D3D11 renders, 100 alternating atlas colors and 200 frame clears on one preview device.");
    } catch (std::exception const& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
