#pragma once
#include "ModelPack.h"
#include "NativeTextureSet.h"
namespace mce { class TextureGroup; }
namespace ysm {
// Owns resource locations as well as the uploaded layers. Multiple entities
// can share one upload, while their materials and animation state stay private.
class NativeTextureUpload {
    struct Impl;
    std::unique_ptr<Impl> impl;
    NativeTextureUpload();
public:
    ~NativeTextureUpload();
    static std::shared_ptr<NativeTextureUpload> create(std::shared_ptr<mce::TextureGroup> const& group,
        ModelTexture const& texture, PbrEncoding encoding, std::string const& identity);
    NativeTextureSet textures() const;
};
}
