#pragma once
#include "ModelPack.h"
#include "ModelParticles.h"
#include "mc/deps/ecs/WeakEntityRef.h"

namespace ysm {
struct ModelEffects {
    std::shared_ptr<void const> owner;
    std::shared_ptr<ModelPack const> model;
    WeakEntityRef entity;
    std::vector<SoundCue> sounds;
    std::vector<ParticleCue> particles;
};
}
