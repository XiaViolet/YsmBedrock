#pragma once
#include "ModelParticles.h"
#include "ModelPack.h"
class IClientInstance;
namespace ysm {
void dispatchModelParticles(IClientInstance& client, std::span<ParticleCue const> cues);
void resetModelParticleTransitions();
Json particleRenderReport();
}
