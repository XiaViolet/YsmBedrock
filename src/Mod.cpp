#include "ll/api/mod/NativeMod.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/mod/ModManagerRegistry.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/input/KeyInputEvent.h"
#include "ll/api/event/input/MouseInputEvent.h"
#include "ll/api/event/render/UIRenderEvent.h"
#include "ll/api/event/client/ClientStartJoinLevelEvent.h"
#include "ll/api/event/client/ClientExitLevelEvent.h"
#include "ll/api/event/client/ClientCancelJoinLevelEvent.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/memory/Hook.h"
#include "mc/client/components/ClientSkinSystem.h"
#include "mc/client/components/ClientSkin.h"
#include "mc/client/model/GeometryGroup.h"
#include "mc/client/model/GeometryInfo.h"
#include "mc/client/model/Geometry.h"
#include "mc/world/actor/player/SerializedSkinRef.h"
#include "mc/world/actor/player/SerializedSkinImpl.h"
#include "mc/client/game/ClientInstance.h"
#include "mc/client/input/ClientMoveInputHandler.h"
#include "mc/entity/components/MoveInputComponent.h"
#include "mc/client/options/IOptionRegistry.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/gui/GuiData.h"
#include "mc/client/sound/SoundEngine.h"
#include "mc/deps/audio/SoundSystemBase.h"
#include "mc/deps/audio/ServerSoundHandle.h"
#include "mc/deps/audio/PlaySoundOptions.h"
#include "mc/deps/audio/PlayingSoundAttributes.h"
#include "mc/deps/core/file/Path.h"
#include "mc/client/renderer/screen/MinecraftUIRenderContext.h"
#include "ModelPack.h"
#include "ModelLibrary.h"
#include "SkinRuntime.h"
#include "NativePbrDrawProbe.h"
#include "AnimationRuntime.h"
#include "NativeRenderBindings.h"
#include "NativeReplacedResources.h"
#include "ll/api/event/world/ServerLevelTickEvent.h"
#include "mc/world/Minecraft.h"
#include "mc/deps/nbt/CompoundTag.h"
#include "NativeModelRelay.h"
#include "NativeBoneVisual.h"
#include "mc/client/gui/screens/ScreenContext.h"
#include "NativeModelParticles.h"
#include "NativeModelAudio.h"
#include "mc/client/renderer/actor/v2/DataDrivenRendererV2.h"
#include "mc/deps/minecraft_renderer/resources/MERSUniformData.h"
#include "KeyBindings.h"
#include "NativeYsmUi.h"
#include "NativeMenuTouch.h"
#include "NativeModelPicker.h"
#include "NativeWeather.h"
#include "mc/client/network/ClientNetworkHandler.h"
#include "mc/client/network/LegacyClientNetworkHandler.h"
#include "mc/client/multiplayer/ClientLevel.h"
#include "mc/network/packet/AddActorPacket.h"
#include "mc/network/packet/LevelEventPacket.h"
#include "mc/network/packet/StartGamePacket.h"
#include "mc/client/renderer/controller/RenderController.h"
#include "mc/client/model/models/DataDrivenGeometry.h"
#include "mc/client/renderer/game/ItemInHandRenderer.h"
#include "mc/client/renderer/BaseActorRenderContext.h"
#include "mc/deps/renderer/Camera.h"
#include <glm/gtc/matrix_transform.hpp>
#include "mc/client/world/actor/animation/ClientAnimationComponent.h"
#include "mc/client/renderer/actor/ActorRenderData.h"
#include "mc/client/renderer/actor/ActorRenderDispatcher.h"
#include "mc/client/renderer/actor/ActorRenderer.h"
#include "mc/client/model/models/DataDrivenModel.h"
#include "mc/client/model/models/SkinnedMeshGroup.h"
#include "mc/client/model/models/SkinnedMesh.h"
#include "mc/client/model/models/MeshInstanceConstants.h"
#include "mc/client/model/geom/ModelPart.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/item/Item.h"
#include "mc/world/item/components/ProjectileItemComponent.h"
#include "mc/world/item/components/ShooterItemComponent.h"
#include "mc/world/item/CrossbowItem.h"
#include "mc/world/level/Spawner.h"
#include "mc/world/actor/player/PlayerItemInUse.h"
#include "mc/world/actor/ActorDefinitionIdentifier.h"
#include "mc/world/actor/ActorDamageSource.h"
#include "mc/world/actor/ActorType.h"
#include "NativeShoulderParrot.h"
#include "mc/world/actor/animation/AttachableSlotIndex.h"
#include "mc/legacy/ActorUniqueID.h"
#include "mc/entity/components_json_legacy/ProjectileComponent.h"
#include <Windows.h>
#include <shellapi.h>
#include <fstream>
#include <optional>
#include <atomic>
#include <chrono>
#include <future>
#include <cstdint>
#include <utility>

namespace ysm {
static void installHooks();
static void removeHooks();
// The immediate player renderer can clone a skinned mesh after geometry
// registration. Keep its exact owner for this draw, including nested renders.
struct ActorDrawScope {
    static inline thread_local Actor const* current = nullptr;
    Actor const* previous;
    explicit ActorDrawScope(Actor const* actor) : previous(std::exchange(current, actor)) {}
    ~ActorDrawScope() { current = previous; }
};
// Only valid on the game call stack that is creating this shot. No player
// or item pointer from the integrated server is retained by the room worker.
struct ArrowLaunchScope {
    static inline thread_local ArrowLaunchScope* current = nullptr;
    Player* player;
    std::optional<bool> crossbow;
    ArrowLaunchScope* previous;
    static std::optional<bool> weapon(ItemStack const& stack) {
        if (stack.isNull() || !stack.mItem) return {};
        auto const& id = stack.mItem->mFullName->getString();
        if (id == "minecraft:bow") return false;
        if (id == "minecraft:crossbow") return true;
        return {};
    }
    ArrowLaunchScope(Player* player, ItemStack const& stack)
        : player(player), crossbow(weapon(stack)), previous(std::exchange(current, this)) {}
    ~ArrowLaunchScope() { current = previous; }
};
class Mod {
    std::optional<ModelPack> pack;
    std::optional<ModelLibrary> library;
    AnimationRuntime::Prepared startupAnimation;
    SkinRuntime runtime;
    AnimationRuntime animation;
    NativeModelRelay relay;
    std::shared_ptr<NativeReplacedResources> targetStorage = std::make_shared<NativeReplacedResources>();
    NativeReplacedResources& targetResources = *targetStorage;
    bool targetRenderingEnabled = false;
    std::set<std::string> targetRendererTypes;
    Json targetRendererReports = Json::array();
    struct Voice {
        uint64 handle;
        std::string owner;
        bool firstPerson;
        std::shared_ptr<ModelAudio const> clip;
        std::optional<std::string> id;
        bool looping = false;
        bool observedPlaying = false;
        WeakEntityRef entity;
        PlayingSoundState playback;
        bool reportedMove = false;
        std::string event;
        std::optional<std::weak_ptr<void const>> source;
        NativeSoundReadback lastUpdate;
    };
    std::vector<Voice> voices;
    std::set<std::string> missingSounds;
    std::set<std::string> reportedSounds;
    Json audioHistory = Json::array();
    MolangRoamingSession roaming;
    void const* roamingLevel = nullptr;
    ll::mod::NativeMod* self = nullptr;
    Player* lastPlayer = nullptr;
    std::optional<std::chrono::steady_clock::time_point> applyAfter;
    std::atomic<bool> toggleRequested = false;
    KeyBindings keys;
    NativeYsmUi ui;
    Json uiSettings = YsmUi::defaultSettings();
    bool debugOverlay = false;
    std::atomic<bool> arkeShaderEnabled{false};
    std::chrono::steady_clock::time_point nextArkeCheck{};
    void refreshArkeShader() {
        auto now = std::chrono::steady_clock::now();
        if (now < nextArkeCheck) return;
        nextArkeCheck = now + std::chrono::seconds(2);
        auto arke = ll::mod::ModManagerRegistry::getInstance().getMod("arke");
        bool enabled = false;
        if (arke && arke->isEnabled() && uiSettings.value("arke_extra_emissive_layer", false)) {
            std::ifstream stream(arke->getModDir() / "config.json");
            // Arke 0.1.5 stores shader selection in a small JSON document.
            // A truncated config during a preset switch falls back on the next tick.
            std::array<char, 4097> bytes;
            stream.read(bytes.data(), bytes.size());
            auto count = stream.gcount();
            if (!stream.bad() && count > 0 && count < static_cast<std::streamsize>(bytes.size())) {
                auto config = Json::parse(bytes.begin(), bytes.begin() + count, nullptr, false);
                enabled = config.is_object() && config.contains("enabled") && config["enabled"] == true &&
                    config.contains("shaderpack") && config["shaderpack"].is_string() &&
                    !config["shaderpack"].get_ref<std::string const&>().empty();
            }
        }
        arkeShaderEnabled.store(enabled, std::memory_order_relaxed);
    }
    std::shared_ptr<ModelPreview const> uiPreview;
    std::uint64_t uiGeneration = 1;
    struct LoadRequest { std::string source; bool wear; };
    std::optional<LoadRequest> queuedModel;
    std::string uiMessage;
    struct PreparedSelection {
        bool wear = false;
        ModelChoice choice;
        AnimationRuntime::Prepared animations;
        std::shared_ptr<ModelPreview const> preview;
        std::optional<ModelLibrary> library;
    };
    struct ModelJob {
        std::future<PreparedSelection> future;
        std::stop_source cancellation;
        std::uint64_t epoch;
    };
    std::optional<ModelJob> modelJob;
    enum class BrowseKind { Catalog, ImportFile, ImportFolder };
    struct BrowseRequest {
        BrowseKind kind;
        std::string source;
        HWND owner;
    };
    struct BrowseResult {
        bool scanned = false;
        ModelChoice choice;
        std::shared_ptr<LibraryPreview> info;
        std::shared_ptr<ModelPreview const> renderer;
    };
    std::future<BrowseResult> browseJob;
    std::stop_source browseStop;
    std::optional<BrowseRequest> nextBrowse;
    std::shared_ptr<LibraryPreview const> browseInfo;
    std::shared_ptr<ModelPreview const> browseRenderer;
    std::string browseLoading;
    bool returnAfterPicker = false;
    struct CardRead {
        ModelCardRequest request;
        std::future<std::optional<CachedModelMetadata>> future;
    };
    std::optional<CardRead> cardRead;
    std::optional<PreparedSelection> pendingModel;
    std::uint64_t selectionEpoch = 0;
    ll::event::ListenerPtr keyListener;
    ll::event::ListenerPtr mouseListener, uiListener;
    std::vector<ll::event::ListenerPtr> weatherListeners;
    std::map<std::string, std::string> modVersions;
    Json uiRenderContexts = Json::array();
public:
    bool usesArkeShader() const { return arkeShaderEnabled.load(std::memory_order_relaxed); }
    std::filesystem::path animationDiagnostics() const {
        return debugOverlay ? self->getModDir() / "animation-status.json" : std::filesystem::path{};
    }
    static Mod& getInstance() { static Mod instance; return instance; }
    bool load() {
        self = ll::mod::NativeMod::current().get();
        relay.configure(self->getModDir());
        debugOverlay = std::filesystem::exists(self->getModDir() / "diagnostics.enable");
        ui.diagnostics(self->getModDir(),debugOverlay);
        targetRenderingEnabled = std::filesystem::exists(self->getModDir() / "target-render-probe.enable");
        self->getLogger().setFlushLevel(ll::io::LogLevel::Info);
        try {
            auto path = self->getModDir() / "ui-settings.json";
            if (std::filesystem::exists(path) && std::filesystem::file_size(path) <= 65536) {
                std::ifstream stream(path); auto input = Json::parse(stream);
                for (auto const& [key, value] : uiSettings.items()) if (input.contains(key)) {
                    auto const& saved = input.at(key);
                    if (value.is_boolean() && saved.is_boolean()) uiSettings[key] = saved;
                    else if (key=="hud_fps" && saved.is_number_integer()) uiSettings[key]=static_cast<int>(std::clamp(saved.get<double>(),5.,30.));
                    else if ((key == "sound_volume" || key.starts_with("hud_")) && saved.is_number_integer())
                        uiSettings[key] = static_cast<int>(std::clamp(saved.get<double>(), 0.0, 100.0));
                    else if ((key == "favorites" || key == "hidden_players") && saved.is_array() && saved.size() <= 1024) {
                        uiSettings[key] = Json::array(); for (auto const& s : saved) if (s.is_string() && s.get_ref<std::string const&>().size() <= 512) uiSettings[key].push_back(s);
                    }
                }
            }
        } catch (std::exception const& error) { self->getLogger().warn("Cannot restore UI settings: {}", error.what()); }
        relay.setVisibility(uiSettings.value("disable_other_models", false), uiSettings.at("hidden_players"));
        targetResources.presentation(!uiSettings.value("disable_vehicle_model", false), !uiSettings.value("disable_projectile_model", false));
        try {
            auto directory = self->getModDir() / "models";
            std::filesystem::create_directories(directory);
            library.emplace(directory, self->getModDir() / "selection.json");
            auto choice = library->initial([&](ModelPack const& model) {
                startupAnimation = AnimationRuntime::prepare(model, animationDiagnostics());
            });
            for (auto const& message : choice.diagnostics) self->getLogger().warn("{}", message);
            if (!choice.pack) {
                library->observe(choice);
                startupAnimation.reset();
                std::ofstream(self->getModDir() / "model-library.json") << library->report().dump(2);
                return true;
            }
            for (auto const& message : AnimationRuntime::preparedDiagnostics(startupAnimation)) choice.diagnostics.push_back(message);
            library->accept(choice);
            pack = std::move(choice.pack);
            uiPreview = std::make_shared<ModelPreview>(*pack, AnimationRuntime::preparedBodySampler(startupAnimation));
            std::ofstream(self->getModDir() / "model-library.json") << library->report().dump(2);
            std::ofstream(self->getModDir() / "inspection.json") << pack->summary().dump(2);
            self->getLogger().info("Loaded source package: {} bones, {} cubes, {} animation clips", pack->boneCount, pack->cubeCount, pack->animationCount);
        } catch (std::exception const& error) {
            self->getLogger().error("Model load failed: {}", error.what());
            return false;
        }
        return true;
    }
    bool enable() {
        keys.reset();
        modVersions.clear();
        for (auto& mod : ll::mod::ModManagerRegistry::getInstance().mods()) {
            auto const& version = mod.getManifest().version;
            if (version) modVersions[mod.getName()] = version->to_string();
        }
        // Source models use the Java mod ID; report this port's own version.
        if (auto const& version = self->getManifest().version) modVersions["ysm"] = version->to_string();
        clearNativeWeather();
        installHooks();
        startPbrDrawProbe(self->getModDir(), runtime);
        auto& bus = ll::event::EventBus::getInstance();
        weatherListeners.push_back(bus.emplaceListener<ll::event::ServerLevelTickEvent>([this](auto& event) {
            targetResources.serverTick(event.level());
            relay.serverTick(event.level());
        }));
        weatherListeners.push_back(bus.emplaceListener<ll::event::ClientStartJoinLevelEvent>([this](auto&) { clearNativeWeather(); resetRoamingConnection(); relay.boundary(); }));
        weatherListeners.push_back(bus.emplaceListener<ll::event::ClientExitLevelEvent>([this](auto&) { clearNativeWeather(); resetRoamingConnection(); relay.boundary(); }));
        weatherListeners.push_back(bus.emplaceListener<ll::event::ClientCancelJoinLevelEvent>([this](auto&) { clearNativeWeather(); resetRoamingConnection(); relay.boundary(); }));
        keyListener = ll::event::EventBus::getInstance().emplaceListener<ll::event::KeyInputEvent>([this](auto& event) {
            auto action = keys.feed(event.keyCode(), event.isDown());
            auto client = ll::service::getClientInstance();
            if (client) {
                if (ui.isOpen()) {
                    if (action == KeyAction::ModelMenu) { ui.toggle(*client, false); event.cancel(); return; }
                    auto held = keys.glfwKeys();
                    if (ui.key(*client, event.keyCode(), event.isDown(), held.contains(341) || held.contains(345))) {
                        if (!ui.isOpen()) keys.reset();
                        event.cancel();
                        return;
                    }
                }
                if (action != KeyAction::None && client->getLocalPlayer() && client->getTopScreenName() == "hud_screen") {
                    ui.toggle(*client, action == KeyAction::Roulette);
                    event.cancel();
                }
            }
        });
        mouseListener = bus.emplaceListener<ll::event::MouseInputEvent>([this](auto& event) {
            if (auto client = ll::service::getClientInstance())
                if (ui.mouse(*client, event.actionButtonId(), event.buttonData(), event.x(), event.y(), event.dx(), event.dy())) event.cancel();
        });
        uiListener = bus.emplaceListener<ll::event::AfterUIRenderEvent>([this](auto& event) {
            try { ui.render(event.uiRenderContext(), animation); }
            catch (std::exception const& error) { self->getLogger().error("UI drawing failed: {}", error.what()); if (auto client = ll::service::getClientInstance()) ui.close(&*client); }
        });
        self->getLogger().info("YsmBedrock enabled; Alt+Y model menu, Z animation roulette");
        status("enabled");
        return true;
    }
    void status(std::string const& stage) {
        std::ofstream(self->getModDir() / "runtime-status.json") << Json{{"stage", stage}}.dump(2);
    }
    void saveUiSettings() {
        try {
            auto path = self->getModDir() / "ui-settings.json", temporary = self->getModDir() / "ui-settings.json.tmp";
            { std::ofstream out(temporary, std::ios::binary | std::ios::trunc); out << uiSettings.dump(2); out.close(); if (!out) throw std::runtime_error("Cannot save UI settings"); }
            std::filesystem::rename(temporary, path);
        } catch (std::exception const& error) { self->getLogger().warn("UI settings save failed: {}", error.what()); uiMessage = "设置保存失败，请检查日志"; }
    }
    void updateCatalogCards() {
        if (!library || modelJob || pendingModel || queuedModel) return;
        if (cardRead) {
            if (cardRead->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            try {
                if (auto metadata = cardRead->future.get()) library->applyCard(cardRead->request, std::move(*metadata));
            } catch (std::exception const& error) {
                self->getLogger().warn("Cannot read model card {}: {}", cardRead->request.source, error.what());
            }
            cardRead.reset();
        }
        if (ui.showsModels()) {
            if (auto request = library->takeUnreadCard()) {
                try {
                    auto task = std::async(std::launch::async, ModelLibrary::readCard, *request);
                    cardRead.emplace(CardRead{std::move(*request), std::move(task)});
                } catch (std::system_error const& error) {
                    self->getLogger().warn("Cannot start model card reader: {}", error.what());
                }
            }
        }
    }
    void resetBrowse() {
        browseStop.request_stop();
        returnAfterPicker = false;
        nextBrowse.reset(); browseLoading.clear(); browseRenderer.reset(); browseInfo.reset();
    }
    void requestBrowse(BrowseKind kind, std::string const& source = {}) {
        HWND owner = GetForegroundWindow();
        DWORD process{};
        GetWindowThreadProcessId(owner, &process);
        if (process != GetCurrentProcessId()) owner = nullptr;
        browseStop.request_stop();
        nextBrowse = BrowseRequest{kind, source, owner};
        browseLoading = kind == BrowseKind::Catalog ? (source.empty() ? "模型库" : source) : "导入模型";
        uiMessage.clear();
    }
    void pollBrowse() {
        if (!library) return;
        if (browseJob.valid() && browseJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                auto result = browseJob.get();
                if (!browseStop.stop_requested()) {
                    browseLoading.clear();
                    if (result.scanned) library->observe(result.choice);
                    if (result.info) {
                        browseRenderer = std::move(result.renderer);
                        browseInfo = std::move(result.info);
                    } else if (!result.choice.diagnostics.empty()) uiMessage = "模型无法预览：" + result.choice.diagnostics.front();
                }
            } catch (std::exception const& e) {
                if (!browseStop.stop_requested()) { browseLoading.clear(); uiMessage = e.what(); }
            }
        }
        if (!nextBrowse || browseJob.valid()) return;
        auto request = std::move(*nextBrowse);
        nextBrowse.reset();
        browseStop = std::stop_source{};
        auto snapshot = *library;
        snapshot.shared(relay.modelChoices());
        try {
            browseJob = std::async(std::launch::async, [request, snapshot=std::move(snapshot), stop=browseStop.get_token()]() mutable {
                BrowseResult result;
                if (request.kind != BrowseKind::Catalog) {
                    auto path = pickModelSource(request.kind == BrowseKind::ImportFolder, request.owner, stop);
                    if (!path || stop.stop_requested()) return result;
                    result.choice = snapshot.importSource(*path, stop);
                } else result.choice = snapshot.browse(request.source, stop);
                result.scanned = true;
                if (!result.choice.pack || stop.stop_requested()) return result;
                auto const& model = *result.choice.pack;
                auto info = std::make_shared<LibraryPreview>();
                info->source = result.choice.source;
                info->name = model.descriptor.at("metadata").value("name", model.id);
                info->languages = model.languages;
                auto const& properties = model.descriptor.at("properties");
                info->animation = properties.value("preview_animation", std::string{});
                info->fixedRotation = properties.value("disable_preview_rotation", false);
                info->texture = model.selectedTexture;
                std::set<std::string> unique;
                for (auto const& [group, doc] : model.animations.items()) if (group != "fp_arm")
                    for (auto const& [name, clip] : doc.at("animations").items())
                        if (unique.insert(name).second) info->clips.push_back(name);
                result.renderer = std::make_shared<ModelPreview>(model);
                result.info = std::move(info);
                result.choice.pack.reset();
                return result;
            });
        } catch (std::exception const& error) { browseLoading.clear(); uiMessage = error.what(); }
    }
    void refreshUi(IClientInstance& client) {
        updateCatalogCards();
        bool enabled = runtime.isActive();
        auto session = ui.updateHud(enabled, enabled && debugOverlay, uiSettings, uiPreview, pack ? pack->selectedTexture : 0);
        if (!session) return;
        UiSnapshot snapshot; snapshot.generation = uiGeneration; snapshot.settings = uiSettings;
        snapshot.locale = modelLocale(client.getOptions().getLanguage());
        snapshot.enabled = enabled; snapshot.loading = modelJob.has_value() || pendingModel.has_value() || queuedModel.has_value(); snapshot.message = uiMessage;
        snapshot.browsing = browseInfo; snapshot.browseLoading = browseLoading;
        snapshot.debugOverlay = snapshot.enabled && debugOverlay;
        snapshot.players = relay.playerRows(lastPlayer);
        snapshot.room = relay.roomInfo();
        if (library) {
            auto report = library->report();
            snapshot.source = library->source();
            for (auto const& row : report.at("catalog"))
                if (row.value("kind", "") != "server") snapshot.catalog.push_back(row);
            for (auto const& row : relay.catalogRows()) snapshot.catalog.push_back(row);
        }
        if (pack) {
            snapshot.descriptor = pack->descriptor;
            snapshot.language = modelLanguage(pack->languages, snapshot.locale);
            snapshot.texture = pack->currentTexture().name;
            for (auto const& texture : pack->textures) snapshot.textures.push_back(texture.name);
            for (auto const& [group, document] : pack->animations.items()) if (group != "fp_arm")
                for (auto const& [name, clip] : document.at("animations").items()) snapshot.clips.push_back(name);
            snapshot.values = animation.modelUiValues();
        }
        ui.updateMenu(client, *session, std::move(snapshot), browseRenderer);
    }
    void uiCommands(IClientInstance& client, Player& player) {
        for (auto const& c : ui.takeCommands()) {
            if (c.generation != uiGeneration) continue;
            try {
                if (c.kind == "browse") {
                    if(c.key.starts_with("server:")){
                        relay.roomCommand("select:"+c.key.substr(7));
                        if(!relay.modelChoices().contains(c.key)){uiMessage="正在优先下载此模型，完成后可预览";continue;}
                    }
                    requestBrowse(BrowseKind::Catalog, c.key);
                } else if(c.kind=="room"){
                    if(c.key=="folder" || c.key=="settings"){
                        auto path=self->getModDir()/(c.key=="folder"?"room-share":"room-sync.json");
                        auto result=ShellExecuteW(nullptr,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
                        if(reinterpret_cast<std::intptr_t>(result)<=32)uiMessage="无法打开房间文件";
                    } else relay.roomCommand(c.key);
                }
                else if (c.kind == "open_models_folder") {
                    if (!library) { uiMessage = "模型库尚未就绪"; continue; }
                    auto folder = std::filesystem::absolute(library->root());
                    std::filesystem::create_directories(folder);
                    auto result = ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    uiMessage = reinterpret_cast<std::intptr_t>(result) <= 32 ? "无法打开模型文件夹" : "";
                }
                else if (c.kind == "import_file" || c.kind == "import_folder") {
                    // A native dialog can temporarily move Bedrock to its pause screen.
                    // Keep the import alive and restore the menu when the HUD is available.
                    returnAfterPicker = true;
                    requestBrowse(c.kind == "import_file" ? BrowseKind::ImportFile : BrowseKind::ImportFolder);
                }
                else if (c.kind == "wear" && browseInfo && c.key == browseInfo->source && browseLoading.empty()) {
                    uiMessage.clear(); requestModelLoad(client, c.key, true);
                } else if (c.kind == "reload") {
                    if (library) requestBrowse(BrowseKind::Catalog, browseInfo ? browseInfo->source : library->source());
                }
                else if (c.kind == "toggle") toggleRequested = true;
                else if (c.kind == "texture") { invalidateModelLoad(); cycleTexture(client, player, c.key); }
                else if (c.kind == "extra") {
                    if (!runtime.isActive()) uiMessage = "请先启用模型";
                    else if (!animation.playExtra(c.key)) uiMessage = "模型没有提供这个动作片段";
                    else if (uiSettings.value("print_animation_roulette_msg", true)) client.getGuiData()->setActionBarMessage("额外动作：" + c.key, std::nullopt);
                } else if (c.kind == "stop") animation.stopExtra();
                else if (c.kind == "form") {
                    if (!runtime.isActive() || !animation.modelUiAction(c.key, c.value, c.label)) uiMessage = "模型设置未生效，请启用模型后重试";
                } else if (c.kind == "favorite") {
                    auto& list = uiSettings["favorites"]; auto i = std::find(list.begin(), list.end(), c.key);
                    if (i != list.end()) list.erase(i); else if (list.size() < 1024) list.push_back(c.key); saveUiSettings();
                } else if (c.kind == "peer_visibility" && !c.key.empty() && c.key.size() <= 64 &&
                    (c.key.starts_with("uuid:") || c.key.find_first_not_of("0123456789") == std::string::npos)) {
                    auto& list = uiSettings["hidden_players"];
                    auto found = std::find(list.begin(), list.end(), c.key);
                    if (found != list.end()) list.erase(found);
                    else if (list.size() < 1024) list.push_back(c.key);
                    relay.setVisibility(uiSettings.value("disable_other_models", false), list);
                    saveUiSettings();
                } else if (c.kind == "setting" && uiSettings.contains(c.key) && !uiSettings.at(c.key).is_array()) {
                    if (c.key == "sound_volume") uiSettings[c.key] = std::clamp(static_cast<int>(c.value), 0, 100);
                    else if (c.key=="hud_fps") uiSettings[c.key]=std::clamp(static_cast<int>(c.value),5,30);
                    else if (c.key.starts_with("hud_")) uiSettings[c.key] = std::clamp(static_cast<int>(c.value), 0, 100);
                    else uiSettings[c.key] = c.value != 0;
                    if (c.key == "arke_extra_emissive_layer") {
                        nextArkeCheck = {};
                        refreshArkeShader();
                    }
                    if (c.key == "disable_self_model") {
                        if (c.value != 0 && runtime.isActive()) { endModel(client, player); runtime.restore(client, player); }
                        else if (c.value == 0 && !runtime.isActive()) apply(client, player);
                    }
                    if (c.key == "disable_other_models")
                        relay.setVisibility(uiSettings.at(c.key).get<bool>(), uiSettings.at("hidden_players"));
                    targetResources.presentation(!uiSettings.value("disable_vehicle_model", false), !uiSettings.value("disable_projectile_model", false));
                    saveUiSettings();
                } else if (c.kind == "link" && c.key.size() < 2048 && (c.key.starts_with("https://") || c.key.starts_with("http://"))) {
                    // Only an explicit click on author-supplied HTTP(S) links.
                    auto wide = std::filesystem::path(std::u8string(c.key.begin(), c.key.end())).wstring();
                    ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            } catch (std::exception const& error) { uiMessage = "操作失败，当前模型已保留"; self->getLogger().warn("UI action failed: {}", error.what()); }
        }
    }
    void renderFirstPerson(std::function<void()> const& draw) {
        if (lastPlayer && uiSettings.value("disable_self_hands", false)) runtime.vanillaHands(*lastPlayer, draw); else draw();
    }
    bool suppressMenuMouseCapture(IClientInstance const& client) { return ui.suppressMouseCapture(client); }
    void geometryStatus(ClientSkin const& skin, SerializedSkinRef const& serialized) {
        Json report = {{"initialized", skin.mInitialized}, {"loading", skin.mIsLoadingGeometry},
            {"valid_texture", skin.mSkinInfoData->mValid},
            {"full_id", serialized.mSkinImpl->mObject.mFullId}};
        report["loaded_geometry"] = Json::array();
        if (auto group = skin.mClientSkinGeometryGroup) {
            for (auto const& [name, info] : *group->mGeometries) {
                if (!info || !info->mPtr) continue;
                auto const& geometry = *info->mPtr;
                std::size_t boxes = 0, meshParts = 0, meshFaces = 0;
                for (auto const& [bone, node] : *geometry.mNodes) {
                    boxes += node.mBoxes->size();
                    auto faces = node.mMesh->mTris->size() + node.mMesh->mQuads->size();
                    if (faces) { ++meshParts; meshFaces += faces; }
                }
                report["loaded_geometry"].push_back(Json{{"name", name.getString()},
                    {"bones", geometry.mNodes->size()}, {"boxes", boxes},
                    {"mesh_parts", meshParts}, {"mesh_faces", meshFaces},
                    {"texture_dimensions", {geometry.mTextureDimensions->x, geometry.mTextureDimensions->y}}});
            }
        }
        std::ofstream(self->getModDir() / "render-diagnostics.json") << report.dump(2);
    }
    void updateRenderTexture(ClientSkinInfoData& skin, SerializedSkinRef const& serialized) {
        try {
            if (auto client = ll::service::getClientInstance()) {
                relay.updateTexture(*client, skin, serialized);
                runtime.updateRenderTexture(*client, skin, serialized);
                animation.setRenderTexture(runtime.renderTexture(), runtime.textureName());
            }
        } catch (std::exception const& error) { self->getLogger().warn("Using fallback skin texture: {}", error.what()); }
    }
    void prepareGeometry(RenderController& controller, std::shared_ptr<DataDrivenGeometry> const& geometry, RenderParams& params) {
        animation.prepareGeometry(controller, geometry, params);
        relay.prepareGeometry(controller, geometry, params);
        if (targetRenderingEnabled) targetResources.prepareGeometry(controller, geometry, params);
    }
    std::span<BoneVisual const> boneVisuals(SkinnedMesh const& mesh, RenderController const* controller,
        std::span<BoneVisual> local, std::vector<BoneVisual>& overflow) {
        if (ActorDrawScope::current && animation.rendersPlayer(*ActorDrawScope::current))
            return animation.boneVisuals(mesh, controller, local, overflow, ActorDrawScope::current);
        if (ActorDrawScope::current && relay.rendersPlayer(*ActorDrawScope::current))
            return relay.visuals(mesh, controller, local, overflow, ActorDrawScope::current);
        if (targetRenderingEnabled) {
            auto target = targetResources.visuals(mesh, controller, local, overflow);
            if (!target.empty()) return target;
        }
        auto visuals = animation.boneVisuals(mesh, controller, local, overflow);
        return visuals.empty() ? relay.visuals(mesh, controller, local, overflow) : visuals;
    }
    bool needsIndividualSurfaces(Actor const& actor) const {
        if (auto* owner = shoulderParrotOwner(actor);
            owner && (animation.rendersPlayer(*owner) || relay.rendersPlayer(*owner))) return true;
        return animation.needsIndividualSurfaces(actor) || relay.needsSurfaces(actor) || (targetRenderingEnabled
            && (targetResources.hasActiveTarget(actor) || (actor.isPlayer() && actor.getVehicle()
                && targetResources.hasActiveTarget(*actor.getVehicle()))));
    }
    unsigned handDrawMask(Actor const& actor, AttachableSlotIndex slot, bool nativeUi) const {
        if (!actor.isPlayer() || FirstPersonRenderScope::isActive()) return 1;
        if (slot != AttachableSlotIndex::CarriedItem && slot != AttachableSlotIndex::OffhandItem) return 1;
        bool offhand = slot == AttachableSlotIndex::OffhandItem;
        if (runtime.isActive() && animation.rendersPlayer(actor)) return animation.handDrawMask(actor, offhand, nativeUi);
        return relay.handDrawMask(actor, offhand, nativeUi);
    }
    bool drivesLanceCharge(Actor const& actor) const {
        if (runtime.isActive() && animation.rendersPlayer(actor)) return animation.drivesLanceCharge(actor);
        return relay.drivesLanceCharge(actor);
    }
    bool drawParrot(BaseActorRenderContext& context, ActorRenderData& data, std::function<void()> const& draw) {
        auto* owner = data.mActor ? shoulderParrotOwner(*data.mActor) : nullptr;
        if (!owner) return false;
        observeShoulderPassengers(*owner, targetResources, relay.shoulderSeats(*owner));
        if (!animation.rendersPlayer(*owner)) return relay.drawParrot(*owner, context, data, draw);
        drawShoulderParrot(animation, *owner, context, data, draw);
        return true;
    }
    bool drawPassenger(Actor& actor, BaseActorRenderContext& context, std::function<void()> const& draw) {
        static thread_local Actor const* applying = nullptr;
        if (applying == &actor || !targetRenderingEnabled || !actor.isPlayer() || FirstPersonRenderScope::isActive()) return false;
        auto* vehicle = actor.getVehicle();
        auto& screen = static_cast<ScreenContext&>(context.mScreenContext);
        if (!vehicle || screen.isDrawingUI || animation.rendersPlayer(actor) || relay.rendersPlayer(actor)) return false;
        auto alpha = context.mFrameAlpha;
        auto matrix = targetResources.passengerTransform(actor, *vehicle, alpha, false);
        if (!matrix) return false;
        auto pos = actor.getInterpolatedPosition(std::clamp(alpha, 0.f, 1.f));
        auto camera = *context.mImpl->mCameraPosition;
        glm::vec3 pivot(pos.x - camera.x, pos.y - camera.y + actor.getFeetPos().y - actor.getPosition().y, pos.z - camera.z);
        auto& stack = static_cast<mce::Camera&>(screen.camera).worldMatrixStack;
        auto scope = stack->push(false);
        auto& current = *stack->getTop()._m;
        current = glm::translate(current, pivot) * *matrix * glm::translate(glm::mat4(1), -pivot);
        struct Restore { Actor const*& slot; Actor const* old; ~Restore() { slot = old; } };
        Restore guard{applying, std::exchange(applying, &actor)};
        draw();
        return true;
    }
    bool suppressNativeGlideRotation(Actor const& actor, bool firstPerson, std::array<float, 2> const& rotation) {
        return (runtime.isActive() && animation.suppressNativeGlideRotation(actor, firstPerson, rotation))
            || relay.suppressGlide(actor, firstPerson, rotation);
    }
    void bindResourceTexture(SerializedSkinRef const& skin, std::shared_ptr<ActorResourceDefinition> const& definition) {
        runtime.bindResourceTexture(skin, definition);
        relay.bindTexture(skin, definition);
    }
    void frameUpdate(IClientInstance& client) {
        ui.maintainMouse(client);
        ui.beginFrame();
        relay.frame();
        if (targetRenderingEnabled)
            targetResources.preparePeerArrows(client, self->getModDir() / "target-render-probe-status.json");
        if (runtime.isActive()) {
            bool inGame = client.getTopScreenName() == "hud_screen" && !ui.isOpen();
            if (client.getTopScreenName() != "hud_screen") keys.reset();
            animation.setHostInputs(keys.glfwKeys(), inGame, modVersions, true);
            animation.setPaused(client.isShowingPauseScreen() && client.isEligibleForPauseFeature());
        }
    }
    void reportFirstPersonPass(bool rendered) {
        if (runtime.isActive()) animation.reportFirstPersonPass(rendered);
    }
    void prepareAnimationForRender(ActorRenderData& data, AnimationComponent& component) {
        if (data.mActor && data.mActor->isPlayer() && !data.mIsInUI) observeShoulderPassengers(*data.mActor, targetResources, relay.shoulderSeats(*data.mActor));
        relay.prepareForRender(data, component);
        if (targetRenderingEnabled) targetResources.prepareFrame(*component.mRenderParams,
            self->getModDir() / "target-render-probe-status.json");
        if (runtime.isActive() && lastPlayer && (data.mIsInUI || UiRenderContextTracker::isActive()) && uiRenderContexts.size() < 8) {
            uiRenderContexts.push_back({{"same_actor", data.mActor == lastPlayer},
                {"native_ui_flag", static_cast<bool>(data.mIsInUI)}, {"ui_context", UiRenderContextTracker::isActive()},
                {"same_component", &component == &lastPlayer->getAnimationComponent()},
                {"component_initialized", static_cast<bool>(component.mAnimationComponentInitialized)}});
            std::ofstream(self->getModDir() / "render-contexts.json") << Json{{"ui", uiRenderContexts}}.dump(2);
        }
        if (!runtime.isActive() || !lastPlayer || data.mActor != lastPlayer
            || !lastPlayer->mSkin || !lastPlayer->mSkin->mSkinImpl->mObject.mId->starts_with("ysm.client.")) return;
        animation.prepareForRender(*lastPlayer, component);
    }
    std::shared_ptr<DataDrivenModel> resolveTargetModel(ActorRenderData& data, std::shared_ptr<DataDrivenModel> model) {
        if (!targetRenderingEnabled || !data.mActor) return model;
        targetResources.observeProjectile(*data.mActor);
        if (data.mActor->isPlayer()) targetResources.rememberPrototype(model);
        if (auto client = ll::service::getClientInstance())
            return targetResources.resolveModel(*data.mActor, std::move(model), *client,
                self->getModDir() / "target-render-probe-status.json");
        return model;
    }
    std::shared_ptr<ActorRenderer> resolveTargetRenderer(Actor& actor, std::shared_ptr<ActorRenderer> renderer) {
        if (targetRenderingEnabled) {
            targetResources.observeProjectile(actor);
            if (auto client = ll::service::getClientInstance())
                renderer = targetResources.resolveRenderer(actor, std::move(renderer), *client,
                    self->getModDir() / "target-render-probe-status.json");
        }
        targetResources.observeRenderRoute(actor, targetRenderingEnabled);
        return renderer;
    }
    bool usesTargetRendering() const { return targetRenderingEnabled; }
    void entityLoaded(Actor const& actor, CompoundTag const& data) { targetResources.entityLoaded(actor, data); }
    void entitySaving(Actor const& actor, CompoundTag& data) { targetResources.entitySaving(actor, data); }
    void observeTargetMount(Actor const& actor) {
        if (targetRenderingEnabled) targetResources.observeMount(actor);
    }
    void recordProjectileOwner(Actor const& actor, ActorUniqueID const& owner) {
        if (targetRenderingEnabled && (actor.getEntityContext().tryGetComponent<ProjectileComponent>()
            || actor.hasType(ActorType::FishingHook) || actor.hasType(ActorType::FireworksRocket)
            || actor.hasType(ActorType::Projectile)))
            targetResources.projectileOwnerSet(actor, owner.rawID);
    }
    void publishSpawnedArrow(Actor* arrow, Actor* shooter) {
        if (!arrow || !shooter || !shooter->isPlayer() || arrow->getLevel().isClientSide()
            || &arrow->getLevel() != &shooter->getLevel()
            || arrow->getActorIdentifier().mCanonicalName->getString() != "minecraft:arrow") return;
        auto& player = static_cast<Player&>(*shooter);
        auto* launch = ArrowLaunchScope::current;
        auto weapon = launch && launch->player == &player ? launch->crossbow : std::nullopt;
        if (!weapon) weapon = ArrowLaunchScope::weapon(*player.mItemInUse->mItem);
        if (!weapon) weapon = ArrowLaunchScope::weapon(player.getCarriedItem());
        if (!weapon) weapon = ArrowLaunchScope::weapon(player.getOffhandSlot());
        if (!weapon) return;
        auto id = arrow->getOrCreateUniqueID().rawID;
        relay.recordServerArrow(player, id, *weapon);
        static thread_local std::int64_t last = 0;
        static thread_local unsigned samples = 0;
        if (last != id && samples < 16) {
            last = id;
            ++samples;
            self->getLogger().info("Room arrow launch entity={} owner={} host={} item={}", id,
                player.getOrCreateUniqueID().rawID, bool(player.mIsHostingPlayer), *weapon ? "crossbow" : "bow");
        }
    }
    void recordProjectileShot(Actor& actor, ProjectileComponent const& component) {
        targetResources.projectileOwnerSet(actor, component.mOwnerId->rawID);
        if (actor.getLevel().isClientSide()) return;
        auto* launch = ArrowLaunchScope::current;
        auto* shooter = launch ? static_cast<Actor*>(launch->player) : nullptr;
        if (!shooter) shooter = actor.getLevel().fetchEntity(*component.mOwnerId, false);
        if (!shooter) shooter = actor.getLevel().fetchEntity(actor.getOwnerId(), false);
        publishSpawnedArrow(&actor, shooter);
    }
    void recordClientActorSpawn(Actor const& actor, LocalPlayer const* localPlayer) {
        if (targetRenderingEnabled) targetResources.projectileClientSpawned(actor, localPlayer);
    }
    bool drawTarget(ActorRenderer& original, BaseActorRenderContext& context, ActorRenderData& data) {
        static thread_local unsigned depth = 0;
        if (depth || !data.mActor || !targetRenderingEnabled) return false;
        // A renderer retained by the game can enter draw without a fresh
        // dispatcher lookup after the host confirmed the owner.
        if (data.mActor->hasType(ActorType::AbstractArrow)) targetResources.observeProjectile(*data.mActor);
        if (!targetResources.hasActiveTarget(*data.mActor)) return false;
        auto borrowed = std::shared_ptr<ActorRenderer>(&original, [](ActorRenderer*) {});
        auto replacement = resolveTargetRenderer(*data.mActor, borrowed);
        if (replacement == borrowed) return false;
        struct DrawScope {
            unsigned& depth;
            explicit DrawScope(unsigned& value) : depth(value) { ++depth; }
            ~DrawScope() { --depth; }
        } scope(depth);
        replacement->render(context, data);
        return true;
    }
    void probeActorRenderer(Actor& actor, std::shared_ptr<ActorRenderer> const& renderer) {
        if (!targetRenderingEnabled || !renderer || targetRendererTypes.size() >= 32) return;
        auto id = actor.getActorIdentifier().mCanonicalName->getString();
        if (!targetRendererTypes.insert(id).second) return;
        auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"Minecraft.Windows.exe"));
        auto rva = [&](void const* object) -> Json {
            if (!object) return nullptr;
            auto value = *reinterpret_cast<std::uintptr_t const*>(object);
            return value >= base ? Json(value - base) : Json();
        };
        targetRendererReports.push_back({{"entity", id}, {"renderer_vtable_rva", rva(renderer.get())},
            {"model_vtable_rva", rva(renderer->mModel.get())},
            {"target_match", pack && targetResources.hasActiveTarget(actor)}});
        std::ofstream(self->getModDir() / "target-renderer-routes.json") << targetRendererReports.dump(2);
    }
    void resetRoamingConnection() { roaming.resetConnection(); roamingLevel = nullptr; clearShoulderPassengers(); }
    RoamingInstallationId drainRoaming(AnimationRuntime& source, bool retire = false) {
        auto update = source.takeRoamingChanges(retire);
        if (update.installation) roaming.adopt(update.installation, update.delta);
        return update.installation;
    }
    void recordAudio(IClientInstance& client, SoundCue const& cue, std::string const& result) {
        if (audioHistory.size() >= 64) return;
        auto engine = client.getSoundEngine();
        Json active = Json::array();
        for (auto const& voice : voices) {
            Json report{{"handle", voice.handle}, {"owner", voice.owner},
            {"id", voice.id ? Json(*voice.id) : Json()}, {"first_person", voice.firstPerson},
            {"looping", voice.looping}, {"playing", engine->isPlayingSound(voice.handle)},
            {"observed_playing", voice.observedPlaying}, {"event", voice.event},
            {"origin", voice.playback.position}, {"authored_volume", voice.playback.authoredVolume},
            {"volume", voice.playback.volume}};
            if (voice.lastUpdate.position) report["last_sampled_channel_position"] = *voice.lastUpdate.position;
            if (voice.lastUpdate.volume) report["last_sampled_channel_volume"] = *voice.lastUpdate.volume;
            if (auto attributes = engine->tryGetPlayingSoundAttributes(voice.handle))
                report["engine"] = {{"spatial", static_cast<bool>(attributes->mIs3D)},
                    {"pitch", static_cast<float>(attributes->mPitch)}, {"volume", static_cast<float>(attributes->mVolume)},
                    {"min_distance", static_cast<float>(attributes->mMinDistance)},
                    {"max_distance", static_cast<float>(attributes->mMaxDistance)}};
            active.push_back(std::move(report));
        }
        audioHistory.push_back({{"sound", cue.name}, {"owner", cue.owner}, {"id", cue.id ? Json(*cue.id) : Json()},
            {"stop_id", cue.stopId}, {"force", cue.force}, {"result", result}, {"voices", std::move(active)}});
        std::ofstream(self->getModDir() / "sound-status.json") << audioHistory.dump(2);
    }
    void stopModelAudio(IClientInstance& client, bool all = false) {
        if (!voices.empty()) {
            auto engine = client.getSoundEngine();
            auto stopped = std::erase_if(voices, [&](auto const& voice) {
                if (!all && voice.source) return false;
                engine->stop(voice.handle);
                return true;
            });
            if (stopped) {
                self->getLogger().info("Stopped {} model sound handles", stopped);
                recordAudio(client, {"*", {}}, "model_stopped");
            }
        }
        missingSounds.clear();
        reportedSounds.clear();
    }
    void updateModelAudio(IClientInstance& client) {
        if (voices.empty()) return;
        auto engine = client.getSoundEngine();
        std::vector<std::pair<SoundCue, char const*>> reports;
        int percent = uiSettings.value("sound_volume", 100);
        std::erase_if(voices, [&](Voice& voice) {
            auto context = voice.entity.lock();
            auto actor = context.tryUnwrap<Actor>();
            if (!actor || (voice.source && voice.source->expired())) {
                engine->stop(voice.handle);
                return true;
            }
            if (!engine->isPlayingSound(voice.handle)) return voice.observedPlaying;
            auto report = [&](char const* result) {
                SoundCue cue{voice.owner, voice.event, voice.firstPerson};
                cue.id = voice.id;
                reports.emplace_back(std::move(cue), result);
            };
            if (!voice.observedPlaying) { voice.observedPlaying = true; report("started"); }
            auto feet = actor->getFeetPos();
            voice.playback.follow({feet.x, feet.y, feet.z}, percent, [&](auto* pos, float const* gain) {
                bool sample = audioHistory.size() < 64 && (gain || !voice.reportedMove);
                if (!updatePlayingSound(**engine->mSoundSystem, voice.handle, pos, gain, sample ? &voice.lastUpdate : nullptr)) return false;
                if (sample) report("updated");
                if (pos) voice.reportedMove = true;
                return true;
            });
            return false;
        });
        // A valid game handle may still be loading. Keep its author ownership
        // until playback completes or an explicit scope/ID stop cancels it.
        for (auto const& [cue, result] : reports) recordAudio(client, cue, result);
    }
    void playModelAudio(IClientInstance& client, ModelPack const& model, WeakEntityRef const& entity, std::span<SoundCue const> cues,
        std::shared_ptr<void const> const& source = {}) {
        if (cues.empty()) return;
        auto context = entity.lock();
        auto actor = context.tryUnwrap<Actor>();
        if (!actor) return;
        auto origin = actor->getFeetPos();
        auto engine = client.getSoundEngine();
        auto matchesOwner = [&](Voice const& voice, SoundCue const& cue) {
            if (voice.owner != cue.owner || voice.firstPerson != cue.firstPerson) return false;
            return source ? (voice.source && voice.source->lock() == source) : !voice.source;
        };
        for (auto const& cue : cues) {
            if (cue.name.empty()) {
                auto previousCount = voices.size();
                std::erase_if(voices, [&](Voice const& voice) {
                    if (!matchesOwner(voice, cue)) return false;
                    if (cue.stopId && (!cue.id || voice.id != cue.id)) return false;
                    engine->stop(voice.handle);
                    if (reportedSounds.size() < 16 && reportedSounds.insert("stop:" + voice.owner).second)
                        self->getLogger().info("Stopped animation sound handle={} playing={}", voice.handle, engine->isPlayingSound(voice.handle));
                    return true;
                });
                if (voices.size() != previousCount || cue.stopId) recordAudio(client, cue, "stopped");
                continue;
            }
            auto gameSound = resolveGameSound(cue.name);
            auto found = model.sounds.find(cue.name);
            if (!gameSound && (cue.name.find(':') != std::string::npos || found == model.sounds.end())) {
                if (missingSounds.size() < 64 && missingSounds.insert(cue.name).second)
                    self->getLogger().warn("Unresolved animation sound: {}", cue.name);
                recordAudio(client, cue, "missing");
                continue;
            }
            if (cue.id) {
                auto old = std::find_if(voices.begin(), voices.end(), [&](Voice const& voice) {
                    return matchesOwner(voice, cue) && voice.id == cue.id;
                });
                if (old != voices.end()) {
                    if (!cue.force) { recordAudio(client, cue, "duplicate"); continue; }
                    engine->stop(old->handle);
                    voices.erase(old);
                }
            }
            if (voices.size() >= 256) { recordAudio(client, cue, "voice_limit"); continue; }
            auto localVolume = PlayingSoundState::scaledVolume(cue.volume, uiSettings.value("sound_volume", 100));
            uint64 handle = UINT64_MAX;
            if (gameSound) {
                handle = engine->play(PlaySoundOptions{gameSound->name, origin, localVolume,
                    cue.pitch * gameSound->pitchMultiplier, cue.looping ? -1 : 0, std::nullopt,
                    false, std::nullopt, SoundPriority::Default});
            } else {
                auto const& clip = *found->second;
                auto const& bytes = clip.wave.empty() ? clip.encoded : clip.wave;
                auto extension = clip.wave.empty() ? ".ogg" : ".wav";
                Core::Path path("sounds/ysm_bedrock/" + model.localContentKey + "/" + cue.name + extension);
                auto& system = **engine->mSoundSystem;
                if (system.loadFromMemory(path, reinterpret_cast<char const*>(bytes.data()), bytes.size()))
                    handle = system.playAt(path, nullptr, "", system.getNewSoundHandle(), origin.x, origin.y, origin.z,
                        localVolume, cue.pitch, true, 1.f, 16.f, false, false, std::nullopt, cue.looping ? -1 : 0, 1.f);
            }
            bool playing = engine->isPlayingSound(handle);
            bool pending = gameSound && handle != UINT64_MAX && !playing;
            if (playing || pending) voices.push_back({handle, cue.owner, cue.firstPerson,
                gameSound ? nullptr : found->second, cue.id, cue.looping, playing, entity,
                {cue.volume, {origin.x, origin.y, origin.z}, localVolume}, false,
                gameSound ? gameSound->name : cue.name,
                source ? std::optional(std::weak_ptr(source)) : std::nullopt, {}});
            recordAudio(client, cue, playing ? "playing" : pending ? "pending" : "rejected");
            if (reportedSounds.size() < 16 && reportedSounds.insert(cue.name).second)
                self->getLogger().info("Animation sound {} source={} handle={} playing={}", cue.name,
                    gameSound ? gameSound->name : found->second->info.opus ? "Opus" : "Vorbis", handle, playing);
        }
    }
    void endModel(IClientInstance& client, Player& player) {
        ++uiGeneration;
        if (targetRenderingEnabled) targetResources.clearOwner(player.getOrCreateUniqueID().rawID);
        stopModelAudio(client);
        animation.suspend(player);
        roaming.modelEnded(drainRoaming(animation));
        animation.detach(player);
    }
    void dispatchAnimationEffects(IClientInstance& client, Player& player) {
        updateModelAudio(client);
        std::vector<ParticleCue> particles;
        std::size_t remaining = 4096;
        auto appendParticles = [&](std::vector<ParticleCue> cues) {
            for (auto& cue : cues) {
                if (cue.spawns.size() > remaining) continue;
                remaining -= cue.spawns.size();
                particles.push_back(std::move(cue));
            }
        };
        if (runtime.isActive() && pack) {
            playModelAudio(client, *pack, player.getEntityContext().getWeakRef(), animation.takeSoundCues());
            appendParticles(animation.takeParticleCues());
        }
        auto appendEffects = [&](std::vector<ModelEffects> batches) {
            for (auto& batch : batches) {
                playModelAudio(client, *batch.model, batch.entity, batch.sounds, batch.owner);
                appendParticles(std::move(batch.particles));
            }
        };
        appendEffects(relay.takeEffects(player));
        if (targetRenderingEnabled) appendEffects(targetResources.takeEffects());
        batchCampfireSmoke(particles);
        dispatchModelParticles(client, particles);
        static auto lastParticleReport = std::chrono::steady_clock::time_point{};
        auto now = std::chrono::steady_clock::now();
        if (debugOverlay && now - lastParticleReport > std::chrono::seconds(1)) {
            auto report = particleRenderReport();
            if (report.at("requested").get<std::uint64_t>() > 0)
                std::ofstream(self->getModDir() / "particle-status.json") << report.dump(2);
            lastParticleReport = now;
        }
    }
    void tick(IClientInstance& client) {
        auto* player = client.getLocalPlayer();
        if (!player) {
            if (lastPlayer) { resetBrowse(); resetModelParticleTransitions(); }
            relay.tick(client, nullptr, nullptr, animation, targetResources);
            ui.close(&client); queuedModel.reset();
            stopModelAudio(client, true);
            keys.reset();
            if (lastPlayer || pendingModel) invalidateModelLoad();
            if (lastPlayer || roamingLevel) resetRoamingConnection();
            animation.reset(true); runtime.reset(); targetResources.reset(); lastPlayer = nullptr; applyAfter.reset();
            toggleRequested = false;
            pollModelLoad(client, nullptr);
            return;
        }
        refreshArkeShader();
        auto* level = &player->getLevel();
        if (player != lastPlayer || roamingLevel != level) {
            ui.close(&client); resetBrowse(); queuedModel.reset(); ++uiGeneration;
            stopModelAudio(client, true);
            keys.reset();
            uiRenderContexts = Json::array();
            invalidateModelLoad();
            if (roamingLevel && roamingLevel == level) roaming.modelEnded(drainRoaming(animation, true));
            else { animation.takeRoamingChanges(true); resetRoamingConnection(); }
            animation.reset(true);
            runtime.reset();
            targetResources.reset();
            auto server = ll::service::getMinecraft(false);
            targetResources.attachAuthority(server ? server->getLevel() : nullptr, level);
            lastPlayer = player;
            roamingLevel = level;
            applyAfter = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            status("waiting_for_player_skin");
        }
        if (applyAfter && std::chrono::steady_clock::now() >= *applyAfter) { applyAfter.reset(); if (!uiSettings.value("disable_self_model", false)) apply(client, *player); }
        if (ui.isOpen() && client.getTopScreenName() != "hud_screen") ui.close(&client);
        if (!returnAfterPicker && !ui.isOpen() && (!browseLoading.empty() || browseInfo)) resetBrowse();
        uiCommands(client, *player);
        pollBrowse();
        if (returnAfterPicker && !nextBrowse && !browseJob.valid() && client.getTopScreenName() == "hud_screen") {
            if (!ui.isOpen()) ui.toggle(client, false);
            returnAfterPicker = false;
        }
        if (runtime.isActive()) {
            drainRoaming(animation);
            animation.setPaused(client.isShowingPauseScreen() && client.isEligibleForPauseFeature());
            animation.attach(*player);
            if (uiSettings.value("show_hud_avatar", false)) {
                observeShoulderPassengers(*player, targetResources);
            }
        }
        // Menu transitions may consume a key release. Forget modifiers
        // before gameplay resumes rather than carrying a stuck shortcut.
        if (client.getTopScreenName() != "hud_screen") keys.reset();
        if (toggleRequested.exchange(false)) {
            if (client.getTopScreenName() != "hud_screen") return;
            invalidateModelLoad();
            applyAfter.reset();
            self->getLogger().info("Model visibility changed from menu");
            if (runtime.isActive()) { endModel(client, *player); runtime.restore(client, *player); uiSettings["disable_self_model"] = true; status("restored"); }
            else { uiSettings["disable_self_model"] = false; apply(client, *player); }
            saveUiSettings();
        }
        pollModelLoad(client, player);
        if (queuedModel && !modelJob && client.getTopScreenName() == "hud_screen") { auto target = std::move(*queuedModel); queuedModel.reset(); requestModelLoad(client, std::move(target.source), target.wear); }
        if (targetRenderingEnabled) {
            if (library) targetResources.resolveSavedModels(*library);
            targetResources.collectRetired();
        }
        if (targetRenderingEnabled && runtime.isActive() && pack) {
            targetResources.updateOwner(player->getOrCreateUniqueID().rawID, *pack, animation.roamingSnapshot());
            animation.trackSeats(targetStorage);
            targetResources.observeMount(*player);
        }
        static auto lastAssignmentReport = std::chrono::steady_clock::time_point{};
        if (auto now = std::chrono::steady_clock::now(); debugOverlay && now - lastAssignmentReport > std::chrono::seconds(1)) {
            std::ofstream(self->getModDir() / "target-assignments-status.json") << targetResources.assignmentReport(targetRenderingEnabled).dump(2);
            lastAssignmentReport = now;
        }
        if (runtime.isActive() && !ui.isOpen() && client.getTopScreenName() == "hud_screen") {
            if (auto input=ClientMoveInputHandler::getMoveInput(client)) {
                auto const& flags=*input->mInputState->mFlagValues;
                animation.updateExtraMovement(uiSettings.value("movement_lock",false),input->mMove->x,input->mMove->y,
                    flags.test(static_cast<int>(MoveInputState::Flag::JumpInputCurrentlyDown)) ||
                        flags.test(static_cast<int>(MoveInputState::Flag::JumpDown)),
                    flags.test(static_cast<int>(MoveInputState::Flag::SneakInputCurrentlyDown)) ||
                        input->mFlagValues->test(static_cast<int>(MoveInputComponent::Flag::Sneaking)));
            }
        }
        relay.tick(client, player, runtime.isActive() && pack ? &*pack : nullptr, animation, targetResources);
        if (targetRenderingEnabled) relay.updateEntityOwners(targetResources, *player);
        if (runtime.isActive() && library && !relay.canSelect(library->source())) {
            endModel(client, *player);
            runtime.restore(client, *player);
            uiMessage = "服务器模型已离线";
        }
        dispatchAnimationEffects(client, *player);
        refreshUi(client);
    }
    void cycleTexture(IClientInstance& client, Player& player, std::string const& selected = {}) {
        if (!pack || pack->textures.empty()) return;
        auto previous = pack->selectedTexture;
        if (selected.empty()) pack->selectedTexture = (previous + 1) % pack->textures.size();
        else if (!pack->selectTexture(selected)) throw std::runtime_error("Unknown UI texture");
        try {
            if (runtime.isActive()) {
                runtime.apply(client, player, *pack, {}, animation.skinGeometry());
                animation.setRenderTexture(runtime.renderTexture(), runtime.textureName());
                animation.attach(player);
            }
        } catch (std::exception const& error) {
            pack->selectedTexture = previous;
            if (runtime.isActive()) {
                animation.setRenderTexture(runtime.renderTexture(), runtime.textureName());
                animation.attach(player);
            }
            else endModel(client, player);
            self->getLogger().error("Cannot switch texture: {}", error.what());
            client.getGuiData()->setActionBarMessage("纹理切换失败，保留原纹理", std::nullopt);
            return;
        }
        auto const& name = pack->currentTexture().name;
        client.getGuiData()->setActionBarMessage("纹理：" + name, std::nullopt);
        std::ofstream(self->getModDir() / "inspection.json") << pack->summary().dump(2);
        try {
            if (library) { library->rememberTexture(*pack); library->save(); }
        } catch (std::exception const& error) {
            self->getLogger().error("Texture applied but selection could not be saved: {}", error.what());
            client.getGuiData()->setActionBarMessage("纹理已切换，但选择保存失败", std::nullopt);
        }
        self->getLogger().info("Selected texture: {}", name);
    }
    void apply(IClientInstance& client, Player& player) {
        if (library && !relay.canSelect(library->source())) return;
        if (!pack) return;
        try {
            auto next = startupAnimation ? std::exchange(startupAnimation, {})
                : AnimationRuntime::prepare(*pack, animationDiagnostics());
            publishModel(client, player, *pack, std::move(next));
            self->getLogger().info("Applied YSM geometry and texture to local player");
            status("skin_submitted");
        } catch (std::exception const& error) {
            self->getLogger().error("Cannot apply model: {}", error.what());
            status(std::string("failed: ") + error.what());
        }
    }
    void invalidateModelLoad() {
        ++selectionEpoch;
        if (modelJob) modelJob->cancellation.request_stop();
        pendingModel.reset();
        queuedModel.reset();
    }
    void requestModelLoad(IClientInstance& client, std::string target = {}, bool wear = false) {
        if (!library) return;
        if (modelJob || pendingModel) {
            if (!target.empty()) { invalidateModelLoad(); queuedModel = LoadRequest{std::move(target), wear}; return; }
            client.getGuiData()->setActionBarMessage("正在读取模型，请稍候", std::nullopt);
            return;
        }
        std::stop_source cancellation;
        ++selectionEpoch;
        auto token = cancellation.get_token();
        auto snapshot = *library;
        snapshot.shared(relay.modelChoices());
        auto diagnostics = animationDiagnostics();
        try {
            auto task = std::async(std::launch::async, [snapshot = std::move(snapshot), token, diagnostics, wear, target = std::move(target)]() mutable {
                PreparedSelection result;
                result.wear = wear;
                auto validate = [&](ModelPack const& model) {
                    result.animations = AnimationRuntime::prepare(model, diagnostics);
                    result.preview = std::make_shared<ModelPreview>(model, AnimationRuntime::preparedBodySampler(result.animations));
                };
                result.choice = !target.empty() ? snapshot.select(target, validate, token)
                    : snapshot.source().empty() ? snapshot.initial(validate, token) : snapshot.next(validate, token);
                if (result.choice.pack) {
                    for (auto const& message : AnimationRuntime::preparedDiagnostics(result.animations)) result.choice.diagnostics.push_back(message);
                    snapshot.accept(result.choice);
                    result.library = std::move(snapshot);
                } else { result.animations.reset(); }
                return result;
            });
            modelJob.emplace(ModelJob{std::move(task), std::move(cancellation), selectionEpoch});
            client.getGuiData()->setActionBarMessage("正在读取下一个模型…", std::nullopt);
        } catch (std::exception const& error) {
            self->getLogger().error("Cannot start model preparation: {}", error.what());
            client.getGuiData()->setActionBarMessage("模型读取未能开始", std::nullopt);
        }
    }
    void pollModelLoad(IClientInstance& client, Player* player) {
        if (modelJob && modelJob->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto epoch = modelJob->epoch;
            try {
                auto candidate = modelJob->future.get();
                modelJob.reset();
                if (epoch == selectionEpoch && player && player == lastPlayer && !candidate.choice.cancelled) {
                    if (candidate.choice.pack) pendingModel = std::move(candidate);
                    else {
                        for (auto const& message : candidate.choice.diagnostics) self->getLogger().warn("{}", message);
                        library->observe(candidate.choice);
                        uiMessage = "模型读取失败，保留当前模型";
                        std::ofstream(self->getModDir() / "model-library.json") << library->report().dump(2);
                        if (client.getTopScreenName() == "hud_screen") client.getGuiData()->setActionBarMessage("没有其他可用模型", std::nullopt);
                    }
                }
            } catch (std::exception const& error) {
                modelJob.reset();
                self->getLogger().error("Model preparation failed: {}", error.what());
                if (epoch == selectionEpoch && player && client.getTopScreenName() == "hud_screen")
                    client.getGuiData()->setActionBarMessage("模型读取失败，保留当前选择", std::nullopt);
            }
        }
        if (!pendingModel || !player || player != lastPlayer || client.getTopScreenName() != "hud_screen") return;
        auto candidate = std::move(*pendingModel);
        pendingModel.reset();
        if (!relay.canSelect(candidate.choice.source)) { uiMessage = "服务器模型已离线"; return; }
        try {
            publishModel(client, *player, *candidate.choice.pack, std::move(candidate.animations), candidate.wear);
        } catch (std::exception const& error) {
            self->getLogger().error("Cannot publish prepared model: {}", error.what());
            client.getGuiData()->setActionBarMessage("模型启用失败，保留原选择", std::nullopt);
            return;
        }
        library = std::move(candidate.library);
        pack = std::move(candidate.choice.pack);
        uiPreview = std::move(candidate.preview); resetBrowse(); uiMessage.clear();
        if (candidate.wear && uiSettings.value("disable_self_model", false)) {
            uiSettings["disable_self_model"] = false;
            saveUiSettings();
        }
        startupAnimation.reset();
        applyAfter.reset();
        for (auto const& message : candidate.choice.diagnostics) self->getLogger().warn("{}", message);
        try {
            std::ofstream(self->getModDir() / "inspection.json") << pack->summary().dump(2);
            std::ofstream(self->getModDir() / "model-library.json") << library->report().dump(2);
            status("model_selected");
        } catch (std::exception const& error) {
            self->getLogger().warn("Model applied but diagnostics could not be recorded: {}", error.what());
        }
        client.getGuiData()->setActionBarMessage("模型：" + library->source(), std::nullopt);
        try { library->save(); }
        catch (std::exception const& error) {
            self->getLogger().error("Model applied but selection could not be saved: {}", error.what());
            client.getGuiData()->setActionBarMessage("模型已切换，但选择保存失败", std::nullopt);
        }
    }
    void publishModel(IClientInstance& client, Player& player, ModelPack const& incoming, AnimationRuntime::Prepared prepared, bool wear = false) {
        AnimationRuntime previous;
        bool wasActive = runtime.isActive(), publishedSkin = false, swapped = false;
        animation.suspend(player);
        try {
            drainRoaming(animation);
            auto info = AnimationRuntime::roamingPreparation(prepared);
            // The port currently has a LOCAL model session (no YSM channel).
            // Establish its baseline only after native publication succeeds.
            auto restore = roaming.prepare(incoming.localContentKey, std::move(info.installation), std::move(info.fields), true);
            AnimationRuntime::restoreRoaming(prepared, restore.values());
            previous.start(std::move(prepared));
            animation.swap(previous);
            swapped = true;
            animation.setHostInputs(keys.glfwKeys(), client.getTopScreenName() == "hud_screen", modVersions);
            // Register before skin publication, then reconcile a component
            // rebuilt by the native skin system. Native objects stay on this thread.
            animation.attach(player);
            runtime.apply(client, player, incoming, {}, animation.skinGeometry());
            publishedSkin = true;
            // Different packages can share geometry and pixels. The skin fast
            // path then has no upload hook to populate this fresh animation owner.
            animation.setRenderTexture(runtime.renderTexture(), runtime.textureName());
            animation.attach(player);
            roaming.activate(std::move(restore));
            if (targetRenderingEnabled) {
                targetResources.observeMount(player);
                targetResources.updateOwner(player.getOrCreateUniqueID().rawID, incoming, animation.roamingSnapshot());
                targetResources.observeMount(player);
            }
            stopModelAudio(client);
            if (!wear && uiSettings.value("disable_self_model", false)) { endModel(client, player); runtime.restore(client, player); }
            ++uiGeneration;
        } catch (...) {
            if (publishedSkin) {
                try {
                    if (wasActive && pack) runtime.apply(client, player, *pack, {}, previous.skinGeometry());
                    else runtime.restore(client, player);
                } catch (...) { runtime.reset(); }
            }
            if (swapped) {
                animation.detach(player);
                animation.swap(previous);
            }
            if (runtime.isActive()) {
                animation.setRenderTexture(runtime.renderTexture(), runtime.textureName());
                animation.attach(player);
            } else { roaming.modelEnded(drainRoaming(animation, true)); animation.reset(); }
            throw;
        }
    }
    bool disable() {
        if (auto client = ll::service::getClientInstance()) ui.close(&*client);
        NativeMenuTouch::shutdown();
        if (mouseListener) ll::event::EventBus::getInstance().removeListener(mouseListener);
        if (uiListener) ll::event::EventBus::getInstance().removeListener(uiListener);
        mouseListener.reset(); uiListener.reset(); queuedModel.reset();
        if (keyListener) ll::event::EventBus::getInstance().removeListener(keyListener);
        keyListener.reset();
        for (auto const& listener : weatherListeners) ll::event::EventBus::getInstance().removeListener(listener);
        weatherListeners.clear();
        keys.reset();
        invalidateModelLoad();
        // No frame waits for loading. Unload must join the worker before this
        // DLL's code and the CPU resources it owns can be released.
        if (modelJob) { modelJob->future.wait(); modelJob.reset(); }
        resetBrowse();
        if (browseJob.valid()) { browseJob.wait(); browseJob = {}; }
        ui.retireLibraryPreview();
        if (cardRead) { cardRead->future.wait(); cardRead.reset(); }
        if (auto client = ll::service::getClientInstance()) relay.stop(&*client);
        else relay.stop(nullptr);
        if (auto client = ll::service::getClientInstance()) {
            stopModelAudio(*client, true);
            if (auto* player = client->getLocalPlayer(); player && player == lastPlayer) {
                animation.detach(*player);
                runtime.restore(*client, *player);
            }
        }
        runtime.reset();
        animation.reset();
        targetResources.reset();
        lastPlayer = nullptr;
        applyAfter.reset();
        toggleRequested = false;
        stopPbrDrawProbe();
        removeHooks();
        clearNativeWeather();
        resetRoamingConnection();
        return true;
    }
};

LL_TYPE_INSTANCE_HOOK(InitialWeatherHook, ll::memory::HookPriority::Normal, LegacyClientNetworkHandler,
    static_cast<void (LegacyClientNetworkHandler::*)(NetworkIdentifier const&, StartGamePacket const&)>(&LegacyClientNetworkHandler::$handle),
    void, NetworkIdentifier const& source, StartGamePacket const& packet) {
    origin(source, packet);
    initializeNativeWeather(mClient.getLevel(), packet.mSettings->mRainLevel, packet.mSettings->mLightningLevel);
}
LL_TYPE_INSTANCE_HOOK(WeatherEventHook, ll::memory::HookPriority::Normal, ClientNetworkHandler,
    static_cast<void (ClientNetworkHandler::*)(NetworkIdentifier const&, LevelEventPacket const&)>(&ClientNetworkHandler::$handle),
    void, NetworkIdentifier const& source, LevelEventPacket const& packet) {
    origin(source, packet);
    receiveNativeWeather(mClient.getLevel(), packet.mEventId, packet.mData);
}

LL_STATIC_HOOK(ClientSkinTickHook, ll::memory::HookPriority::Normal, &ClientSkinSystem::tick, void, IClientInstance& client) {
    origin(client);
    Mod::getInstance().tick(client);
}
LL_STATIC_HOOK(ClientSkinFrameHook, ll::memory::HookPriority::Normal, &ClientSkinSystem::frameUpdate, void,
    FrameUpdateContext& context, IClientInstance& client) {
    Mod::getInstance().frameUpdate(client);
    origin(context, client);
}
LL_STATIC_HOOK(SkinGeometryHook, ll::memory::HookPriority::Normal, &ClientSkinSystem::_updateClientSkin, void,
    IClientInstance& client, Bedrock::NotNullNonOwnerPtr<ResourceLoadManager> resources,
    ClientSkin& clientSkin, SerializedSkinRef const& skin, mce::Image const* image,
    mce::Image const* cape, bool async, std::function<void()> finished) {
    bool ours = skin.mSkinImpl->mObject.mId->starts_with("ysm.client.");
    origin(client, resources, clientSkin, skin, image, cape, ours ? false : async, std::move(finished));
    if (ours) {
        Mod::getInstance().geometryStatus(clientSkin, skin);
    }
}
LL_TYPE_INSTANCE_HOOK(SkinTextureHook, ll::memory::HookPriority::Normal, ClientSkinInfoData,
    &ClientSkinInfoData::$updateSkin, void, SerializedSkinRef const& skin, mce::Image const* image, mce::Image const* cape) {
    origin(skin, image, cape);
    Mod::getInstance().updateRenderTexture(*this, skin);
}
LL_STATIC_HOOK(EarlyResourceTextureHook, ll::memory::HookPriority::Normal,
    &ClientSkinSystem::_setupPersonaLodTextureAndGeometry, bool, ClientSkin const& clientSkin,
    SerializedSkinRef const& skin, std::shared_ptr<ActorResourceDefinition> const& definition,
    Bedrock::NotNullNonOwnerPtr<ActorResourceDefinitionGroup> const& definitions) {
    auto result = origin(clientSkin, skin, definition, definitions);
    Mod::getInstance().bindResourceTexture(skin, definition);
    return result;
}
LL_TYPE_INSTANCE_HOOK(PrepareGeometryHook, ll::memory::HookPriority::Normal, RenderController,
    &RenderController::getGeometry, std::shared_ptr<DataDrivenGeometry>, RenderParams& params) {
    auto geometry = origin(params);
    if (geometry) Mod::getInstance().prepareGeometry(*this, geometry, params);
    return geometry;
}
LL_TYPE_INSTANCE_HOOK(FirstPersonHook, ll::memory::HookPriority::Normal, ItemInHandRenderer,
    &ItemInHandRenderer::renderFirstPerson, void, BaseActorRenderContext& context, Matrix const& projection, ItemContextFlags flags) {
    FirstPersonRenderScope scope;
    Mod::getInstance().renderFirstPerson([&] { origin(context, projection, flags); });
    Mod::getInstance().reportFirstPersonPass(true);
}
LL_TYPE_INSTANCE_HOOK(MenuMouseCaptureHook, ll::memory::HookPriority::Normal, ClientInstance,
    &ClientInstance::$grabMouse, void) {
    if (Mod::getInstance().suppressMenuMouseCapture(*this)) return;
    origin();
}
LL_TYPE_INSTANCE_HOOK(PassengerSeatHook, ll::memory::HookPriority::Normal, ActorRenderDispatcher,
    static_cast<void (ActorRenderDispatcher::*)(BaseActorRenderContext&, Actor&, Vec3 const&, Vec3 const&, Vec2 const&, bool)>(
        &ActorRenderDispatcher::render), void, BaseActorRenderContext& ctx, Actor& actor,
    Vec3 const& cameraTarget, Vec3 const& position, Vec2 const& rotation, bool ignoreLighting) {
    auto draw = [&] { origin(ctx, actor, cameraTarget, position, rotation, ignoreLighting); };
    if (!Mod::getInstance().drawPassenger(actor, ctx, draw)) draw();
}
LL_TYPE_INSTANCE_HOOK(TargetRendererRouteHook, ll::memory::HookPriority::Normal, ActorRenderDispatcher,
    &ActorRenderDispatcher::_getRenderer, std::shared_ptr<ActorRenderer>, Actor& actor) {
    auto renderer = origin(actor);
    Mod::getInstance().probeActorRenderer(actor, renderer);
    return Mod::getInstance().resolveTargetRenderer(actor, std::move(renderer));
}
LL_TYPE_INSTANCE_HOOK(TargetOwnerSetHook, ll::memory::HookPriority::Normal, Actor,
    ll::memory::SymbolView("?setOwner@Actor@@UEAAXUActorUniqueID@@@Z"), void, ActorUniqueID const owner) {
    origin(owner);
    Mod::getInstance().recordProjectileOwner(*this, owner);
}
LL_TYPE_INSTANCE_HOOK(TargetSaveHook, ll::memory::HookPriority::Normal, Actor,
    &Actor::save, bool, CompoundTag& data) {
    bool saved = origin(data);
    if (saved) Mod::getInstance().entitySaving(*this, data);
    return saved;
}
LL_TYPE_INSTANCE_HOOK(TargetLoadHook, ll::memory::HookPriority::Normal, Actor,
    &Actor::$load, bool, CompoundTag const& data, DataLoadHelper& helper) {
    bool loaded = origin(data, helper);
    if (loaded) Mod::getInstance().entityLoaded(*this, data);
    return loaded;
}
LL_TYPE_INSTANCE_HOOK(TargetMountHook, ll::memory::HookPriority::Normal, Actor,
    &Actor::$startRiding, bool, Actor& vehicle, bool force) {
    auto result = origin(vehicle, force);
    if (result) Mod::getInstance().observeTargetMount(*this);
    return result;
}
LL_TYPE_INSTANCE_HOOK(BowLaunchHook, ll::memory::HookPriority::Normal, ShooterItemComponent,
    &ShooterItemComponent::_shootProjectiles, void, ItemStack& stack, Player* player, int remaining) {
    ArrowLaunchScope scope(player, stack);
    origin(stack, player, remaining);
}
LL_TYPE_INSTANCE_HOOK(CrossbowLaunchHook, ll::memory::HookPriority::Normal, CrossbowItem,
    &CrossbowItem::$use, ItemStack&, ItemStack& stack, Player& player, HandSlot hand) {
    ArrowLaunchScope scope(&player, stack);
    return origin(stack, player, hand);
}
LL_TYPE_INSTANCE_HOOK(PlayerProjectileHook, ll::memory::HookPriority::Normal, ProjectileItemComponent,
    &ProjectileItemComponent::shootProjectile, Actor*, BlockSource& region, Vec3 const& pos,
    Vec3 const& direction, float power, Player* player) {
    auto* arrow = origin(region, pos, direction, power, player);
    Mod::getInstance().publishSpawnedArrow(arrow, player);
    return arrow;
}
LL_TYPE_INSTANCE_HOOK(SpawnerProjectileHook, ll::memory::HookPriority::Normal, Spawner,
    &Spawner::$spawnProjectile, Actor*, BlockSource& region, ActorDefinitionIdentifier const& type,
    Actor* shooter, Vec3 const& pos, Vec3 const& dir) {
    auto* arrow = origin(region, type, shooter, pos, dir);
    Mod::getInstance().publishSpawnedArrow(arrow, shooter);
    return arrow;
}
LL_TYPE_INSTANCE_HOOK(TargetProjectileShootHook, ll::memory::HookPriority::Normal, ProjectileComponent,
    static_cast<void (ProjectileComponent::*)(Actor&, Vec3 const&, float, float, Vec3 const&, Actor*)>(&ProjectileComponent::shoot),
    void, Actor& actor, Vec3 const& direction, float power, float uncertainty, Vec3 const& baseSpeed, Actor* target) {
    origin(actor, direction, power, uncertainty, baseSpeed, target);
    Mod::getInstance().recordProjectileShot(actor, *this);
}
LL_TYPE_INSTANCE_HOOK(TargetActorSpawnHook, ll::memory::HookPriority::Normal, LegacyClientNetworkHandler,
    static_cast<void (LegacyClientNetworkHandler::*)(NetworkIdentifier const&, AddActorPacket const&)>(&LegacyClientNetworkHandler::$handle),
    void, NetworkIdentifier const& source, AddActorPacket const& packet) {
    origin(source, packet);
    // Java's client recreates the entity from its spawn packet and calls
    // AbstractArrow.setOwner there. The simulation's launch uses an older
    // hand slot when the shooter switches slots in the same tick.
    if (auto* level = mClient.getLevel())
        if (auto* actor = level->fetchEntity(*packet.mEntityId, false))
            Mod::getInstance().recordClientActorSpawn(*actor, mClient.getLocalPlayer());
}
// These renderers can be retained outside the dispatcher lookup. Include the
// first distinguishing instructions from each render vtable entry on 1.26.60.05.
// The shared prologues also occur in unrelated render routines.
static auto const TargetDataDrawSignature = ll::memory::Signature::parse(
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC C8 01 00 00 48 8D AC 24 80 00 00 00 "
    "44 0F 29 BD 30 01 00 00 44 0F 29 B5 20 01 00 00 44 0F 29 AD 10 01 00 00 44 0F 29 A5 00 01 00 00 "
    "44 0F 29 9D F0 00 00 00 44 0F 29 95 E0 00 00 00 44 0F 29 8D D0 00 00 00 44 0F 29 85 C0 00 00 00 "
    "0F 29 BD B0 00 00 00 0F 29 B5 A0 00 00 00 48 C7 85 98 00 00 00 FE FF FF FF 4C 89 C6 48 89 55 38");
static auto const TargetBoatDrawSignature = ll::memory::Signature::parse(
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 28 01 00 00 48 8D AC 24 80 00 00 00 "
    "0F 29 B5 90 00 00 00 48 C7 85 88 00 00 00 FE FF FF FF 48 89 4D 78 49 8B 38 48 85 FF 0F 84 2B 07 00 00 "
    "4D 89 C6 48 89 D3 4C 8B 7A 28 49 8B 77 18 4C 8D 6E 40 C6 46 78 01 48 8B 46 60 48 8B 4E 58 48 01 C8 "
    "48 FF C8 48 8B 4E 48 48 8B 56 50 48 FF CA 48 21 C2 48 8B 14 D1 4C 89 E9 E8 15 EB A5 FF 48 8B 46 60");
static auto const TargetTridentDrawSignature = ll::memory::Signature::parse(
    "55 41 57 41 56 41 55 41 54 56 57 53 48 81 EC 28 02 00 00 48 8D AC 24 80 00 00 00 "
    "44 0F 29 BD 90 01 00 00 44 0F 29 B5 80 01 00 00 44 0F 29 AD 70 01 00 00 44 0F 29 A5 60 01 00 00 "
    "44 0F 29 9D 50 01 00 00 44 0F 29 95 40 01 00 00 44 0F 29 8D 30 01 00 00 44 0F 29 85 20 01 00 00 "
    "0F 29 BD 10 01 00 00 0F 29 B5 00 01 00 00 48 C7 85 F8 00 00 00 FE FF FF FF 48 89 95 A8 00 00 00 4D 8B 38");
LL_TYPE_INSTANCE_HOOK(TargetDataDrawHook, ll::memory::HookPriority::Normal, ActorRenderer,
    TargetDataDrawSignature.view(), void, BaseActorRenderContext& context, ActorRenderData& data) {
    ActorDrawScope drawOwner(data.mActor);
    if (data.mActor && !data.mIsInUI &&
        Mod::getInstance().drawPassenger(*data.mActor, context, [&] { origin(context, data); })) return;
    if (Mod::getInstance().drawParrot(context, data, [&] { origin(context, data); })) return;
    if (!Mod::getInstance().drawTarget(*this, context, data)) origin(context, data);
}
LL_TYPE_INSTANCE_HOOK(TargetBoatDrawHook, ll::memory::HookPriority::Normal, ActorRenderer,
    TargetBoatDrawSignature.view(), void, BaseActorRenderContext& context, ActorRenderData& data) {
    if (!Mod::getInstance().drawTarget(*this, context, data)) origin(context, data);
}
LL_TYPE_INSTANCE_HOOK(TargetTridentDrawHook, ll::memory::HookPriority::Normal, ActorRenderer,
    TargetTridentDrawSignature.view(), void, BaseActorRenderContext& context, ActorRenderData& data) {
    if (!Mod::getInstance().drawTarget(*this, context, data)) origin(context, data);
}
LL_TYPE_INSTANCE_HOOK(PrepareAnimationHook, ll::memory::HookPriority::Normal, ClientAnimationComponent,
    &ClientAnimationComponent::prepRenderParamsForActorRendering, RenderParams&, ActorRenderData& data,
    BaseActorRenderContext* context, std::shared_ptr<DataDrivenModel> model, float frameAlpha) {
    model = Mod::getInstance().resolveTargetModel(data, std::move(model));
    auto& params = origin(data, context, std::move(model), frameAlpha);
    observeShoulderPreview(context, data);
    Mod::getInstance().prepareAnimationForRender(data, *this);
    return params;
}
using NativePlayerRotation = std::optional<std::pair<float, float>>;
LL_STATIC_HOOK(NativeGlideRotationHook, ll::memory::HookPriority::Normal,
    &ActorRenderData::getDamageOrGlidingXYRotation, NativePlayerRotation,
    Mob const& mob, float frameAlpha, bool firstPerson) {
    auto rotation = origin(mob, frameAlpha, firstPerson);
    if (rotation && Mod::getInstance().suppressNativeGlideRotation(mob, firstPerson,
            {rotation->first, rotation->second})) rotation.reset();
    return rotation;
}
LL_TYPE_INSTANCE_HOOK(UiContextCreateHook, ll::memory::HookPriority::Normal, MinecraftUIRenderContext,
    &MinecraftUIRenderContext::$ctor, void*, IClientInstance& client, ScreenContext& screen, UIScene const& scene) {
    auto* result = origin(client, screen, scene);
    UiRenderContextTracker::enter(this);
    return result;
}
LL_TYPE_INSTANCE_HOOK(UiContextDestroyHook, ll::memory::HookPriority::Normal, MinecraftUIRenderContext,
    &MinecraftUIRenderContext::$dtor, void) {
    UiRenderContextTracker::leave(this);
    origin();
}
LL_TYPE_INSTANCE_HOOK(SetupAttachableHook, ll::memory::HookPriority::Normal, DataDrivenModel,
    &DataDrivenModel::setupAttachable, void, ItemStack const& item, AttachableSlotIndex const& slot, RenderParams& params, Actor& actor) {
    HeldItemBoneScope items(params);
    // The slot must process item changes even during a hidden animation
    // frame. Only the draw call may be suppressed by the locator mask.
    origin(item, slot, params, actor);
    bindAttachableParent(params, slot, item, actor);
}
LL_TYPE_INSTANCE_HOOK(RenderAttachableNoChecksHook, ll::memory::HookPriority::Normal, DataDrivenModel,
    &DataDrivenModel::renderAttachableNoChecks, void, ItemStack const& item, AttachableSlotIndex const& slot, RenderParams& params, Actor& actor) {
    auto mask = Mod::getInstance().handDrawMask(actor, slot, params.mActorRenderData && params.mActorRenderData->mIsInUI);
    if (!mask) return;
    HeldItemBoneScope items(params);
    bool ysmCharge = slot == AttachableSlotIndex::CarriedItem && !FirstPersonRenderScope::isActive()
        && !(params.mActorRenderData && params.mActorRenderData->mIsInUI) && Mod::getInstance().drivesLanceCharge(actor);
    auto draw = [&] { origin(item, slot, params, actor); };
    auto drawGrip = [&](unsigned copy) { drawAtAttachableGrip(params, slot, item, actor, copy, ysmCharge, draw); };
    if (mask & 1u) drawGrip(0);
    auto hand = slot == AttachableSlotIndex::OffhandItem ? "leftitem" : "rightitem";
    for (unsigned copy = 2; mask > 1 && copy <= 8; ++copy) {
        if ((mask & (1u << (copy - 1))) == 0) continue;
        HeldItemBoneScope copyPose(params, hand, copy);
        if (copyPose.bound()) drawGrip(copy);
    }
    if (mask > 1 || ysmCharge) bindAttachableParent(params, slot, item, actor);
}
LL_TYPE_INSTANCE_HOOK(BoneVisualMeshHook, ll::memory::HookPriority::Normal, SkinnedMesh,
    &SkinnedMesh::draw, void, ScreenContext& screen, dragon::RenderMetadata const& metadata,
    RenderController const* controller, uint subLayer, MaterialVariants const& material,
    ClientPBRTextureData const& texture, SortedMeshDrawList* queue,
    std::optional<glm::mat4> const& previous, std::optional<MERSUniformData> const& mers,
    ShadowContext shadow, bool dithering) {
    if (skipCollapsedYsmMesh(*this)) return;
    // Timing all vanilla and model meshes adds overhead to the hot path.
    thread_local std::uint32_t drawCount = 0;
    bool measure = (++drawCount & 63) == 0;
    auto started = measure ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::array<BoneVisual, 16> localVisuals;
    std::vector<BoneVisual> overflowVisuals;
    auto visuals = Mod::getInstance().boneVisuals(*this, controller, localVisuals, overflowVisuals);
    auto lookedUp = measure ? std::chrono::steady_clock::now() : started;
    if (visuals.empty()) {
        origin(screen, metadata, controller, subLayer, material, texture, queue, previous, mers, shadow, dithering);
        return;
    }
    bool arkeWorldPass = !screen.isDrawingUI && !dithering && shadow != ShadowContext::ShadowOnly
        && shadow != ShadowContext::PlayerShadowMapOnly && Mod::getInstance().usesArkeShader();
    drawBoneVisuals(*this, screen, queue, controller, material, visuals, arkeWorldPass, [&](MaterialVariants const& selected) {
        origin(screen, metadata, controller, subLayer, selected, texture, queue, previous, mers, shadow, dithering);
    });
    if (measure) recordSurfaceCpuCost(
        std::chrono::duration<double, std::micro>(lookedUp - started).count(),
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - lookedUp).count());
}
// Another renderer may already own the entry detour. Resolve the native
// function from its interior and verify the entry using Windows unwind data.
static uintptr_t surfaceRouteEntry() {
    static auto const interior = ll::memory::Signature::parse(
        "4C 8B 51 08 4D 85 D2 0F 84 FB 00 00 00 49 8B 00 48 85 C0 0F 84 EF 00 00 00 4D 8B 42 38 4D 8B 5A 40 4D 29 C3");
    auto address = reinterpret_cast<uintptr_t>(interior.view().resolve(true));
    if (!address) return 0;
    DWORD64 base = 0;
    auto function = RtlLookupFunctionEntry(address, &base, nullptr);
    if (!function || base != reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))) return 0;
    auto entry = base + function->BeginAddress;
    constexpr uintptr_t interiorOffset = 25; // 1.26.60.05 x64 unwind entry to the first stable load
    return address == entry + interiorOffset ? entry : 0;
}
static uintptr_t surfaceRouteAddress = 0;
LL_TYPE_INSTANCE_HOOK(WorldSurfaceRouteHook, ll::memory::HookPriority::Normal, DataDrivenRendererV2,
    surfaceRouteAddress, bool, Actor& actor, std::shared_ptr<ActorRenderer> const& renderer, ShadowContext shadow) {
    bool individual = Mod::getInstance().needsIndividualSurfaces(actor);
    recordSurfaceRoute(individual);
    if (individual) return false;
    return origin(actor, renderer, shadow);
}
using SurfaceHooks = ll::memory::HookRegistrar<WorldSurfaceRouteHook>;
using TargetDataDrawHooks = ll::memory::HookRegistrar<TargetDataDrawHook>;
using TargetBoatDrawHooks = ll::memory::HookRegistrar<TargetBoatDrawHook>;
using TargetTridentDrawHooks = ll::memory::HookRegistrar<TargetTridentDrawHook>;
using TargetOwnerHooks = ll::memory::HookRegistrar<TargetOwnerSetHook, TargetMountHook>;
using TargetShootHooks = ll::memory::HookRegistrar<TargetProjectileShootHook, BowLaunchHook, CrossbowLaunchHook, PlayerProjectileHook, SpawnerProjectileHook>;
using TargetSpawnHooks = ll::memory::HookRegistrar<TargetActorSpawnHook>;
using EntityStorageHooks = ll::memory::HookRegistrar<TargetSaveHook, TargetLoadHook>;
LL_TYPE_INSTANCE_HOOK(ArrowDamageHook, ll::memory::HookPriority::Normal, Mob,
    ll::memory::SymbolView("?hurtEffects@Mob@@UEAAXAEBVActorDamageSource@@MAEBUHurtEffectsSettings@@@Z"),
    void, ActorDamageSource const& source, float damage, HurtEffectsSettings const& settings) {
    if (damage > 0 && isPlayer() && source.mCause == SharedTypes::Legacy::ActorDamageCause::Projectile
        && source.getEntityType() == ActorType::Arrow) {
        auto arrow = source.getEntityUniqueID().rawID;
        if (arrow) recordNativeArrowHit(*this, arrow);
    }
    origin(source, damage, settings);
}
using ArrowDamageHooks = ll::memory::HookRegistrar<ArrowDamageHook>;
using Hooks = ll::memory::HookRegistrar<ClientSkinTickHook, ClientSkinFrameHook, SkinGeometryHook, SkinTextureHook, EarlyResourceTextureHook, PrepareGeometryHook, FirstPersonHook, PrepareAnimationHook, BoneVisualMeshHook,
    SetupAttachableHook, RenderAttachableNoChecksHook, NativeGlideRotationHook, TargetRendererRouteHook, PassengerSeatHook,
    InitialWeatherHook, WeatherEventHook, UiContextCreateHook, UiContextDestroyHook, MenuMouseCaptureHook>;
static std::unique_ptr<Hooks> hooks;
static std::unique_ptr<SurfaceHooks> surfaceHooks;
static std::unique_ptr<TargetDataDrawHooks> targetDataDrawHooks;
static std::unique_ptr<TargetBoatDrawHooks> targetBoatDrawHooks;
static std::unique_ptr<TargetTridentDrawHooks> targetTridentDrawHooks;
static std::unique_ptr<TargetOwnerHooks> targetOwnerHooks;
static std::unique_ptr<TargetShootHooks> targetShootHooks;
static std::unique_ptr<TargetSpawnHooks> targetSpawnHooks;
static std::unique_ptr<EntityStorageHooks> entityStorageHooks;
static std::unique_ptr<ArrowDamageHooks> arrowDamageHooks;
static void installHooks() {
    hooks = std::make_unique<Hooks>();
    entityStorageHooks = std::make_unique<EntityStorageHooks>();
    // Optional native diagnostics must not make unsupported clients fail load.
    if (TargetDataDrawSignature.view().resolve(true)) targetDataDrawHooks = std::make_unique<TargetDataDrawHooks>();
    if (Mod::getInstance().usesTargetRendering()) {
        if (TargetBoatDrawSignature.view().resolve(true)) targetBoatDrawHooks = std::make_unique<TargetBoatDrawHooks>();
        if (TargetTridentDrawSignature.view().resolve(true)) targetTridentDrawHooks = std::make_unique<TargetTridentDrawHooks>();
    }
    if (Mod::getInstance().usesTargetRendering()
        && ll::memory::SymbolView("?setOwner@Actor@@UEAAXUActorUniqueID@@@Z").resolve(true))
        targetOwnerHooks = std::make_unique<TargetOwnerHooks>();
    if (Mod::getInstance().usesTargetRendering()) {
        targetShootHooks = std::make_unique<TargetShootHooks>();
        targetSpawnHooks = std::make_unique<TargetSpawnHooks>();
    }
    if (ll::memory::SymbolView("?hurtEffects@Mob@@UEAAXAEBVActorDamageSource@@MAEBUHurtEffectsSettings@@@Z").resolve(true))
        arrowDamageHooks = std::make_unique<ArrowDamageHooks>();
    surfaceRouteAddress = surfaceRouteEntry();
    bool supported = surfaceRouteAddress != 0;
    setSurfaceRouteSupport(supported);
    if (supported) surfaceHooks = std::make_unique<SurfaceHooks>();
}
static void removeHooks() {
    entityStorageHooks.reset();
    arrowDamageHooks.reset();
    targetSpawnHooks.reset();
    targetShootHooks.reset();
    targetOwnerHooks.reset();
    targetTridentDrawHooks.reset();
    targetBoatDrawHooks.reset();
    targetDataDrawHooks.reset();
    surfaceHooks.reset();
    hooks.reset();
    setSurfaceRouteSupport(false);
}
}
LL_REGISTER_MOD(ysm::Mod, ysm::Mod::getInstance());
