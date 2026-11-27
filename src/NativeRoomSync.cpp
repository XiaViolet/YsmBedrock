#include "Localization.h"
#include "NativeRoomSync.h"
#include "RoomAssets.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <future>
#include <algorithm>
#include <atomic>
#include <thread>

namespace ysm {
namespace fs = std::filesystem;
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto maxBacklog = 2u << 20;
struct LocalState { std::string source, title; RelayPose pose; std::set<std::string> visible; RoomDisplays displays; };
struct Settings {
    RoomRules rules;
    std::uint64_t capacity{};
    std::vector<std::string> shares;
};
Settings settings(fs::path const& root) {
    auto file = root / "room-sync.json";
    Json defaults{{"auto_receive_current",true},{"shared_only",false},{"denied_content_keys",Json::array()},
        {"shared_models",Json::array()},{"bytes_per_second",RoomRules::defaultBytesPerSecond},{"cache_mib",512}};
    if (!fs::exists(file)) {
        std::ofstream out(file);
        if (!(out << defaults.dump(2))) throw std::runtime_error("Cannot create room-sync.json");
    }
    if (fs::file_size(file) > 65536) throw std::runtime_error("room-sync.json exceeds 64 KiB");
    auto j = Json::parse(std::ifstream(file));
    Settings result;
    result.rules.autoReceiveCurrent=j.value("auto_receive_current",true);
    result.rules.sharedOnly=j.value("shared_only",false);
    result.rules.bytesPerSecond=std::clamp(j.value("bytes_per_second",RoomRules::defaultBytesPerSecond),16384,4<<20);
    result.capacity=static_cast<std::uint64_t>(std::clamp(j.value("cache_mib",512),128,8192))<<20;
    result.rules.deniedKeys=j.value("denied_content_keys",std::set<std::string>{});
    result.shares=j.value("shared_models",std::vector<std::string>{});
    if (result.shares.size()>64 || result.rules.deniedKeys.size()>256) throw std::runtime_error("Room list limit exceeded");
    return result;
}
fs::path libraryPath(fs::path const& root,std::string const& source) {
    if (source.empty() || source.size()>2048) throw std::runtime_error("Invalid model source");
    auto base=fs::canonical(root);
    auto path=fs::canonical(base/fs::path(std::u8string(source.begin(),source.end())));
    auto rel=path.lexically_relative(base);
    if (rel.empty() || rel=="." || rel.is_absolute() || *rel.begin()=="..") throw std::runtime_error("Model outside selected directory");
    return path;
}
std::string filename(fs::path const& path) {
    auto name=path.filename().u8string();
    return {name.begin(),name.end()};
}
}
struct NativeRoomSync::Impl {
    RoomTransport wire;
    fs::path root;
    std::mutex mutex;
    std::condition_variable wake;
    std::jthread worker;
    RoomRole role=RoomRole::None;
    std::string self;
    std::uint64_t generation{};
    std::deque<RoomWireEvent> input;
    struct Pending {RoomWire wire;std::shared_ptr<std::atomic_bool> permit;};
    std::deque<Pending> output;
    using Permission = std::tuple<std::string,std::string,std::string>;
    std::map<Permission,std::shared_ptr<std::atomic_bool>> permits;
    std::size_t incomingBytes{}, outgoingBytes{};
    std::uint64_t acceptedTx{}, received{};
    std::set<std::string> endpoints;
    struct Shot {std::string shooter;std::int64_t entity;bool crossbow;};
    std::deque<Shot> shots;
    std::deque<std::string> commands;
    std::optional<LocalState> local;
    std::atomic<std::shared_ptr<RoomView const>> latest;
    void change(RoomRole next,std::string id) {
        std::lock_guard lock(mutex);
        ++generation; role=next; self=std::move(id);
        for(auto const& [_,permit]:permits)permit->store(false);
        permits.clear();input.clear(); output.clear(); endpoints.clear(); shots.clear(); commands.clear(); local.reset();
        incomingBytes=outgoingBytes=0;acceptedTx=received=0; latest.store({}); wake.notify_one();
    }
    void run(std::stop_token stop) {
        std::uint64_t live{};
        std::unique_ptr<RoomAssets> files;
        std::unique_ptr<RoomProtocol> room;
        std::string source, key, message;
        std::optional<RelayAsset> current;
        std::deque<fs::path> shareWork;
        std::vector<RelayAsset> shares;
        std::string pendingCollect;
        using Job=std::future<RoomAssets::Publication>;
        struct PublishJob {Job task;std::uint64_t generation,revision;bool worn;std::string key;};
        struct VerifyJob {Job task;std::uint64_t generation;RelayAsset asset;bool cancelled{};};
        std::vector<PublishJob> publications;
        std::vector<VerifyJob> verifications;
        std::uint64_t sourceRevision{},shareRevision{};
        RoomRules activeRules;
        std::map<std::string,std::pair<std::string,fs::path>> readyFiles;
        auto checkPaths=Clock::now();
        auto nextRates=Clock::time_point{};
        fs::path pendingSource;
        std::string pendingTitle;
        auto done=[](Job& f){return f.wait_for(std::chrono::seconds(0))==std::future_status::ready;};
        while (!stop.stop_requested()) {
            std::unique_lock lock(mutex);
            auto hasWork=[&]{return stop.stop_requested() || generation!=live || !input.empty() || !commands.empty() || !shots.empty() || local.has_value();};
            if(endpoints.empty() && publications.empty() && verifications.empty()){
                 
                 
                wake.wait(lock,hasWork);
            }else{
                wake.wait_for(lock,std::chrono::milliseconds(25),hasWork);
            }
            if (stop.stop_requested()) break;
            auto gen=generation;
            auto next=role;
            auto who=self;
            std::deque<RoomWireEvent> events; events.swap(input); incomingBytes=0;
            std::deque<std::string> tasks; tasks.swap(commands);
            std::deque<Shot> newShots;newShots.swap(shots);
            auto state=std::exchange(local,{});
            bool haveEndpoints=!endpoints.empty();
             
            auto congested=outgoingBytes>maxBacklog-(256u<<10) || output.size()>440;
            lock.unlock();
            try {
                auto applyRules=[&](RoomRules rules){
                    rules.sessionDenied=activeRules.sessionDenied;
                    for(auto const& k:rules.deniedKeys)rules.sessionDenied.erase(k);
                    room->setRules(rules);activeRules=std::move(rules);
                };
                auto refresh=[&]{
                    auto cfg=settings(root);
                    applyRules(cfg.rules);files->setCapacity(cfg.capacity);
                    ++shareRevision;shares.clear(); shareWork.clear(); room->share({});
                    for (auto const& s:cfg.shares) {
                        try { shareWork.push_back(libraryPath(root/"models",s)); }
                        catch(std::exception const& e){message=e.what();}
                    }
                    auto shared=root/"room-share";
                    fs::create_directories(shared);
                    for(auto const& entry:fs::directory_iterator(shared)){
                        if(shareWork.size()>=64) break;
                        if(entry.is_symlink()) continue;
                        auto p=libraryPath(shared,filename(entry.path()));
                        if(entry.is_directory() || entry.is_regular_file()) shareWork.push_back(std::move(p));
                    }
                };
                if (gen!=live) {
                    room.reset();activeRules={};pendingSource.clear();readyFiles.clear();
                    if(files){files->clearTransfers();files->pin({});}
                    live=gen; source.clear(); key.clear(); current.reset(); shares.clear(); shareWork.clear(); pendingCollect.clear();message.clear();
                    if(next!=RoomRole::None && !who.empty()) {
                        if(!files) files=std::make_unique<RoomAssets>(root/"room-cache");
                        RoomFiles io;
                        io.has=[&](auto const& a){return files->has(a) || files->reuseLibrary(a, root/"models");};
                        io.read=[&](auto const& a,auto o,auto n){return files->read(a,o,n);};
                        io.append=[&](auto const& a,auto o,auto b){files->append(a,o,b);};
                        io.pause=[&](auto const& a){files->pause(a);};
                        io.reserve=[&](auto const& a){return files->reserveTransfer(a);};
                        io.finish=[&](auto const& a){
                            if(verifications.size()>=8)throw std::runtime_error("Room verification queue is full");
                            verifications.push_back({{},live,a});
                        };
                        io.pollFinish=[&](auto const& a){
                            for(auto it=verifications.begin();it!=verifications.end();++it){
                                if(it->generation!=live || it->cancelled || it->asset.digest!=a.digest)continue;
                                if(!it->task.valid() || !done(it->task))return 0;
                                auto task=std::move(it->task);verifications.erase(it);
                                try {files->commitPublish(task.get());return 1;}
                                catch(std::exception const& e){message=e.what();return -1;}
                            }
                            return -1;
                        };
                        io.cancel=[&](auto const& a){
                            for(auto& v:verifications)if(v.generation==live && v.asset.digest==a.digest)v.cancelled=true;
                            files->cancel(a);
                        };
                        room=std::make_unique<RoomProtocol>(next==RoomRole::Host,who,ModelRelay::newSession(),std::move(io));
                        try {refresh();} catch(std::exception const& e) {RoomRules closed; closed.sharedOnly=true; closed.autoReceiveCurrent=false;applyRules(closed);message=e.what();}
                    }
                }
                std::erase_if(verifications,[&](auto& v){return (v.generation!=live || v.cancelled) && (!v.task.valid() || done(v.task));});
                std::erase_if(publications,[&](auto& j){
                    if(!done(j.task))return false;
                    if(!room || j.generation!=live || j.revision!=(j.worn?sourceRevision:shareRevision))return true;
                    try {
                        auto asset=files->commitPublish(j.task.get());
                        if(j.worn){
                            if(asset.contentKey!=j.key)throw std::runtime_error("Worn model content changed on disk");
                            current=std::move(asset);
                        } else {shares.push_back(std::move(asset));room->share(shares);}
                    }catch(std::exception const& e){message=e.what();}
                    return true;
                });
                if(!room) continue;
                auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
                for(auto& event:events){
                    auto peer=next==RoomRole::Guest ? std::string{}:event.peer;
                    if(event.kind==RoomWireKind::Joined)room->joined(peer);
                    else if(event.kind==RoomWireKind::Left)room->left(peer);
                    else room->receive(peer,event.payload,ms);
                }
                for(auto const& shot:newShots)room->serverArrow(shot.shooter,shot.entity,shot.crossbow);
                for(auto const& task:tasks){
                    if(task=="reload"){refresh();source.clear();}
                    else if(next==RoomRole::Host && (task.starts_with("deny-once:") || task.starts_with("allow-once:"))){
                        auto rules=activeRules;
                        if(task.starts_with("deny-once:")){
                            auto k=task.substr(10);
                            if(!room->frame().assets.contains(k))continue;
                            if(rules.sessionDenied.size()>=256)throw std::runtime_error("Session deny list is full");
                            rules.sessionDenied.insert(k);
                        }else if(!rules.sessionDenied.erase(task.substr(11)))continue;
                        room->setRules(rules);activeRules=std::move(rules);
                    }
                    else if(task=="cycle-rate" || (next==RoomRole::Host && (task=="toggle-auto" || task.starts_with("deny:") || task.starts_with("allow:")))){
                        auto path=root/"room-sync.json";
                        if(fs::file_size(path)>65536)throw std::runtime_error("Room settings are too large");
                        auto config=Json::parse(std::ifstream(path));
                        if(task=="cycle-rate"){
                            constexpr std::size_t presets[]={256u<<10,1u<<20,2u<<20,4u<<20};
                            auto nextRate=std::ranges::upper_bound(presets,activeRules.bytesPerSecond);
                            config["bytes_per_second"]=nextRate==std::end(presets)?presets[0]:*nextRate;
                        }else if(task=="toggle-auto")config["auto_receive_current"]=!activeRules.autoReceiveCurrent;
                        else{
                            auto keys=config.value("denied_content_keys",std::set<std::string>{});
                            if(task.starts_with("allow:")){
                                if(!keys.erase(task.substr(6)))continue;
                            }else{
                                auto k=task.substr(5);
                                if((!room->frame().assets.contains(k) && !activeRules.sessionDenied.contains(k)) || keys.contains(k))continue;
                                if(keys.size()>=256)throw std::runtime_error("Room deny list is full");
                                keys.insert(k);
                            }
                            config["denied_content_keys"]=std::move(keys);
                        }
                        auto encoded=config.dump(2);
                        if(encoded.size()>65536)throw std::runtime_error("Room settings are too large");
                        auto temp=root/("room-settings-"+ModelRelay::newSession()+".tmp");
                        {std::ofstream out(temp,std::ios::binary);out<<encoded;out.close();if(!out)throw std::runtime_error("Cannot save room rules");}
                        std::error_code ec;fs::rename(temp,path,ec);
                        if(ec){fs::remove(temp);throw std::runtime_error("Cannot replace room-sync.json");}
                        applyRules(settings(root).rules);
                    }
                    else if(task.starts_with("retry:") && task.size()>39 && task[38]=='|'){
                        auto id=task.substr(39),streamId=task.substr(6,32);
                        auto wearer=room->frame().players.find(id);
                        if(wearer!=room->frame().players.end() && wearer->second.session==streamId){
                            readyFiles.erase(wearer->second.contentKey);room->retry(id,streamId);
                            message=gameText("已请求重新下载玩家模型", "Requested player model download again");
                        }
                    }
                    else if(task.starts_with("retry-upload:") && haveEndpoints && room->connected()
                        && !key.empty() && task.substr(13)==room->session()+"|"+key){
                        if(current && room->canRetryUpload()){
                            room->retryUpload();message=gameText("已请求重新上传当前模型", "Requested current model upload again");
                        }else if(!current && pendingSource.empty() && !std::ranges::any_of(publications,[&](auto const& job){
                            return job.worn && job.generation==live && job.revision==sourceRevision;
                        })){
                             
                            source.clear();message=gameText("正在重新准备上传文件", "Preparing file for upload again");
                        }
                    }
                    else if(task.starts_with("select:"))room->select(task.substr(7));
                    else if(task.starts_with("collect:")){
                        auto k=task.substr(8);auto a=room->frame().assets.find(k);
                        if(a!=room->frame().assets.end() && a->second.selectable){pendingCollect=k;room->select(k);}
                    } else if(task.starts_with("offer:")){
                        auto split=task.find('|',6);
                        if(split!=std::string::npos)room->acceptOffer(task.substr(6,split-6),task.substr(split+1));
                    }
                }
                 
                if(!haveEndpoints){
                    if(!source.empty() || current){
                        source.clear();key.clear();current.reset();pendingSource.clear();++sourceRevision;
                        room->publish({},RelayPose{});
                    }
                    state.reset();
                }
                if(state){
                    if(source!=state->source || key!=state->pose.contentKey){
                        source=state->source;key=state->pose.contentKey;current.reset();++sourceRevision;pendingSource.clear();
                        try {
                            if(source.starts_with("server:")){
                                auto a=room->frame().assets.find(key);
                                if(a!=room->frame().assets.end() && a->second.selectable && files->has(a->second)) current=a->second;
                            } else if(!key.empty()){
                                pendingSource=libraryPath(root/"models",source);
                                pendingTitle=state->title;
                            }
                        } catch(std::exception const& e){message=e.what();}
                    }
                    room->setVisible(std::move(state->visible));room->publish(current,std::move(state->pose));
                    room->reportDisplays(state->displays);
                }
                std::vector<RoomWire> batch;
                if(next==RoomRole::Host && haveEndpoints && Clock::now()>=nextRates){
                    room->updateRates(wire.peerDownloads(),wire.uploadBytesPerSecond(),ms);
                    nextRates=Clock::now()+std::chrono::seconds(1);
                }
                if(!congested) batch=room->tick(ms);
                 
                 
                auto running=std::count_if(verifications.begin(),verifications.end(),[](auto const& job){return job.task.valid();});
                VerifyJob* best=nullptr;
                for(auto& job:verifications){
                    if(job.cancelled || job.generation!=live || job.task.valid())continue;
                    if(!best || room->priority(job.asset.contentKey)<room->priority(best->asset.contentKey))best=&job;
                }
                if(best && (running==0 || (running==1 && room->priority(best->asset.contentKey)<=RoomPriority::Current))){
                    try {best->task=files->beginFinish(best->asset);}
                    catch(std::exception const& e){message=e.what();best->cancelled=true;files->cancel(best->asset);}
                }
                bool busy=false;
                for(auto const& [k,s]:room->statuses()) if(s=="downloading" || s=="waiting" || s=="verifying"){busy=true;break;}
                if(!pendingSource.empty() && publications.size()<2){
                    auto path=std::exchange(pendingSource,{});
                    publications.push_back({files->beginPublish(path,pendingTitle,false),live,sourceRevision,true,key});
                }
                if(haveEndpoints && !busy && batch.empty() && !shareWork.empty() && publications.empty()){
                    auto path=shareWork.front();shareWork.pop_front();
                    try{publications.push_back({files->beginPublish(path,filename(path),true),live,shareRevision,false,{}});}
                    catch(std::exception const& e){message=e.what();}
                }
                auto view=std::make_shared<RoomView>();view->frame=room->frame();view->states=room->statuses();
                view->peerStates=room->peerStates();view->offers=room->offers();view->connected=room->connected();
                if(haveEndpoints && view->connected && !key.empty() && (room->canRetryUpload() || (!current && pendingSource.empty()
                    && !std::ranges::any_of(publications,[&](auto const& job){return job.worn && job.generation==live && job.revision==sourceRevision;}))))
                    view->uploadRetryTarget=room->session()+"|"+key;
                view->deniedModels=room->deniedModels();
                view->sessionDenied=activeRules.sessionDenied;
                view->progress=room->progress();view->observers=room->observers();
                view->downloadRate=room->downloadBytesPerSecond();view->uploadRate=wire.uploadBytesPerSecond();
                view->rateLimit=activeRules.bytesPerSecond;
                view->peerRates=room->peerRates();
                std::set<std::string> pins;
                if(current)pins.insert(current->digest);
                for(auto const& a:shares)pins.insert(a.digest);
                for(auto const& [id,p]:view->frame.players){
                    auto a=view->frame.assets.find(p.contentKey);
                    if(a!=view->frame.assets.end())pins.insert(a->second.digest);
                }
                files->pin(std::move(pins));
                if(Clock::now()>=checkPaths){
                    readyFiles.clear();checkPaths=Clock::now()+std::chrono::seconds(3);
                }
                for(auto const& [k,a]:view->frame.assets){
                    auto found=readyFiles.find(k);
                    if(found!=readyFiles.end() && found->second.first==a.digest){view->ready.emplace(k,found->second.second);continue;}
                    if(view->states[k]=="cached" || (current && a.digest==current->digest)){
                        if(files->has(a)){auto path=files->readyPath(a);readyFiles[k]={a.digest,path};view->ready.emplace(k,std::move(path));}
                    }
                }
                if(!pendingCollect.empty()){
                    auto a=view->frame.assets.find(pendingCollect);
                    if(a==view->frame.assets.end() || !a->second.selectable)pendingCollect.clear();
                    else if(view->ready.contains(pendingCollect)){
                        pendingCollect.clear();
                        message=gameText("已收藏 ", "Saved ")+filename(files->collect(a->second,root/"models"));
                    }
                }
                view->autoReceive=activeRules.autoReceiveCurrent;view->message=message;
                lock.lock();
                if(generation==gen){
                    view->acceptedTxBytes=acceptedTx;view->receivedBytes=received;latest.store(std::move(view));
                    std::erase_if(permits,[&](auto const& entry){
                        auto const& [endpoint,k,digest]=entry.first;
                        if(room->maySendChunk(endpoint,k,digest)) return false;
                        entry.second->store(false,std::memory_order_release);
                        return true;
                    });
                    for(auto& item:batch){
                        std::shared_ptr<std::atomic_bool> permit;
                        if(!item.control){
                            if(!room->maySendChunk(item.peer,item.assetKey,item.assetDigest))continue;
                            auto& token=permits[{item.peer,item.assetKey,item.assetDigest}];
                            if(!token)token=std::make_shared<std::atomic_bool>(true);
                            permit=token;
                        }
                        outgoingBytes+=item.bytes.size();output.push_back({std::move(item),std::move(permit)});
                    }
                }
            }catch(std::exception const& e){
                message=e.what();
                if(!lock.owns_lock())lock.lock();
                if(generation==gen){auto v=latest.load();auto error=std::make_shared<RoomView>();if(v)*error=*v;error->message=message;latest.store(error);}
            }
        }
        room.reset();
    }
};
NativeRoomSync::NativeRoomSync():impl(std::make_unique<Impl>()){}
NativeRoomSync::~NativeRoomSync(){stop();}
void NativeRoomSync::start(fs::path const& root){
    stop();
     
     
     
    impl->root=fs::canonical(root);
    fs::create_directories(impl->root/"room-share");
    if(!fs::exists(impl->root/"room-sync.json"))settings(impl->root);
    if(!impl->wire.start())return;
    impl->worker=std::jthread([this](std::stop_token stop){impl->run(stop);});
}
void NativeRoomSync::stop(){
    if(impl->worker.joinable()){
        {std::lock_guard lock(impl->mutex);impl->worker.request_stop();}
        impl->wake.notify_all();impl->worker.join();
    }
    impl->wire.stop();impl->change(RoomRole::None,{});
}
void NativeRoomSync::reset(){impl->wire.reset();impl->change(RoomRole::None,{});}
void NativeRoomSync::recordServerArrow(std::string shooter,bool hostPlayer,std::int64_t entity,bool crossbow){
    std::lock_guard lock(impl->mutex);
    if(impl->role!=RoomRole::Host || impl->endpoints.empty() || !entity)return;
    if(hostPlayer)shooter=impl->self;
    if(shooter.empty())return;
    if(std::ranges::any_of(impl->shots,[&](auto const& shot){return shot.entity==entity;}))return;
    if(impl->shots.size()>=128)impl->shots.pop_front();
    impl->shots.push_back({std::move(shooter),entity,crossbow});impl->wake.notify_one();
}
void NativeRoomSync::serverTick(Level& level){impl->wire.serverTick(level);}
void NativeRoomSync::tick(IClientInstance& client,Player* player){
    if(!impl->worker.joinable())return;
    impl->wire.clientTick(client,player);
    auto role=impl->wire.role();auto id=impl->wire.localId();
    if(role!=impl->role || id!=impl->self)impl->change(role,id);
    auto events=impl->wire.drain();
    std::unique_lock lock(impl->mutex);
    for(auto& e:events){
        if(impl->input.size()>=512 || impl->incomingBytes+e.payload.size()>maxBacklog){
            lock.unlock();reset();return;
        }
        if(e.kind==RoomWireKind::Joined)impl->endpoints.insert(e.peer);
        if(e.kind==RoomWireKind::Left)impl->endpoints.erase(e.peer);
        impl->received+=e.payload.size();impl->incomingBytes+=e.payload.size();impl->input.push_back(std::move(e));
    }
    if(!events.empty())impl->wake.notify_one();
    unsigned sent=0;
    std::set<std::string> blocked;
    for(bool control:{true,false}) {
        for(auto it=impl->output.begin();it!=impl->output.end() && sent<32;){
            auto& item=it->wire;
            if(item.control!=control){++it;continue;}
            auto peer=role==RoomRole::Guest?std::string(roomHostPeer):item.peer;
            if(!impl->endpoints.contains(peer) || (it->permit && !it->permit->load())){
                impl->outgoingBytes-=item.bytes.size();it=impl->output.erase(it);continue;
            }
            if(blocked.contains(peer)){++it;continue;}
            std::function<bool()> guard;
            if(it->permit)guard=[flag=it->permit]{return flag->load(std::memory_order_acquire);};
            if(!impl->wire.send(peer,item.bytes,control,std::move(guard),item.modelBytes)){
                blocked.insert(peer);++it;continue;
            }
            ++sent;impl->acceptedTx+=item.bytes.size();impl->outgoingBytes-=item.bytes.size();it=impl->output.erase(it);
        }
    }
}
bool NativeRoomSync::active()const{return impl->role!=RoomRole::None;}
std::optional<int> NativeRoomSync::latencyMs()const{return impl->wire.latencyMs();}
bool NativeRoomSync::host()const{return impl->role==RoomRole::Host;}
bool NativeRoomSync::hasEndpoints()const{std::lock_guard lock(impl->mutex);return !impl->endpoints.empty();}
bool NativeRoomSync::backpressured()const{std::lock_guard lock(impl->mutex);return impl->local.has_value();}
std::string NativeRoomSync::localId()const{return impl->self;}
std::shared_ptr<RoomView const> NativeRoomSync::view()const{return impl->latest.load();}
void NativeRoomSync::update(std::string source,std::string title,RelayPose pose,std::set<std::string> visible,RoomDisplays displays){
    std::lock_guard lock(impl->mutex);
    if(impl->endpoints.empty())return;
    impl->local=LocalState{std::move(source),std::move(title),std::move(pose),std::move(visible),std::move(displays)};impl->wake.notify_one();
}
void NativeRoomSync::command(std::string cmd){
    if(cmd.size()>2048)return;
    std::lock_guard lock(impl->mutex);if(impl->commands.size()<32)impl->commands.push_back(std::move(cmd));impl->wake.notify_one();
}
}
