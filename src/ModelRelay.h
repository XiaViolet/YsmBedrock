#pragma once
#include "ModelPack.h"
#include "MolangRoaming.h"
#include <array>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <cstdint>
#include <functional>
#include <optional>

namespace ysm {
struct RelayAsset {
    std::string contentKey, digest, extension;
    std::size_t size{};
    std::string title;
    bool selectable{};
    unsigned sourceSpec = 2;
    std::optional<unsigned> originVersion{};
};

struct RelayEvent {
    std::uint64_t sequence{};
    std::vector<float> arguments;
};

struct RelayReceipt {
    std::string session, model;
    std::uint64_t sequence{};
};

struct RelayPose {
    std::string xuid, session, contentKey, texture, action;
    std::uint64_t serial{};
    // Absent on older clients. Zero means both flight modes have ended.
    std::optional<unsigned> flight;
    std::map<std::string, float> formValues;
    std::uint64_t actionSerial{};
    double actionTime{};
    std::array<std::int64_t, 2> shoulderBirds{};
    RoamingPayload roaming;
    std::vector<RelayEvent> events;
    std::map<std::string, RelayReceipt> receipts;
};

// Both native rooms and the optional relay carry the same bounded additions.
inline void writePoseExtras(Json& message, RelayPose const& state) {
    if (state.flight) message["flight"] = *state.flight;
    if (!state.formValues.empty()) message["form_values"] = state.formValues;
}
inline void readPoseExtras(Json const& message, RelayPose& state) {
    if (auto f = message.find("flight"); f != message.end()) {
        if (!f->is_number_unsigned() || f->get<std::uint64_t>() > 3) throw std::runtime_error("Invalid flight state");
        state.flight = f->get<unsigned>();
    }
    if (auto values = message.find("form_values"); values != message.end()) {
        if (!values->is_object() || values->size() > 128) throw std::runtime_error("Invalid model form state");
        for (auto const& [name, value] : values->items()) {
            if (name.size() > 96 || !name.starts_with("variable.") || name.starts_with(roamingPrefix)
                || !value.is_number() || !std::isfinite(value.get<float>())) throw std::runtime_error("Invalid model form field");
            state.formValues.emplace(name, value.get<float>());
        }
    }
}
struct RelayFrame {
    bool approved{};
    std::uint64_t acknowledgedEvent{};
    std::map<std::string, RelayAsset> assets;
    std::map<std::string, RelayPose> players;
};

class RelayPreparationQueue {
    using Clock = std::chrono::steady_clock;
    struct Retry {
        std::string session, model, texture;
        Clock::time_point after;
    };
    std::map<std::string, Retry> failures;
    std::string lastTried;
public:
    void failed(RelayPose const& peer, Clock::time_point now);
    void retry(std::string const& player) { failures.erase(player); }
    RelayPose const* next(RelayFrame const& frame, Clock::time_point now,
        std::function<bool(std::string const&)> const& eligible);
};

class ModelRelay {
    std::wstring host, authorization;
    unsigned short port{};
    bool secure{};
public:
    std::string serverHost;
    int serverPort{};
    static std::optional<ModelRelay> readConfig(std::filesystem::path const& file);
    static std::string newSession();
    RelayFrame exchange(RelayPose const& state, std::uint64_t serial) const;
    ModelPack obtain(RelayAsset const& asset, std::filesystem::path const& cache) const;
private:
    std::vector<unsigned char> request(std::wstring const& path, std::string const& body, std::size_t limit) const;
};
}
