#include "NativeModelRelay.h"
#include "NativeModelRelay.inl"
#include "NativeShoulderParrot.h"
#include "NativeReplacedResources.h"
#include "SkinRuntime.h"
#include "NativeRoomSync.h"
#include "mc/client/game/IClientInstance.h"
#include "mc/client/player/LocalPlayer.h"
#include "mc/client/renderer/actor/ActorRenderData.h"
#include "mc/network/GameConnectionInfo.h"
#include "mc/platform/UUID.h"
#include "mc/world/actor/RenderParams.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
#include <windows.h>
#include <psapi.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <utility>

namespace ysm {
using Clock = std::chrono::steady_clock;
namespace {
struct InstalledPeer {
    std::string xuid, session, contentKey, texture, digest;
    Player* actor{};
    std::int64_t actorId{};
    std::atomic<std::array<std::int64_t,2>> shoulderBirds{};
    AnimationRuntime animation;
    SkinRuntime skin;
    std::shared_ptr<ModelPack const> model;
    std::uint64_t actionSerial{}, eventCursor{}, lastSerial{};
    bool worldPoseReported = false;
};
struct Candidate {
    std::shared_ptr<ModelPack const> model;
    AnimationRuntime::Prepared animation;
};
bool matches(IClientInstance& client, ModelRelay const& transport) {
    auto connection = client.getGameConnectionInfo();
    return connection && connection->mPort == transport.serverPort
        && (*connection->mHostIpAddress == transport.serverHost || *connection->mUnresolvedUrl == transport.serverHost);
}
std::string playerXuid(Player& player) {
    auto id = player.getXuid();
    return id.empty() || id == "0" ? player.getLevel().getPlayerXUID(player.getUuid()) : id;
}
Player* roomActor(Level const& level,std::string const& id) {
    if (!id.starts_with("uuid:")) return level.getPlayerByXuid(id);
    auto uuid=std::string_view(id).substr(5);
    if(uuid.size()!=36 || uuid[8]!='-' || uuid[13]!='-' || uuid[18]!='-' || uuid[23]!='-'
        || uuid.find_first_not_of("0123456789abcdefABCDEF-")!=std::string_view::npos)return nullptr;
    return level.getPlayer(mce::UUID::fromString(uuid));
}
ModelPack loadRoomFile(std::filesystem::path const& path, RelayAsset const& asset) {
    auto pack=ModelPack::loadRoom(path);
    if(pack.localContentKey!=asset.contentKey || pack.binaryOriginVersion()!=asset.originVersion
        || pack.descriptor.at("spec")!=asset.sourceSpec)throw std::runtime_error("Room cache identity or model format changed");
    return pack;
}

}
struct NativeModelRelay::Impl {
    using DrawPeers = detail::RelayRenderPeers<Actor, InstalledPeer>;
    std::atomic<std::shared_ptr<DrawPeers const>> drawing;
    std::map<std::string, std::shared_ptr<InstalledPeer>> peers;
    std::map<std::string, RelayReceipt> delivered;
    std::filesystem::path root;
    std::optional<ModelRelay> transport;
    NativeRoomSync room;
    bool native=false;
    std::shared_ptr<RoomView const> view;
    std::string selectionTexture;
    std::atomic_bool trackVisibility{};
    std::mutex seenMutex;
    std::map<std::int64_t,Clock::time_point> lastSeen;
    bool recentlyDrawn(Player& actor,Clock::time_point now){
        std::lock_guard lock(seenMutex);
        auto it=lastSeen.find(actor.getOrCreateUniqueID().rawID);
        return it!=lastSeen.end() && now-it->second < std::chrono::milliseconds(750);
    }
    std::int64_t preparingActor{};
    std::uintptr_t preparingInstance{};
    std::map<std::pair<std::string,std::string>,std::weak_ptr<ModelPack const>> decoded;
    std::string session, localXuid, outgoingModel;
    std::uint64_t serial{}, eventSequence{};
    std::vector<RelayEvent> outgoingEvents;
    Level const* level{};
    std::atomic<bool> left = false;
    std::future<RelayFrame> exchange;
    std::string exchangeSession;
    std::future<Candidate> preparation;
    RelayPose preparing;
    std::string preparingSession, preparingDigest;
    RelayFrame latest;
    Clock::time_point lastReply{}, nextPoll{};
    RelayPreparationQueue queue;
    struct PreparationFailure {std::string stream,digest,reason;std::int64_t actor{};unsigned count{};};
    std::map<std::string,PreparationFailure> failures;
    std::string error;
    bool endpointsConfirmed = false;
    bool hideOthers = false;
    bool measure{};
    std::mutex frameTimesMutex;
    std::vector<float> frameTimes;
    Clock::time_point lastFrame{}, nextSample{};
    void sample(Clock::time_point now) {
        if(!measure || !view || now<nextSample)return;
        nextSample=now+std::chrono::seconds(1);
        std::vector<float> times;
        {std::lock_guard lock(frameTimesMutex); times.swap(frameTimes);}
        std::sort(times.begin(),times.end());
        Json result{{"world",session},{"time_ms",std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()},
            {"players",latest.players.size()},{"bound",peers.size()},{"ready_files",view->ready.size()},
            {"sent_to_transport_bytes",view->acceptedTxBytes},{"received_payload_bytes",view->receivedBytes},
            {"frame_samples",times.size()},{"states",view->states}};
        if(auto ping=room.latencyMs())result["host_ping_ms"]=*ping;
        if(!times.empty()){
            result["frame_ms_p50"]=times[(times.size()-1)/2];
            result["frame_ms_p95"]=times[(times.size()-1)*95/100];
        }
        PROCESS_MEMORY_COUNTERS memory{};
        if(GetProcessMemoryInfo(GetCurrentProcess(),&memory,sizeof(memory)))result["working_set_bytes"]=memory.WorkingSetSize;
        auto path=root/"room-metrics.jsonl";
        std::error_code ec;
        auto bytes=std::filesystem::file_size(path,ec);
        if(ec || bytes<4u*1024*1024)std::ofstream(path,std::ios::app)<<result.dump()<<'\n';
    }
    std::set<std::string> hiddenPlayers;

    bool hides(std::string const& id) const { return hideOthers || hiddenPlayers.contains(id); }

    void publishPeers(std::shared_ptr<InstalledPeer> installing = {}) {
        drawing.store(std::make_shared<DrawPeers>(peers, std::move(installing)), std::memory_order_release);
    }
    std::shared_ptr<InstalledPeer> peer(Actor const* actor) const {
        auto map = drawing.load(std::memory_order_acquire);
        if (!map) return {};
        auto found = map->actors.find(actor);
        return found == map->actors.end() || !actor || found->second->actorId!=actor->getOrCreateUniqueID().rawID ? nullptr : found->second;
    }
    void status() const {
        if (root.empty()) return;
        auto connected = native ? endpointsConfirmed && view && view->connected : lastReply != Clock::time_point{};
        auto posed = std::count_if(peers.begin(), peers.end(), [](auto const& entry) { return entry.second->worldPoseReported; });
        std::ofstream(root / "relay-status.json") << Json{{"connected", connected},
            {"role", native ? (room.host() ? "host" : "guest") : (level ? "external" : "none")},
            {"native_peer_confirmed", endpointsConfirmed}, {"remote_player_states", latest.players.size()},
            {"local_model_approved", latest.approved}, {"bound_remote_models", peers.size()}, {"visible_remote_models", posed},
            {"available_models", latest.assets.size()}, {"last_error", error}}.dump(2);
    }
    void terminate(IClientInstance* client) {
        drawing.store({}, std::memory_order_release);
        auto local = client ? client->getLocalPlayer() : nullptr;
        for (auto const& [xuid, entry] : peers) {
            auto actor = local && &local->getLevel() == level ? roomActor(local->getLevel(), xuid) : nullptr;
            if (actor && actor == entry->actor && actor->getOrCreateUniqueID().rawID==entry->actorId) {
                entry->animation.detach(*actor);
                try { entry->skin.restore(*client, *actor); } catch (std::exception const&) {}
            } else {
                entry->animation.reset();
                entry->skin.reset();
            }
        }
        {std::lock_guard lock(frameTimesMutex);frameTimes.clear();lastFrame={};}
        decoded.clear(); view.reset(); native=false; selectionTexture.clear(); trackVisibility=false;
        {std::lock_guard lock(seenMutex);lastSeen.clear();}
        peers.clear();
        delivered.clear();failures.clear();
        session.clear();
        localXuid.clear();
        outgoingModel.clear();
        outgoingEvents.clear();
        latest = {};
        endpointsConfirmed = false;
        lastReply = {};
        nextPoll = {};
        queue = {};
        serial = eventSequence = 0;
        level = nullptr;
    }
    bool stillCurrent(RelayPose const& delivery) const {
        if (hides(delivery.xuid)) return false;
        auto found = latest.players.find(delivery.xuid);
        return found != latest.players.end() && found->second.session == delivery.session
            && found->second.contentKey == delivery.contentKey && found->second.texture == delivery.texture;
    }
};

NativeModelRelay::NativeModelRelay() : impl(std::make_unique<Impl>()) {}
NativeModelRelay::~NativeModelRelay() = default;
void NativeModelRelay::configure(std::filesystem::path directory) {
    impl->root = std::move(directory);
    impl->measure=std::filesystem::exists(impl->root/"room-metrics.enable");
    try { impl->transport = ModelRelay::readConfig(impl->root / "relay.json"); }
    catch (std::exception const& error) { impl->error = error.what(); }
    try { impl->room.start(impl->root); } catch(std::exception const& e){impl->error=e.what();}
    impl->status();
}
void NativeModelRelay::serverTick(Level& level){impl->room.serverTick(level);}
void NativeModelRelay::roomCommand(std::string command){impl->room.command(std::move(command));}
Json NativeModelRelay::roomInfo() const {
    auto view=impl->view;
    Json offers=Json::array();
    if(view)for(auto const& o:view->offers)offers.push_back({{"key",o.peer+"|"+o.asset.contentKey},{"title",o.asset.title}});
    Json denied=Json::array();
    if(view && impl->room.host())for(auto const& [key,title]:view->deniedModels)
        denied.push_back({{"key",key},{"title",title}});
    return {{"host",impl->room.host()},{"active",view && view->connected},{"auto_receive",!view || view->autoReceive},{"message",view?view->message:impl->error},{"offers",std::move(offers)},{"denied",std::move(denied)}};
}
void NativeModelRelay::boundary() {
    impl->left=true;impl->trackVisibility=false;
    impl->drawing.store({},std::memory_order_release);
}
void NativeModelRelay::stop(IClientInstance* client) {
    impl->room.stop();
    impl->terminate(client);
    if (impl->exchange.valid()) impl->exchange.wait();
    impl->exchange = {};
    if (impl->preparation.valid()) impl->preparation.wait();
    impl->preparation = {};
    impl->status();
}
SharedModelCatalog NativeModelRelay::modelChoices() const {
    SharedModelCatalog result;
    if (impl->session.empty() || impl->left.load()) return result;
    if (impl->native) {
        if(impl->view)for(auto const& [k,path]:impl->view->ready){
            auto it=impl->latest.assets.find(k);
            if(it!=impl->latest.assets.end() && it->second.selectable)
                result.emplace("server:"+k,SharedModelSource{it->second.title,[path,asset=it->second]{return loadRoomFile(path,asset);}});
        }
        return result;
    }
    if(!impl->transport)return result;
    for (auto const& [key, asset] : impl->latest.assets) if (asset.selectable) {
        result.emplace("server:" + key, SharedModelSource{asset.title, [relay = *impl->transport, asset, cache = impl->root / "relay-cache"] {
            return relay.obtain(asset, cache);
        }});
    }
    return result;
}
Json NativeModelRelay::catalogRows() const {
    Json result = Json::array();
    if (impl->session.empty() || impl->left.load()) return result;
    for (auto const& [key, asset] : impl->latest.assets) if (asset.selectable)
        result.push_back({{"source", "server:" + key}, {"name", asset.title}, {"author", "服务器"}, {"kind", "server"}, {"status", impl->native && impl->view && !impl->view->ready.contains(key) ? "downloading":"unloaded"}});
    return result;
}
bool NativeModelRelay::canSelect(std::string const& source) const {
    if (!source.starts_with("server:")) return true;
    if (impl->session.empty() || impl->left.load()) return false;
    auto asset = impl->latest.assets.find(source.substr(7));
    return asset != impl->latest.assets.end() && asset->second.selectable;
}
void NativeModelRelay::setVisibility(bool hideOthers, Json const& hiddenPlayers) {
    impl->hideOthers = hideOthers;
    impl->hiddenPlayers.clear();
    for (auto const& item : hiddenPlayers) if (item.is_string()) impl->hiddenPlayers.insert(item.get<std::string>());
}
Json NativeModelRelay::playerRows(Player const* local) const {
    auto rows = Json::array();
    if (!local || &local->getLevel() != impl->level) return rows;
    auto add=[&](std::string const& id,std::string const& state,std::string const& key){
        static const std::map<std::string,std::string> labels{{"waiting","等待下载"},{"downloading","下载中"},{"verifying","校验中"},
            {"cached","等待绘制准备"},{"denied","房主已拒绝"},{"conflicting_archive","同一模型标识的文件不一致"}};
        auto label=labels.find(state);
        if(auto* player=roomActor(local->getLevel(),id))
            rows.push_back({{"id",id},{"name",player->getNameTag()},{"hidden",impl->hiddenPlayers.contains(id)},{"state",label!=labels.end()?label->second:state},{"key",key}});
    };
    for(auto const& [id,pose]:impl->latest.players){
        std::string state="等待模型";
        if(impl->hides(id))state="已屏蔽";
        else if(auto it=impl->peers.find(id);it!=impl->peers.end())state=it->second->worldPoseReported?"已显示":"等待角色绘制";
        else if(impl->preparation.valid() && impl->preparing.xuid==id)state="正在准备模型";
        else if(auto failed=impl->failures.find(id);failed!=impl->failures.end() && failed->second.stream==pose.session)
            state="准备失败 · "+failed->second.reason;
        else if(impl->view && impl->view->states.contains(pose.contentKey))state=impl->view->states.at(pose.contentKey);
        add(id,state,pose.contentKey);
    }
    if(impl->view)for(auto const& [id,state]:impl->view->peerStates)
        if(!impl->latest.players.contains(id))add(id,state,"");
    return rows;
}
void NativeModelRelay::tick(IClientInstance& client, Player* player, ModelPack const* model, AnimationRuntime& local, NativeReplacedResources const& resources) {
    auto& data = *impl;

    auto now = Clock::now();
    if (data.left.exchange(false) || (data.level && (!player || &player->getLevel() != data.level))) {
        data.room.reset();data.terminate(&client);
        data.status();
    }
    data.room.tick(client,player);
    auto roomView=data.room.view();
    bool useExternal=player && data.transport && !data.room.host() && !(roomView && roomView->connected) && matches(client,*data.transport);
    bool useNative=player && data.room.active() && !useExternal;
    if(data.level && (data.native!=useNative || (!useNative && !useExternal)))data.terminate(&client);
    if (data.exchange.valid() && data.exchange.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto reply = data.exchange.get();
            if (data.exchangeSession == data.session && !data.session.empty()) {
                data.latest = std::move(reply);
                data.lastReply = now;
                data.error.clear();
                std::erase_if(data.outgoingEvents, [&](auto const& event) { return event.sequence <= data.latest.acknowledgedEvent; });
                data.status();
            }
        } catch (std::exception const& error) {
            data.error = error.what();
            data.nextPoll = now + std::chrono::seconds(1);
            data.status();
        }
    }
    if (!player || (!useNative && !useExternal)) {
        local.relayEnabled(false);
        return;
    }
    if (!data.level) {
        data.native=useNative;
        auto id = useNative ? data.room.localId() : playerXuid(*player);
        if (id.empty() || id == "0") return;
        data.level = &player->getLevel();
        data.localXuid = std::move(id);
        data.session = ModelRelay::newSession();
    }
    if (!useNative && data.lastReply != Clock::time_point{} && now - data.lastReply > std::chrono::seconds(7)) {
        data.latest = {};
        data.lastReply = {};
        data.status();
    }
    auto key = model ? model->localContentKey : std::string();
    if (key != data.outgoingModel) {
        data.outgoingModel = key;
        data.outgoingEvents.clear();
    }
    auto hasEndpoints = !useNative || data.room.hasEndpoints();
    if(useNative){
        bool changed = data.endpointsConfirmed != hasEndpoints;
        data.endpointsConfirmed = hasEndpoints;
        if(data.view!=roomView){
            bool wasConnected=data.view && data.view->connected;
            changed |= !data.view || !roomView || data.latest.players.size() != roomView->frame.players.size()
                || data.latest.assets.size() != roomView->frame.assets.size();
            data.view=std::move(roomView);
            data.latest=data.view ? data.view->frame : RelayFrame{};
            changed |= wasConnected!=static_cast<bool>(data.view && data.view->connected);
        }
        if(changed)data.status();
        data.sample(now);
        data.trackVisibility = data.measure || (hasEndpoints && !data.latest.players.empty());
    }
    local.relayEnabled(model && hasEndpoints && data.latest.approved);
    if(useNative){
        if (!hasEndpoints) {
            data.outgoingEvents.clear();
        } else if(now>=data.nextPoll && !data.room.backpressured()){
            std::erase_if(data.outgoingEvents,[&](auto const& event){return event.sequence<=data.latest.acknowledgedEvent;});
            auto pose=model?local.outgoingPose(data.eventSequence,data.outgoingEvents.empty()):RelayPose{};
            if(model)pose.shoulderBirds=observeShoulderPassengers(*player,resources);
            if(pose.texture!=data.selectionTexture){
                data.outgoingEvents.clear();data.selectionTexture=pose.texture;
            }
            if(!pose.events.empty())data.outgoingEvents=std::move(pose.events);
            pose.events=data.outgoingEvents;
            pose.xuid=data.localXuid;pose.serial=++data.serial;pose.contentKey=key;
            std::set<std::string> visible;
            for(auto const& [id,state]:data.latest.players)
                if(auto* remote=roomActor(player->getLevel(),id);remote && !data.hides(id) && data.recentlyDrawn(*remote,now))visible.insert(id);
            data.room.update(model?model->librarySource:std::string{},model?model->id:std::string{},std::move(pose),std::move(visible));
            data.nextPoll=now+std::chrono::milliseconds(100);
        }
    }else if (!data.exchange.valid() && now >= data.nextPoll) {
        auto wire = model ? local.outgoingPose(data.eventSequence, data.outgoingEvents.empty()) : RelayPose{};
        if (model) wire.shoulderBirds = observeShoulderPassengers(*player, resources);
        wire.xuid = data.localXuid;
        wire.session = data.session;
        wire.contentKey = key;
        if (!wire.events.empty()) data.outgoingEvents = std::move(wire.events);
        wire.events = data.outgoingEvents;
        std::erase_if(data.delivered, [&](auto const& item) {
            auto peer = data.latest.players.find(item.first);
            return peer == data.latest.players.end() || peer->second.session != item.second.session
                || peer->second.contentKey != item.second.model;
        });
        for (auto const& [id, peer] : data.latest.players) {
            if (data.hides(id) || !roomActor(player->getLevel(), id)) continue;
            auto cursor = data.delivered.try_emplace(id, RelayReceipt{peer.session, peer.contentKey, 0}).first;
            wire.receipts.emplace(id, cursor->second);
        }
        data.exchangeSession = data.session;
        data.nextPoll = now + std::chrono::milliseconds(250);
        data.exchange = std::async(std::launch::async, [relay = *data.transport, wire = std::move(wire), serial = ++data.serial] {
            return relay.exchange(wire, serial);
        });
    }
    auto& level = player->getLevel();
    std::erase_if(data.failures,[&](auto const& f){
        auto it=data.latest.players.find(f.first);
        return it==data.latest.players.end() || it->second.session!=f.second.stream;
    });
    for (auto it = data.peers.begin(); it != data.peers.end();) {
        auto entry = it->second;
        auto actor = roomActor(level, entry->xuid);
        auto found = data.latest.players.find(entry->xuid);
        if (!actor || actor != entry->actor || actor->getOrCreateUniqueID().rawID!=entry->actorId || data.hides(entry->xuid) || found == data.latest.players.end() || entry->session != found->second.session
            || entry->contentKey != found->second.contentKey || entry->texture != found->second.texture
            || !data.latest.assets.contains(entry->contentKey) || data.latest.assets.at(entry->contentKey).digest!=entry->digest) {
            it = data.peers.erase(it);
            data.publishPeers();
            data.status();
            if (actor && actor == entry->actor && actor->getOrCreateUniqueID().rawID==entry->actorId) {
                entry->animation.detach(*actor);
                try { entry->skin.restore(client, *actor); }
                catch (std::exception const& error) { data.error = error.what(); data.status(); }
            } else {
                entry->animation.reset();
                entry->skin.reset();
            }
        } else {
            // The native skin system may rebuild this component after publication.
            entry->animation.attach(*actor);
            if (!entry->worldPoseReported && entry->animation.hasWorldBodyPose()) {
                entry->worldPoseReported = true;
                data.status();
            }
            auto const& state = found->second;
            if (entry->lastSerial != state.serial || (!state.events.empty() && state.events.back().sequence > entry->eventCursor)) {
                entry->shoulderBirds.store(state.shoulderBirds);
                entry->animation.applyPeerPose(state, entry->eventCursor, entry->actionSerial != state.actionSerial, *actor);
                data.delivered[entry->xuid] = {entry->session, entry->contentKey, entry->eventCursor};
                entry->actionSerial = state.actionSerial;
                entry->lastSerial = state.serial;
            }
            ++it;
        }
    }
    if (data.preparation.valid() && data.preparation.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto candidate = data.preparation.get();
            auto const& pending = data.preparing;
            auto actor = roomActor(level, pending.xuid);
            if (actor && actor != player && actor->mSkin && reinterpret_cast<std::uintptr_t>(actor)==data.preparingInstance
                && actor->getOrCreateUniqueID().rawID==data.preparingActor && data.preparingSession == data.session && data.stillCurrent(pending)
                && data.latest.assets.contains(pending.contentKey) && data.latest.assets.at(pending.contentKey).digest==data.preparingDigest) {
                auto const& current = data.latest.players.at(pending.xuid);
                auto entry = std::make_shared<InstalledPeer>();
                entry->xuid = pending.xuid;
                entry->session = pending.session;
                entry->contentKey = pending.contentKey;
                entry->texture = pending.texture;entry->digest=data.preparingDigest;
                entry->actor = actor;
                entry->actionSerial = current.actionSerial;
                entry->actorId = actor->getOrCreateUniqueID().rawID;
                entry->shoulderBirds.store(current.shoulderBirds);
                entry->lastSerial = current.serial;
                entry->animation.start(std::move(candidate.animation));
                if (auto receipt = data.delivered.find(entry->xuid); receipt != data.delivered.end()
                    && receipt->second.session == current.session && receipt->second.model == current.contentKey)
                    entry->eventCursor = receipt->second.sequence;
                if(data.native && !current.events.empty())entry->eventCursor=std::max(entry->eventCursor,current.events.back().sequence);
                data.publishPeers(entry);
                try {
                    entry->animation.attach(*actor);
                    entry->animation.applyPeerPose(current, entry->eventCursor, true, *actor);
                    entry->skin.apply(client, *actor, *candidate.model, entry->xuid, entry->animation.skinGeometry());
                    entry->animation.setRenderTexture(entry->skin.renderTexture(), entry->skin.textureName());
                    entry->animation.attach(*actor);
                    entry->model=std::move(candidate.model);
                    data.delivered[entry->xuid] = {entry->session, entry->contentKey, entry->eventCursor};
                    data.failures.erase(entry->xuid);
                    data.peers.emplace(entry->xuid,entry);data.publishPeers();
                    data.decoded[{entry->digest,entry->texture}]=entry->model;
                } catch (...) {
                    data.peers.erase(entry->xuid);
                    data.publishPeers();
                    entry->animation.detach(*actor);
                    entry->skin.restore(client, *actor);
                    throw;
                }
                data.status();
            }
        } catch (std::exception const& error) {
            if (data.preparingSession == data.session && data.stillCurrent(data.preparing)) {
                if(data.native){
                    auto& failure=data.failures[data.preparing.xuid];
                    if(failure.stream!=data.preparing.session || failure.digest!=data.preparingDigest || failure.actor!=data.preparingActor)
                        failure={data.preparing.session,data.preparingDigest,{},data.preparingActor,0};
                    ++failure.count;failure.reason=error.what();
                }
                data.queue.failed(data.preparing, now);
                data.error = error.what();
                data.status();
            }
        }
    }
    if (data.preparation.valid()) return;
    auto eligible=[&](std::string const& id,bool visibleOnly) {
        auto actor=roomActor(level,id);
        auto const& k=data.latest.players.at(id).contentKey;
        if(data.native && actor && data.latest.assets.contains(k)){
            auto it=data.failures.find(id);
            if(it!=data.failures.end() && it->second.count>=3 && it->second.digest==data.latest.assets.at(k).digest
                && it->second.actor==actor->getOrCreateUniqueID().rawID)return false;
        }
        return data.latest.assets.contains(k) && (!data.native || (data.view && data.view->ready.contains(k))) && actor && actor!=player
            && actor->mSkin && !data.hides(id) && !data.peers.contains(id) && (!visibleOnly || data.recentlyDrawn(*actor,now));
    };
    auto pending=data.native?data.queue.next(data.latest,now,[&](auto const& id){return eligible(id,true);}):nullptr;
    if(!pending)pending=data.queue.next(data.latest,now,[&](auto const& id){return eligible(id,false);});
    if (!pending) return;
    auto asset = data.latest.assets.at(pending->contentKey);
    auto cache = data.root / "relay-cache";
    data.preparing = *pending;data.preparingDigest=asset.digest;
    data.preparingSession = data.session;
    auto* actor=roomActor(level,pending->xuid);
    if(!actor)return;
    data.preparingActor=actor->getOrCreateUniqueID().rawID;
    data.preparingInstance=reinterpret_cast<std::uintptr_t>(actor);
    std::erase_if(data.decoded,[](auto const& item){return item.second.expired();});
    auto existing=data.decoded[{asset.digest,pending->texture}].lock();
    auto path=data.native?data.view->ready.at(pending->contentKey):std::filesystem::path{};
    auto trace = data.measure ? data.root / "room-diagnostics" / std::to_string(data.preparingActor) / "animation-status.json" : std::filesystem::path{};
    data.preparation = std::async(std::launch::async, [relay=data.transport,asset,cache,path,existing,trace,texture=pending->texture] {
        auto shared=existing;
        if(!shared){
            auto pack=path.empty()?relay->obtain(asset,cache):loadRoomFile(path,asset);
            if(!texture.empty())pack.selectTexture(texture);
            pack.librarySource="server:"+asset.contentKey;
            shared=std::make_shared<ModelPack const>(std::move(pack));
        }
        if (!trace.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(trace.parent_path(), ec);
            if (ec) return Candidate{shared,AnimationRuntime::prepare(*shared,{})};
        }
        return Candidate{shared,AnimationRuntime::prepare(*shared,trace)};
    });
}

std::array<std::int64_t,2> NativeModelRelay::shoulderSeats(Actor const& owner) const {
    auto peer = impl->peer(&owner);
    if (!peer || peer->actorId != owner.getOrCreateUniqueID().rawID
        || roomActor(owner.getLevel(), peer->xuid) != &owner) return {};
    return peer->shoulderBirds.load();
}
void NativeModelRelay::frame() {
    if(impl->measure && impl->trackVisibility){
        auto now=Clock::now();std::lock_guard lock(impl->frameTimesMutex);
        if(impl->lastFrame!=Clock::time_point{} && impl->frameTimes.size()<4096)
            impl->frameTimes.push_back(std::chrono::duration<float,std::milli>(now-impl->lastFrame).count());
        impl->lastFrame=now;
    }
    auto peers = impl->drawing.load(std::memory_order_acquire);
    if (peers) for (auto const& [actor, peer] : peers->actors) peer->animation.remoteFrame();
}
std::vector<ModelEffects> NativeModelRelay::takeEffects(Player const& local) {
    std::vector<ModelEffects> result;
    for (auto const& [id, peer] : impl->peers) {
        auto actor = roomActor(local.getLevel(), id);
        if (!actor || actor != peer->actor) continue;
        auto sounds = peer->animation.takeSoundCues();
        auto particles = peer->animation.takeParticleCues();
        if (!peer->model || (sounds.empty() && particles.empty())) continue;
        result.push_back({peer, peer->model, actor->getEntityContext().getWeakRef(), std::move(sounds), std::move(particles)});
    }
    return result;
}
void NativeModelRelay::updateEntityOwners(NativeReplacedResources& resources, Player const& local) const {
    std::vector<NativeReplacedResources::PeerOwner> owners;
    for (auto const& [id, peer] : impl->peers) {
        // Previously bound projectiles still depend on this visible owner.
        if (!peer->model) continue;
        auto* player = roomActor(local.getLevel(), id);
        auto state = impl->latest.players.find(id);
        if (!player || player != peer->actor || state == impl->latest.players.end()) continue;
        NativeReplacedResources::PeerOwner owner{player->getOrCreateUniqueID().rawID, peer->model, state->second.roaming, {}};
        if (auto* vehicle = player->getVehicle())
            if (auto* first = vehicle->getFirstPassenger())
                owner.mount.emplace(vehicle->getOrCreateUniqueID().rawID, first->getOrCreateUniqueID().rawID);
        owners.push_back(std::move(owner));
    }
    resources.updatePeerOwners(owners);
}
void NativeModelRelay::prepareForRender(ActorRenderData const& render, AnimationComponent& component) {
    if (impl->trackVisibility && !render.mIsInUI && render.mActor && render.mActor->isPlayer()){
        std::lock_guard lock(impl->seenMutex);
        auto id=render.mActor->getOrCreateUniqueID().rawID;
        if(impl->lastSeen.size()>=512 && !impl->lastSeen.contains(id))
            std::erase_if(impl->lastSeen,[now=Clock::now()](auto const& item){return now-item.second>std::chrono::seconds(2);});
        if(impl->lastSeen.size()<512 || impl->lastSeen.contains(id))impl->lastSeen[id]=Clock::now();
    }
    if (auto peer = impl->peer(render.mActor); peer && peer->skin.isActive())
        peer->animation.prepareForRender(*peer->actor, component);
}
void NativeModelRelay::prepareGeometry(RenderController& controller, std::shared_ptr<DataDrivenGeometry> const& geometry, RenderParams& params) {
    if (auto peer = impl->peer(params.mActor)) peer->animation.prepareGeometry(controller, geometry, params);
}
void NativeModelRelay::updateTexture(IClientInstance& client, ClientSkinInfoData& skin, SerializedSkinRef const& serialized) {
    auto peers = impl->drawing.load(std::memory_order_acquire);
    if (peers) peers->forEachSkin([&](SkinRuntime& target) { target.updateRenderTexture(client, skin, serialized); });
}
void NativeModelRelay::bindTexture(SerializedSkinRef const& serialized, std::shared_ptr<ActorResourceDefinition> const& resource) {
    auto peers = impl->drawing.load(std::memory_order_acquire);
    if (peers) peers->forEachSkin([&](SkinRuntime& target) { target.bindResourceTexture(serialized, resource); });
}
bool NativeModelRelay::needsSurfaces(Actor const& actor) const {
    auto peer = impl->peer(&actor);
    return peer && peer->animation.needsIndividualSurfaces(actor);
}
bool NativeModelRelay::rendersPlayer(Actor const& actor) const {
    auto peer = impl->peer(&actor);
    return peer && peer->skin.isActive() && peer->animation.rendersPlayer(actor);
}
unsigned NativeModelRelay::handDrawMask(Actor const& actor, bool offhand, bool nativeUi) const {
    auto peer = impl->peer(&actor);
    return peer && peer->skin.isActive() ? peer->animation.handDrawMask(actor, offhand, nativeUi) : 1u;
}
bool NativeModelRelay::drivesLanceCharge(Actor const& actor) const {
    auto peer = impl->peer(&actor);
    return peer && peer->skin.isActive() && peer->animation.drivesLanceCharge(actor);
}
bool NativeModelRelay::drawParrot(Actor const& owner, BaseActorRenderContext& context,
    ActorRenderData& data, std::function<void()> const& draw) {
    auto peer = impl->peer(&owner);
    if (!peer || !peer->skin.isActive() || !peer->animation.rendersPlayer(owner)) return false;
    drawShoulderParrot(peer->animation, owner, context, data, draw);
    return true;
}
bool NativeModelRelay::suppressGlide(Actor const& actor, bool firstPerson, std::array<float, 2> const& rotation) {
    auto peer = impl->peer(&actor);
    return peer && peer->animation.suppressNativeGlideRotation(actor, firstPerson, rotation);
}
std::span<BoneVisual const> NativeModelRelay::visuals(SkinnedMesh const& mesh, RenderController const* controller,
    std::span<BoneVisual> scratch, std::vector<BoneVisual>& overflow) {
    auto peers = impl->drawing.load(std::memory_order_acquire);
    if (peers) for (auto const& [actor, peer] : peers->actors) {
        auto result = peer->animation.boneVisuals(mesh, controller, scratch, overflow);
        if (!result.empty()) return result;
    }
    return {};
}
}
