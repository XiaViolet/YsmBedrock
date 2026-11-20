#include "NativeRoomSync.h"
#include "ContentHash.h"
#include <array>
#include <chrono>
#include <deque>
#include <fstream>
#include <iostream>
#include <thread>

// Only the Bedrock transport and game-owned handles are substituted. All
// participants use production workers, protocol, publication, verification,
// permissions, disk caches, real importer/archives and presentation snapshots.
class IClientInstance {};
class Level {};
class Player {};
namespace {
using namespace ysm;
using namespace std::chrono_literals;
namespace fs=std::filesystem;
std::array<std::string,3> ids={"Host","Alice","Bea"};
std::map<std::string,std::deque<RoomWireEvent>> queues;
std::map<std::tuple<std::string,std::string,std::string>,unsigned> chunks;
std::size_t nextEndpoint{};
bool drop{};
void check(bool ok,std::string const& message){if(!ok)throw std::runtime_error(message);}
void join(std::string const& guest){
    queues["Host"].push_back({RoomWireKind::Joined,guest,{}});
    queues[guest].push_back({RoomWireKind::Joined,std::string(roomHostPeer),{}});
}
}
namespace ysm {
struct RoomTransport::Impl {std::string id;bool live{};};
RoomTransport::RoomTransport():impl(std::make_unique<Impl>()){impl->id=ids.at(nextEndpoint++);}
RoomTransport::~RoomTransport()=default;
bool RoomTransport::start(){impl->live=true;return true;}
void RoomTransport::stop(){impl->live=false;queues[impl->id].clear();}
void RoomTransport::reset(){impl->live=false;queues[impl->id].clear();}
void RoomTransport::serverTick(Level&){}
void RoomTransport::clientTick(IClientInstance&,Player*){}
RoomRole RoomTransport::role() const{return !impl->live?RoomRole::None:impl->id=="Host"?RoomRole::Host:RoomRole::Guest;}
std::string RoomTransport::localId() const{return impl->live?impl->id:"";}
std::optional<int> RoomTransport::latencyMs() const{return std::nullopt;}
std::vector<RoomWireEvent> RoomTransport::drain(){
    auto& q=queues[impl->id];std::vector<RoomWireEvent> result;
    while(!q.empty()){result.push_back(std::move(q.front()));q.pop_front();}
    return result;
}
bool RoomTransport::send(std::string_view peer,std::span<std::uint8_t const> payload,bool control,std::function<bool()> allowed){
    if(allowed && !allowed())return true;
    if(drop)return false;
    bool fromHost=impl->id=="Host";
    std::string target=fromHost?std::string(peer):"Host";
    check(fromHost || peer==roomHostPeer,"guest sent to third party");
    if(!control){
        auto n=payload[0]+256u*payload[1];auto j=Json::parse(payload.begin()+2,payload.begin()+2+n);
        ++chunks[{impl->id,target,j.at("sha").get<std::string>()}];
    }
    queues[target].push_back({RoomWireKind::Data,fromHost?std::string(roomHostPeer):impl->id,{payload.begin(),payload.end()}});
    return true;
}
}
namespace {
struct Participant{
    NativeRoomSync sync;
    fs::path root;
    std::string key,texture,digest,sourceFile;
    void start(fs::path path){
        root=std::move(path);
        sourceFile=fs::exists(root/"models"/"current.ysm")?"current.ysm":"current.zip";
        auto file=root/"models"/sourceFile;
        auto original=ModelPack::load(file);key=original.localContentKey;texture=original.currentTexture().name;
        digest=contentFileSha256(file,128ull<<20);sync.start(root);
    }
    void publish(std::string const& who){
        RelayPose p;p.contentKey=key;p.texture=texture;p.action="pose_"+who;p.actionTime=.75;p.actionSerial=1;
        p.roaming["outfit"]=static_cast<float>(who.size());sync.update(sourceFile,"Same model name",std::move(p),{"Host","Alice","Bea"});
    }
};
void run(fs::path const& root){
    IClientInstance client;Player player;
    std::array<Participant,3> nodes;
    for(std::size_t i=0;i<3;++i)nodes[i].start(root/ids[i]);
    auto until=[&](auto ok,std::string const& condition){
        auto deadline=std::chrono::steady_clock::now()+45s;
        int i=0;
        while(std::chrono::steady_clock::now()<deadline){
            for(int n=0;n<3;++n){nodes[n].sync.tick(client,&player);if(i%10==0)nodes[n].publish(ids[n]);}
            if(ok())return;
            ++i;std::this_thread::sleep_for(5ms);
        }
        for(int n=0;n<3;++n)if(auto v=nodes[n].sync.view()){
            std::cerr<<ids[n]<<" connected="<<v->connected<<" players="<<v->frame.players.size()<<" ready="<<v->ready.size()
                <<" states="<<Json(v->states).dump()<<" refusals="<<Json(v->peerStates).dump()<<" message="<<v->message<<'\n';
        }
        throw std::runtime_error("Timeout "+condition);
    };
    until([&]{return nodes[0].sync.view()!=nullptr;},"initial solo host snapshot");
    auto solo=nodes[0].sync.view();
    for(int i=0;i<20;++i){
        nodes[0].sync.tick(client,&player);nodes[0].publish(ids[0]);std::this_thread::sleep_for(5ms);
    }
    check(nodes[0].sync.view()==solo,"idle solo host kept rebuilding snapshots");
    check(solo->frame.assets.empty() && solo->ready.empty(),"solo model was unnecessarily prepared");
    nodes[0].sync.command("toggle-auto");
    until([&]{auto host=nodes[0].sync.view();return host && !host->autoReceive;},"solo host applies room controls");
    nodes[0].sync.command("toggle-auto");
    until([&]{auto host=nodes[0].sync.view();return host && host->autoReceive;},"solo host restores policy");
    std::cout<<"PASS solo host retains the same snapshot across 20 local pose submissions and responds to policy changes.\n";
    join("Alice");join("Bea");
    auto allReady=[&]{
        for(int n=0;n<3;++n){
            auto v=nodes[n].sync.view();
            if(!v || !v->connected || v->frame.players.size()!=2)return false;
            for(int k=0;k<3;++k)if(k!=n){
                auto p=v->frame.players.find(ids[k]);if(p==v->frame.players.end())return false;
                if(p->second.contentKey!=nodes[k].key || !v->ready.contains(nodes[k].key))return false;
            }
        }
        return true;
    };
    until(allReady,"three peer snapshots and ready paths");
    for(int i=0;i<3;++i){
        auto v=nodes[i].sync.view();
        for(int k=0;k<3;++k)if(k!=i){
            auto const& a=nodes[k];auto ready=v->ready.at(a.key);
            check(contentFileSha256(ready,128ull<<20)==a.digest,"wrong file bytes on disk");
            auto imported=ModelPack::loadRoom(ready);
            check(imported.localContentKey==a.key && imported.currentTexture().name==a.texture,"wrong imported model");
            auto const& announced=v->frame.assets.at(a.key);
            check(imported.descriptor.at("spec").get<unsigned>()==announced.sourceSpec &&
                imported.binaryOriginVersion()==announced.originVersion,"negotiated source format does not match original bytes");
            auto const& p=v->frame.players.at(ids[k]);check(p.action=="pose_"+ids[k] && p.roaming.at("outfit")==ids[k].size(),"mixed wearer state");
        }
    }
    auto& a=nodes[1];
    unsigned expected=static_cast<unsigned>((fs::file_size(a.root/"models"/a.sourceFile)+RoomProtocol::chunkSize-1)/RoomProtocol::chunkSize);
    check(chunks[{"Alice","Host",a.digest}]==expected && chunks[{"Host","Bea",a.digest}]==expected,"incorrect host redistribution");
    std::cout<<"PASS unchanged A archive "<<a.digest<<" in "<<expected<<" chunks A->host and "<<expected<<" chunks host->B. "
        <<"Six remote-ready paths passed disk SHA, ModelPack::loadRoom, texture and presentation identity checks.\n";
    auto cfg=nodes[0].root/"room-sync.json";
    nodes[0].sync.command("deny:"+a.key);
    until([&]{
        auto h=nodes[0].sync.view(),g=nodes[2].sync.view(),owner=nodes[1].sync.view();
        return h && g && owner && h->peerStates.contains("Alice") && g->peerStates.contains("Alice")
            && !h->frame.players.contains("Alice") && !g->frame.players.contains("Alice")
            && !g->ready.contains(a.key) && !owner->frame.approved
            && h->deniedModels.contains(a.key) && !h->deniedModels.at(a.key).empty()
            && g->deniedModels.empty();
    },"policy revocation despite previously committed model");
    auto before=chunks;
    nodes[2].sync.command("allow:"+a.key);
    nodes[0].sync.command("reload");
    check(Json::parse(std::ifstream(cfg)).at("denied_content_keys")==Json::array({a.key}),"host denial not saved");
    queues["Host"].push_back({RoomWireKind::Left,"Alice",{}});
    queues["Alice"].push_back({RoomWireKind::Left,std::string(roomHostPeer),{}});
    until([&]{auto h=nodes[0].sync.view();return h && !h->peerStates.contains("Alice")
        && h->deniedModels.contains(a.key) && nodes[1].sync.view() && !nodes[1].sync.view()->connected;},
        "host retains undo after wearer leaves");
    nodes[0].sync.command("allow:"+a.key);
    until([&]{return nodes[0].sync.view()->deniedModels.empty();},"host restores an offline model");
    check(Json::parse(std::ifstream(cfg)).at("denied_content_keys").empty(),"allow not persisted");
    join("Alice");
    until(allReady,"restore allowed view from valid cache");
    check(chunks==before,"duplicate transfer after reauthorization");
    nodes[0].sync.command("deny:"+a.key);
    until([&]{return !nodes[0].sync.view()->frame.players.contains("Alice") && !nodes[2].sync.view()->frame.players.contains("Alice");},"second live refusal");
    nodes[0].sync.command("allow:"+a.key);
    until(allReady,"live undo with all wearers still in the room");
    check(chunks==before,"live undo downloaded again");
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto prior = nodes[2].sync.view()->frame.players.at("Alice").session;
        queues["Host"].push_back({RoomWireKind::Left, "Alice", {}});
        queues["Alice"].push_back({RoomWireKind::Left, std::string(roomHostPeer), {}});
        until([&] {
            auto h = nodes[0].sync.view(), b = nodes[2].sync.view(), guest = nodes[1].sync.view();
            return h && b && guest && !guest->connected && !h->frame.players.contains("Alice")
                && !b->frame.players.contains("Alice") && b->frame.players.contains("Host");
        }, "one guest leaves while other guest stays");
        join("Alice");
        until([&] { return allReady() && nodes[2].sync.view()->frame.players.at("Alice").session != prior; },
              "remaining guest reacquires the rejoining wearer and their current state");
        check(chunks == before, "rejoining guest caused redundant transfers");
    }
    std::cout << "PASS same guest rejoined the live room twice; remaining guest rebound the new wearer session; no repeat file chunks.\n";
    for(auto const& id:{"Alice","Bea"}){
        queues["Host"].push_back({RoomWireKind::Left,id,{}});
        queues[id].push_back({RoomWireKind::Left,std::string(roomHostPeer),{}});
    }
    until([&]{auto h=nodes[0].sync.view();return h && !nodes[0].sync.hasEndpoints()
        && h->frame.players.empty() && h->frame.assets.empty();},"last departure clears host bindings");
    join("Alice");join("Bea");
    until(allReady,"reconnect after solo idle");
    check(chunks==before,"duplicate transfer after reconnect from solo");
    std::cout<<"PASS last departure clears host model bindings; both guests rejoin with cached files.\n";
    for(auto& n:nodes){n.sync.reset();check(!n.sync.view(),"stale view survived reset");n.sync.stop();}
    std::cout<<"PASS host revocation removes cached user's peer binding, own approval and ready path; restoring scope uses verified disk cache with zero more chunks. World reset clears views.\n";
}
}
int wmain(int argc,wchar_t** argv){
    try{check(argc==2,"expected isolated test data directory");run(argv[1]);return 0;}
    catch(std::exception const& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
