#pragma once
#include "ModelPack.h"

namespace ysm {
// Authored clips win. Keep provenance for legacy hand-loop compatibility.
Json withDefaultAnimations(Json document, bool firstPerson = false, Json const& geometry = Json());
std::string defaultTargetAnimationDomain(ReplacedModelKind kind, std::vector<std::string> matches);
Json withDefaultTargetAnimations(Json document, ReplacedModelKind kind, std::vector<std::string> const& matches);
}
