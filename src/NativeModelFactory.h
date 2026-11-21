#pragma once
#include <memory>
#include <set>
#include <string>
class ActorResourceDefinition;
class DataDrivenModel;
class Model;
class GeometryInfo;
class ActorRenderer;
namespace mce { class TextureGroup; }
namespace ysm {
struct NativeModelInstance {
    // Model destruction precedes its definition. Native models retain a weak
    // resource reference; the instance provides the strong resource ownership.
    std::shared_ptr<ActorResourceDefinition> definition;
    std::shared_ptr<DataDrivenModel> model;
    NativeModelInstance() = default;
    NativeModelInstance(NativeModelInstance const&) = delete;
    NativeModelInstance& operator=(NativeModelInstance const&) = delete;
    NativeModelInstance(NativeModelInstance&&) noexcept = default;
    NativeModelInstance& operator=(NativeModelInstance&& other) noexcept {
        if (this != &other) {
            model.reset();
            definition.reset();
            definition = std::move(other.definition);
            model = std::move(other.model);
        }
        return *this;
    }
};
class NativeModelFactory {
public:
    static bool supports(Model const& original);
    static NativeModelInstance createTarget(DataDrivenModel const& original,
        std::shared_ptr<GeometryInfo> const& geometry, std::set<std::string> const& bones,
        std::string const& identity, float drawScale);
    static std::shared_ptr<ActorRenderer> createRenderer(NativeModelInstance const& instance,
        std::shared_ptr<mce::TextureGroup> const& textures);
};
}
