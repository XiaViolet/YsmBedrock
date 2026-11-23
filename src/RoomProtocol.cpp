#include "RoomProtocol.h"
#include "RoomByteRate.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <random>
#include <tuple>
#include <stdexcept>

namespace ysm {
namespace {
using Bytes = std::vector<unsigned char>;
using Tick = std::uint64_t;
constexpr std::size_t maxAsset = 128u << 20;
constexpr std::size_t maxMembers = 16, maxCatalog = 64, maxTransfers = 4;
constexpr Tick maxSerial = (Tick(1) << 53) - 1;
constexpr Tick retryMs = 5000;
constexpr std::size_t maxWindowChunks=16;

void require(bool ok) { if (!ok) throw std::runtime_error("Invalid room message"); }
bool hex(std::string_view s, std::size_t n) {
    return s.size() == n && std::ranges::all_of(s, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool key(std::string const& k) { return k.starts_with("local1_") && hex(std::string_view(k).substr(7), 64); }
bool textOk(std::string_view s, std::size_t n) {
    return s.size() <= n && std::ranges::none_of(s, [](unsigned char c) { return c < 32 || c == 127; });
}
std::string field(Json const& j, char const* k, std::size_t n) {
    auto s = j.at(k).get<std::string>(); require(textOk(s,n)); return s;
}
Tick number(Json const& j, Tick maximum = maxSerial) {
    require(j.is_number_unsigned()); auto v = j.get<Tick>(); require(v <= maximum); return v;
}
bool same(RelayAsset const& a, RelayAsset const& b) {
    return a.contentKey == b.contentKey && a.digest == b.digest && a.size == b.size && a.extension == b.extension
        && a.sourceSpec==b.sourceSpec && a.originVersion==b.originVersion;
}
bool valid(RelayAsset const& a) {
    return key(a.contentKey) && hex(a.digest,64) && (a.extension==".zip" || a.extension==".ysm")
        && a.size && a.size<=maxAsset && textOk(a.title,256) && a.sourceSpec==2
        && (a.extension==".ysm" || !a.originVersion);
}
Json meta(RelayAsset const& a) {
    return {{"key",a.contentKey},{"sha",a.digest},{"ext",a.extension},{"size",a.size},{"title",a.title},
        {"select",a.selectable},{"format",1},{"source_spec",a.sourceSpec},
        {"origin_version",a.originVersion?Json(*a.originVersion):Json(nullptr)}};
}
RelayAsset asset(Json const& j) {
    RelayAsset a{field(j,"key",71),field(j,"sha",64),field(j,"ext",4),
        static_cast<std::size_t>(number(j.at("size"),maxAsset)),field(j,"title",256),j.at("select").get<bool>(),
        static_cast<unsigned>(number(j.at("source_spec"),std::numeric_limits<unsigned>::max())),{}};
    if(!j.at("origin_version").is_null())
        a.originVersion=static_cast<unsigned>(number(j.at("origin_version"),std::numeric_limits<std::uint32_t>::max()));
    require(valid(a) && number(j.at("format"))==1); return a;
}
using Catalog = std::map<std::string,RelayAsset>;
Json list(Catalog const& assets) {
    Json a=Json::array(); for (auto const& [_,entry]:assets) a.push_back(meta(entry)); return a;
}
Catalog catalog(Json const& j) {
    require(j.is_array() && j.size()<=maxCatalog);
    Catalog out;
    for(auto const& item:j){auto a=asset(item); auto k=a.contentKey; require(out.emplace(k,std::move(a)).second);}
    return out;
}
std::string stream(std::string token,Tick revision) {
    // Encode the model selection revision independently of the high-frequency pose serial.
    constexpr char digits[]="0123456789abcdef";
    for(std::size_t i=0;i<16;++i){token[31-i]=digits[revision&15];revision>>=4;}
    return token;
}
std::string newConnectionNonce(){
    std::random_device entropy;
    std::uniform_int_distribution<unsigned> byte(0,255);
    constexpr char digits[]="0123456789abcdef";
    std::string value(32,'0');
    for(std::size_t i=0;i<value.size();i+=2){
        auto n=byte(entropy);value[i]=digits[n>>4];value[i+1]=digits[n&15];
    }
    return value;
}
Json presentation(RelayPose const& p, Tick floor=0) {
    Json events=Json::array();
    for(auto const& e:p.events) if(e.sequence>floor) events.push_back({e.sequence,e.arguments});
    Json state={{"serial",p.serial},{"texture",p.texture},{"action",p.action},{"play",p.actionSerial},
        {"seconds",p.actionTime},{"vars",p.roaming},{"events",events},{"seats",p.shoulderBirds}};
    writePoseExtras(state,p);
    return state;
}
// State frames coalesce under backpressure. Keep unacknowledged edge events
// in bounded sequence order independently of how many pose samples are merged.
void retainEvents(RelayPose& next,RelayPose const& prior){
    if(prior.events.empty())return;
    auto events=prior.events;
    for(auto const& event:next.events)if(event.sequence>events.back().sequence)events.push_back(event);
    if(events.size()>64)events.erase(events.begin(),events.end()-64);
    next.events=std::move(events);
    // A frame with many long roaming fields must still fit after merging history.
    if(next.roaming.size()>400 && next.events.size()>8)
        while(!next.events.empty() && presentation(next).dump().size()>54000)
            next.events.erase(next.events.begin(),next.events.begin()+(next.events.size()+1)/2);
}
RelayPose pose(Json const& j) {
    RelayPose p;
    readPoseExtras(j,p);
    p.serial=number(j.at("serial")); p.texture=field(j,"texture",512); p.action=field(j,"action",512);
    p.actionSerial=number(j.at("play"));p.actionTime=j.at("seconds").get<double>();
    require(std::isfinite(p.actionTime) && p.actionTime>=0 && p.actionTime<=86400);
    auto const& vars=j.at("vars");require(vars.is_object() && vars.size()<=1024);
    for(auto const& [n,v]:vars.items()){require(textOk(n,256) && v.is_number());p.roaming.emplace(n,v.get<float>());}
    MolangRoamingState::validateNamedPayload(p.roaming,RoamingLimit::LocalOnly);
    auto const& seats=j.at("seats");require(seats.is_array() && seats.size()==2);
    for(int i=0;i<2;++i){require(seats[i].is_number_integer());p.shoulderBirds[i]=seats[i].get<std::int64_t>();}
    auto const& events=j.at("events");require(events.is_array() && events.size()<=64);
    Tick previous=0;
    for(auto const& row:events) {
        require(row.is_array() && row.size()==2 && row[1].is_array() && row[1].size()<=16);
        RelayEvent e{number(row[0]),{}};require(e.sequence>previous);previous=e.sequence;
        for(auto const& v:row[1]){require(v.is_number());auto f=v.get<float>();require(std::isfinite(f));e.arguments.push_back(f);}
        p.events.push_back(std::move(e));
    }
    return p;
}
Bytes wire(Json const& header,std::span<unsigned char const> tail={}) {
    auto h=header.dump(); require(h.size()<=65533 && h.size()+2+tail.size()<=RoomProtocol::maxWire);
    Bytes out;out.reserve(2+h.size()+tail.size());out.push_back(static_cast<unsigned char>(h.size()));
    out.push_back(static_cast<unsigned char>(h.size()>>8));out.insert(out.end(),h.begin(),h.end());
    out.insert(out.end(),tail.begin(),tail.end());return out;
}
Json header(std::span<unsigned char const>& bytes) {
    require(bytes.size()>=3 && bytes.size()<=RoomProtocol::maxWire);
    std::size_t n=bytes[0]+(std::size_t(bytes[1])<<8);require(n && n<=bytes.size()-2);
    auto h=bytes.subspan(2,n);bytes=bytes.subspan(2+n);
    // Bound nesting before the JSON parser allocates container frames.
    int depth=0; bool quote=false,escape=false;
    for(char c:h){
        if(quote){if(escape) escape=false;else if(c=='\\') escape=true;else if(c=='"') quote=false;}
        else if(c=='"') quote=true;
        else if(c=='{' || c=='[') require(++depth<=10);
        else if(c=='}' || c==']') require(--depth>=0);
    }
    require(depth==0 && !quote);
    auto j=Json::parse(h.begin(),h.end(),nullptr,false);
    require(j.is_object() && j.size()<=16);return j;
}
bool compatible(Json const& j,bool zip,bool ysm) {
    if(number(j.at("version"))!=1) return false;
    auto specs=j.find("source_specs");
    if(specs==j.end() || !specs->is_array() || specs->size()>8 || std::find(specs->begin(),specs->end(),Json(2))==specs->end()
        || j.value("origin_encoding","")!="optional_u32")return false;
    auto const& formats=j.at("formats");
    return formats.is_array() && formats.size()<=8
        && ((!zip || std::ranges::find(formats,".zip")!=formats.end())
            && (!ysm || std::ranges::find(formats,".ysm")!=formats.end()));
}
Json handshake(std::string_view type) {
    return {{"t",type},{"version",1},{"formats",Json::array({".zip",".ysm"})},
        {"source_specs",Json::array({2})},{"origin_encoding","optional_u32"},{"display_reports",true},{"chunk_windows",true},{"peer_rates",true},{"transfer_retry",true}};
}
bool displayStage(std::string const& code) {
    static std::set<std::string> const allowed{"waiting","awaiting_host","downloading","paused","verifying","cached",
        "preparing","awaiting_actor","awaiting_draw","displayed","failed","failed_download","failed_prepare","failed_draw","vanilla"};
    return allowed.contains(code);
}
} // namespace

struct RoomProtocol::Impl {
    struct Member {
        std::string nonce;
        Tick revision{}, serial{};
        std::optional<RelayAsset> current;
        RelayPose state;
        Catalog offers;
        std::map<std::string,Tick> eventFloors;
        bool supportsReports{}, chunkWindows{};
        bool supportsRates{}, supportsRetry{};
        std::map<std::string,Tick> retryReceipts;
        RoomByteRate uploaded;
        RoomDisplays observed;
        std::map<std::string,Tick> reportSeq;
    };
    struct Request {std::string peer; RelayAsset asset; Tick id{}; std::size_t offset{}; int rank{}; Tick order{}; std::size_t end{};};
    struct Incoming {
        RelayAsset asset;
        std::string provider;
        Tick id{},sent{};
        std::size_t offset{};
        int attempts{};
        bool pending{};
        Tick waitingSince{};
        std::size_t upstreamOffset{};
        bool verifying{};
        bool paused{}, admission=true;
        std::size_t requestedEnd{};
        RoomByteRate downloadRate;
        Tick upstreamRate{}, upstreamSampleTime{};
    };
    bool host{}, link{}, online{};
    std::string self,token,room,selected;
    RoomRules rules;
    RoomFiles files;
    RelayFrame view;
    Member local;
    std::map<std::string,Member> members;
    Catalog shared, accepted;
    std::map<std::string,std::string> acceptedFrom;
    std::map<std::string,std::string> status;
    std::map<std::string,std::string> peerStatus;
    std::set<std::string> visible,failed;
    std::map<std::string,Incoming> downloads;
    std::map<std::pair<std::string,std::string>,Request> sends;
    std::map<std::string,std::map<std::string,Bytes>> controls;
    std::map<std::string,Tick> recheck;
    std::map<std::string,std::pair<std::string,Tick>> remoteVersion;
    Tick seq{},order{},now{},previous{},lastHello{};
    Tick catalogVersion{}, broadcastVersion{};
    std::map<std::string,Tick> stateVersion;
    RoomDisplays lastReported;
    std::map<std::string,Tick> observedShotSequence;
    bool hostReports{}, hostChunkWindows{}, hostRetry{};
    RoomByteRate downloadRate;
    RoomPeerRates rates;
    Tick ratesAt{}, ratesSeenAt{};
    struct RetryRequest {std::string stream; Tick ticket{}, nextSend{}; unsigned attempts{};};
    std::map<std::string,RetryRequest> pendingRetries;
    std::map<std::string,Tick> retryAfter, retryEpoch;
    double balance=RoomProtocol::chunkSize, pullBalance=RoomProtocol::chunkSize;
    std::array<std::string,4> lastPull;
    std::string lastReceiver;
    Impl(bool h,std::string id,std::string nonce,RoomFiles fs):host(h),online(h),self(std::move(id)),
        token(std::move(nonce)),room(h?token:""),files(std::move(fs)) {
        require(!self.empty() && textOk(self,80) && hex(token,32));local.nonce=token;
    }
    ~Impl(){for(auto const& [_,x]:downloads)cancel(x.asset);}
    void cancel(RelayAsset const& a){try{files.cancel(a);}catch(std::exception const&){}}
    bool send(std::string const& peer,std::string slot,Json msg) {
        if(!host && !link)return false;
        if(host){auto it=members.find(peer);if(it==members.end())return false;msg["to"]=it->second.nonce;}
        else msg["from"]=token;
        if(host && (msg["t"]=="state" || msg["t"]=="unavailable" || msg["t"]=="gone" || msg["t"]=="catalog"))msg["v"]=++broadcastVersion;
        msg["room"]=room;
        auto& queue=controls[peer];
        if(queue.size()>=72 && !queue.contains(slot))return false;
        queue[std::move(slot)]=wire(msg);return true;
    }
    bool allowed(RelayAsset const& a) const {
        if(rules.deniedKeys.contains(a.contentKey) || rules.sessionDenied.contains(a.contentKey))return false;
        auto explicitMatch=[&](Catalog const& values){
            auto it=values.find(a.contentKey);return it!=values.end() && same(it->second,a);
        };
        return explicitMatch(shared) || explicitMatch(accepted) ||
            (!rules.sharedOnly && ((local.current && same(*local.current,a)) || rules.autoReceiveCurrent));
    }
    Member const* owner(std::string const& id) const {
        if(id==self)return &local;
        auto it=members.find(id);return it==members.end()?nullptr:&it->second;
    }
    bool currentAllowed(Member const& m) const {
        if(!m.current)return false;
        auto it=view.assets.find(m.current->contentKey);
        return it!=view.assets.end() && same(it->second,*m.current) && allowed(*m.current);
    }
    std::string refusal(Member const& m) const {
        auto it=view.assets.find(m.current->contentKey);
        if(allowed(*m.current) && it!=view.assets.end() && !same(it->second,*m.current))return "conflicting_archive";
        return "denied";
    }
    void putState(std::string const& dest,std::string const& id,Member const& m,bool initial) {
        if(m.nonce.empty() || !m.revision)return;
        bool ok=currentAllowed(m);
        auto& floor=members.at(dest).eventFloors[id];
        if(m.current && !ok){
            if(!m.state.events.empty())floor=std::max(floor,m.state.events.back().sequence);
            Json msg={{"t","unavailable"},{"subject",id},{"reason",refusal(m)}};
            if(id==dest){msg["rev"]=m.revision;msg["key"]=m.current->contentKey;}
            send(dest,"p/"+id,std::move(msg));
            return;
        }
        if(initial && !m.state.events.empty())floor=std::max(floor,m.state.events.back().sequence);
        Json msg={{"t","state"},{"subject",id},{"stream",m.nonce},{"rev",m.revision},
            {"key",m.current?m.current->contentKey:""},{"ok",ok},{"state",presentation(m.state,floor)}};
        if(ok && members.at(dest).supportsRetry){
            auto epoch=retryEpoch.find(m.current->contentKey);
            if(epoch!=retryEpoch.end())msg["retry_epoch"]=epoch->second;
            auto const& receipts=members.at(dest).retryReceipts;
            if(auto receipt=receipts.find(id);receipt!=receipts.end())msg["retry_ack"]=receipt->second;
        }
        send(dest,"p/"+id,std::move(msg));
    }
    void putArrows(std::string const& dest,std::string const& id,Member const& wearer){
        if(id==dest)return;
        if(!currentAllowed(wearer) || wearer.state.arrows.empty()){
            controls[dest].erase("shots/"+id);
            return;
        }
        send(dest,"shots/"+id,{{"t","arrows"},{"subject",id},{"stream",wearer.state.session},{"shots",wearer.state.arrows},{"seq",++seq}});
    }
    void syncTo(std::string const& id,bool initial) {
        auto& dest=members.at(id);if(dest.nonce.empty())return;
        send(id,"catalog",{{"t","catalog"},{"assets",list(view.assets)},{"auto",rules.autoReceiveCurrent},{"shared_only",rules.sharedOnly}});
        putState(id,self,local,initial);
        putArrows(id,self,local);
        for(auto const& [who,m]:members){putState(id,who,m,initial);putArrows(id,who,m);}
    }
    void rebuild(bool catalogChanged=true,std::string const& subject={}) {
        if(!host)return;
        view.assets.clear();view.players.clear();peerStatus.clear();
        auto add=[&](RelayAsset a,bool selectable){
            if(!allowed(a) || (view.assets.size()>=maxCatalog && !view.assets.contains(a.contentKey)))return;
            a.selectable=selectable;
            auto [it,inserted]=view.assets.emplace(a.contentKey,a);
            if(!inserted && same(it->second,a) && selectable)it->second.selectable=true;
        };
        // Reserve capacity first for the models presently worn by occupants.
        if(local.current)add(*local.current,false);
        for(auto const& [_,m]:members)if(m.current)add(*m.current,false);
        for(auto const& [_,a]:shared)add(a,true);
        for(auto const& [_,a]:accepted)add(a,true);
        for(auto const& [id,m]:members)if(m.current){
            if(currentAllowed(m))view.players[id]=m.state;
            else {
                peerStatus[id]=refusal(m);
                if(!view.assets.contains(m.current->contentKey))status[m.current->contentKey]="denied";
            }
        }
        view.approved=!local.current || currentAllowed(local);
        if(!view.approved)peerStatus[self]=refusal(local);
        reconcile();
        for(auto const& [id,m]:members)if(!m.nonce.empty()){
            if(catalogChanged)syncTo(id,false);
            else if(auto peer=owner(subject))putState(id,subject,*peer,false);
        }
    }
    void submitCurrent(){
        if(host || !online || !local.revision)return;
        send("","publish",{{"t","publish"},{"rev",local.revision},{"asset",local.current?meta(*local.current):Json(nullptr)},
            {"state",presentation(local.state)}});
    }
    void hello() {
        auto j=handshake("hello");send("","hello",std::move(j));lastHello=now;
    }
    RoomPriority priority(std::string const& k) const {
        RoomPriority rank=RoomPriority::Background;
        for(auto const& [id,p]:view.players)if(p.contentKey==k){
            if(visible.contains(id))return RoomPriority::Visible;
            rank=RoomPriority::Current;
        }
        if(local.current && local.current->contentKey==k)return RoomPriority::Current;
        if(rank==RoomPriority::Background && selected==k)return RoomPriority::Selected;
        return rank;
    }
    std::optional<std::string> source(RelayAsset const& a) const {
        if(!host)return std::string{};
        for(auto const& [id,m]:members){
            if(m.current && same(a,*m.current) && currentAllowed(m))return id;
            auto it=m.offers.find(a.contentKey);auto accept=acceptedFrom.find(a.contentKey);
            if(it!=m.offers.end() && same(it->second,a) && accept!=acceptedFrom.end() && accept->second==id)return id;
        }
        return {};
    }
    bool mayServe(std::string const& dest,RelayAsset const& a) const {
        if(host){
            auto m=members.find(dest);auto it=view.assets.find(a.contentKey);
            return m!=members.end() && !m->second.nonce.empty() && it!=view.assets.end() && same(it->second,a) && allowed(a);
        }
        if(!online || !dest.empty())return false;
        if(local.current && same(*local.current,a))return true;
        auto it=shared.find(a.contentKey);return it!=shared.end() && same(it->second,a);
    }
    RelayAsset const* sendable(std::string const& dest,std::string const& k,std::string const& digest) const {
        RelayAsset const* a=nullptr;
        if(host){auto it=view.assets.find(k);if(it!=view.assets.end())a=&it->second;}
        else if(local.current && local.current->contentKey==k && local.current->digest==digest)a=&*local.current;
        else if(auto it=shared.find(k);it!=shared.end())a=&it->second;
        return a && a->digest==digest && mayServe(dest,*a) ? a : nullptr;
    }
    void reconcile(){
        for(auto it=downloads.begin();it!=downloads.end();){
            auto found=view.assets.find(it->first);
            if(found==view.assets.end() || !same(found->second,it->second.asset) || source(found->second)!=it->second.provider){
                cancel(it->second.asset);it=downloads.erase(it);
            }else ++it;
        }
        std::erase_if(sends,[&](auto const& pair){return !mayServe(pair.second.peer,pair.second.asset);});
        std::erase_if(recheck,[&](auto const& p){return !view.assets.contains(p.first);});
        std::erase_if(failed,[&](auto const& k){return !view.assets.contains(k);});
        std::erase_if(retryAfter,[&](auto const& p){return !view.assets.contains(p.first);});
        std::erase_if(retryEpoch,[&](auto const& p){return !view.assets.contains(p.first);});
        std::erase_if(observedShotSequence,[&](auto const& entry){return !view.players.contains(entry.first);});
        std::erase_if(lastReported,[&](auto const& p){return !view.players.contains(p.first);});
        if(host)for(auto& [_,member]:members){
            std::erase_if(member.retryReceipts,[&](auto const& p){auto m=owner(p.first);return !m || !currentAllowed(*m);});
            std::erase_if(member.observed,[&](auto const& p){
                auto source=owner(p.first);
                return !source || !currentAllowed(*source) || p.second.stream!=source->state.session;
            });
            std::erase_if(member.reportSeq,[&](auto const& p){return !member.observed.contains(p.first);});
        }
        std::erase_if(status,[&](auto const& p){return !view.assets.contains(p.first) && p.second!="denied";});
        if(status.size()>128)std::erase_if(status,[](auto const& p){return p.second=="denied";});
    }
    void fail(std::string const& k,std::string reason){
        auto it=downloads.find(k);if(it!=downloads.end()){cancel(it->second.asset);downloads.erase(it);}
        failed.insert(k);status[k]="failed: "+std::move(reason);
    }
    // Keep accepted bytes. A fresh id makes late chunks and errors from the old window harmless.
    void resumeTransfer(std::string const& k){
        failed.erase(k);recheck.erase(k);
        auto it=downloads.find(k);
        if(it==downloads.end() || it->second.verifying)return;
        auto& d=it->second;
        d.id=++seq;d.pending=false;d.sent=d.waitingSince=0;d.attempts=0;
    }
    void announceRetry(std::string const& id,Member const& wearer){
        auto const& k=wearer.current->contentKey;
        resumeTransfer(k);
        retryEpoch[k]=++seq;
        for(auto const& [dest,m]:members)if(!m.nonce.empty())putState(dest,id,wearer,false);
    }
    void requestRetry(std::string const& subject,std::string const& streamId){
        if(host){auto m=owner(subject);if(m && currentAllowed(*m))announceRetry(subject,*m);return;}
        if(!online)return;
        if(hostRetry){
            // Repeated clicks share the outstanding request for this selection.
            auto& r=pendingRetries[subject];
            if(r.stream!=streamId || !r.ticket || r.attempts>=3)r={streamId,++seq};
        }else if(hostReports && subject!=self){
            send("","0retry/"+subject,{{"t","retry_model"},{"subject",subject},{"stream",streamId}});
        }
    }
    void sendRetries(){
        std::erase_if(pendingRetries,[&](auto& entry){
            auto const& [subject,r]=entry;
            auto peer=view.players.find(subject);
            bool current=subject==self ? local.current && local.state.session==r.stream
                : peer!=view.players.end() && peer->second.session==r.stream;
            return !current || (r.attempts>=3 && now>=r.nextSend);
        });
        for(auto& [id,r]:pendingRetries)if(now>=r.nextSend){
            if(send("","0retry/"+id,{{"t","retry_model"},{"subject",id},{"stream",r.stream},{"ticket",r.ticket}})){
                ++r.attempts;r.nextSend=now+retryMs*r.attempts;
            }
        }
    }
    bool newerState(std::string const& id,Tick version){
        auto known=stateVersion.find(id);
        if(known!=stateVersion.end() && version<=known->second)return false;
        if(known==stateVersion.end() && stateVersion.size()>=maxMembers*4){
            auto oldest=std::min_element(stateVersion.begin(),stateVersion.end(),
                [](auto const& a,auto const& b){return a.second<b.second;});
            stateVersion.erase(oldest);
        }
        stateVersion[id]=version;return true;
    }
    void receiveState(Json const& j){
        std::string id=field(j,"subject",80),origin=field(j,"stream",32),k=field(j,"key",71);
        require(!id.empty() && hex(origin,32) && (k.empty() || key(k)));
        auto revision=number(j.at("rev"));auto p=pose(j.at("state"));auto serial=p.serial;
        require(revision && serial);
        if(id==self && (origin!=token || revision!=local.revision ||
            (local.current ? k!=local.current->contentKey : !k.empty())))return;
        bool ok=j.at("ok").get<bool>() && view.assets.contains(k);
        auto streamKey=stream(origin,revision);auto known=remoteVersion.find(id);
        if(known==remoteVersion.end())require(remoteVersion.size()<maxMembers+1);
        if(known!=remoteVersion.end() && known->second.first==streamKey && known->second.second>serial)return;
        if(!newerState(id,number(j.at("v"))))return;
        remoteVersion[id]={streamKey,serial};
        peerStatus.erase(id);
        if(ok && hostRetry){
            if(auto ack=j.find("retry_ack");ack!=j.end()){
                auto pending=pendingRetries.find(id);
                if(pending!=pendingRetries.end() && pending->second.stream==streamKey && pending->second.ticket==number(*ack))
                    pendingRetries.erase(pending);
            }
            if(auto e=j.find("retry_epoch");e!=j.end()){
                auto epoch=number(*e);
                if(epoch>retryEpoch[k]){
                    retryEpoch[k]=epoch;
                    if(id!=self)resumeTransfer(k);
                }
            }
        }
        if(!p.events.empty()){
            send("","receipt/"+id,{{"t","receipt"},{"subject",id},{"stream",streamKey},{"sequence",p.events.back().sequence}});
        }
        if(id==self){
            view.approved=k.empty() || ok;
            if(!view.approved)status[k]="denied";
            if(ok && local.current && local.revision==revision && origin==token && !p.events.empty())
                view.acknowledgedEvent=std::max(view.acknowledgedEvent,p.events.back().sequence);
            return;
        }
        if(ok){
            auto prior=view.players.find(id);
            if(prior!=view.players.end() && prior->second.session==streamKey){
                retainEvents(p,prior->second);p.arrows=std::move(prior->second.arrows);
            }
            p.xuid=id;p.contentKey=k;p.session=streamKey;view.players[id]=std::move(p);
        }else{
            view.players.erase(id);if(!k.empty())status[k]="denied";
        }
    }
    void handle(std::string const& peer,Json const& j,std::span<unsigned char const> tail){
        auto type=field(j,"t",20);if(type!="chunk")require(tail.empty());
        if(host){
            auto mit=members.find(peer);if(mit==members.end())return;
            auto& m=mit->second;
            auto from=field(j,"from",32);require(hex(from,32));
            if(type=="hello"){
                if(!compatible(j,true,true)){auto nack=handshake("incompatible");nack["to"]=from;nack["room"]=room;
                    controls[peer]["0welcome"]=wire(nack);return;}
                if(!m.nonce.empty() && m.nonce!=from)return;
                bool first=m.nonce.empty();m.nonce=from;
                m.supportsReports=j.value("display_reports",false);
                m.chunkWindows=j.value("chunk_windows",false);
                m.supportsRates=j.value("peer_rates",false);
                m.supportsRetry=j.value("transfer_retry",false);
                send(peer,"0welcome",handshake("welcome"));syncTo(peer,first);return;
            }
            if(m.nonce.empty() || m.nonce!=from || field(j,"room",32)!=room)return;
            if(type=="display" || type=="retry_model"){
                auto id=field(j,"subject",80);
                auto source=owner(id);
                if(!m.supportsReports || !source || !currentAllowed(*source)
                    || field(j,"stream",32)!=source->state.session)return;
                if(type=="retry_model"){
                    auto const& k=source->current->contentKey;
                    auto ticket=m.supportsRetry && j.contains("ticket")?number(j.at("ticket")):0;
                    if(id==peer && !ticket)return;
                    if(ticket && ticket<=m.retryReceipts[id]){putState(peer,id,*source,false);return;}
                    if(now<retryAfter[k])return;
                    if(ticket)m.retryReceipts[id]=ticket;
                    retryAfter[k]=now+retryMs;
                    announceRetry(id,*source);
                    return;
                }
                if(id==peer)return;
                auto stage=field(j,"stage",16);auto serial=number(j.at("seq"));
                if(displayStage(stage) && serial>m.reportSeq[id]){
                    m.observed[id]={source->state.session,stage};m.reportSeq[id]=serial;
                }
                return;
            }
            if(type=="receipt"){
                auto id=field(j,"subject",80);auto source=owner(id);
                if(!source || !currentAllowed(*source) || source->state.events.empty()
                    || field(j,"stream",32)!=source->state.session)return;
                auto seen=number(j.at("sequence"));
                if(seen<=source->state.events.back().sequence)m.eventFloors[id]=std::max(m.eventFloors[id],seen);
                return;
            }
            if(type=="offers"){
                auto offered=catalog(j.at("assets"));
                m.offers=std::move(offered);
                std::erase_if(acceptedFrom,[&](auto const& p){
                    if(p.second!=peer)return false;
                    auto a=m.offers.find(p.first);
                    if(a!=m.offers.end() && same(a->second,accepted.at(p.first)))return false;
                    accepted.erase(p.first);return true;
                });
                rebuild();return;
            }
            if(type=="publish"){
                auto rev=number(j.at("rev"));auto p=pose(j.at("state"));require(rev && p.serial);
                if(rev<m.revision || p.serial<=m.serial)return;
                std::optional<RelayAsset> next;
                if(!j.at("asset").is_null())next=asset(j.at("asset"));
                bool changed=next.has_value()!=m.current.has_value() || (next && !same(*next,*m.current));
                if(changed && rev==m.revision)return;
                require(presentation(p).dump().size()<48000);
                if(rev!=m.revision)for(auto& [_,member]:members)member.eventFloors.erase(peer);
                else {retainEvents(p,m.state);p.arrows=std::move(m.state.arrows);}
                p.xuid=peer;p.contentKey=next?next->contentKey:"";p.session=stream(from,rev);
                m.serial=p.serial;m.revision=rev;m.state=std::move(p);m.current=std::move(next);
                rebuild(changed,peer);return;
            }
        }else{
            if(!link || !peer.empty() || field(j,"to",32)!=token)return;
            auto s=field(j,"room",32);if(!hex(s,32))return;
            if(type=="incompatible"){if(!online)status["protocol"]="failed: incompatible room protocol";return;}
            if(type=="welcome"){
                if(!compatible(j,true,true) || (online && room!=s))return;
                if(!online){room=s;online=true;hostReports=j.value("display_reports",false);status.erase("protocol");
                    hostChunkWindows=j.value("chunk_windows",false);hostRetry=j.value("transfer_retry",false);
                    send("","offers",{{"t","offers"},{"assets",list(shared)}});submitCurrent();}
                return;
            }
            if(!online || s!=room)return;
            if(type=="arrows"){
                auto id=field(j,"subject",80);
                auto wearer=view.players.find(id);
                if(wearer==view.players.end() || field(j,"stream",32)!=wearer->second.session)return;
                auto const& shots=j.at("shots");
                require(shots.is_array() && shots.size()<=32);
                std::vector<std::pair<std::int64_t,bool>> list;
                for(auto const& s:shots){
                    require(s.is_array() && s.size()==2 && s[0].is_number_integer() && s[1].is_boolean());
                    list.emplace_back(s[0].get<std::int64_t>(),s[1].get<bool>());
                }
                auto stamp=number(j.at("seq"));
                if(stamp<=observedShotSequence[id])return;
                observedShotSequence[id]=stamp;wearer->second.arrows=std::move(list);return;
            }
            if(type=="rates"){
                auto stamp=number(j.at("seq"));if(stamp<=ratesAt)return;
                auto const& rows=j.at("rows");require(rows.is_array() && rows.size()<=maxMembers+1);
                RoomPeerRates next;
                for(auto const& row:rows){
                    auto id=field(row,"peer",80);require(!id.empty());
                    if(id!=self)next[id]={number(row.at("up"),maxAsset),number(row.at("down"),maxAsset)};
                }
                rates=std::move(next);ratesAt=stamp;ratesSeenAt=now;return;
            }
            if(type=="catalog"){
                auto v=number(j.at("v"));if(v<=catalogVersion)return;
                auto metas=catalog(j.at("assets"));
                rules.autoReceiveCurrent=j.at("auto").get<bool>();rules.sharedOnly=j.at("shared_only").get<bool>();
                view.assets=std::move(metas);catalogVersion=v;
                std::erase_if(view.players,[&](auto const& i){return !view.assets.contains(i.second.contentKey);});
                reconcile();return;
            }
            if(type=="state"){receiveState(j);return;}
            if(type=="unavailable"){
                auto id=field(j,"subject",80),reason=field(j,"reason",32);
                require(!id.empty() && (reason=="denied" || reason=="conflicting_archive"));
                if(id==self){
                    auto k=field(j,"key",71);
                    if(!local.current || k!=local.current->contentKey || number(j.at("rev"))!=local.revision)return;
                }
                auto version=number(j.at("v"));
                if(!newerState(id,version))return;
                if(id==self){view.approved=false;status[local.current->contentKey]=reason;}
                view.players.erase(id);remoteVersion.erase(id);peerStatus[id]=std::move(reason);return;
            }
            if(type=="gone"){
                auto id=field(j,"subject",80);if(!newerState(id,number(j.at("v"))))return;
                view.players.erase(id);remoteVersion.erase(id);peerStatus.erase(id);return;
            }
        }
        if(type=="want"){
            auto k=field(j,"key",71);require(key(k));auto digest=field(j,"sha",64);require(hex(digest,64));
            Tick id=number(j.at("id"));auto off=number(j.at("off"),maxAsset);auto rank=number(j.at("priority"),3);
            auto own=sendable(peer,k,digest);
            if(!own){
                send(peer,"result/"+k,{{"t","error"},{"key",k},{"id",id},{"reason","denied"}});return;
            }
            require(off<own->size && off%RoomProtocol::chunkSize==0 && id);
            auto chunks=j.contains("count")?number(j.at("count"),maxWindowChunks):1;
            require(chunks>0);
            auto index=std::make_pair(peer,k);
            if(!sends.contains(index) && std::count_if(sends.begin(),sends.end(),
                [&](auto const& x){return x.second.peer==peer;})>=static_cast<std::ptrdiff_t>(maxTransfers))return;
            // The host establishes the current-model class itself. Guests may elevate
            // a catalog entry to selected, but cannot label an unused model visible.
            int permitted=static_cast<int>(rank);
            if(host && priority(k)>RoomPriority::Current && (!local.current || local.current->contentKey!=k))
                permitted=std::max(permitted,static_cast<int>(RoomPriority::Selected));
            auto end=std::min(own->size,static_cast<std::size_t>(off+chunks*RoomProtocol::chunkSize));
            sends[index]={peer,*own,id,static_cast<std::size_t>(off),permitted,++order,end};return;
        }
        if(type=="cancel"){
            auto it=sends.find({peer,field(j,"key",71)});
            if(it!=sends.end() && it->second.id==number(j.at("id")))sends.erase(it);
            return;
        }
        if(type!="chunk" && type!="error" && type!="wait")return;
        auto k=field(j,"key",71);auto it=downloads.find(k);
        if(it==downloads.end())return;
        auto& t=it->second;
        if(!t.pending || t.provider!=peer || t.id!=number(j.at("id")) || !view.assets.contains(k)
            || !same(t.asset,view.assets.at(k)) || source(t.asset)!=peer)return;
        if(type=="error"){auto reason=field(j,"reason",100);fail(k,reason=="denied" || reason=="upstream failed"?reason:"source unavailable");return;}
        if(type=="wait"){
            auto off=number(j.at("off"),maxAsset);
            if(t.offset==off){
                auto progress=number(j.at("upstream"),t.asset.size);
                t.upstreamRate=j.contains("upstream_bps")?number(j.at("upstream_bps"),maxAsset):0;
                t.upstreamSampleTime=now;
                if(!t.waitingSince || progress>t.upstreamOffset){t.waitingSince=now;t.upstreamOffset=progress;}
                Tick timeout=progress==t.asset.size?180000:45000;
                if(now-t.waitingSince>=timeout){fail(k,"host is not acquiring this file");return;}
                t.pending=false;t.sent=now+1500;t.attempts=0;
                status[k]="awaiting_host";
            }
            return;
        }
        if(number(j.at("off"),maxAsset)!=t.offset || field(j,"sha",64)!=t.asset.digest)return;
        auto count=std::min(RoomProtocol::chunkSize,t.asset.size-t.offset);
        if(tail.size()!=count){fail(k,"invalid chunk length");return;}
        try{
            if(t.admission && files.reserve){
                // Make room for a foreground download before abandoning its request.
                while(!files.reserve(t.asset) && discardPaused()){}
            }
            t.admission=false;
            files.append(t.asset,t.offset,tail);
            downloadRate.add(now,count);t.downloadRate.add(now,count);
            if(host)members.at(peer).uploaded.add(now,count);
            t.offset+=count;t.pending=t.offset<t.requestedEnd;t.attempts=0;t.sent=t.pending?now:0;t.waitingSince=0;
            if(t.offset==t.asset.size){
                status[k]="verifying";files.finish(t.asset);
                if(files.pollFinish){t.verifying=true;t.sent=now;}
                else {status[k]="cached";recheck[k]=now+30000;downloads.erase(it);}
            }else status[k]="downloading";
        }catch(std::exception const&){fail(k,"file verification or storage failed");}
    }
    bool discardPaused(){
        auto victim=downloads.end();
        for(auto it=downloads.begin();it!=downloads.end();++it){
            if(it->second.paused && (victim==downloads.end() || it->second.offset<victim->second.offset))victim=it;
        }
        if(victim==downloads.end())return false;
        cancel(victim->second.asset);status[victim->first]="waiting";downloads.erase(victim);return true;
    }
    void schedule(){
        std::size_t verifying=0;
        for(auto it=downloads.begin();it!=downloads.end();){
            auto active=it++;
            auto& task=active->second;
            if(!task.verifying)continue;
            int result=0;
            try{result=files.pollFinish(task.asset);}catch(std::exception const&){result=-1;}
            if(result>0){status[active->first]="cached";recheck[active->first]=now+30000;downloads.erase(active);}
            else if(result<0 || now-task.sent>=180000){auto k=active->first;fail(k,"file verification or storage failed");}
            else ++verifying;
        }
        std::vector<std::pair<RoomPriority,std::string>> candidates;
        for(auto const& [k,a]:view.assets){
            if(failed.contains(k))continue;
            if(auto t=downloads.find(k);t!=downloads.end() && t->second.verifying)continue;
            if(!downloads.contains(k)){
                if(auto check=recheck.find(k);check!=recheck.end() && check->second>now)continue;
                bool have=false;
                try{have=files.has(a);}catch(std::exception const&){fail(k,"invalid local cache");continue;}
                if(have){status[k]="cached";recheck[k]=now+30000;continue;}
                status[k]="waiting";
            }
            if(source(a))candidates.emplace_back(priority(k),k);
        }
        std::sort(candidates.begin(),candidates.end());
        std::set<std::string> top;
        for(auto const& [rank,k]:candidates){if(top.size()+verifying==maxTransfers)break;top.insert(k);}
        for(auto it=downloads.begin();it!=downloads.end();){
            if(!it->second.verifying && !it->second.paused && !top.contains(it->first)){
                send(it->second.provider,"cancel/"+it->first,{{"t","cancel"},{"key",it->first},{"id",it->second.id}});
                auto& d=it->second;
                if(!d.offset){status[it->first]="waiting";it=downloads.erase(it);continue;}
                d.paused=true;d.pending=false;d.id=++seq;d.sent=d.waitingSince=0;d.attempts=0;d.admission=true;
                if(files.pause)files.pause(d.asset);
                status[it->first]="paused";++it;
            }else ++it;
        }
        candidates.resize(top.size());
        std::ranges::sort(candidates,[&](auto const& a,auto const& b){
            if(a.first!=b.first)return a.first<b.first;
            auto const& cursor=lastPull[static_cast<std::size_t>(a.first)];
            return std::tuple(a.second<=cursor,std::string_view(a.second))<std::tuple(b.second<=cursor,std::string_view(b.second));
        });
        for(auto const& [rank,k]:candidates){
            auto const& a=view.assets.at(k);auto it=downloads.find(k);
            if(it==downloads.end())it=downloads.emplace(k,Incoming{a,*source(a),++seq}).first;
            auto& d=it->second;
            if(d.paused){d.paused=false;status[k]="downloading";}
            // Give a delayed response time to arrive before spending the remaining attempts.
            auto timeout=retryMs*(1u<<std::clamp(d.attempts-1,0,2));
            if(d.pending && now-d.sent<timeout)continue;
            if(!d.pending && d.sent>now)continue;
            if(d.attempts>=3){fail(k,"provider timed out");continue;}
            auto firstChunk=std::min(RoomProtocol::chunkSize,d.asset.size-d.offset);
            if(pullBalance<double(firstChunk))continue;
            // One bounded request covers up to 250 ms of the local rate limit.
            // The sender still paces each chunk and checks permission at send time.
            bool windows=host?members.at(d.provider).chunkWindows:hostChunkWindows;
            auto chunks=windows?std::clamp<std::size_t>(rules.bytesPerSecond/(4*RoomProtocol::chunkSize),1,maxWindowChunks):1;
            auto cost=std::min(chunks*RoomProtocol::chunkSize,d.asset.size-d.offset);
            Json request={{"t","want"},{"key",k},{"sha",a.digest},{"off",d.offset},{"id",d.id},{"priority",static_cast<unsigned>(rank)}};
            if(chunks>1)request["count"]=chunks;
            if(!send(d.provider,"want/"+k,std::move(request)))continue;
            lastPull[static_cast<std::size_t>(rank)]=k;
            d.requestedEnd=d.offset+cost;
            pullBalance-=double(cost);
            d.pending=true;d.sent=now;++d.attempts;
            if(!d.waitingSince)status[k]="downloading";
        }
        while(std::count_if(downloads.begin(),downloads.end(),[](auto const& p){return p.second.paused;})>static_cast<std::ptrdiff_t>(maxTransfers))
            discardPaused();
    }
    std::vector<RoomWire> flush(){
        std::vector<RoomWire> out;
        // Drain state and control envelopes ahead of bulk chunks. Coalescing is per
        // endpoint and subject, so a busy sender cannot grow the control queue.
        constexpr std::size_t maxControlBytes=128*1024;
        std::size_t bytes=0;
        auto i=controls.upper_bound(lastReceiver);
        for(auto left=controls.size();left;--left){
            if(i==controls.end())i=controls.begin();
            auto& [to,q]=*i++;
            while(!q.empty() && out.size()<32 && bytes+q.begin()->second.size()<=maxControlBytes){
                auto first=q.begin();bytes+=first->second.size();out.push_back({to,std::move(first->second),true,{},{}});q.erase(first);
            }
            lastReceiver=to;
            if(out.size()>=32 || bytes>=maxControlBytes)break;
        }
        std::erase_if(controls,[](auto const& p){return p.second.empty();});
        if(!controls.empty())return out;
        for(int emitted=0;emitted<4 && !sends.empty();++emitted){
            auto best=std::min_element(sends.begin(),sends.end(),[](auto const& a,auto const& b){
                return std::tie(a.second.rank,a.second.order)<std::tie(b.second.rank,b.second.order);
            });
            auto r=best->second;
            if(!mayServe(r.peer,r.asset)){sends.erase(best);continue;}
            auto n=std::min(RoomProtocol::chunkSize,r.asset.size-r.offset);
            if(balance<double(n))break;
            sends.erase(best);
            Json h={{"t","chunk"},{"id",r.id},{"key",r.asset.contentKey},{"sha",r.asset.digest},{"off",r.offset},{"room",room}};
            if(host)h["to"]=members.at(r.peer).nonce;
            else h["from"]=token;
            try{
                if(host && failed.contains(r.asset.contentKey)){
                    send(r.peer,"result/"+r.asset.contentKey,{{"t","error"},{"key",r.asset.contentKey},{"id",r.id},{"reason","upstream failed"}});
                    continue;
                }
                auto upstream=downloads.find(r.asset.contentKey);
                if(host && (upstream!=downloads.end() || !files.has(r.asset))){
                    std::size_t done=upstream==downloads.end()?0:upstream->second.offset;
                    send(r.peer,"result/"+r.asset.contentKey,{{"t","wait"},{"key",r.asset.contentKey},
                        {"id",r.id},{"off",r.offset},{"upstream",done},
                        {"upstream_bps",upstream==downloads.end()?0:upstream->second.downloadRate.perSecond(now)}});
                    continue;
                }
                auto payload=files.read(r.asset,r.offset,n);require(payload.size()==n);balance-=double(n);
                out.push_back({r.peer,wire(h,payload),false,r.asset.contentKey,r.asset.digest,n});
                r.offset+=n;
                if(r.offset<r.end){r.order=++order;sends.emplace(std::make_pair(r.peer,r.asset.contentKey),r);}
            }catch(std::exception const&){
                send(r.peer,"result/"+r.asset.contentKey,{{"t","error"},{"key",r.asset.contentKey},{"id",r.id},{"reason","source unavailable"}});
            }
        }
        return out;
    }
};

RoomProtocol::RoomProtocol(bool host,std::string self,std::string freshToken,RoomFiles files)
    :impl(std::make_unique<Impl>(host,std::move(self),std::move(freshToken),std::move(files))){}
RoomProtocol::~RoomProtocol()=default;
void RoomProtocol::joined(std::string const& id){
    auto& i=*impl;
    if(i.host){
        if(id.empty() || id==i.self || !textOk(id,80) || i.members.size()>=maxMembers)return;
        i.members.try_emplace(id);
    }else if(id.empty() && !i.link){i.link=true;i.hello();}
}
void RoomProtocol::left(std::string const& id){
    auto& i=*impl;
    if(i.host){
        if(!i.members.erase(id))return;
        i.controls.erase(id);
        std::erase_if(i.acceptedFrom,[&](auto const& p){if(p.second!=id)return false;i.accepted.erase(p.first);return true;});
        for(auto& [who,m]:i.members)if(!m.nonce.empty()){
            m.eventFloors.erase(id);i.controls[who].erase("shots/"+id);i.send(who,"p/"+id,{{"t","gone"},{"subject",id}});
        }
        i.rebuild();
    }else if(id.empty()){
        for(auto const& [_,d]:i.downloads)i.cancel(d.asset);
        i.downloads.clear();i.controls.clear();i.sends.clear();i.recheck.clear();i.failed.clear();i.status.clear();
        i.remoteVersion.clear();i.stateVersion.clear();i.peerStatus.clear();i.lastReported.clear();i.observedShotSequence.clear();i.retryAfter.clear();i.hostReports=false;
        i.catalogVersion=0;i.view={};i.link=false;i.online=false;i.room.clear();i.hostChunkWindows=false;i.downloadRate={};
        i.pendingRetries.clear();i.retryEpoch.clear();i.hostRetry=false;
        i.rates.clear();i.ratesAt=i.ratesSeenAt=0;
        i.token=newConnectionNonce();i.local.nonce=i.token;
        i.local.state.session=stream(i.token,i.local.revision);
        i.local.state.events.clear();
    }
}
void RoomProtocol::receive(std::string const& id,std::span<unsigned char const> bytes,Tick now){
    impl->now=now;
    try{auto j=header(bytes);impl->handle(id,j,bytes);}catch(std::exception const&){}
}
std::vector<RoomWire> RoomProtocol::tick(Tick now){
    auto& i=*impl;i.now=now;
    if(i.previous && now>=i.previous){
        double increase=double(std::min<Tick>(now-i.previous,1000))*double(i.rules.bytesPerSecond)/1000.0;
        i.balance=std::min<double>(4*chunkSize,i.balance+increase);
        i.pullBalance=std::min<double>(maxWindowChunks*chunkSize,i.pullBalance+increase);
    }
    i.previous=now;
    if(!i.host && i.link && !i.online && now>=i.lastHello+retryMs)i.hello();
    if(i.online){i.sendRetries();i.schedule();}
    return i.flush();
}
void RoomProtocol::setRules(RoomRules rules){
    require(rules.deniedKeys.size()<=256 && std::ranges::all_of(rules.deniedKeys,key));
    require(rules.sessionDenied.size()<=256 && std::ranges::all_of(rules.sessionDenied,key));
    rules.bytesPerSecond=std::clamp<std::size_t>(rules.bytesPerSecond,16*1024,4u<<20);
    impl->rules=std::move(rules);
    if(impl->host)impl->rebuild();
}
void RoomProtocol::share(std::vector<RelayAsset> assets){
    require(assets.size()<=maxCatalog);Catalog updated;
    for(auto& a:assets){require(valid(a));a.selectable=true;auto k=a.contentKey;updated.emplace(std::move(k),std::move(a));}
    impl->shared=std::move(updated);
    if(impl->host)impl->rebuild();
    else if(impl->online)impl->send("","offers",{{"t","offers"},{"assets",list(impl->shared)}});
}
std::map<std::string,std::string> RoomProtocol::deniedModels() const {
    std::map<std::string,std::string> result;
    if (!impl->host) return result;
    for (auto const& k : impl->rules.deniedKeys) result.emplace(k, "");
    for (auto const& k : impl->rules.sessionDenied) result.emplace(k, "");
    if(result.empty())return result;
    auto label = [&](RelayAsset const& asset) {
        auto it = result.find(asset.contentKey);
        if (it != result.end() && it->second.empty()) it->second = asset.title;
    };
    if (impl->local.current) label(*impl->local.current);
    for (auto const& [_, member] : impl->members) if (member.current) label(*member.current);
    for (auto const& [_, asset] : impl->shared) label(asset);
    for (auto const& [_, asset] : impl->accepted) label(asset);
    return result;
}
std::vector<RoomOffer> RoomProtocol::offers() const{
    std::vector<RoomOffer> out;
    for(auto const& [id,m]:impl->members)for(auto const& [_,a]:m.offers)out.push_back({id,a});
    return out;
}
bool RoomProtocol::acceptOffer(std::string const& id,std::string const& k){
    auto& i=*impl;if(!i.host || i.rules.deniedKeys.contains(k) || i.rules.sessionDenied.contains(k) || i.accepted.size()>=maxCatalog)return false;
    auto m=i.members.find(id);if(m==i.members.end())return false;
    auto found=m->second.offers.find(k);if(found==m->second.offers.end())return false;
    i.accepted[k]=found->second;i.acceptedFrom[k]=id;i.rebuild();return true;
}
void RoomProtocol::publish(std::optional<RelayAsset> a,RelayPose p){
    auto& i=*impl;if(a)require(valid(*a));
    auto& m=i.local;
    if(!a){
        if(!m.current && m.revision)return;
        // A pending local archive can already have animation and outfit state.
        // Peers only need the removal until the model can be published.
        p={};
    }
    bool selection=m.revision==0 || a.has_value()!=m.current.has_value() || (a && !same(*a,*m.current))
        || p.texture!=m.state.texture;
    auto revision=m.revision+(selection?1:0);
    p.xuid=i.self;p.serial=m.serial+1;p.session=stream(i.token,revision);p.contentKey=a?a->contentKey:"";
    auto encoded=presentation(p).dump();require(encoded.size()<48000);(void)pose(Json::parse(encoded));
    if(i.host && !selection){retainEvents(p,m.state);p.arrows=std::move(m.state.arrows);}
    m.serial=p.serial;m.revision=revision;
    m.state=std::move(p);m.current=std::move(a);
    if(i.host){
        if(selection)for(auto& [_,peer]:i.members)peer.eventFloors.erase(i.self);
        i.view.acknowledgedEvent=m.state.events.empty()?i.view.acknowledgedEvent:m.state.events.back().sequence;
        i.rebuild(selection,i.self);
    }else{
        if(selection){i.view.approved=!m.current;i.peerStatus.erase(i.self);}
        i.submitCurrent();
    }
}
void RoomProtocol::serverArrow(std::string const& shooter,std::int64_t entity,bool crossbow){
    auto& i=*impl;
    if(!i.host || !entity || i.members.empty())return;
    auto it=i.members.find(shooter);
    auto* wearer=shooter==i.self?&i.local:it==i.members.end()?nullptr:&it->second;
    if(!wearer || !i.currentAllowed(*wearer))return;
    auto& shots=wearer->state.arrows;
    if(std::ranges::find(shots,entity,&std::pair<std::int64_t,bool>::first)!=shots.end())return;
    if(shots.size()==32)shots.erase(shots.begin());
    shots.emplace_back(entity,crossbow);
    if(auto visible=i.view.players.find(shooter);visible!=i.view.players.end())visible->second.arrows=shots;
    for(auto const& [id,m]:i.members)if(!m.nonce.empty())i.putArrows(id,shooter,*wearer);
}
void RoomProtocol::setVisible(std::set<std::string> ids){impl->visible=std::move(ids);}
void RoomProtocol::retry(std::string const& subject,std::string const& streamId){
    auto& i=*impl;
    auto it=i.view.players.find(subject);
    if(it==i.view.players.end() || it->second.session!=streamId || !i.view.assets.contains(it->second.contentKey))return;
    auto const& k=it->second.contentKey;
    if(!i.host)i.resumeTransfer(k);
    i.requestRetry(subject,streamId);
}
bool RoomProtocol::canRetryUpload() const{
    auto& i=*impl;
    if(!i.online || !i.local.current)return false;
    if(i.host)return i.currentAllowed(i.local);
    return i.hostRetry && !i.peerStatus.contains(i.self);
}
void RoomProtocol::retryUpload(){
    if(!canRetryUpload())return;
    if(!impl->host){
        // A lost catalog or publication leaves no remote row to click. Request a fresh
        // snapshot with the existing nonce and advertise the same model revision.
        impl->hello();
        impl->submitCurrent();
    }
    impl->requestRetry(impl->self,impl->local.state.session);
}
std::map<std::string,RoomProgress> RoomProtocol::progress() const{
    std::map<std::string,RoomProgress> result;
    for(auto const& [k,d]:impl->downloads)result[k]={d.offset,d.asset.size,d.upstreamOffset,d.downloadRate.perSecond(impl->now),
        impl->now-d.upstreamSampleTime<3500?d.upstreamRate:0};
    return result;
}
void RoomProtocol::reportDisplays(RoomDisplays const& displays){
    auto& i=*impl;
    if(i.host || !i.online || !i.hostReports)return;
    std::erase_if(i.lastReported,[&](auto const& p){return !displays.contains(p.first);});
    for(auto const& [id,report]:displays){
        auto wearer=i.view.players.find(id);
        if(wearer==i.view.players.end() || wearer->second.session!=report.stream || !displayStage(report.stage))continue;
        auto old=i.lastReported.find(id);
        if(old!=i.lastReported.end() && old->second==report)continue;
        if(i.send("","display/"+id,{{"t","display"},{"subject",id},{"stream",report.stream},{"stage",report.stage},{"seq",++i.seq}}))
            i.lastReported[id]=report;
    }
}
std::map<std::string,RoomDisplays> RoomProtocol::observers() const{
    std::map<std::string,RoomDisplays> result;
    if(!impl->host)return result;
    for(auto const& [observer,m]:impl->members){
        if(m.nonce.empty())continue;
        auto add=[&](std::string const& id,Impl::Member const& wearer){
            if(observer==id || !impl->currentAllowed(wearer))return;
            auto found=m.observed.find(id);
            result[observer][id]=found!=m.observed.end()?found->second:RoomDisplay{wearer.state.session,m.supportsReports?"unreported":"unsupported"};
        };
        add(impl->self,impl->local);
        for(auto const& [id,wearer]:impl->members)add(id,wearer);
    }
    return result;
}
void RoomProtocol::select(std::string k){if(k.empty() || impl->view.assets.contains(k)){impl->selected=k;impl->failed.erase(k);}}
RelayFrame const& RoomProtocol::frame() const{return impl->view;}
RoomPriority RoomProtocol::priority(std::string const& k) const{return impl->priority(k);}
std::uint64_t RoomProtocol::downloadBytesPerSecond() const{return impl->downloadRate.perSecond(impl->now);}
void RoomProtocol::updateRates(std::map<std::string,std::uint64_t> const& downToPeer,std::uint64_t ownUpload,Tick now){
    auto& i=*impl;if(!i.host)return;
    bool wasActive=!i.rates.empty();
    i.rates.clear();
    auto ownDownload=i.downloadRate.perSecond(now);
    if(ownUpload || ownDownload)i.rates[i.self]={ownUpload,ownDownload};
    for(auto const& [id,member]:i.members){
        if(member.nonce.empty())continue;
        auto it=downToPeer.find(id);
        auto down=it==downToPeer.end()?0:it->second;
        auto up=member.uploaded.perSecond(now);
        if(up || down)i.rates[id]={up,down};
    }
    if(!wasActive && i.rates.empty())return;
    Json rows=Json::array();
    for(auto const& [id,r]:i.rates)rows.push_back({{"peer",id},{"up",r.upload},{"down",r.download}});
    for(auto const& [id,member]:i.members)if(member.supportsRates)
        i.send(id,"rates",{{"t","rates"},{"rows",rows},{"seq",++i.ratesAt}});
}
RoomPeerRates RoomProtocol::peerRates() const{
    if(!impl->host && impl->now-impl->ratesSeenAt>3500)return {};
    return impl->rates;
}
std::map<std::string,std::string> const& RoomProtocol::statuses() const{return impl->status;}
std::map<std::string,std::string> const& RoomProtocol::peerStates() const{return impl->peerStatus;}
bool RoomProtocol::maySendChunk(std::string const& peer,std::string const& k,std::string const& digest) const{
    return impl->sendable(peer,k,digest)!=nullptr;
}
std::string const& RoomProtocol::session() const{return impl->room;}
bool RoomProtocol::connected() const{return impl->online;}
} // namespace ysm
