// C7 host fixture: actual editor.cpp (including History/Draw), production CORE/physics/server queues,
// actual native-boundary outcomes. No MoveMany/policy mock. All asynchronous barriers are armed before
// triggering work and wait on exact events with bounded timeouts. No sleeps or time-based polling.
#include "../../asi/cdmodkit/editor.cpp"
#include "production_host.h"
#include <thread>
#include <condition_variable>
#include <chrono>
#include <stdexcept>
#include <sstream>
#include <limits>
#include <atomic>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")

namespace {
int assertions = 0, failures = 0;
std::string current, root;
struct Case { std::string id; int assertions = 0, failures = 0; };
std::vector<Case> cases;
std::vector<std::string> timeline, identities, outcomes;
void Check(bool ok, const char* name) {
    ++assertions; ++cases.back().assertions;
    if (!ok) { ++failures; ++cases.back().failures; }
    std::printf("CHECK %s %s/%s\n", ok ? "PASS" : "FAIL", current.c_str(), name);
    core::Log("CHECK %s %s/%s", ok ? "PASS" : "FAIL", current.c_str(), name);
}
void Require(bool ok, const char* name) { Check(ok, name); if (!ok) throw std::runtime_error(name); }
bool Near(float a, float b) { return std::fabs(a - b) < 0.001f; }
std::string V(Vec3 p) { std::ostringstream s; s << '[' << p.x << ',' << p.y << ',' << p.z << ']'; return s.str(); }
void Write(const std::string& name, const std::vector<std::string>& rows) {
    FILE* f = std::fopen((root + "\\" + name).c_str(), "wb");
    if (!f) throw std::runtime_error("receipt open failed");
    std::fputs("[\n", f);
    for (size_t i = 0; i < rows.size(); ++i) std::fprintf(f, "%s%s\n", rows[i].c_str(), i + 1 == rows.size() ? "" : ",");
    std::fputs("]\n", f); std::fclose(f);
}
void Trace(const char* event, const core::GroundHandle& op = {}) {
    std::ostringstream s;
    s << "{\"case\":\"" << current << "\",\"event\":\"" << event << "\",\"serial\":" << editor::g_historySerial
      << ",\"branch\":" << editor::g_historyBranch << ",\"leases\":" << host::GroundLeaseCount()
      << ",\"deferred\":" << host::GroundDeferredCount() << ",\"undo\":[";
    for (size_t i = 0; i < editor::g_undo.size(); ++i) { if (i) s << ','; s << editor::g_undo[i].serial; }
    s << "],\"redo\":[";
    for (size_t i = 0; i < editor::g_redo.size(); ++i) { if (i) s << ','; s << editor::g_redo[i].serial; }
    s << "]}"; timeline.push_back(s.str());
    if (!op) return;
    const auto v = core::GroundStateOf(op);
    std::ostringstream id;
    id << "{\"case\":\"" << current << "\",\"event\":\"" << event << "\",\"request\":" << v.id << ",\"serial\":" << v.serial
       << ",\"branch\":" << v.branch << ",\"epoch\":" << v.epoch << ",\"placement\":" << v.carried.generation
       << ",\"transform\":" << v.carried.transform << ",\"probe\":" << v.probe << ",\"state\":" << v.state << ",\"reason\":\"" << v.reason << "\",\"members\":[";
    for (size_t i = 0; i < v.members.size(); ++i) {
        const auto& m = v.members[i]; if (i) id << ',';
        id << "{\"uid\":" << m.before.uid << ",\"materialization\":" << m.before.gen << ",\"pose\":" << m.before.poseGen << '}';
        std::ostringstream result;
        result << "{\"case\":\"" << current << "\",\"event\":\"" << event << "\",\"request\":" << v.id << ",\"uid\":" << m.before.uid
               << ",\"before\":" << V(m.before.pos) << ",\"after\":" << V(m.after.pos) << ",\"accepted\":" << (m.accepted ? "true" : "false")
               << ",\"terminal\":" << (m.terminal ? "true" : "false") << ",\"hiddenAfter\":" << (m.after.hidden ? "true" : "false")
               << ",\"reason\":\"" << m.reason << "\"}";
        outcomes.push_back(result.str());
    }
    id << "]}"; identities.push_back(id.str());
}

class LockProbe {
    std::mutex m; std::condition_variable cv; bool request = false, done = false, quit = false, free = false;
    std::thread worker;
public:
    LockProbe() : worker([this] {
        for (;;) {
            std::unique_lock<std::mutex> l(m); cv.wait(l, [this] { return request || quit; });
            if (quit) return;
            request = false; free = host::GroundLocksFree(); done = true; l.unlock(); cv.notify_all();
        }
    }) {}
    bool CheckFree() {
        std::unique_lock<std::mutex> l(m); done = false; request = true; cv.notify_all();
        return cv.wait_for(l, std::chrono::seconds(5), [this] { return done; }) && free;
    }
    ~LockProbe() { { std::lock_guard<std::mutex> l(m); quit = true; } cv.notify_all(); worker.join(); }
};
std::atomic<int> gateTimeouts{0};
class Gate {
    std::mutex m; std::condition_variable cv; bool entered = false, released = false;
public:
    void Arrive() {
        std::unique_lock<std::mutex> l(m); entered = true; cv.notify_all();
        if (!cv.wait_for(l, std::chrono::seconds(5), [this] { return released; })) ++gateTimeouts;
    }
    bool Entered() { std::unique_lock<std::mutex> l(m); return cv.wait_for(l, std::chrono::seconds(5), [this] { return entered; }); }
    void Release() { { std::lock_guard<std::mutex> l(m); released = true; } cv.notify_all(); }
};
struct Object { Vec3 pos; Rot rot; float scale; bool live = true; uintptr_t actor = 0; };
std::map<uintptr_t, Object> objects;
uintptr_t nextHandle = 100;
std::set<uintptr_t> refuseMove;
uintptr_t faultMove = 0;
int createCount = 0, failCreateAt = -1, movementCount = 0, teleportCount = 0;
bool noSurface = false, insideSurface = false, teleportSuccess = true, allLocksFree = true;
bool selfBody = false; float selfLow = 0, selfHigh = 0;   // optional own-collider simulation: a contact on/inside [selfLow,selfHigh] from above, the ground below it
std::function<float(float)> height = [](float) { return 0.0f; };
Gate* gate = nullptr;
Gate* castGate = nullptr;
int castCount = 0;
LockProbe* probe = nullptr;
void EngineBoundary() { allLocksFree &= probe->CheckFree(); }
uintptr_t Create(Vec3 p, Rot r, float sc) {
    EngineBoundary(); if (++createCount == failCreateAt) return 0;
    const uintptr_t h = nextHandle++; objects.emplace(h, Object{p,r,sc,true,0}); return h;
}
void Install() {
    auto& e = host::Seam(); e.ready = e.probeReady = true;
    e.createGeneric = [](const std::string&, Vec3 p, Rot r, float sc) { return Create(p,r,sc); };
    e.remove = [](uintptr_t h) { EngineBoundary(); auto it=objects.find(h); if(it==objects.end()||!it->second.live)return false; it->second.live=false; return true; };
    e.moveInPlace = [](uintptr_t h, Vec3 p, Rot r, float sc) {
        EngineBoundary(); if(gate)gate->Arrive(); ++movementCount;
        if(h==faultMove)RaiseException(EXCEPTION_ACCESS_VIOLATION,0,0,nullptr);
        auto it=objects.find(h); if(it==objects.end()||!it->second.live||refuseMove.count(h))return false;
        it->second.pos=p;it->second.rot=r;it->second.scale=sc;return true;
    };
    e.liveMove=e.moveInPlace;
    e.groundCast=[](Vec3 p,float length,core::GroundHit* hit) {
        EngineBoundary(); if(castGate)castGate->Arrive();++castCount; hit->done=true;hit->hit=!noSurface;hit->centerY=height(p.x);
        if(selfBody&&p.y>selfLow) { hit->hit=true;hit->centerY=selfHigh;hit->fraction=(p.y-selfHigh)/length;return true; }   // closest hit: the body's top from above, penetration inside it
        hit->fraction=insideSurface?0:(p.y-hit->centerY)/length;return true;
    };
    e.teleport=[](Vec3) { EngineBoundary(); ++teleportCount; return teleportSuccess; };
    e.directGimmick=[](const std::string&,Vec3 p,Rot r,float sc,uintptr_t* so,uintptr_t* actor) {
        *so=Create(p,r,sc);if(!*so)return false;*actor=*so+100000;objects[*so].actor=*actor;return true;
    };
    e.removeActor=[](uintptr_t actor) { EngineBoundary();for(auto& o:objects)if(o.second.actor==actor&&o.second.live){o.second.live=false;return true;}return false; };
    e.templateReady=[] {return false;};
    host::SetDirectGimmick(true);
    std::vector<core::PrefabInfo> index;
    for(const char* name:{"/object/ground.prefab","/object/other.prefab","/object/cd_gimmick/ground.prefab"}) {
        core::PrefabInfo p;p.path=name;p.hasCenter=true;p.sx=p.sy=p.sz=2;index.push_back(p);
    }
    host::SetPrefabIndex(index);
}
void DrainGame() { for(int i=0;host::PumpGame();++i)if(i>4096)throw std::runtime_error("CORE queue did not drain"); }
void Drain() { DrainGame(); for(int i=0;i<5;++i){host::PumpServer();DrainGame();} }
SpawnedObj Rec(int uid) { for(const auto& o:core::Spawned())if(o.uid==uid)return o;throw std::runtime_error("missing uid"); }
float ActualY(int uid) { return objects.at(Rec(uid).obj).pos.y; }
int Spawn(float x=0,float y=10,const char* prefab="/object/ground.prefab",Rot rot={}) {
    int uid=core::SpawnAt(prefab,{x,y,0},rot);Drain();return uid;
}
void Select(const std::vector<int>& uids) { editor::host_seam::ClearSelection();for(int uid:uids)editor::host_seam::SelectAdd(uid); }
void Reset() {
    gate=castGate=nullptr;faultMove=0;refuseMove.clear();noSurface=insideSurface=false;failCreateAt=-1;teleportSuccess=true;
    selfBody=false;selfLow=selfHigh=0;
    editor::host_seam::SetTickNow(0);
    { ImGuiIO& io=ImGui::GetIO(); for(int i=0;i<5;++i) { io.MouseDown[i]=false; io.MouseClicked[i]=false; io.MouseReleased[i]=false; io.MouseDownDuration[i]=-1.0f; io.MouseDownDurationPrev[i]=-1.0f; } io.MousePos={-100,-100}; }
    host::Seam().templateReady=[] {return false;};host::Seam().replay={};host::SetDirectGimmick(true);
    editor::g_selPrefab=-1;editor::g_previewShown=false;core::PreviewClear();
    height=[](float){return 0.0f;};host::Seam().probeReady=true;core::g_recreateOnMove=false;
    editor::ClearSceneAction(false);Drain();editor::PumpSnapJobs();editor::host_seam::ResetPlacement();
    editor::host_seam::ResetHistory();editor::g_numericSceneUid=0;objects.clear();
    Require(host::GroundLeaseCount()==0&&host::GroundDeferredCount()==0,"reset-clean-leases");
}
core::GroundHandle Begin(const std::vector<int>& uids,bool rigid=true,bool carried=false) {
    Select(uids);
    auto v=editor::BeginGrounding(uids,rigid,carried?editor::GroundPlacementOf(editor::g_place):core::GroundPlacement{});
    Require(!v.empty(),"request-admitted");Trace("reserved",v.front());return v.front();
}
void Queue(bool reverse=false) {
    host::PumpPhysics(reverse);DrainGame();editor::PumpSnapJobs();
}
void Reconcile() { editor::PumpGroundHistory(); }
void DrawFrame() {
    ImGui::NewFrame();editor::Draw();ImGui::Render();
}
void Reason(const core::GroundHandle& op,const char* reason) { Check(core::GroundStateOf(op).reason==reason,reason);Trace("terminal",op); }
void NoLeaks() { Drain();editor::PumpSnapJobs();Check(host::GroundLeaseCount()==0&&host::GroundDeferredCount()==0,"lease-cleanup");Check(host::GroundTicketCount()==0,"ticket-cleanup"); }

void Happy(bool rigid,bool reverse) {
    Reset();int a=Spawn(0,10),b=Spawn(20,15);
    height=[](float x){return x>10?3.0f:0.0f;};
    auto op=Begin({a,b},rigid);auto batch=editor::g_pendingGround;
    const auto first=core::GroundStateOf(op);const uint64_t serial=first.serial,branch=first.branch;
    Check(editor::g_undo.empty(),"reservation-not-history");Queue(reverse);
    Check(core::GroundStateOf(op).state==core::GroundQueued,"queued-not-applied");
    if(!rigid) {
        Require(host::PumpGameAt(reverse?1:0),"release-first-application");Reconcile();
        Check(editor::g_undo.empty(),"batch-waits-for-all");
    }
    Drain();Reconcile();
    if(rigid) { Check(Near(ActualY(a),1)&&Near(ActualY(b),6),"rigid-shared-Y"); }
    else { Check(Near(ActualY(a),1)&&Near(ActualY(b),4),"per-object-independent-Y"); }
    Check(editor::g_undo.size()==(rigid?1u:2u),"successful-acts-only");
    Check(editor::g_undo.front().serial==serial,"reserved-serial-retained");
    Check(editor::g_undo.front().branch==branch,"originating-branch-retained");
    Check(editor::g_historyBranch>branch,"branch-after-batch");
    if(!rigid)Check(editor::g_undo[0].serial<editor::g_undo[1].serial,"original-order-not-completion-order");
    for(auto& h:batch)Reason(h,"applied");
    editor::Undo();Drain();editor::Redo();Drain();
    Check(Near(ActualY(a),1)&&Near(ActualY(b),rigid?6.0f:4.0f),"undo-redo-final-poses");NoLeaks();
}
void Carried() {
    Reset();int a=Spawn(),b=Spawn(4,12);Select({a,b});editor::StartGrab({a,b},false,"carried");
    const auto before=editor::g_place;auto op=Begin({a,b},true,true);Queue();Drain();
    Check(Near(editor::g_place.center.y,before.center.y),"carried-not-updated-by-engine-callback");
    Reconcile();Check(Near(editor::g_place.center.y,before.center.y-9),"carried-center-reconciled");
    std::vector<core::MoveReq> poses;editor::MembersTo(poses,editor::g_place,editor::g_place.center,editor::g_place.yaw,editor::g_place.scale);
    Check(poses.size()==2&&Near(poses[0].pos.y,ActualY(a))&&Near(poses[1].pos.y,ActualY(b)),"carried-member-poses-valid");
    Check(editor::g_undo.size()==1,"carried-one-ground-history");editor::DropCarried();Drain();
    Check(editor::g_undo.size()==1,"drop-does-not-record-ground-twice");Reason(op,"applied");NoLeaks();
}
void Partial(bool recreate) {
    Reset();int a=Spawn(),b=Spawn(4,12);const auto beforeA=Rec(a),beforeB=Rec(b);
    core::g_recreateOnMove=recreate;
    if(recreate)failCreateAt=createCount+2;else refuseMove.insert(beforeB.obj);
    auto op=Begin({a,b});Queue();Drain();Reconcile();const auto v=core::GroundStateOf(op);
    Check(v.members[0].accepted&&!v.members[1].accepted,"actual-member-outcomes");
    Check(Near(ActualY(a),1)&&Near(Rec(b).pos.y,12),"no-refused-success-pose");
    Check(Rec(b).hidden==recreate,"honest-recreate-failure-visibility");
    Check(editor::g_undo.size()==1&&editor::g_undo[0].acts.size()==1&&editor::g_undo[0].acts[0].uid==a,"only-accepted-member-act");
    Check(v.members[0].before.gen==beforeA.gen&&v.members[1].before.gen==beforeB.gen,"before-identities-retained");
    Reason(op,"partial");NoLeaks();
}
void StaleTeleport(bool queued,bool partialWrite) {
    Reset();int a=Spawn();auto op=Begin({a});
    if(queued)Queue();else {host::PumpPhysics();DrainGame();}
    const int calls=movementCount;teleportSuccess=!partialWrite;
    Check(core::SetPlayerPos({100,20,100})==!partialWrite,"synchronous-teleport-result");
    editor::PumpSnapJobs();Drain();Reconcile();
    Check(Near(ActualY(a),10)&&movementCount==calls,"no-stale-teleport-movement");Check(editor::g_undo.empty(),"no-stale-history");
    Reason(op,"epoch-invalidated");NoLeaks();
}
void HideRestore() {
    Reset();int a=Spawn();const auto old=Rec(a);auto op=Begin({a});Queue();
    core::HideUid(a);core::RestoreUid(a);Drain();Reconcile();
    Check(Rec(a).gen!=old.gen&&Rec(a).obj!=old.obj,"fresh-materialization");
    Check(Near(ActualY(a),10)&&editor::g_undo.empty(),"restored-target-never-snapped");Reason(op,"epoch-invalidated");NoLeaks();
}
void CarriedReplacement() {
    Reset();int a=Spawn(),b=Spawn(4);editor::StartGrab({a},false,"old");auto op=Begin({a},true,true);Queue();
    editor::StartGrab({b},false,"replacement");Drain();Reconcile();
    Check(editor::g_place.m.size()==1&&editor::g_place.m[0].uid==b&&Near(ActualY(a),10),"replacement-not-moved");
    Reason(op,"canceled");NoLeaks();
}
void OrderedMembers() {
    Reset();int a=Spawn(),b=Spawn(4);editor::StartGrab({a,b},false,"ordered");auto op=Begin({a,b},true,true);Queue();
    auto context=editor::GroundPlacementOf(editor::g_place);std::reverse(context.members.begin(),context.members.end());
    Require(core::PublishGroundPlacement(context),"publish-reordered-members");Drain();Reconcile();
    Check(Near(ActualY(a),10)&&Near(ActualY(b),10),"exact-ordered-members-required");Reason(op,"epoch-invalidated");NoLeaks();
}
void InterveningLate() {
    Reset();int a=Spawn();auto op=Begin({a});Queue();Drain();const auto v=core::GroundStateOf(op);
    Check(v.state==core::GroundSettled&&editor::g_undo.empty(),"actual-result-before-ui");Trace("notification-withheld",op);
    editor::ApplyNumeric(a,{5,7,0},{},1,true);Drain();
    Check(editor::g_undo.size()==2&&editor::g_undo[0].serial==v.serial,"ground-before-intervening-edit");
    editor::Undo();Drain();Check(Near(ActualY(a),1)&&Near(Rec(a).pos.x,0),"undo-intervening-not-ground");
    const auto redo=editor::g_redo.back().serial;const auto size=editor::g_undo.size();
    Reconcile();editor::PumpSnapJobs();Check(editor::g_redo.size()==1&&editor::g_redo.back().serial==redo&&editor::g_undo.size()==size,"late-ack-no-append-or-redo-clear");
    editor::Undo();Drain();Check(Near(ActualY(a),10),"undo-ground-original-before");Reason(op,"applied");NoLeaks();
}
void FailedRedo(bool engineFailure) {
    Reset();int a=Spawn();Select({a});editor::ApplyNumeric(a,{0,12,0},{},1,true);Drain();editor::Undo();Drain();
    const uint64_t redo=editor::g_redo.back().serial,serial=editor::g_historySerial;
    if(engineFailure)refuseMove.insert(Rec(a).obj);else noSurface=true;
    auto op=Begin({a});Check(editor::g_redo.size()==1,"reservation-preserves-redo");Queue();Drain();Reconcile();
    Check(editor::g_redo.size()==1&&editor::g_redo.back().serial==redo&&editor::g_undo.empty(),"failure-preserves-redo");
    Check(editor::g_historySerial>serial&&Near(ActualY(a),10),"failed-reservation-no-effect");Reason(op,engineFailure?"move-refused":"no-surface");NoLeaks();
}
void ApplyingCancel() {
    Reset();int a=Spawn();auto op=Begin({a});Queue();Gate barrier;gate=&barrier;
    std::thread worker([] {host::PumpGame();}); // barrier already armed
    const bool entered=barrier.Entered();Check(entered,"applying-event-reached");
    if(entered) {
        Check(core::GroundStateOf(op).state==core::GroundApplying&&host::GroundLeaseCount()==1,"lease-at-engine-boundary");
        Check(Near(Rec(a).pos.y,10)&&Near(ActualY(a),10),"applying-does-not-publish-unaccepted-pose");
        core::GroundCancel(op);const int teleports=teleportCount;
        Check(!core::SetPlayerPos({100,0,0})&&teleportCount==teleports,"applying-teleport-busy-no-hidden-write");
        for(int i=0;i<300;++i)core::GroundFrame();
        Check(core::GroundStateOf(op).state==core::GroundApplying,"expiry-never-falsely-cancels-applying");
        editor::Undo();Check(bool(editor::g_deferredEditor.action),"one-copied-deferred-undo");
        editor::RotateSel(90);Check(editor::g_undo.empty(),"second-mutation-rejected");
        DrawFrame();Check(bool(editor::g_deferredEditor.action),"draw-remains-usable-while-applying");Trace("applying-cancel",op);
    }
    barrier.Release();worker.join();gate=nullptr;
    Check(core::GroundStateOf(op).state==core::GroundSettled,"terminal-published-before-draw");
    DrawFrame();Drain();Check(!editor::g_deferredEditor.action&&editor::g_undo.empty()&&editor::g_redo.size()==1,"draw-resumes-undo-after-reconcile");
    Check(Near(ActualY(a),10),"deferred-undo-actual-pose");Reason(op,"applied");NoLeaks();
}
void CoreConflict(const std::string& kind) {
    Reset();int a=Spawn();auto op=Begin({a});Queue();const auto before=Rec(a);Gate barrier;gate=&barrier;
    std::thread worker([]{host::PumpGame();});bool entered=barrier.Entered();Check(entered,"lease-entered");
    if(entered) {
        if(kind=="move")core::MoveMany({{a,{0,99,0},{},1}},true);
        else if(kind=="hide-restore"){core::HideUid(a);core::RestoreUid(a);}
        else if(kind=="forget")core::ForgetUid(a);
        else { Check(!core::ClearScene(),"busy-clear-explicitly-refused"); core::DeleteAllSpawned(); }
        const auto now=Rec(a);Check(now.gen==before.gen&&!now.hidden&&
            (kind=="delete-all" ? host::GroundDeferredCount()==0 : host::GroundDeferredCount()>0),"core-mutation-does-not-change-leased-identity");
    }
    barrier.Release();worker.join();gate=nullptr;Drain();Reconcile();
    if(kind=="move")Check(Near(ActualY(a),99),"deferred-core-move-after-ground");
    else if(kind=="hide-restore")Check(Near(ActualY(a),1)&&Rec(a).gen!=before.gen,"deferred-hide-restore-after-ground");
    else if(kind=="delete-all") {
        Check(core::IndexOfUid(a)>=0&&Near(ActualY(a),1),"busy-clear-never-resumes-implicitly");
        Check(core::ClearScene()&&core::IndexOfUid(a)<0,"explicit-clear-retry-after-ground");
    } else Check(core::IndexOfUid(a)<0,"deferred-core-removal-after-ground");
    Check(core::GroundStateOf(op).members[0].accepted,"core-conflict-does-not-erase-actual-success");Reason(op,"applied");NoLeaks();
}
void Timeout(int mode) {
    Reset();int a=Spawn();if(mode==0)host::Seam().probeReady=false;auto op=Begin({a});
    if(mode==2)Queue();
    const int budget=mode==2?240:120;const int elapsed=mode==2?1:0;
    for(int i=elapsed;i<budget-1;++i)core::GroundFrame();
    Check(!core::GroundStateOf(op).terminal(),"one-frame-before-budget-pending");core::GroundFrame();
    Reason(op,mode==0?"ready-timeout":mode==1?"ticket-timeout":"result-timeout");
    Drain();editor::PumpSnapJobs();Check(Near(ActualY(a),10)&&editor::g_undo.empty(),"expired-work-never-applies");NoLeaks();
}
void EpochAndPose(bool world) {
    Reset();int a=Spawn(),b=Spawn(4);auto op=Begin({a,b});Queue();const auto gen=Rec(b).gen,pose=Rec(b).poseGen;
    if(world)core::InvalidateGroundWorld();else core::MoveMany({{b,{4,20,0},{},1}},false);
    Drain();Reconcile();Check(Near(ActualY(a),10),"rigid-stale-member-admits-none");
    if(!world)Check(Rec(b).gen==gen&&Rec(b).poseGen>pose&&Near(ActualY(b),20),"pose-generation-separate-from-materialization");
    Reason(op,"epoch-invalidated");NoLeaks();
}
void Server() {
    Reset();int a=Spawn(0,10,"/object/cd_gimmick/ground.prefab");auto op=Begin({a});Queue();DrainGame();
    Check(core::GroundStateOf(op).state==core::GroundApplying&&host::GroundLeaseCount()==1,"lease-crosses-server-queue");
    core::GroundCancel(op);for(int i=0;i<300;++i)core::GroundFrame();
    host::PumpServer();DrainGame();Reconcile();Check(Near(ActualY(a),1),"actual-server-final-pose");Reason(op,"applied");NoLeaks();
}
void Bounds() {
    Reset();int a=Spawn(0,10,"/object/ground.prefab",Rot{25,35,45});auto op=Begin({a});proj_codec::Bounds bounds;
    Require(core::GroundBounds(op,bounds),"rotated-bounds-valid");Check(bounds.max.y-bounds.min.y>2.1,"all-eight-rotated-corners");
    Queue();Drain();Reconcile();Check(Near(ActualY(a),10-(float)bounds.min.y),"rotated-bottom-grounded");Reason(op,"applied");NoLeaks();
}
void MutationBarrier(const std::string& action) {
    Reset();int a=Spawn();Select({a});
    const bool projectAction=action=="new"||action=="delete-all"||action=="load"||action=="replace";
    const auto selection=editor::g_sel; const std::string projectName=editor::g_projName;
    if(action=="drop"||action=="cancel")editor::StartGrab({a},false,"barrier");
    if(action=="load"||action=="replace")Require(core::SaveProject("ground-barrier"),"saved-load-fixture");
    if(action=="preview") {core::PreviewSet("/object/ground.prefab",{20,10,0},0,1);Drain();editor::g_selPrefab=0;editor::g_previewShown=true;}
    auto op=Begin({a},true,editor::g_place.active);Queue();Gate barrier;gate=&barrier;
    std::thread worker([]{host::PumpGame();});bool entered=barrier.Entered();Check(entered,"mutation-barrier-entered");
    if(entered) {
        if(action=="spawn")editor::host_seam::SpawnRecorded("/object/ground.prefab",{20,10,0},{},1,0,0);
        else if(action=="grab")editor::StartGrab({a},false,"deferred");
        else if(action=="drop")editor::DropCarried();
        else if(action=="cancel")editor::CancelCarried();
        else if(action=="delete")editor::DeleteSel();
        else if(action=="forget")editor::ForgetSelection();
        else if(action=="group")editor::GroupSel(true);
        else if(action=="rotate")editor::RotateSel(45);
        else if(action=="align")editor::AlignSel(0);
        else if(action=="numeric"||action=="numeric-live")editor::ApplyNumeric(a,{0,7,0},{},1,action=="numeric");
        else if(action=="preview")editor::SpawnSelected({});
        else if(action=="load"||action=="replace")editor::LoadProjectAction("ground-barrier",action=="replace");
        else if(action=="paste"){editor::CopySel();editor::Paste(true);}
        else if(action=="redo")editor::Redo();
        else if(action=="clear")editor::host_seam::ResetHistory();
        else editor::ClearSceneAction(action=="new");
        if(projectAction) {
            Check(!editor::g_deferredEditor.action&&!core::ProjectError().empty(),"busy-project-refused-without-deferred-closure");
            Check(editor::g_sel==selection&&std::string(editor::g_projName)==projectName,"busy-project-preserves-selection-and-name");
        } else Check(bool(editor::g_deferredEditor.action),"mutation-copied-not-run");
        Check(core::Spawned().size()==1&&editor::g_undo.empty(),"scene-and-history-wait-for-terminal");
    }
    barrier.Release();worker.join();gate=nullptr;DrawFrame();Drain();
    Check(!editor::g_deferredEditor.action&&core::GroundStateOf(op).state==core::GroundReconciled,"exact-terminal-reconciles-ground");
    if(projectAction) {
        Check(core::IndexOfUid(a)>=0&&Near(ActualY(a),1)&&editor::g_undo.size()==1,"ground-settlement-does-not-run-refused-project-action");
        if(action=="load"||action=="replace") Check(editor::LoadProjectAction("ground-barrier",action=="replace"),"explicit-project-retry-succeeds");
        else editor::ClearSceneAction(action=="new");
        Check(editor::g_undo.empty()&&editor::g_sel.empty(),"successful-project-retry-cleans-history-and-selection");
    }
    Reason(op,"applied");NoLeaks();
}
void StaleIntent() {
    Reset();int a=Spawn(),b=Spawn(4);auto op=Begin({a});Queue();Gate barrier;gate=&barrier;
    std::thread worker([]{host::PumpGame();});bool entered=barrier.Entered();Check(entered,"stale-intent-entered");
    if(entered){editor::DeleteSel();Select({b});}
    barrier.Release();worker.join();gate=nullptr;DrawFrame();Drain();
    Check(!Rec(a).hidden&&!Rec(b).hidden&&!editor::g_deferredEditor.action,"stale-selection-intent-canceled");Reason(op,"applied");NoLeaks();
}
void ProbeInFlight() {
    Reset();int a=Spawn();auto op=Begin({a});Gate barrier;castGate=&barrier;
    std::thread worker([] {host::PumpPhysics();});bool entered=barrier.Entered();Check(entered,"physics-event-entered");
    if(entered)Check(core::SetPlayerPos({50,0,0}),"teleport-during-probe");
    barrier.Release();worker.join();castGate=nullptr;Drain();editor::PumpSnapJobs();
    Check(Near(ActualY(a),10)&&editor::g_undo.empty(),"in-flight-probe-cannot-publish-stale-move");Reason(op,"epoch-invalidated");NoLeaks();
}
void Attempts() {
    Reset();int a=Spawn();insideSurface=true;const int before=castCount;auto op=Begin({a});
    for(int i=0;i<40;++i)Queue();
    Check(castCount-before==40*5,"exactly-forty-GroundStep-attempts-with-five-native-samples");Reason(op,"no-surface");NoLeaks();
}
void Overlap(bool applying) {
    Reset();int a=Spawn();auto first=Begin({a});Queue();core::GroundHandle second;
    if(applying) {
        Gate barrier;gate=&barrier;std::thread worker([]{host::PumpGame();});bool entered=barrier.Entered();Check(entered,"overlap-applying-entered");
        if(entered){auto h=editor::BeginGrounding({a},true);Check(h.empty()&&bool(editor::g_deferredEditor.action),"overlap-waits-for-applying");}
        barrier.Release();worker.join();gate=nullptr;DrawFrame();
        Require(!editor::g_pendingGround.empty(),"overlap-resumed-new-request");second=editor::g_pendingGround.back();
        Check(Near(core::GroundStateOf(second).members[0].before.pos.y,1),"fresh-before-after-applying");
        height=[](float){return -2.0f;}; // a distinct surface below the already-grounded object's own box
    } else {
        second=Begin({a});Check(core::GroundStateOf(first).reason=="canceled","overlap-cancels-unapplied");
        Check(Near(core::GroundStateOf(second).members[0].before.pos.y,10),"unapplied-overlap-fresh-before");
    }
    Queue();Drain();Reconcile();Check(Near(ActualY(a),applying?-1.0f:1.0f),"overlap-final-pose");NoLeaks();Trace("overlap-first",first);Trace("overlap-second",second);
}
void ApplyOnce() {
    Reset();int a=Spawn();auto op=Begin({a});Queue();const auto v=core::GroundStateOf(op);
    Check(v.probe>0,"probe-identity-recorded");
    Check(!core::GroundApply(op,{v.members[0].requested}),"cannot-requeue-request");const int before=movementCount;
    Drain();Reconcile();auto settled=core::GroundStateOf(op);core::GroundCancel(op);core::InvalidateGroundWorld();Reconcile();
    auto after=core::GroundStateOf(op);
    Check(movementCount==before+1&&after.state==core::GroundReconciled&&after.reason==settled.reason&&after.serial==settled.serial&&after.probe==settled.probe,"immutable-terminal-no-second-application");NoLeaks();
}
void CarriedAfterConflict() {
    Reset();int a=Spawn();editor::StartGrab({a},false,"carried-conflict");auto op=Begin({a},true,true);Queue();
    Gate barrier;gate=&barrier;std::thread worker([]{host::PumpGame();});bool entered=barrier.Entered();Check(entered,"carried-conflict-entered");
    if(entered){core::MoveMany({{a,{0,70,0},{},1}},true);auto context=editor::GroundPlacementOf(editor::g_place);++context.transform;Check(!core::PublishGroundPlacement(context),"applying-carried-update-refused");}
    barrier.Release();worker.join();gate=nullptr;Drain();Reconcile();
    Check(!editor::g_place.active&&Near(ActualY(a),70),"no-stale-carried-state-after-core-mutation");Reason(op,"applied");NoLeaks();
}
void CarriedTransform() {
    Reset();int a=Spawn();editor::StartGrab({a},false,"carried-transform");auto op=Begin({a},true,true);Queue();
    auto context=editor::GroundPlacementOf(editor::g_place);++context.transform;core::PublishGroundPlacement(context);Drain();Reconcile();
    Check(Near(ActualY(a),10),"final-apply-checks-carried-transform");Reason(op,"epoch-invalidated");NoLeaks();
}
void ServerFailure() {
    Reset();int a=Spawn(0,10,"/object/cd_gimmick/ground.prefab");host::SetDirectGimmick(false);
    host::Seam().templateReady=[] {return true;};host::Seam().replay=[](int,const std::string&,Vec3,Rot,float,uintptr_t*,uintptr_t*){EngineBoundary();return false;};
    failCreateAt=createCount+1;auto op=Begin({a});Queue();Drain();Reconcile();
    Check(core::GroundStateOf(op).terminal()&&Rec(a).hidden&&Near(Rec(a).pos.y,10),"server-refusal-honest-terminal");
    Check(editor::g_undo.empty(),"server-failure-no-Act");Reason(op,"move-refused");NoLeaks();
}
void InvalidDestination() {
    Reset();int a=Spawn();auto op=Begin({a});const auto before=core::GroundStateOf(op).members[0].before;
    Check(!core::GroundApply(op,{{a,{0,std::numeric_limits<float>::infinity(),0},before.rot,before.scale}}),"nonfinite-destination-refused");
    Drain();Reconcile();Check(Near(ActualY(a),10)&&editor::g_undo.empty(),"invalid-destination-no-effect");Reason(op,"refused");NoLeaks();
}
void AppliedBeforeTeleport() {
    Reset();int a=Spawn();auto op=Begin({a});Queue();Drain();
    Check(core::SetPlayerPos({100,0,0}),"teleport-after-lease-release");Reconcile();
    Check(Near(ActualY(a),1)&&editor::g_undo.size()==1,"applied-before-invalidation-retained");Reason(op,"applied");NoLeaks();
}
void ImportedBounds() {
    Reset();proj_codec::Document doc;std::string error;
    const std::string text="# cdproj v3 kind=group\n# wb-document anchor=10,1,20 min=0,1,10 max=20,21,30 quality=approx\n# wb-envelope id=1 anchor=10,1,20 min=0,1,10 max=20,21,30 quality=approx\n# wb-member record=1 envelope=1\n/object/ground.prefab|12|4|25|30|1|42|11|-7\n";
    Require(proj_codec::Parse(text,"ground.cdgroup",proj_codec::Kind::Group,doc,error),"saved-bounds-fixture");
    Require(editor::PlaceGroupCopy(doc,{30,10,40},0,1,"saved"),"production-group-grab");Drain();
    int uid=editor::g_place.m[0].uid;auto op=Begin({uid},true,true);proj_codec::Bounds bounds;
    Require(core::GroundBounds(op,bounds),"import-bounds-valid");Check(bounds.approximate&&Near((float)bounds.min.y,10),"saved-bounds-not-receiver-box");
    Queue();Drain();Reconcile();Check(Near(ActualY(uid),3)&&Near(editor::g_place.center.y,0),"saved-anchor-and-member-shift-together");
    editor::DropCarried();Drain();
    Require(core::SaveProject("ground-import"),"save-grounded-copy");
    core::ExportContext context;context.selection={uid};context.name="ground-import";core::PublishExportContext(context);
    core::GroupExportApproval approval;Require(core::PrepareGroupExport({uid},"ground-import",false,approval),"grounded-copy-export-preflight");
    Check(Near((float)approval.document.bounds.min.y,0)&&Near((float)approval.document.bounds.anchor.y,0),"persisted-ground-bounds-valid");Reason(op,"applied");NoLeaks();
}
void CompareOrders() {
    std::vector<float> forward;
    for(bool reverse:{false,true}) {
        Reset();int a=Spawn(),b=Spawn(20,15);height=[](float x){return x>10?3.0f:0.0f;};
        const auto serial=editor::g_historySerial,branch=editor::g_historyBranch;auto op=Begin({a,b},false);Queue(reverse);
        host::PumpGameAt(reverse?1:0);Reconcile();Check(editor::g_undo.empty(),"first-notification-does-not-order-History");
        Trace(reverse?"reverse-first":"forward-first",op);Drain();Reconcile();
        Require(editor::g_undo.size()==2,"two-reserved-entries");
        std::vector<float> observed{ActualY(a),ActualY(b),(float)(editor::g_undo[0].serial-serial),(float)(editor::g_undo[1].serial-serial),
            (float)(editor::g_undo[0].branch-branch),(float)(editor::g_undo[1].branch-branch),(float)editor::g_redo.size(),(float)host::GroundLeaseCount()};
        if(!reverse)forward=observed;else Check(observed==forward,"both-release-orders-identical-poses-serial-branch-redo-leases");
        Reason(op,"applied");NoLeaks();
    }
}
void FaultOutcome() {
    Reset();int a=Spawn(),b=Spawn(4);faultMove=Rec(b).obj;auto op=Begin({a,b});Queue();Drain();Reconcile();
    Check(Near(ActualY(a),1)&&Near(ActualY(b),10),"engine-fault-honest-partial-pose");
    Check(editor::g_undo.size()==1&&editor::g_undo[0].acts.size()==1,"faulted-member-no-success-act");Reason(op,"partial");NoLeaks();
}
void CarriedPreEdit() {
    Reset();int a=Spawn(),b=Spawn(4,12);editor::StartGrab({a,b},false,"edited-carry");
    auto candidate=editor::g_place;candidate.center.y+=10;
    Require(editor::ApplyCarriedPose(candidate,true),"carried-live-edit-admitted");Drain();
    auto op=Begin({a,b},true,true);Queue();Drain();Reconcile();editor::DropCarried();Drain();
    Check(editor::g_undo.size()==2,"pre-ground-carried-edit-before-ground-History");
    editor::Undo();Drain();Check(Near(ActualY(a),20)&&Near(ActualY(b),22),"undo-ground-restores-carried-before");
    editor::Undo();Drain();Check(Near(ActualY(a),10)&&Near(ActualY(b),12),"undo-prior-carry-restores-grab-start");
    Reason(op,"applied");NoLeaks();
    Reset();a=Spawn();editor::StartGrab({a},false,"cancel-ground");op=Begin({a},true,true);Queue();Drain();Reconcile();
    editor::CancelCarried();Drain();Check(Near(ActualY(a),10),"cancel-retains-upstream-grab-origin");NoLeaks();
}
void NumericPreEdit() {
    Reset();int a=Spawn();Select({a});
    // Scene's real widgets publish these values before invoking its live-apply callback.
    editor::g_edit[0]=0;editor::g_edit[1]=20;editor::g_edit[2]=0;editor::g_editRot={};editor::g_editScale=1;
    editor::SceneNumericApply(a,{0,20,0},{},1,false);Drain();
    auto op=Begin({a});Queue();Drain();Reconcile();
    Check(editor::g_undo.size()==2,"numeric-edit-checkpoint-before-ground");
    editor::Undo();Drain();Check(Near(ActualY(a),20),"numeric-undo-ground-before");
    editor::Undo();Drain();Check(Near(ActualY(a),10),"numeric-undo-prior-edit");Reason(op,"applied");NoLeaks();
}
// R2: the production Scene numeric surface (SceneNumericApply), never the helper. A final edit issued
// while grounding is Applying must not record History off the pre-ground pose: after the batch, Undo
// must return the grounding pose, never the pre-ground pose.
void NumericApplyingUndo() {
    Reset();int a=Spawn();Select({a});
    editor::g_editPos0={0,10,0};editor::g_editRot0={};editor::g_editScale0=1;editor::g_numericSceneUid=0;
    auto op=Begin({a});Queue();Gate barrier;gate=&barrier;
    std::thread worker([]{host::PumpGame();});bool entered=barrier.Entered();Check(entered,"numeric-applying-entered");
    if(entered) {
        editor::SceneNumericApply(a,{0,7,0},{},1,true);
        Check(editor::g_undo.empty(),"applying-scene-edit-defers-history");
        Check(bool(editor::g_deferredEditor.action),"scene-edit-intent-copied");
    }
    barrier.Release();worker.join();gate=nullptr;Drain();Reconcile();Reason(op,"applied");
    Check(editor::g_undo.size()==2,"ground-then-numeric-entries");
    editor::Undo();Drain();
    Check(Near(ActualY(a),1),"undo-scene-edit-returns-ground-pose");NoLeaks();
}
// R2: the production gizmo live-send surface (PlaceCommitLive). A drag published while carried grounding
// is Applying must not reach carried authority yet, and the ground receipt must not erase it: the
// still-held drag re-applies onto the post-ground pose instead.
void CarriedDragPreserved() {
    Reset();int a=Spawn();Select({a});editor::StartGrab({a},false,"drag-carry");
    auto op=Begin({a},true,true);Queue();Gate barrier;gate=&barrier;
    std::thread worker([]{host::PumpGame();});bool entered=barrier.Entered();Check(entered,"drag-applying-entered");
    if(entered) {
        auto drag=editor::g_place;drag.center.x+=5;
        editor::PlaceCommitLive(drag,false);
        Check(Near(editor::g_place.center.x,0),"applying-drag-not-yet-authority");
    }
    barrier.Release();worker.join();gate=nullptr;Drain();Reconcile();Reason(op,"applied");
    Check(Near(editor::g_place.center.x,0),"receipt-does-not-invent-drag");
    auto held=editor::g_place;held.center.x+=5;
    Require(editor::PlaceCommitLive(held,true),"held-drag-reapplies");
    Drain();
    Check(Near(objects.at(Rec(a).obj).pos.x,5),"drag-survives-receipt");
    Check(Near(ActualY(a),1),"ground-y-kept");
    editor::DropCarried();Drain();NoLeaks();
}
// R3 real-mouse PlaceTick harness: crafted camera + scripted ImGui pointer frames in production
// order (PlaceTick then PumpSnapJobs). No sleeps; engine coordination uses the existing bounded Gate.
// World displacement per frame is exact by construction: the pointer pixel is the projection of the
// axis point itself, so the LineRayParam closest-approach equals the scripted arclength.
bool Near9(float a, float b) { return std::fabs(a - b) < 0.0001f; }
editor::CamFrame R3Cam(Vec3 pos, Vec3 fwd, Vec3 up) {
    editor::CamFrame c; c.pos = pos; c.fwd = fwd; c.up = up;
    c.right = { up.y*fwd.z - up.z*fwd.y, up.z*fwd.x - up.x*fwd.z, up.x*fwd.y - up.y*fwd.x };
    c.f = 1.5f; c.aspect = 1280.0f/720.0f; c.w = 1280.0f; c.h = 720.0f; c.ok = true; return c;
}
void R3Frame(ImVec2 px, bool down) {
    ImGuiIO& io = ImGui::GetIO(); io.MousePos = px; io.MouseDown[0] = down;
    ImGui::NewFrame(); editor::PlaceTick(); editor::PumpSnapJobs(); ImGui::Render();
}
struct R3Handle { ImVec2 click; float sClick; Vec3 axis; };
R3Handle R3AxisHandle(const editor::CamFrame& cf, Vec3 origin, Vec3 axis, int wantId, float radius) {
    const float size = editor::GizmoScreenSize(cf, origin, radius);
    const auto g = editor::GizmoAt(cf, origin, 0, 0, size);
    Require(g.ok, "r3-gizmo-visible");
    for (float f = 0.2f; f <= 0.8f; f += 0.05f) {
        Vec3 w = { origin.x + axis.x*size*f, origin.y + axis.y*size*f, origin.z + axis.z*size*f };
        ImVec2 px; if (!editor::WorldToScreen(cf, w, &px)) continue;
        if (editor::GizmoHover(cf, g, px) == wantId) {
            const float s = editor::LineRayParam(origin, axis, cf.pos, editor::MouseRay(cf, px));
            return { px, s, axis };
        }
    }
    Require(false, "r3-axis-handle-found"); return {};
}
void R3AxisFrame(const editor::CamFrame& cf, Vec3 origin, Vec3 axis, float sAbs, bool down) {
    ImVec2 px; Require(editor::WorldToScreen(cf, Vec3{ origin.x + axis.x*sAbs, origin.y + axis.y*sAbs, origin.z + axis.z*sAbs }, &px), "r3-axis-project");
    R3Frame(px, down);
}
// JoinGuard: a worker blocked in the engine gate must always be released+joined, even when a
// behavioral assert throws mid-schedule (destroying a joinable thread would terminate the run).
struct WorkerJoin { std::thread& w; Gate& b; ~WorkerJoin() { b.Release(); if (w.joinable()) w.join(); gate = nullptr; } };
// R3-O1: a real held X drag across carried-grounding completion. The same still-down pointer that
// dragged pre-ground must rebuild its candidate from the rebased origin, never resurrect Y=10.
void HeldDragX() {
    Reset(); editor::host_seam::SetTickNow(100000);
    const auto cf = R3Cam({0,10,-12}, {0,0,1}, {0,1,0}); editor::g_currentCam = cf;
    int a=Spawn(); Select({a}); editor::StartGrab({a}, false, "held-x");
    Require(Near(editor::g_place.center.x,0)&&Near(editor::g_place.center.y,10), "held-x-grabbed");
    const Vec3 origin = editor::g_place.center;
    const auto h = R3AxisHandle(cf, origin, {1,0,0}, 1, editor::g_place.radius);
    const uint64_t tf0 = editor::g_place.transform;
    auto op=Begin({a},true,true); Queue(); Gate barrier; gate=&barrier;
    std::thread worker([]{host::PumpGame();}); WorkerJoin wj{worker, barrier}; bool entered=barrier.Entered(); Check(entered,"held-x-applying-entered");
    if(entered) {
        Check(core::GroundStateOf(op).state==core::GroundApplying&&host::GroundLeaseCount()==1,"held-x-lease");
        R3Frame(h.click, true);
        Check(editor::g_place.drag==1,"held-x-drag-latched");
        Check(Near(editor::g_place.dragCenter0.x,0)&&Near(editor::g_place.dragCenter0.y,10),"held-x-origin-stored");
        for(int k=1;k<=3;++k) R3AxisFrame(cf, origin, h.axis, h.sClick+k, true);
        Check(Near(editor::g_place.center.x,0)&&Near(editor::g_place.center.y,10),"held-x-authority-unchanged");
        Check(editor::g_place.transform==tf0,"held-x-transform-unchanged");
        Check(bool(editor::g_deferredEditor.action),"held-x-one-deferred");
        Check(editor::g_undo.empty(),"held-x-no-premature-history");
    }
    barrier.Release(); worker.join(); gate=nullptr;
    editor::PumpSnapJobs(); Drain(); Reconcile(); Reason(op,"applied");
    Check(editor::g_undo.size()==1,"held-x-ground-entry");
    Check(core::GroundStateOf(op).serial==editor::g_undo.back().serial,"held-x-reserved-serial");
    Check(core::GroundStateOf(op).branch==editor::g_undo.back().branch,"held-x-originating-branch");
    Check(Near(ActualY(a),1),"held-x-ground-applied");
    Check(editor::g_place.transform==tf0+1,"held-x-one-transform-advance");
    R3AxisFrame(cf, origin, h.axis, h.sClick+4, true); Drain();
    Check(std::fabs(objects.at(Rec(a).obj).pos.x-4)<0.05f,"held-x-drag-motion-kept");
    Check(Near(ActualY(a),1),"held-x-native-ground-kept");
    Check(editor::g_place.drag==1,"held-x-drag-continues");
    Check(Near(editor::g_place.dragCenter0.y,1),"held-x-origin-rebased");
    Check(editor::g_place.transform==tf0+2,"held-x-readmit-advance");
    editor::DropCarried(); Drain();
    editor::Undo(); Drain();
    Check(Near(ActualY(a),1)&&Near(Rec(a).pos.x,0),"held-x-undo-drop-returns-ground");
    editor::Undo(); Drain();
    Check(Near(ActualY(a),10),"held-x-undo-ground-returns-origin");
    R3Frame({-100,-100}, false); NoLeaks();
}
// R3-O1 second axis: held Z drag across completion, same rebasing contract.
void HeldDragZ() {
    Reset(); editor::host_seam::SetTickNow(100000);
    const auto cf = R3Cam({12,10,0}, {-1,0,0}, {0,1,0}); editor::g_currentCam = cf;
    int a=Spawn(); Select({a}); editor::StartGrab({a}, false, "held-z");
    const Vec3 origin = editor::g_place.center;
    const auto h = R3AxisHandle(cf, origin, {0,0,1}, 3, editor::g_place.radius);
    const uint64_t tf0 = editor::g_place.transform;
    auto op=Begin({a},true,true); Queue(); Gate barrier; gate=&barrier;
    std::thread worker([]{host::PumpGame();}); WorkerJoin wj{worker, barrier}; bool entered=barrier.Entered(); Check(entered,"held-z-applying-entered");
    if(entered) {
        R3Frame(h.click, true);
        Check(editor::g_place.drag==3,"held-z-drag-latched");
        for(int k=1;k<=2;++k) R3AxisFrame(cf, origin, h.axis, h.sClick+k, true);
        Check(Near(editor::g_place.center.z,0),"held-z-authority-unchanged");
        Check(bool(editor::g_deferredEditor.action),"held-z-one-deferred");
    }
    barrier.Release(); worker.join(); gate=nullptr;
    editor::PumpSnapJobs(); Drain(); Reconcile(); Reason(op,"applied");
    Check(Near(ActualY(a),1),"held-z-ground-applied");
    R3AxisFrame(cf, origin, h.axis, h.sClick+3, true); Drain();
    Check(std::fabs(objects.at(Rec(a).obj).pos.z-3)<0.05f,"held-z-drag-motion-kept");
    Check(Near(ActualY(a),1),"held-z-native-ground-kept");
    Check(Near(editor::g_place.dragCenter0.y,1),"held-z-origin-rebased");
    Check(editor::g_place.transform==tf0+2,"held-z-readmit-advance");
    editor::DropCarried(); Drain();
    editor::Undo(); Drain();
    Check(Near(ActualY(a),1),"held-z-undo-drop-returns-ground");
    R3Frame({-100,-100}, false); NoLeaks();
}
// R3-O2: sub-threshold real frames accumulate against the last sent pose through admission.
// Eight 0.0009 steps (total 0.0072) at a held clock: UI pose tracks, identity advances per frame,
// native stays, dirty flips only past tolerance; age 59 stays silent, age 60 sends exactly once.
void AccumSmallSteps() {
    Reset(); editor::host_seam::SetTickNow(100000);
    const auto cf = R3Cam({0,10,-2}, {0,0,1}, {0,1,0}); editor::g_currentCam = cf;
    int a=Spawn(); Select({a}); editor::StartGrab({a}, false, "accum");
    const Vec3 origin = editor::g_place.center;
    const auto h = R3AxisHandle(cf, origin, {1,0,0}, 1, editor::g_place.radius);
    R3Frame(h.click, true);
    Require(editor::g_place.drag==1, "accum-latched");
    ImVec2 bpx; Require(editor::WorldToScreen(cf, Vec3{ origin.x + h.axis.x*(h.sClick+0.5f), origin.y + h.axis.y*(h.sClick+0.5f), origin.z + h.axis.z*(h.sClick+0.5f) }, &bpx), "accum-base-project");
    R3Frame(bpx, true); Drain();
    Require(Near(objects.at(Rec(a).obj).pos.x, editor::g_place.center.x), "accum-baseline-sent");
    Require(editor::g_place.dirty==false, "accum-baseline-clean");
    const uint64_t tf1 = editor::g_place.transform;
    const float lastX = editor::g_place.lastCenter.x;
    Require(std::fabs(lastX-0.5f)<0.05f, "accum-baseline-checkpoint");
    const float bx = floorf(bpx.x), by = floorf(bpx.y);
    const Vec3 dc0 = editor::g_place.dragCenter0;
    float ts[12] = {};
    ts[0] = editor::LineRayParam(dc0, h.axis, cf.pos, editor::MouseRay(cf, {bx, by}));
    for(int k=1;k<=10;++k) {
        ts[k] = editor::LineRayParam(dc0, h.axis, cf.pos, editor::MouseRay(cf, {bx+k, by}));
        Require(ts[k] > ts[k-1], "accum-steps-advance");
        Require(ts[k] - ts[k-1] < 0.005f, "accum-step-subthreshold");
    }
    Require(ts[8] - ts[0] > 0.005f, "accum-total-crosses");
    const float expect8 = lastX + (ts[8] - ts[0]);
    const float expect10 = lastX + (ts[10] - ts[0]);
    editor::host_seam::SetTickNow(100010);
    R3Frame({bx+1, by}, true);
    const float auth1 = editor::g_place.center.x;
    std::printf("DIAG accum-first delta=%.9g threshold=.005 dirty=%d lastX=%.9g currentX=%.9g\n",ts[1]-ts[0],editor::g_place.dirty,lastX,editor::g_place.center.x);
    Check(editor::g_place.dirty==false,"accum-clean-before-crossing");
    for(int k=2;k<=8;++k) R3Frame({bx+k, by}, true);
    Drain();
    Check(Near9(editor::g_place.center.x - auth1, ts[8] - ts[1]),"accum-ui-accumulates");
    Check(Near9(objects.at(Rec(a).obj).pos.x, lastX),"accum-native-held");
    Check(editor::g_place.dirty==true,"accum-dirty-after-crossing");
    Check(editor::g_place.transform==tf1+8,"accum-identity-per-frame");
    editor::host_seam::SetTickNow(100010+49);
    R3Frame({bx+8, by}, true); Drain();
    Check(Near9(objects.at(Rec(a).obj).pos.x, lastX),"accum-59ms-no-send");
    editor::host_seam::SetTickNow(100010+60);
    R3Frame({bx+8, by}, true); Drain();
    Check(Near9(objects.at(Rec(a).obj).pos.x, expect8),"accum-60ms-sends");
    Check(editor::g_place.dirty==false,"accum-clean-after-send");
    Check(Near9(editor::g_place.lastCenter.x, expect8),"accum-checkpoint-catchup");
    editor::host_seam::SetTickNow(200000);
    R3Frame({bx+9, by}, true); Drain();
    std::printf("DIAG accum-eligible delta=%.9g expectedHeld=%.9g actualNative=%.9g dirty=%d\n",ts[9]-ts[8],expect8,objects.at(Rec(a).obj).pos.x,editor::g_place.dirty);
    Check(Near9(objects.at(Rec(a).obj).pos.x, expect8),"accum-eligible-holds-before-crossing");
    Check(Near9(editor::g_place.center.x - expect8, ts[9] - ts[8]),"accum-eligible-tracks");
    R3Frame({bx+10, by}, true); Drain();
    Check(Near9(objects.at(Rec(a).obj).pos.x, expect10),"accum-eligible-sends-after-crossing");
    R3Frame({-100,-100}, false);
    editor::DropCarried(); Drain(); NoLeaks();
}
// R3-O2 yaw micro-steps are not representable: ImGui floors the pointer, so one ring pixel
// already exceeds the 0.01-degree yaw threshold. Yaw shares the exact X admission/accumulation
// gates proven by O2-ACCUMULATED-STEPS; no separate oracle is kept.
// R3-O3a: a sub-threshold frame during held Applying publishes nothing but keeps bookkeeping.
void SubThresholdHeld() {
    Reset(); editor::host_seam::SetTickNow(100000);
    const auto cf = R3Cam({0,10,-2}, {0,0,1}, {0,1,0}); editor::g_currentCam = cf;
    int a=Spawn(); Select({a}); editor::StartGrab({a}, false, "sub-held");
    const Vec3 origin = editor::g_place.center;
    const auto h = R3AxisHandle(cf, origin, {1,0,0}, 1, editor::g_place.radius);
    const uint64_t tf0 = editor::g_place.transform;
    auto op=Begin({a},true,true); Queue(); Gate barrier; gate=&barrier;
    std::thread worker([]{host::PumpGame();}); WorkerJoin wj{worker, barrier}; bool entered=barrier.Entered(); Check(entered,"sub-held-applying-entered");
    if(entered) {
        R3Frame(h.click, true);
        Check(editor::g_place.drag==1, "sub-held-latched");
        const float bx = floorf(h.click.x), by = floorf(h.click.y);
        R3Frame({bx+1, by}, true);
        Check(Near9(editor::g_place.center.x,0)&&Near9(editor::g_place.center.y,10),"sub-held-no-publication");
        Check(editor::g_place.transform==tf0,"sub-held-no-identity");
        std::printf("DIAG sub-held delta=%.9g threshold=.005 deferred=%d transform=%llu\n",editor::LineRayParam(origin,h.axis,cf.pos,editor::MouseRay(cf,{bx+1,by}))-editor::LineRayParam(origin,h.axis,cf.pos,editor::MouseRay(cf,{bx,by})),bool(editor::g_deferredEditor.action),static_cast<unsigned long long>(editor::g_place.transform));
        Check(!editor::g_deferredEditor.action,"sub-held-no-intent");
        Check(editor::g_place.drag==1,"sub-held-bookkeeping-usable");
    }
    barrier.Release(); worker.join(); gate=nullptr;
    editor::PumpSnapJobs(); Drain(); Reconcile(); Reason(op,"applied");
    R3AxisFrame(cf, origin, h.axis, h.sClick+1, true); Drain();
    Check(std::fabs(objects.at(Rec(a).obj).pos.x-1)<0.05f,"sub-held-continues-coherent");
    Check(Near(ActualY(a),1),"sub-held-ground-kept");
    R3Frame({-100,-100}, false);
    editor::DropCarried(); Drain(); NoLeaks();
}
// R3-O3b: real queue backpressure at a due send keeps dirty and the checkpoint for the retry.
void BackpressureRetry() {
    Reset(); editor::host_seam::SetTickNow(300000);
    const auto cf = R3Cam({0,10,-12}, {0,0,1}, {0,1,0}); editor::g_currentCam = cf;
    int a=Spawn(); Select({a}); editor::StartGrab({a}, false, "backpressure");
    const Vec3 origin = editor::g_place.center;
    const auto h = R3AxisHandle(cf, origin, {1,0,0}, 1, editor::g_place.radius);
    R3Frame(h.click, true);
    Require(editor::g_place.drag==1, "bp-latched");
    R3AxisFrame(cf, origin, h.axis, h.sClick+0.5f, true); Drain();
    Require(std::fabs(objects.at(Rec(a).obj).pos.x-0.5f)<0.05f, "bp-baseline-sent");
    const DWORD sentAt = editor::g_place.lastSend;
    editor::host_seam::SetTickNow(sentAt+60);
    for(int i=0;i<3;++i) Require(core::MoveMany({{a, Rec(a).pos, Rec(a).rot, Rec(a).scale}}, false), "bp-prefill-queued");
    R3AxisFrame(cf, origin, h.axis, h.sClick+1.0f, true);
    Check(editor::g_place.dirty==true,"bp-dirty-survives-refusal");
    Check(editor::g_place.lastSend==sentAt,"bp-checkpoint-survives-refusal");
    Check(std::fabs(objects.at(Rec(a).obj).pos.x-0.5f)<0.05f,"bp-no-send-under-pressure");
    Drain();
    R3AxisFrame(cf, origin, h.axis, h.sClick+1.0f, true); Drain();
    Check(std::fabs(objects.at(Rec(a).obj).pos.x-1.0f)<0.05f,"bp-retry-sends");
    Check(editor::g_place.dirty==false,"bp-clean-after-retry");
    R3Frame({-100,-100}, false);
    editor::DropCarried(); Drain(); NoLeaks();
}
// The owner r6 screenshot failure: an ALREADY GROUNDED rock re-grounded. The terrain surface sits exactly at
// the object's bottom (bottom 610.56, first contact cy = center 610.66 - radius 0.094 = 610.566 = bottom+0.006,
// fraction 0.0014 > 0.0005). The old band [bottom-0.03, top+0.03] called that contact the object's own collider,
// jumped to bottom-0.1 below the terrain, then every cast started inside the ground (negative fraction) and
// stepped down 0.5 m at a time until the cast reported no hit -> GroundCancel "no-surface" (#3 Failed).
void GroundedRebound() {
    Reset();int a=Spawn();auto first=Begin({a});Queue();Drain();Reconcile();Reason(first,"applied");
    Check(Near(ActualY(a),1),"first-ground-settled");const int before=castCount;
    auto op=Begin({a});for(int i=0;i<3;++i)Queue();
    Check(castCount-before==5,"surface-accepted-on-first-five-sample-query");
    Drain();Reconcile();Reason(op,"applied");
    Check(Near(ActualY(a),1),"grounded-object-stays-on-surface");NoLeaks();
}
// Adversarial: a contact strictly inside the body's vertical range is the object's own collider and must still
// be stepped past, never accepted as the surface (the fix may not blindly accept the first inside hit).
void GroundedSelfCollision() {
    Reset();int a=Spawn(0,50);selfBody=true;selfLow=49;selfHigh=51;height=[](float){return 0.0f;};
    auto op=Begin({a});const int before=castCount;Queue();
    Check(castCount-before==5&&Near(core::GroundStateOf(op).members[0].before.pos.y,50),"own-top-contact-jumps-below");
    Queue();Drain();Reconcile();Reason(op,"applied");
    Check(Near(ActualY(a),1),"floating-object-lands-on-real-ground");NoLeaks();
}
// Secondary inside/below-ground condition preserved: a collider reaching below the declared bounds is stepped
// out of in 0.5 m increments until the cast leaves it, then the real ground is accepted.
void BuriedColliderStepOut() {
    Reset();int a=Spawn(0,50);selfBody=true;selfLow=47;selfHigh=51;height=[](float){return 0.0f;};
    auto op=Begin({a});const int before=castCount;
    for(int i=0;i<7;++i)Queue();Check(castCount-before==6*5,"buried-steps-until-outside");
    Drain();Reconcile();Reason(op,"applied");
    Check(Near(ActualY(a),1),"buried-collider-finds-ground");NoLeaks();
}
void SettledOverlapBatch() {
    Reset();int a=Spawn(),b=Spawn(20);editor::StartGrab({a},false,"overlap-batch");
    auto first=Begin({a},true,true);auto other=Begin({b});Queue();
    host::PumpGame();Reconcile();Check(editor::g_undo.empty(),"settled-first-waits-for-other");
    auto next=Begin({a},true,true);Check(core::GroundStateOf(first).state==core::GroundReconciled,"overlap-reconciles-old-branch");
    Check(core::GroundStateOf(other).reason=="canceled","overlap-cancels-unapplied-other");
    height=[](float){return -2.0f;};Queue();Drain();Reconcile();editor::DropCarried();Drain();
    Check(editor::g_undo.size()==2&&Near(ActualY(a),-1)&&Near(ActualY(b),10),"no-late-duplicate-prior-carried-act");Reason(next,"applied");NoLeaks();
}
#include "raw_brush_cases.h"

void Run(const std::string& id,std::function<void()> f) {
    current=id;cases.push_back({id});std::printf("CASE %s\n",id.c_str());
    try{f();}catch(const std::exception& e){Check(false,e.what());}
    Trace("case-end");
}
} // namespace
int main(int argc,char** argv) {
    if(argc!=2)return 2;root=argv[1];host::SetModDir(root);host::OpenLog(root+"\\grounding.log");
    ImGui::CreateContext();ImGui::GetIO().DisplaySize={1280,720};ImGui::GetIO().DeltaTime=1.0f/60;
    ImGui::GetIO().IniFilename=nullptr;ImGui::GetIO().Fonts->AddFontDefault();unsigned char* pixels;int width,heightPixels;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels,&width,&heightPixels);
    LockProbe lockProbe;probe=&lockProbe;Install();
    Run("RIGID",[]{Happy(true,false);});Run("PER-OBJECT",[]{Happy(false,false);});Run("TWO-REVERSED",[]{Happy(false,true);});
    Run("CARRIED",Carried);Run("PARTIAL-ACTUAL",[]{Partial(false);});Run("PARTIAL-RECREATE",[]{Partial(true);});
    Run("STALE-PROBE-TELEPORT",[]{StaleTeleport(false,false);});Run("STALE-QUEUE-TELEPORT",[]{StaleTeleport(true,false);});
    Run("PARTIAL-TELEPORT-WRITE",[]{StaleTeleport(true,true);});Run("HIDE-RESTORE",HideRestore);Run("CARRIED-REPLACEMENT",CarriedReplacement);
    Run("ORDERED-MEMBERS",OrderedMembers);Run("INTERVENING-EDIT-UNDO-LATE",InterveningLate);
    Run("FAILED-WITH-REDO",[]{FailedRedo(true);});Run("NO-SURFACE-WITH-REDO",[]{FailedRedo(false);});Run("APPLYING-CANCEL",ApplyingCancel);
    for(const char* k:{"move","hide-restore","forget","delete-all"})Run(std::string("CORE-APPLYING-")+k,[k]{CoreConflict(k);});
    Run("FRAME-TIMEOUT-READY",[]{Timeout(0);});Run("FRAME-TIMEOUT-TICKET",[]{Timeout(1);});Run("FRAME-TIMEOUT-RESULT",[]{Timeout(2);});
    Run("WORLD-REPLACEMENT",[]{EpochAndPose(true);});Run("STALE-MEMBER-POSE",[]{EpochAndPose(false);});Run("SERVER-LEASE",Server);Run("BOUNDS",Bounds);
    for(const char* k:{"spawn","preview","grab","drop","cancel","delete","forget","group","rotate","align","numeric","numeric-live","paste","redo","clear","new","delete-all","load","replace"})
        Run(std::string("EDITOR-BARRIER-")+k,[k]{MutationBarrier(k);});
    Run("STALE-DEFERRED-INTENT",StaleIntent);
    Run("IN-FLIGHT-PHYSICS-TELEPORT",ProbeInFlight);Run("FORTY-ATTEMPTS",Attempts);
    Run("GROUNDED-REBOUND",GroundedRebound);Run("GROUNDED-SELF-COLLISION",GroundedSelfCollision);Run("BURIED-COLLIDER-STEPOUT",BuriedColliderStepOut);
    Run("OVERLAP-QUEUED",[]{Overlap(false);});Run("OVERLAP-APPLYING",[]{Overlap(true);});Run("APPLY-ONCE-IMMUTABLE",ApplyOnce);
    Run("CARRIED-AFTER-CONFLICT",CarriedAfterConflict);Run("CARRIED-TRANSFORM",CarriedTransform);Run("SERVER-FAILURE",ServerFailure);
    Run("INVALID-DESTINATION",InvalidDestination);Run("APPLIED-BEFORE-TELEPORT",AppliedBeforeTeleport);
    Run("IMPORTED-BOUNDS",ImportedBounds);Run("BOTH-RELEASE-ORDERS",CompareOrders);Run("ENGINE-FAULT-PARTIAL",FaultOutcome);
    Run("CARRIED-INTERVENING-EDIT",CarriedPreEdit);Run("NUMERIC-INTERVENING-EDIT",NumericPreEdit);Run("NUMERIC-APPLYING-UNDO",NumericApplyingUndo);Run("CARRIED-DRAG-PRESERVED",CarriedDragPreserved);Run("O1-HELD-DRAG-X",HeldDragX);Run("O1-HELD-DRAG-Z",HeldDragZ);Run("O2-ACCUMULATED-STEPS",AccumSmallSteps);Run("O3-HELD-SUBTHRESHOLD",SubThresholdHeld);Run("O3-BACKPRESSURE-RETRY",BackpressureRetry);Run("SETTLED-OVERLAP-BATCH",SettledOverlapBatch);
    for(int mode=0;mode<4;++mode)Run(std::string("MAIN-RAW-BRUSH-")+std::to_string(mode),[mode](){RawBrushInvalidation(mode);});
    Run("CLEANUP",[]{Reset();Check(allLocksFree&&host::LockViolations()==0,"all-engine-boundaries-lock-free");Check(gateTimeouts==0,"all-event-barriers-released-without-timeout");NoLeaks();});
    std::vector<std::string> cr;for(const auto& c:cases)cr.push_back("{\"id\":\""+c.id+"\",\"assertions\":"+std::to_string(c.assertions)+",\"failures\":"+std::to_string(c.failures)+",\"status\":\""+(c.failures?"FAIL":"PASS")+"\"}");
    Write("cases.json",cr);Write("history-timeline.json",timeline);Write("identity-trace.json",identities);Write("member-outcomes.json",outcomes);
    std::printf("ASSERTIONS=%d\nFAILURES=%d\n",assertions,failures);ImGui::DestroyContext();return failures?1:0;
}
