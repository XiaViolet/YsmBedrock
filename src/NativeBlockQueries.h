#pragma once
#include "Molang.h"
#include "BedrockWeather.h"
class Actor;
class Block;
class BaseActorRenderContext;

namespace ysm {
struct EntityQueryInput;
BedrockWeatherSnapshot sampleNativeSceneQueries(EntityQueryInput& input, Actor const& subject, BaseActorRenderContext* render);
// The block source is borrowed only during a single synchronous evaluation.
// Read and cache on demand so models without block queries do no world reads.
class NativeBlockQueryScope {
    MolangContext& context;
    MolangContext::BlockLookup previous;
    std::map<std::array<int, 3>, Block const*> sampled;
public:
    NativeBlockQueryScope(MolangContext& context, Actor const& subject);
    ~NativeBlockQueryScope();
    NativeBlockQueryScope(NativeBlockQueryScope const&) = delete;
    NativeBlockQueryScope& operator=(NativeBlockQueryScope const&) = delete;
};
}
