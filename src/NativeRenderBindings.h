#pragma once
#include "ModelPack.h"
#include "NativeTextureSet.h"
#include <memory>
#include <functional>
#include <map>
#include <string_view>
class RenderParams;
class ItemStack;
class Actor;
class RenderController;
class DataDrivenGeometry;
struct BedrockTextureData;
enum class AttachableSlotIndex : int;
namespace ysm {
class FirstPersonRenderScope {
public:
    FirstPersonRenderScope();
    ~FirstPersonRenderScope();
    static bool isActive();
    static unsigned depth();
};
class UiRenderContextTracker {
public:
    static void enter(void const* context);
    static void leave(void const* context);
    static bool isActive();
};
unsigned defaultAttachableGrip(Actor const& actor, AttachableSlotIndex slot, unsigned mask);
void bindAttachableParent(RenderParams& params, AttachableSlotIndex slot, ItemStack const& item, Actor const& actor,
    unsigned primaryCopy = 0);
void drawAtAttachableGrip(RenderParams& params, AttachableSlotIndex slot, ItemStack const& item, Actor const& actor,
    unsigned copy, bool lanceCharge, std::function<void()> const& draw);
class HeldItemBoneScope {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    explicit HeldItemBoneScope(RenderParams& params, std::string_view hand = {}, unsigned copy = 0);
    bool bound() const;
    ~HeldItemBoneScope();
};
class NativeRenderBindings {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    NativeRenderBindings();
    ~NativeRenderBindings();
    bool bind(RenderParams& params, NativeTextureSet const& textures,
        std::map<std::string, std::string> const& armParts);
    std::size_t prepare(RenderController& controller, std::shared_ptr<DataDrivenGeometry> const& geometry,
        std::map<std::string, std::string> const& armParts, bool drawEnabled = true);
    Json report(RenderParams& params) const;
    void reset();
};
}
