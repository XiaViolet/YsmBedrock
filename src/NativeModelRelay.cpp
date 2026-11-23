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
#include <format>
#include <future>
#include <utility>

namespace ysm {
using Clock = std::chrono::steady_clock;
namespace {
struct RemoteModelResources {
    std::shared_ptr<ModelPack const> model;
    std::shared_ptr<CompiledPlayerModel const> compiled;
};
struct InstalledPeer {
    std::string xuid, session, contentKey, texture, digest;
    Player* actor{};
    std::int64_t actorId{};
    std::atomic<std::array<std::int64_t,2>> shoulderBirds{};
    AnimationRuntime animation;
    SkinRuntime skin;
    RemoteModelResources resources;
    Clock::time_point nextSkinCheck{};
    std::uint64_t actionSerial{}, eventCursor{}, lastSerial{};
    bool worldPoseReported = false;
    std::atomic_bool textureChanged{};
};
struct Candidate {
    RemoteModelResources resources;
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
std::string stageLabel(std::string const& stage) {
    static std::map<std::string,std::string> const labels{
        {"waiting","等待下载"},{"awaiting_host","等房主收取"},{"downloading","下载中"},{"paused","下载已暂停"},
        {"verifying","校验中"},{"cached","等待准备模型"},{"preparing","正在准备模型"},{"awaiting_actor","等待角色出现"},
        {"awaiting_draw","等待角色绘制"},{"displayed","已显示"},{"vanilla","本机显示原版"},{"failed","失败"},
        {"failed_download","下载或校验失败"},{"failed_prepare","准备模型失败"},{"failed_draw","绘制绑定失败"},
        {"denied","房主已拒绝"},{"conflicting_archive","同一模型标识的文件不一致"},{"unreported","尚未回报"},{"unsupported","此版本不支持回报"},
        {"failed: provider timed out","下载超时"},{"failed: upstream failed","房主收取失败"},
        {"failed: file verification or storage failed","校验或写入缓存失败"},{"failed: source unavailable","模型来源不可用"},
        {"failed: host is not acquiring this file","房主接收超时"},{"failed: invalid local cache","本机缓存读取失败"},
        {"failed: invalid chunk length","收到的分块长度错误"},{"failed: denied","来源已拒绝传输"}};
    auto it=labels.find(stage);return it==labels.end()?stage:it->second;
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
    detail::RelayWarmModels<RemoteModelResources> warmModels;
    using ModelKey = decltype(warmModels)::Key;
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
    struct PreparationFailure {std::string stream,digest,reason;std::int64_t actor{};unsigned count{};bool drawing{};};
    std::map<std::string,PreparationFailure> failures;
    std::string error;
    bool endpointsConfirmed = false;
    bool hideOthers = false;
    bool measure{};
    std::uint64_t freshPreparations{}, reusedPreparations{}, skinReapplications{};
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
            {"frame_samples",times.size()},{"states",view->states},
            {"fresh_model_preparations",freshPreparations},{"reused_model_preparations",reusedPreparations},
            {"remote_skin_reapplications",skinReapplications}};
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
    std::string stage(RelayPose const& pose,Player* actor) const {
        auto const& id=pose.xuid;
        if(hides(id))return "vanilla";
        auto bound=peers.find(id);
        if(actor && bound!=peers.end() && bound->second->session==pose.session && bound->second->actor==actor
            && bound->second->actorId==actor->getOrCreateUniqueID().rawID)
            return bound->second->worldPoseReported?"displayed":"awaiting_draw";
        if(preparation.valid() && preparing.xuid==id && preparing.session==pose.session && preparingSession==session)return "preparing";
        auto failed=failures.find(id);
        if(failed!=failures.end() && failed->second.stream==pose.session)return failed->second.drawing?"failed_draw":"failed_prepare";
        if(view){
            if(view->ready.contains(pose.contentKey))return actor?"cached":"awaiting_actor";
            auto state=view->states.find(pose.contentKey);
            if(state!=view->states.end() && !state->second.empty())return state->second;
        }
        return "waiting";
    }

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
        freshPreparations = reusedPreparations = skinReapplications = 0;
        warmModels.clear(); view.reset(); native=false; selectionTexture.clear(); trackVisibility=false;
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
void NativeModelRelay::recordServerArrow(Player& shooter,std::int64_t entity,bool crossbow){
    bool hostPlayer = shooter.mIsHostingPlayer;
    impl->room.recordServerArrow(hostPlayer ? std::string{} : roomPlayerKey(shooter), hostPlayer, entity, crossbow);
}
void NativeModelRelay::serverTick(Level& level){impl->room.serverTick(level);}
void NativeModelRelay::roomCommand(std::string command){
    if(command.starts_with("retry:")){
        if(command.size()<=39 || command[38]!='|')return;
        auto id=command.substr(39);
        auto wearer=impl->latest.players.find(id);
        if(!impl->native || wearer==impl->latest.players.end() || wearer->second.session!=command.substr(6,32) || impl->hides(id))return;
        impl->failures.erase(id);impl->queue.retry(id);
    }
    impl->room.command(std::move(command));
}
Json NativeModelRelay::roomInfo() const {
    auto view=impl->view;
    Json offers=Json::array();
    if(view)for(auto const& o:view->offers)offers.push_back({{"key",o.peer+"|"+o.asset.contentKey},{"title",o.asset.title}});
    Json denied=Json::array();
    if(view && impl->room.host())for(auto const& [key,title]:view->deniedModels)
        denied.push_back({{"key",key},{"title",title},{"temporary",view->sessionDenied.contains(key)}});
    Json observations=Json::array();
    if(view && impl->room.host() && impl->level){
        std::map<std::string,std::string> names;
        auto name=[&](std::string const& id){
            if(id==impl->localXuid)return std::string("房主");
            auto [it,fresh]=names.try_emplace(id,id);
            if(fresh)if(auto* actor=roomActor(*impl->level,id))it->second=actor->getNameTag();
            return it->second;
        };
        for(auto const& [observer,subjects]:view->observers)for(auto const& [subject,report]:subjects)
            observations.push_back(name(observer)+" 看 "+name(subject)+" · "+stageLabel(report.stage));
    }
    return {{"host",impl->room.host()},{"active",view && view->connected},{"auto_receive",!view || view->autoReceive},
        {"upload_retry",view?view->uploadRetryTarget:std::string{}},{"upload_bps",view?view->uploadRate:0},{"download_bps",view?view->downloadRate:0},{"rate_limit",view?view->rateLimit:0},
        {"message",view?view->message:impl->error},{"offers",std::move(offers)},{"denied",std::move(denied)},{"observations",std::move(observations)}};
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
    auto roomRate=[&](std::string const& id){
        if(!impl->view)return std::string{};
        auto found=impl->view->peerRates.find(id);
        if(found==impl->view->peerRates.end())return std::string{};
        return std::format("上传 {:.0f} · 下载 {:.0f} KiB/s",found->second.upload/1024.,found->second.download/1024.);
    };
    for(auto const& [id,pose]:impl->latest.players){
        auto* actor=roomActor(local->getLevel(),id);
        auto stage=impl->stage(pose,actor),label=stageLabel(stage);
        if(stage=="failed_prepare" || stage=="failed_draw")label+=" · "+impl->failures.at(id).reason;
        if(impl->view && (stage=="downloading" || stage=="awaiting_host" || stage=="paused")){
            auto p=impl->view->progress.find(pose.contentKey);
            if(p!=impl->view->progress.end()){
                auto bytes=stage=="awaiting_host"?p->second.upstream:p->second.received;
                auto rate=stage=="awaiting_host"?p->second.upstreamBytesPerSecond:p->second.bytesPerSecond;
                label+=std::format(" · {:.0f} KiB/s · {:.0f}/{:.0f} KiB",rate/1024.,bytes/1024.,p->second.total/1024.);
            }
        }
        rows.push_back({{"id",id},{"name",actor?actor->getNameTag():id},{"rates",roomRate(id)},{"hidden",impl->hiddenPlayers.contains(id)},
            {"state",label},{"key",pose.contentKey},{"stream",pose.session},{"retry",impl->native && (stage=="waiting" || stage=="downloading"
                || stage=="paused" || stage=="awaiting_host" || stage.starts_with("failed"))}});
    }
    if(impl->view)for(auto const& [id,state]:impl->view->peerStates)
        if(!impl->latest.players.contains(id)){
            auto* actor=roomActor(local->getLevel(),id);
            rows.push_back({{"id",id},{"name",actor?actor->getNameTag():id},{"hidden",impl->hiddenPlayers.contains(id)},{"state",stageLabel(state)},{"key",""}});
        }
    if(impl->view)for(auto const& [id,rate]:impl->view->peerRates){
        if(id==impl->localXuid || impl->latest.players.contains(id) || impl->view->peerStates.contains(id))continue;
        auto* actor=roomActor(local->getLevel(),id);
        if(actor && actor!=local)rows.push_back({{"id",id},{"name",actor->getNameTag()},{"hidden",impl->hiddenPlayers.contains(id)},
            {"rates",roomRate(id)},{"state","正在传输房间模型"},{"key",""}});
    }
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
            auto pose=model?local.outgoingPose(*player,data.eventSequence,data.outgoingEvents.empty()):RelayPose{};
            if(model)pose.shoulderBirds=observeShoulderPassengers(*player,resources);
            if(pose.texture!=data.selectionTexture){
                data.outgoingEvents.clear();data.selectionTexture=pose.texture;
            }
            if(!pose.events.empty())data.outgoingEvents=std::move(pose.events);
            pose.events=data.outgoingEvents;
            pose.xuid=data.localXuid;pose.serial=++data.serial;pose.contentKey=key;
            std::set<std::string> visible;
            RoomDisplays displays;
            for(auto const& [id,state]:data.latest.players){
                auto* remote=roomActor(player->getLevel(),id);
                if(remote && !data.hides(id) && data.recentlyDrawn(*remote,now))visible.insert(id);
                if(!data.room.host()){
                    auto stage=data.stage(state,remote);
                    displays[id]={state.session,stage.starts_with("failed:")?"failed_download":std::move(stage)};
                }
            }
            data.room.update(model?model->librarySource:std::string{},model?model->id:std::string{},std::move(pose),std::move(visible),std::move(displays));
            data.nextPoll=now+std::chrono::milliseconds(100);
        }
    }else if (!data.exchange.valid() && now >= data.nextPoll) {
        auto wire = model ? local.outgoingPose(*player,data.eventSequence, data.outgoingEvents.empty()) : RelayPose{};
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
    auto authorizedModel = [&](Impl::ModelKey const& key) {
        return std::ranges::any_of(data.latest.players, [&](auto const& item) {
            auto const& [id, pose] = item;
            auto asset = data.latest.assets.find(pose.contentKey);
            return !data.hides(id) && pose.texture == key.second && asset != data.latest.assets.end()
                && asset->second.digest == key.first;
        });
    };
    data.warmModels.prune(now,authorizedModel);
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
            Impl::ModelKey modelKey{entry->digest,entry->texture};
            if (authorizedModel(modelKey)) data.warmModels.keep(std::move(modelKey),entry->resources,now);
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
            if (now>=entry->nextSkinCheck && actor->mSkin) {
                entry->nextSkinCheck=now+std::chrono::seconds(1);
                try { if (entry->skin.refreshIfReplaced(client,*actor)) ++data.skinReapplications; }
                catch(std::exception const& error){data.error=error.what();data.status();}
            }
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
        bool binding=false;
        try {
            auto candidate = data.preparation.get();
            auto const& pending = data.preparing;
            auto actor = roomActor(level, pending.xuid);
            if (data.preparingSession==data.session && data.stillCurrent(pending)
                && (!actor || reinterpret_cast<std::uintptr_t>(actor)!=data.preparingInstance)
                && authorizedModel({data.preparingDigest,pending.texture}))
                data.warmModels.keep({data.preparingDigest,pending.texture},candidate.resources,now);
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
                    binding=true;
                    entry->animation.attach(*actor);
                    entry->animation.applyPeerPose(current, entry->eventCursor, true, *actor);
                    entry->skin.apply(client, *actor, *candidate.resources.model, entry->xuid, entry->animation.skinGeometry());
                    entry->animation.setRenderTexture(entry->skin.renderTexture(), entry->skin.textureName());
                    entry->animation.attach(*actor);
                    entry->resources=std::move(candidate.resources);
                    data.delivered[entry->xuid] = {entry->session, entry->contentKey, entry->eventCursor};
                    data.failures.erase(entry->xuid);
                    data.peers.emplace(entry->xuid,entry);data.publishPeers();
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
                    ++failure.count;failure.reason=error.what();failure.drawing=binding;
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
    auto existing = data.warmModels.find({asset.digest, pending->texture});
    if (!existing.model) {
        for (auto const& [id, peer] : data.peers) {
            if (peer->digest == asset.digest && peer->texture == pending->texture) {
                existing = peer->resources;
                break;
            }
        }
    }
    if (existing.model) ++data.reusedPreparations;
    else ++data.freshPreparations;
    auto path=data.native?data.view->ready.at(pending->contentKey):std::filesystem::path{};
    auto trace = data.measure ? data.root / "room-diagnostics" / std::to_string(data.preparingActor) / "animation-status.json" : std::filesystem::path{};
    data.preparation = std::async(std::launch::async, [relay=data.transport,asset,cache,path,existing,trace,texture=pending->texture] {
        auto shared=existing;
        if(!shared.model){
            auto pack=path.empty()?relay->obtain(asset,cache):loadRoomFile(path,asset);
            if(!texture.empty())pack.selectTexture(texture);
            pack.librarySource="server:"+asset.contentKey;
            shared.model=std::make_shared<ModelPack const>(std::move(pack));
            shared.compiled=AnimationRuntime::compileModel(*shared.model);
        }
        if (!trace.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(trace.parent_path(), ec);
            if (ec) return Candidate{shared,AnimationRuntime::prepare(*shared.model,{},shared.compiled)};
        }
        return Candidate{shared,AnimationRuntime::prepare(*shared.model,trace,shared.compiled)};
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
        if (!peer->resources.model || (sounds.empty() && particles.empty())) continue;
        result.push_back({peer, peer->resources.model, actor->getEntityContext().getWeakRef(), std::move(sounds), std::move(particles)});
    }
    return result;
}
void NativeModelRelay::updateEntityOwners(NativeReplacedResources& resources, Player const& local) const {
    std::vector<NativeReplacedResources::PeerOwner> owners;
    for (auto const& [id, peer] : impl->peers) {
        // Previously bound projectiles still depend on this visible owner.
        if (!peer->resources.model) continue;
        auto* player = roomActor(local.getLevel(), id);
        auto state = impl->latest.players.find(id);
        if (!player || player != peer->actor || state == impl->latest.players.end()) continue;
        peer->animation.trackSeats(resources.weak_from_this());
        NativeReplacedResources::PeerOwner owner{player->getOrCreateUniqueID().rawID, peer->resources.model, state->second.roaming, state->second.arrows, {}};
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
    if (auto peer = impl->peer(render.mActor); peer && peer->skin.isActive()) {
        if (peer->textureChanged.exchange(false))
            peer->animation.setRenderTexture(peer->skin.publishedTextures(), peer->skin.textureName());
        peer->animation.prepareForRender(*peer->actor, component);
    }
}
void NativeModelRelay::prepareGeometry(RenderController& controller, std::shared_ptr<DataDrivenGeometry> const& geometry, RenderParams& params) {
    if (auto peer = impl->peer(params.mActor)) peer->animation.prepareGeometry(controller, geometry, params);
}
void NativeModelRelay::updateTexture(IClientInstance& client, ClientSkinInfoData& skin, SerializedSkinRef const& serialized) {
    auto peers = impl->drawing.load(std::memory_order_acquire);
    if (peers) peers->forEachPeer([&](InstalledPeer& target) {
        // A skin callback may run inside a render operation. Let the next
        // actor preparation adopt the publication before evaluating its pose.
        if (target.skin.updateRenderTexture(client, skin, serialized)) target.textureChanged.store(true);
    });
}
void NativeModelRelay::bindTexture(SerializedSkinRef const& serialized, std::shared_ptr<ActorResourceDefinition> const& resource) {
    auto peers = impl->drawing.load(std::memory_order_acquire);
    if (peers) peers->forEachPeer([&](InstalledPeer& target) { target.skin.bindResourceTexture(serialized, resource); });
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
    std::span<BoneVisual> scratch, std::vector<BoneVisual>& overflow, Actor const* owner) {
    if (owner) {
        auto peer = impl->peer(owner);
        return peer ? peer->animation.boneVisuals(mesh, controller, scratch, overflow, owner) : std::span<BoneVisual const>{};
    }
    auto peers = impl->drawing.load(std::memory_order_acquire);
    if (peers) for (auto const& [actor, peer] : peers->actors) {
        auto result = peer->animation.boneVisuals(mesh, controller, scratch, overflow);
        if (!result.empty()) return result;
    }
    return {};
}
}
