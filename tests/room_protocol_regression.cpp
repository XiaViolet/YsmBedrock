#include "RoomProtocol.h"
#include <algorithm>
#include <deque>
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
void hostFiresArrows(){
    Lab l;
    auto asset=model('a');
    l.host.owns(asset);
    l.host.room->publish(asset,state("skin"));
    l.join("A");
    l.run(40);
    using Shots=std::vector<std::pair<std::int64_t,bool>>;
    Shots expected{{-2714419330716LL,false},{-2714419330715LL,true}};
    for(auto [id,crossbow]:expected) l.host.room->serverArrow("H",id,crossbow);
    l.host.room->publish(asset,state("skin","switched_hand"));
    l.run(5);
    check(l.a.room->frame().players.at("H").arrows==expected,"guest lost host bow/crossbow after hand change");
    l.join("B");
    l.run(20);
    check(l.b.room->frame().players.at("H").arrows==expected,"late guest lost host bow/crossbow");
    std::cout<<"PASS observer receives host bow and crossbow with signed game entity IDs before and after join\n";
}
void authoritativeArrows(){
    Lab l;auto asset=model('b');l.a.owns(asset);l.a.room->publish(asset,state("skin"));l.join("A");l.run(40);
    l.host.room->serverArrow("A",-12345,false);
    l.host.room->serverArrow("A",9987,true);
    l.join("B");l.run(40);
    auto shots=l.b.room->frame().players.at("A").arrows;
    check(shots.size()==2 && shots[0].first==-12345 && !shots[0].second && shots[1].first==9987 && shots[1].second,
        "late observer lost bow/crossbow launch identity");
    auto spoof=state("skin");spoof.arrows={{9987,false}};l.a.room->publish(asset,spoof);l.run(10);
    check(l.b.room->frame().players.at("A").arrows==shots,"guest pose changed authoritative arrows");
    // Three arrow entities leave the same crossbow in one game tick.
    for(auto id:{777,778,779,779})l.host.room->serverArrow("A",id,true);
    l.run(6);
    shots=l.b.room->frame().players.at("A").arrows;
    check(shots.size()==5 && shots[2].first==777 && shots[3].first==778 && shots[4].first==779 &&
        shots[2].second && shots[3].second && shots[4].second,"multishot lost or duplicated an entity");
    auto old=std::ranges::find_if(l.trace.rbegin(),l.trace.rend(),[](auto const& sample){
        return sample.to=="B" && sample.h.at("t")=="arrows";
    })->bytes;
    l.a.room->publish(asset,state("next"));l.run(10);
    l.b.room->receive("",old,l.now);
    check(l.b.room->frame().players.at("A").arrows.empty(),"new selection inherited old projectiles");
    l.host.room->serverArrow("A",780,false);
    RoomRules denied;denied.deniedKeys.insert(asset.contentKey);l.host.room->setRules(denied);
    auto before=l.trace.size();l.run(6);
    check(!l.b.room->frame().players.contains("A"),"revoked shooter remains visible as model");
    check(std::none_of(l.trace.begin()+before,l.trace.end(),[](auto const& s){return s.h.at("t")=="arrows";}),
        "pending shot sent after model permission revoked");
    l.host.room->setRules({});l.run(6);
    l.host.room->serverArrow("A",781,false);l.leave("A");l.run(6);l.join("A");l.run(15);
    l.b.room->receive("",old,l.now);
    check(l.b.room->frame().players.at("A").arrows.empty(),"old connection arrows survived shooter rejoin");
    std::cout<<"PASS authoritative bow/crossbow, multishot, late observer, spoof rejection, selection, revoke and rejoin\n";
}
void locomotionInputs(){
    // At 40k blocks, float interpolation on individual render frames can alternate
    // between zero displacement and a whole coordinate unit in the last place.
    auto left=playerTravelAxes({40000,0},{39999.99609375f,0},0);
    auto right=playerTravelAxes({40000,0},{40000.00390625f,0},0);
    check(left[0]==1 && left[1]==0 && right[0]==-1 && right[1]==0,"signed lateral input");
    auto diagonal=playerTravelAxes({10000.f,20000.f},{10000.0703125f,20000.072265625f},45);
    check(diagonal[0]<-.99 && diagonal[1]==0,"rounded strafe manufactured forward/backward motion");
    auto rest=playerTravelAxes({40000,0},{40000,0},0);
    check(rest[0]==0 && rest[1]==0,"stopped player retains travel input");

    Lab l;auto asset=model('b');l.a.owns(asset);
    auto p=state("skin");p.locomotion=PlayerLocomotion{true,true,5.5,.4,left};
    l.a.room->publish(asset,p);l.join("A");l.join("B");l.run(30);
    for(auto* n:{&l.host,&l.b}){
        auto m=n->room->frame().players.at("A").locomotion;
        check(m && m->onGround && m->sprinting && m->axes==left && m->groundSpeed==5.5,"owner sprint and axes lost");
    }
    p.flight=1u;p.locomotion=PlayerLocomotion{false,false,3,.2,right};l.a.room->publish(asset,p);l.run(8);
    auto airborne=l.b.room->frame().players.at("A");
    check(airborne.flight==1 && airborne.locomotion && !airborne.locomotion->onGround && airborne.locomotion->axes==right,
        "low flight kept observer ground contact");
    p.locomotion.reset();l.a.room->publish(asset,p);l.run(8);
    check(!l.b.room->frame().players.at("A").locomotion,"legacy snapshot retained stale owner movement");
    std::cout<<"PASS complete-tick strafe at large coordinates; owner sprint, flight and movement through A -> host -> B\n";
}
void flightAndAuthorFormSnapshots() {
    Lab l; auto asset=model('b'); l.a.owns(asset);
    auto pose=state("skin"); pose.flight=1u; pose.formValues={{"variable.t7",2},{"variable.t8",6}};
    l.a.room->publish(asset,pose); l.join("A"); l.run(20);
    l.join("B"); l.run(20);
    auto const& first=l.b.room->frame().players.at("A");
    check(first.flight==1u && first.formValues==pose.formValues,"late join flight and authored face state");
    pose.flight=0u; pose.formValues["variable.t8"]=0;
    l.a.room->publish(asset,pose); l.run(10);
    for (auto* node : {&l.host,&l.b}) {
        auto const& stopped=node->room->frame().players.at("A");
        check(stopped.flight==0u && stopped.formValues.at("variable.t8")==0,"landing and expression reset");
    }
    pose.flight=2u; l.a.room->publish(asset,pose); l.run(10);
    check(l.b.room->frame().players.at("A").flight==2u,"elytra flight independent of creative flight");
    pose.flight.reset();pose.formValues.clear();l.a.room->publish(asset,pose);l.run(10);
    auto const& legacy=l.b.room->frame().players.at("A");
    check(!legacy.flight && legacy.formValues.empty(),"older peer has no authoritative flight state");
}
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
    l.a.room->publish(b,state(""));l.run(760);
    check(!l.host.files.has(b) && l.host.room->statuses().at(b.contentKey).find("timed out")!=std::string::npos,"lost chunks never timed out");
    auto attempts=std::ranges::count_if(l.trace,[&](auto const& m){
        return m.from=="H" && m.to=="A" && m.h.at("t")=="want" && m.h.at("key")==b.contentKey;
    });
    check(attempts==3 && l.chunks("A","H",b)==3*static_cast<int>((b.size+RoomProtocol::chunkSize-1)/RoomProtocol::chunkSize),"unbounded retry count");
    check(l.host.files.partial.empty() && l.b.files.partial.empty(),"failed transfers kept partial files");
    auto streamId=l.b.room->frame().players.at("A").session;
    l.deliver=[](auto&){return true;};
    l.b.room->retry("A","stale");l.run(8);
    check(!l.host.files.has(b),"stale retry revived host download");
    l.b.room->retry("A",streamId);l.run(150);
    check(l.host.files.has(b) && l.b.files.has(b),"single guest retry did not recover both hops");
    std::cout<<"PASS integrity rejection, upstream failure propagation, bounded retries and cleanup\n";
}
void manualRecovery(bool byUploader){
    Lab l;auto a=model('b',150000);l.a.owns(a);l.a.room->publish(a,state("outfit","dance"));
    l.join("A");l.join("B");
    l.deliver=[](auto& m){return !(m.from=="A" && m.h.at("t")=="chunk");};
    l.run(800);
    check(l.host.room->statuses().at(a.contentKey).starts_with("failed") &&
          l.b.room->statuses().at(a.contentKey).starts_with("failed"),"both hops must have latched the failure");
    auto session=l.b.room->session(),streamId=l.b.room->frame().players.at("A").session;
    struct Delivery {Ms due;Lab::Sample msg;};
    std::deque<Delivery> delayed;
    int attempts=0,acksDropped=0;
    l.deliver=[&](auto& m){
        if(m.h.at("t")=="retry_model" && ++attempts==1)return false;
        if(m.h.contains("retry_ack") && !acksDropped++){return false;}
        delayed.push_back({l.now+800,m});return false;
    };
    if(byUploader){
        check(l.a.room->canRetryUpload(),"wearer has no upload retry");
        l.a.room->retryUpload();l.a.room->retryUpload();
    }else{
        l.b.room->retry("A",streamId);l.b.room->retry("A",streamId);
    }
    for(int n=0;n<1000;++n){
        while(!delayed.empty() && delayed.front().due<=l.now){
            auto& m=delayed.front().msg;l.peer(m.to).room->receive(m.to=="H"?m.from:"",m.bytes,l.now);delayed.pop_front();
        }
        l.step();
    }
    check(attempts==3 && acksDropped>=2,"lost request/response did not recover with bounded retransmission");
    check(l.host.files.has(a) && l.b.files.has(a) && l.b.room->session()==session &&
        l.b.room->frame().players.at("A").session==streamId,"same room recovery lost wearer or file");
    check(l.b.files.writes.size()==1 && l.host.files.writes.size()==1,"duplicate retry committed the same file twice");
    std::cout<<"PASS 1600 ms simulated RTT, "<<(byUploader?"wearer upload":"observer download")
        <<" retry, first control and first acknowledgement lost, A -> host -> B verified in the original session\n";
}
void retryAfterAllControlsLost(){
    Lab l;auto a=model('b');l.a.owns(a);l.a.room->publish(a,state(""));l.join("A");l.join("B");
    int dropped=0;
    l.deliver=[&](auto& m){
        if(m.h.at("t")=="retry_model"){++dropped;return false;}
        return !(m.from=="A" && m.h.at("t")=="chunk");
    };
    l.run(800);l.a.room->retryUpload();
    for(int i=0;i<320 && dropped<3;++i)l.step();
    check(dropped==3 && !l.host.files.has(a),"fixture lost all three upload requests");
    l.deliver=[](auto&){return true;};l.a.room->retryUpload();l.run(40);
    check(l.host.files.has(a) && l.b.files.has(a),"a new click was ignored after exhausting control retries");
    std::cout<<"PASS second manual upload after all three previous requests were lost, same room\n";
}
void retryMissingPublication(){
    Lab l;auto a=model('b');l.a.owns(a);l.a.room->publish(a,state("skin"));
    l.deliver=[](auto& msg){return !(msg.from=="A" && msg.h.at("t")=="publish");};
    l.join("A");l.join("B");l.run(30);
    check(l.a.room->connected() && !l.a.room->frame().approved && l.host.room->frame().players.empty(),
        "missing initial publication fixture");
    check(l.a.room->canRetryUpload(),"unacknowledged publication has no upload retry");
    auto room=l.a.room->session();
    l.deliver=[](auto&){return true;};l.a.room->retryUpload();l.run(200);
    check(l.a.room->session()==room && l.a.room->frame().approved && l.host.files.has(a) && l.b.files.has(a),
        "a lost publication required rejoining");
    RoomRules denied;denied.sessionDenied.insert(a.contentKey);l.host.room->setRules(denied);l.run(5);
    check(!l.a.room->canRetryUpload(),"uploader can retry a refused model");
    std::cout<<"PASS missing first publication: wearer restored the model with the original room and selection\n";
}
void retryPartialAndCached(){
    Lab l;auto a=model('c',400000);l.host.owns(a);l.host.room->publish(a,state(""));l.join("B");
    Lab::Sample oldChunk;
    l.deliver=[&](auto& m){
        if(m.h.at("t")=="chunk" && m.h.at("off")>=RoomProtocol::chunkSize){oldChunk=m;return false;}
        return true;
    };
    l.run(30);auto before=l.b.room->progress().at(a.contentKey).received;
    check(before==RoomProtocol::chunkSize && !oldChunk.bytes.empty(),"partial download fixture");
    auto streamId=l.b.room->frame().players.at("H").session;
    l.b.room->retry("H",streamId);
    l.b.room->receive("",oldChunk.bytes,l.now);
    check(l.b.room->progress().at(a.contentKey).received==before,"stale window consumed after manual restart");
    l.deliver=[](auto&){return true;};
    auto mark=l.trace.size();l.run(120);
    check(l.b.files.has(a),"in-progress retry failed");
    for(auto i=mark;i<l.trace.size();++i)if(l.trace[i].h.at("t")=="want")
        check(l.trace[i].h.at("off")>=before,"retry downloaded the accepted prefix again");
    auto count=l.chunks("H","B",a);l.host.room->retryUpload();l.run(60);
    check(count==l.chunks("H","B",a),"valid cache downloaded again");
    RoomRules rules;rules.sessionDenied.insert(a.contentKey);l.host.room->setRules(rules);l.run(10);
    l.b.room->retry("H",streamId);l.host.room->retryUpload();l.run(80);
    check(!l.host.room->canRetryUpload() && !l.b.room->frame().players.contains("H") &&
        count==l.chunks("H","B",a),"retry bypassed room refusal");
    std::cout<<"PASS partial download resumes at accepted offset, stale window ignored, valid cache reused, denied model stays denied\n";
}
Ms delayedTransfer(bool windowed,std::size_t limit,bool loseOne=false){
    Lab l; auto asset=model('a',(2u<<20)+13);
    RoomRules rules;rules.bytesPerSecond=limit;
    l.host.room->setRules(rules);l.b.room->setRules(rules);
    l.host.owns(asset);l.host.room->publish(asset,state(""));l.join("B");
    struct Delivery {Ms due; Lab::Sample packet;};
    std::deque<Delivery> queue;
    bool sawWindow=false,sawSpeed=false;
    l.deliver=[&](auto& msg){
        if(!windowed && (msg.h.at("t")=="hello" || msg.h.at("t")=="welcome")){
            msg.h.erase("chunk_windows");msg.bytes=packet(msg.h);
        }
        if(msg.h.at("t")=="want" && msg.h.value("count",1)>1)sawWindow=true;
        if(loseOne && msg.h.at("t")=="chunk" && msg.h.at("off")==RoomProtocol::chunkSize){
            loseOne=false;return false;
        }
        queue.push_back({l.now+100,msg});return false;
    };
    auto start=l.now;
    for(unsigned i=0;i<1600 && !l.b.files.has(asset);++i){
        while(!queue.empty() && queue.front().due<=l.now){
            auto& p=queue.front().packet;
            l.peer(p.to).room->receive(p.to=="H"?p.from:"",p.bytes,l.now);queue.pop_front();
        }
        l.step();
        auto progress=l.b.room->progress();
        if(progress.contains(asset.contentKey) && progress.at(asset.contentKey).bytesPerSecond>0)sawSpeed=true;
    }
    auto elapsed=l.now-start;
    check(sawWindow==windowed && sawSpeed && l.b.files.has(asset),"window negotiation, receive rate or delayed transfer failed");
    check((asset.size-16*RoomProtocol::chunkSize)*1000<=limit*elapsed,"sustained download exceeded configured limit");
    l.run(48);check(l.b.room->downloadBytesPerSecond()==0,"idle download meter did not decay");
    return elapsed;
}
void transferWindows(){
    auto legacy=delayedTransfer(false,1u<<20);
    auto windowed=delayedTransfer(true,1u<<20);
    auto capped=delayedTransfer(true,256u<<10);
    auto recovered=delayedTransfer(true,1u<<20,true);
    check(windowed*3<legacy && capped>windowed && recovered>windowed,"window/limit/loss regression");
    std::cout<<"PASS 2 MiB+13 bytes, simulated 200 ms RTT: legacy="<<legacy<<" ms, windowed="<<windowed
             <<" ms, 256 KiB/s cap="<<capped<<" ms, one dropped block="<<recovered<<" ms\n";
}
void peerTransferRates(){
    Lab l;auto file=model('a',3u<<20);
    l.a.owns(file);l.a.room->publish(file,state(""));l.join("A");l.join("B");l.run(12);
    l.host.room->updateRates({{"B",8192}},8192,l.now);l.step();
    auto rates=l.b.room->peerRates();
    check(rates.contains("A") && rates.at("A").upload>0 && !rates.contains("B"),"guest did not see uploader measured by host");
    check(l.a.room->peerRates().at("B").download==8192,"guest did not see other client's file download");
    l.run(73);check(l.b.room->peerRates().empty(),"stale relay rate persisted");
    l.leave("A");l.host.room->updateRates({},0,l.now);l.step();
    check(!l.b.room->peerRates().contains("A"),"disconnected uploader rate persisted");
    std::cout<<"PASS host-measured per-peer rates visible to other guests; stale and disconnected rates removed\n";
}
void pausedTransfer(bool revoke){
    Lab l;
    std::vector<RelayAsset> shared;
    for(char id:{'a','b','c','d'}){
        auto file=model(id,1024*1024);l.host.owns(file);shared.push_back(file);
    }
    auto victim=shared.back();
    l.host.room->share({victim});l.join("B");l.run(15);
    l.host.room->share(shared);l.run(6);
    auto before=l.b.files.partial.at(victim.digest).size();
    check(before>0 && before<victim.size,"preemption setup");
    auto urgent=model('e',300000);l.host.owns(urgent);l.host.room->publish(urgent,state("urgent"));
    for(int i=0;i<30 && l.b.room->statuses().at(victim.contentKey)!="paused";++i)l.step();
    check(l.b.room->statuses().at(victim.contentKey)=="paused","partial file was not paused");
    auto saved=l.b.room->progress().at(victim.contentKey);
    check(saved.received>=before && saved.received==l.b.files.partial.at(victim.digest).size() && saved.total==victim.size,"lost paused bytes");
    auto first=l.trace.size();
    if(revoke){
        RoomRules rules;rules.sessionDenied.insert(victim.contentKey);l.host.room->setRules(rules);l.run(20);
        check(!l.b.files.partial.contains(victim.digest) && !l.b.room->progress().contains(victim.contentKey),"revocation kept paused file");
    }else{
        l.run(800);
        auto request=std::find_if(l.trace.begin()+first,l.trace.end(),[&](auto const& msg){
            return msg.from=="B" && msg.h.at("t")=="want" && msg.h.at("key")==victim.contentKey;
        });
        check(request!=l.trace.end() && request->h.at("off")==saved.received,"resume restarted at wrong offset");
        check(l.b.files.has(victim) && l.b.files.has(urgent),"resumed transfer failed final integrity");
    }
    std::cout<<"PASS preempted file retained "<<saved.received<<" bytes; "<<(revoke?"revocation deleted partial":"resumed from exact offset and verified")<<".\n";
}
void displayReports(){
    Lab l;auto a=model('a');l.a.owns(a);l.a.room->publish(a,state("red"));l.join("A");l.join("B");l.run(40);
    auto streamId=l.b.room->frame().players.at("A").session;
    check(l.host.room->observers().at("B").at("A").stage=="unreported","cache claimed rendering");
    RoomDisplays seen{{"A",{streamId,"preparing"}}};
    l.b.room->reportDisplays(seen);l.run(5);
    check(l.host.room->observers().at("B").at("A").stage=="preparing","no preparation report");
    auto previous=std::find_if(l.trace.rbegin(),l.trace.rend(),[](auto const& e){return e.h.at("t")=="display";})->bytes;
    auto start=l.trace.size();
    for(int i=0;i<8;++i){l.b.room->reportDisplays(seen);l.step();}
    check(std::none_of(l.trace.begin()+start,l.trace.end(),[](auto const& e){return e.h.at("t")=="display";}),"unchanged display stage spammed wire");
    seen["A"].stage="displayed";l.b.room->reportDisplays(seen);l.run(5);
    l.host.room->receive("B",previous,l.now);
    check(l.host.room->observers().at("B").at("A").stage=="displayed","stale receipt overrode displayed");
    l.a.room->publish(a,state("blue"));l.run(5);l.host.room->receive("B",previous,l.now);
    check(l.host.room->observers().at("B").at("A").stage=="unreported","outfit switch kept old receipt");
    l.leave("B");check(!l.host.room->observers().contains("B"),"departed observer survived");
    std::cout<<"PASS authenticated per-observer stages, unchanged-state coalescing, stale receipt and disconnect cleanup.\n";
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
    hostFiresArrows();authoritativeArrows();locomotionInputs();flightAndAuthorFormSnapshots();
    try{distribution();authorization();priorityAndStale();failure();manualRecovery(false);manualRecovery(true);retryAfterAllControlsLost();retryMissingPublication();retryPartialAndCached();pausedTransfer(false);pausedTransfer(true);displayReports();asyncFinish();formatsAndEvents();selectionAcknowledgement();vanillaIdle();transferWindows();peerTransferRates();return 0;}
    catch(std::exception const& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
