#pragma once
#include "YsmUi.h"
#include "ModelPreview.h"
#include <memory>
class IClientInstance;
class MinecraftUIRenderContext;
namespace ysm {
class AnimationRuntime;
class NativeYsmUi {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    NativeYsmUi();
    ~NativeYsmUi();
    std::optional<std::uint64_t> updateHud(bool enabled, bool debugOverlay, Json const& settings,
        std::shared_ptr<ModelPreview const> preview, std::size_t textureIndex);
    void updateMenu(IClientInstance& client, std::uint64_t session, UiSnapshot snapshot, std::shared_ptr<ModelPreview const> candidate);
    void toggle(IClientInstance& client, bool wheel);
    void close(IClientInstance* client = nullptr);
    void diagnostics(std::filesystem::path const& directory, bool debug);
    bool key(IClientInstance& client, int code, bool down, bool control);
    bool mouse(IClientInstance& client, int button, int value, float x, float y, float dx, float dy);
    bool isOpen() const;
    bool showsModels() const;
    bool showsForms() const;
    void beginFrame();
    void retireLibraryPreview();
    bool suppressMouseCapture(IClientInstance const& client);
    void maintainMouse(IClientInstance& client);
    void render(MinecraftUIRenderContext& context, AnimationRuntime const& animation);
    std::vector<UiCommand> takeCommands();
};
}
