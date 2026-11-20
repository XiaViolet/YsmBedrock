#include "RoomProtocol.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
using namespace ysm;
using Bytes=std::vector<unsigned char>;
using Ms=std::uint64_t;
void check(bool ok,char const* why){if(!ok)throw std::runtime_error(why);}
struct Store {
    std::map<std::string,Bytes> originals,partial,committed;
    std::vector<std::string> writes;
    bool async{}, allowFinish{}, rejectFinish{};
    static Bytes content(RelayAsset const& a){
        Bytes data(a.size);
        for(std::size_t i=0;i<data.size();++i)data[i]=static_cast<unsigned char>(i*13+a.digest.front());
        return data;
    }
    bool has(RelayAsset const& a) const{return originals.contains(a.digest)||committed.contains(a.digest);}
    RoomFiles callbacks(){
        RoomFiles f{
            [this](RelayAsset const& a){return has(a);},
            [this](RelayAsset const& a,std::size_t off,std::size_t length){
                auto const& data=originals.contains(a.digest)?originals.at(a.digest):committed.at(a.digest);
                check(off<=data.size() && length<=data.size()-off,"out of range read");
                return Bytes(data.begin()+off,data.begin()+off+length);},
            [this](RelayAsset const& a,std::size_t off,std::span<unsigned char const> payload){
                auto& b=partial[a.digest];check(b.size()==off,"out of order append");
                b.insert(b.end(),payload.begin(),payload.end());},
            [this](RelayAsset const& a){
                auto b=partial.at(a.digest);check(b==content(a),"integrity");
                committed.emplace(a.digest,std::move(b));partial.erase(a.digest);writes.push_back(a.contentKey);},
            [this](RelayAsset const& a){partial.erase(a.digest);}
        };
        if(async){
            auto commit=f.finish;
            f.finish=[](RelayAsset const&){};
            f.pollFinish=[this,commit](RelayAsset const& a){
                if(rejectFinish)return -1;
                if(!allowFinish)return 0;
                commit(a);return 1;
            };
        }
        return f;
    }
};
RelayAsset model(char id,std::size_t size=50000){
    return {"local1_"+std::string(64,id),std::string(64,id),".zip",size,"same display name",false};
}
RelayPose state(std::string texture,std::string action={}){
    RelayPose p;p.texture=std::move(texture);p.action=std::move(action);p.actionSerial=4;p.actionTime=.25;
    p.roaming["outfit"]=3;return p;
}
Json head(Bytes const& bytes){
    std::size_t n=bytes.at(0)+256u*bytes.at(1);return Json::parse(bytes.begin()+2,bytes.begin()+2+n);
}
Bytes packet(Json const& j){
    auto s=j.dump();Bytes out={static_cast<unsigned char>(s.size()),static_cast<unsigned char>(s.size()>>8)};
    out.insert(out.end(),s.begin(),s.end());return out;
}
struct Node {
    Store files;
    std::unique_ptr<RoomProtocol> room;
    bool online{};
    Node(bool host,std::string id,char salt){reset(host,std::move(id),salt);online=host;}
    void reset(bool host,std::string id,char salt){
        room=std::make_unique<RoomProtocol>(host,std::move(id),std::string(32,salt),files.callbacks());
    }
    void owns(RelayAsset const& a){files.originals[a.digest]=Store::content(a);}
};
struct Lab {
    Node host{true,"H",'1'}, a{false,"A",'2'}, b{false,"B",'3'};
    Ms now=100;
    struct Sample{std::string from,to;Json h;Bytes bytes;};
    std::vector<Sample> trace;
    std::function<bool(Sample&)> deliver=[](auto&){return true;};
    Node& peer(std::string const& name){return name=="H"?host:name=="A"?a:b;}
    void join(std::string const& id){
        peer(id).online=true;host.room->joined(id);peer(id).room->joined("");
    }
    void leave(std::string const& id){
        host.room->left(id);auto& n=peer(id);n.room->left("");n.online=false;
    }
    void step(){
        for(std::string who:{"H","A","B"}){
            auto& n=peer(who);if(!n.online)continue;
            for(auto const& m:n.room->tick(now)){
                std::string dest=who=="H"?m.peer:"H";
                auto h=head(m.bytes);
                check((h.at("t")!="chunk")==m.control,"transport priority");
                if(!m.control)check(n.room->maySendChunk(m.peer,m.assetKey,m.assetDigest),"emitted unauthorized block");
                check(m.bytes.size()<=RoomProtocol::maxWire,"frame size");
                Sample sample{who,dest,std::move(h),m.bytes};bool send=deliver(sample);
                trace.push_back(sample);
                if(send && peer(dest).online)peer(dest).room->receive(dest=="H"?who:"",sample.bytes,now);
            }
        }
        now+=50;
    }
    void run(int cycles){while(cycles--)step();}
    int chunks(std::string from,std::string to,RelayAsset const& asset){
        return static_cast<int>(std::count_if(trace.begin(),trace.end(),[&](auto const& x){
            return x.from==from && x.to==to && x.h.at("t")=="chunk" && x.h.at("key")==asset.contentKey;
        }));
    }
};
void distribution(){
    Lab l;auto h=model('a'),a=model('b'),b=model('c'),privateModel=model('d');
    l.host.owns(h);l.a.owns(a);l.b.owns(b);l.a.owns(privateModel);
    l.host.room->publish(h,state("host","idle"));l.a.room->publish(a,state("red","dance"));
    l.b.room->publish(b,state("blue","wave"));l.join("A");l.run(100);
    auto latest=state("red","dance");latest.events={{1,{.75f}}};
    l.a.room->publish(a,latest);l.run(10);l.join("B");l.run(140);
    check(l.b.room->frame().players.at("A").events.empty(),"late join repeated old event");
    for(std::string id:{"H","A","B"}){
        auto& n=l.peer(id);check(n.room->frame().players.size()==2,"all three participants need latest two peers");
        check(n.files.has(a) && n.files.has(b) && n.files.has(h),"all three need independent valid content");
        check(!n.room->frame().assets.contains(privateModel.contentKey),"private file exposed");
        check(!n.room->frame().players.contains(id),"local echo");
    }
    check(l.chunks("A","H",a)==4 && l.chunks("H","B",a)==4,"A-host-B transfer count");
    check(l.b.room->frame().players.at("A").texture=="red","missed A presentation");
    check(l.host.room->frame().players.at("B").action=="wave","B action mixed with A");
    auto cachedChunks=l.chunks("H","B",a);
    l.leave("B");l.b.reset(false,"B",'4');l.b.room->publish(b,state("alternate"));
    l.join("B");l.run(100);
    check(l.chunks("H","B",a)==cachedChunks,"cache hit downloaded twice");
    l.b.room->publish(a,state("different outfit","sitting"));l.run(10);
    check(l.host.room->frame().players.at("A").action=="dance" &&
        l.host.room->frame().players.at("B").action=="sitting","two wearers sharing content lost independence");
    std::cout<<"PASS three endpoints, same-name different assets, A -> host -> B, late join event floor, cache reuse, independent state\n";
}
void authorization(){
    Lab l;auto a=model('a'),extra=model('e'),unknown=model('d');
    l.a.owns(a);l.a.owns(extra);l.a.owns(unknown);l.a.room->publish(a,state("red"));
    l.join("A");l.join("B");l.run(100);
    auto victim=std::find_if(l.trace.rbegin(),l.trace.rend(),[&](auto const& m){
        return m.to=="B" && m.from=="H" && m.h.at("t")=="state" && m.h.at("subject")=="A";
    });check(victim!=l.trace.rend(),"state fixture");auto stale=victim->bytes;
    RoomRules rules;rules.deniedKeys.insert(a.contentKey);l.host.room->setRules(rules);l.run(8);
    check(l.b.files.has(a) && !l.b.room->frame().players.contains("A"),"policy did not override cache");
    check(l.b.room->peerStates().at("A")=="denied" && l.a.room->statuses().at(a.contentKey)=="denied","missing explicit refusal");
    for(auto const& m:l.trace)if(m.from=="H" && m.to=="B" && m.h.at("t")=="unavailable")
        check(!m.h.contains("key") && !m.h.contains("state") && !m.h.contains("sha"),"denial leaks private asset details");
    check(!l.host.room->maySendChunk("B",a.contentKey,a.digest),"backlogged chunk survived revocation");
    l.b.room->receive("",stale,l.now);check(!l.b.room->frame().players.contains("A"),"old allowed state restored revoked recipient");
    auto count=l.chunks("H","B",a);
    auto forged=packet({{"t","want"},{"room",l.host.room->session()},{"from",std::string(32,'3')},
        {"key",a.contentKey},{"sha",a.digest},{"off",0},{"priority",0},{"id",71}});
    l.host.room->receive("B",forged,l.now);l.run(2);check(l.chunks("H","B",a)==count,"unauthorized cached file served");
    rules.deniedKeys.clear();rules.autoReceiveCurrent=false;l.host.room->setRules(rules);l.run(4);
    check(!l.host.room->frame().players.contains("A"),"disabled auto receive still accepts a private current model");
    l.a.room->share({extra});l.run(6);
    check(l.host.room->offers().size()==1 && !l.host.files.has(extra),"opt-in offer downloaded before selection");
    check(!l.host.room->acceptOffer("A",unknown.contentKey),"host guessed a private unoffered key");
    check(l.host.room->acceptOffer("A",extra.contentKey),"valid offer rejected");
    l.run(110);check(l.b.files.has(extra) && l.b.room->frame().assets.at(extra.contentKey).selectable,"offered file distribution failed");
    l.a.room->share({});l.run(8);check(!l.b.room->frame().assets.contains(extra.contentKey),"withdrawn offer remains public");
    std::cout<<"PASS cached denial, actual chunk request authorization, disabled auto-receive, opt-in offerings, revocation\n";
}
void priorityAndStale(){
    Lab l;auto huge=model('a',2*1024*1024),urgent=model('f',32000),next=model('e',35000);
    l.host.owns(huge);l.a.owns(urgent);l.a.owns(next);l.host.room->share({huge});
    l.join("A");l.join("B");l.run(12);check(l.b.files.partial.contains(huge.digest),"background transfer not active");
    l.b.room->setVisible({"A"});l.a.room->publish(urgent,state("urgent","first"));l.run(48);
    check(l.b.room->priority(urgent.contentKey)==RoomPriority::Visible,"visible priority");
    check(l.b.files.has(urgent) && !l.b.files.has(huge),"active model failed to preempt background");
    Bytes oldState,oldCatalog;
    for(auto const& sample:l.trace)if(sample.from=="H" && sample.to=="B"){
        if(sample.h.at("t")=="catalog")oldCatalog=sample.bytes;
        if(sample.h.at("t")=="state" && sample.h.at("subject")=="A")oldState=sample.bytes;
    }
    l.a.room->publish(next,state("new","second"));l.run(5);l.b.room->receive("",oldCatalog,l.now);l.b.room->receive("",oldState,l.now);
    check(l.b.room->frame().players.at("A").contentKey==next.contentKey && !l.b.room->frame().assets.contains(urgent.contentKey),
        "stale catalog/model selection overtook newer session revision");
    l.leave("A");l.run(4);l.b.room->receive("",oldState,l.now);check(!l.b.room->frame().players.contains("A"),"stale session restored departed player");
    l.leave("B");l.b.reset(false,"B",'6');l.join("B");l.run(4);l.b.room->receive("",oldState,l.now);
    check(!l.b.room->frame().players.contains("A"),"old connection replay after reconnect");
    std::cout<<"PASS visible model preemption, quick selection changes, stale packets after player leave/rejoin\n";
}
void failure(){
    Lab l;auto a=model('a'),b=model('b');
    l.a.owns(a);l.a.owns(b);l.join("A");l.join("B");
    l.deliver=[&](Lab::Sample& m){
        if(m.from=="A" && m.h.at("t")=="chunk"){
            if(m.h.at("key")==a.contentKey)m.bytes.back()^=0x40;
            if(m.h.at("key")==b.contentKey)return false;
        }
        return true;
    };
    l.a.room->publish(a,state(""));l.run(100);
    check(!l.host.files.has(a) && l.host.room->statuses().at(a.contentKey).starts_with("failed"),"corrupt file retained");
    check(!l.b.files.has(a) && l.b.room->statuses().at(a.contentKey).starts_with("failed"),"upstream failure not propagated");
    l.a.room->publish(b,state(""));l.run(400);
    check(!l.host.files.has(b) && l.host.room->statuses().at(b.contentKey).find("timed out")!=std::string::npos,"lost chunks never timed out");
    check(l.chunks("A","H",b)==3,"unbounded retry count");
    check(l.host.files.partial.empty() && l.b.files.partial.empty(),"failed transfers kept partial files");
    std::cout<<"PASS integrity rejection, upstream failure propagation, bounded retries and cleanup\n";
}
void asyncFinish(){
    Lab l;auto a=model('a',32768),b=model('b',35000);l.a.owns(a);l.b.owns(b);
    l.host.files.async=true;l.host.reset(true,"H",'7');
    l.join("A");l.join("B");l.a.room->publish(a,state("wearA"));l.b.room->publish(b,state("wearB"));l.run(60);
    check(l.host.room->statuses().at(a.contentKey)=="verifying" && !l.host.files.has(a),"missing verifying stage");
    auto upstream=l.chunks("A","H",a);
    check(upstream==2 && l.chunks("H","B",a)==0,"unverified bytes relayed");
    l.a.room->publish(a,state("wearA","dance"));l.run(15);
    check(l.b.room->frame().players.at("A").action=="dance","verification blocked control snapshot");
    check(l.chunks("A","H",a)==upstream,"verified range was re-requested while poll pending");
    l.host.files.allowFinish=true;l.run(100);
    check(l.b.files.has(a) && l.a.files.has(b),"async commit failed to unlock distribution");
    auto other=model('c');l.a.owns(other);l.host.files.rejectFinish=true;l.a.room->publish(other,state("fail"));l.run(100);
    check(l.host.room->statuses().at(other.contentKey).starts_with("failed") && !l.b.files.has(other),"async verification failure served data");
    std::cout<<"PASS asynchronous verification, control delivery during verification, withheld uncommitted data, failure cleanup\n";
}
void selectionAcknowledgement(){
    Lab l;
    auto allowed=model('a'),denied=model('b');
    l.a.owns(allowed);l.a.owns(denied);
    RoomRules rules;rules.deniedKeys.insert(denied.contentKey);l.host.room->setRules(rules);
    l.a.room->publish(denied,state("first"));l.join("A");l.run(10);
    check(!l.a.room->frame().approved,"denied selection acknowledged");

    Bytes delayed;
    l.deliver=[&](auto& m){
        if(m.to=="A" && m.h.at("t")=="state" && m.h.at("subject")=="A"){
            delayed=m.bytes;return false;
        }
        return true;
    };
    l.a.room->publish(allowed,state("briefly selected"));l.run(10);
    check(!delayed.empty(),"missing host reply for prior selection");
    l.a.room->publish(denied,state("current"));
    l.a.room->receive("",delayed,l.now);
    check(!l.a.room->frame().approved,"previous selection approved the new model");
    l.deliver=[](auto&){return true;};
    l.a.room->publish(allowed,state("current"));l.run(10);
    check(l.a.room->frame().approved,"current selection not approved");
    l.a.room->publish(denied,state("current"));
    check(!l.a.room->frame().approved,"new selection inherited approval before the host replied");
    std::cout<<"PASS rapid reselection limits host approval to the matching local model revision.\n";
}
void vanillaIdle(){
    Lab l;l.join("A");l.join("B");
    l.host.room->publish({},{});l.a.room->publish({},{});l.run(10);
    auto prior=l.trace.size();
    for(int n=0;n<30;++n){l.host.room->publish({},{});l.a.room->publish({},{});l.step();}
    check(l.trace.size()==prior,"unchanged vanilla appearance produced presentation traffic");
    auto a=model('a');l.a.owns(a);l.a.room->publish(a,state("wear"));l.run(25);
    check(l.b.room->frame().players.contains("A"),"selection missed after vanilla silence");
    l.a.room->publish({},state("private outfit","pending model action"));l.run(8);
    check(!l.b.room->frame().players.contains("A"),"switch to vanilla failed to clear remote model");
    auto last=std::find_if(l.trace.rbegin(),l.trace.rend(),[](auto const& m){
        return m.from=="A" && m.h.at("t")=="publish";
    });
    check(last!=l.trace.rend() && last->h.dump().find("pending model action")==std::string::npos,
        "local model preparation exposed unannounced animation state");
    l.leave("B");l.join("B");l.run(8);
    check(!l.b.room->frame().players.contains("A"),"late join replayed removed model");
    std::cout<<"PASS unchanged vanilla appearances send no updates; model activation, removal and late join still synchronize.\n";
}
void formatsAndEvents(){
    Lab l;auto pack=model('a'),packed=model('e'),rest=model('c');
    packed.extension=".ysm";packed.originVersion=0;
    l.host.owns(pack);l.a.owns(packed);l.b.owns(rest);
    l.host.room->publish(pack,state("h"));l.a.room->publish(packed,state("a"));l.b.room->publish(rest,state("b"));
    l.join("A");l.join("B");l.run(100);
    auto const& catalog=l.b.room->frame().assets;
    check(catalog.at(packed.contentKey).sourceSpec==2 && catalog.at(packed.contentKey).originVersion==0
        && !catalog.at(pack.contentKey).originVersion,"zero origin collapsed into missing origin or spec changed");
    auto event=state("h");event.events={{25,{9.5f}}};
    l.host.room->publish(pack,event);l.host.room->publish(pack,state("h"));l.run(10);
    auto const& received=l.b.room->frame().players.at("H").events;
    check(received.size()==1 && received.front().sequence==25,"backpressure swallowed or duplicated one-shot events");
    // A connection that cannot read any common model spec receives an explicit handshake outcome.
    Lab other;other.host.room->joined("A");other.a.room->joined("");
    auto hello=other.a.room->tick(100).front();
    auto j=head(hello.bytes);j["source_specs"]={999};
    other.host.room->receive("A",packet(j),100);
    for(auto const& m:other.host.room->tick(150))other.a.room->receive("",m.bytes,150);
    check(!other.a.room->connected() && other.a.room->statuses().at("protocol").find("incompatible")!=std::string::npos,
        "unsupported model spec handshake silently accepted");
    auto own=state("wear");l.a.room->publish(packed,own);l.run(5);
    Bytes oldWelcome;
    for(auto const& m:l.trace)if(m.to=="A" && m.h.at("t")=="welcome")oldWelcome=m.bytes;
    l.leave("A");l.a.room->joined("");l.a.room->receive("",oldWelcome,l.now);
    check(!l.a.room->connected(),"delayed welcome reused after transport connection reset");
    std::cout<<"PASS real format fields and capability negotiation, coalesced one-shot preservation, reconnect nonce isolation\n";
}
}
int main(){
    try{distribution();authorization();priorityAndStale();failure();asyncFinish();formatsAndEvents();selectionAcknowledgement();vanillaIdle();return 0;}
    catch(std::exception const& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
