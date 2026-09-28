// Real editor raw-drop consumers and real core NPC packet dispatch. No stale-position fallback.
#pragma once
struct RawDropGate {
    HANDLE entered=CreateEventW(nullptr,TRUE,FALSE,nullptr), release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::thread worker; bool timedOut=false;
    ~RawDropGate(){SetEvent(release);if(worker.joinable())worker.join();CloseHandle(entered);CloseHandle(release);}
};
static void QueueRawDrops(int browserTicket,int npcTicket,uint32_t key) {
    editor::BrowserDropJob browser;browser.prefab=0;browser.ticket=browserTicket;browser.queuedAt=GetTickCount();browser.center={0,-100,0};
    editor::NpcDropJob npc;npc.key=key;npc.ticket=npcTicket;npc.queuedAt=GetTickCount();npc.at={4,-100,0};
    editor::g_browserDropJobs.push_back(browser);editor::g_npcDropJobs.push_back(npc);
}
static void CaseRawDropInvalidation() {
    BeginCase("main-raw-drop-invalidation");ResetWorld();
    const auto originalCast=host::Seam().groundCast;
    struct Restore {
        std::function<bool(Vec3,float,core::GroundHit*)> cast;
        ~Restore(){host::Seam().groundCast=cast;core::g_npcExecute=nullptr;core::g_npcHandler=0;core::g_serverSession=core::g_sessionVt=0;}
    } restore{originalCast};
    uintptr_t vtable=0,session=(uintptr_t)&vtable;
    core::g_npcExecute=(void*)&NpcExecute;core::g_npcHandler=1;core::g_serverSession=(uintptr_t)&session;core::g_sessionVt=session;
    npcCalls=0;npcKeys.clear();npcPositions.clear();host::Seam().probeReady=true;host::SetGroundRadius(.25f);
    auto hit=[](Vec3 start,float,core::GroundHit* out){out->done=out->hit=true;out->fraction=.5f;out->centerY=10.25f;out->center={start.x,10.25f,start.z};return true;};
    host::Seam().groundCast=hit;
    const auto before=CaptureWorld();
    for(int mode=0;mode<4;++mode){
        const int browser=core::GroundProbe({0,30,0},40),npc=core::GroundProbe({4,30,0},40);
        Require(browser!=0&&npc!=0,"raw-drop-tickets-admitted");
        QueueRawDrops(browser,npc,321);
        if(mode==1)host::PumpPhysics();
        if(mode==2){
            RawDropGate gate;Require(gate.entered&&gate.release,"raw-drop-events-created");
            host::Seam().groundCast=[&](Vec3 p,float len,core::GroundHit* out){SetEvent(gate.entered);gate.timedOut=WaitForSingleObject(gate.release,5000)!=WAIT_OBJECT_0;return hit(p,len,out);};
            gate.worker=std::thread([](){host::PumpPhysics();});
            Require(WaitForSingleObject(gate.entered,5000)==WAIT_OBJECT_0,"raw-drop-inflight-entry");
            core::InvalidateGroundWorld();editor::PumpBrowserDropJobs();editor::PumpNpcDropJobs();
            SetEvent(gate.release);gate.worker.join();Check("raw-drop-inflight-released-without-timeout",!gate.timedOut);host::Seam().groundCast=hit;
        }else{
            core::InvalidateGroundWorld();
            if(mode==3){core::GroundHit ignored;core::GroundResultState(browser,&ignored);core::GroundResultState(npc,&ignored);}
            editor::PumpBrowserDropJobs();editor::PumpNpcDropJobs();
        }
        PumpAll();
        Check("stale-drop-jobs-terminally-erased",editor::g_browserDropJobs.empty()&&editor::g_npcDropJobs.empty());
        Check("stale-drop-no-native-npc-spawn",npcCalls==0);
        CheckWorldUnchanged("stale-drop-no-prefab-grab-history-or-fallback",before);
        core::GroundHit ignored;
        Check("stale-drop-results-not-resurrected",core::GroundResultState(browser,&ignored)==core::GroundProbeStatus::Unknown&&core::GroundResultState(npc,&ignored)==core::GroundProbeStatus::Unknown);
    }
    const int browser=core::GroundProbe({0,30,0},40),npc=core::GroundProbe({4,30,0},40);
    QueueRawDrops(browser,npc,654);
    host::PumpPhysics();host::SetGroundRadius(.75f);
    editor::PumpBrowserDropJobs();editor::PumpNpcDropJobs();PumpAll();
    Check("fresh-npc-drop-uses-own-radius",npcCalls==1&&npcPositions.size()==1&&std::fabs(npcPositions[0].y-10.0f)<.001f);
    Check("fresh-browser-drop-enters-real-grab",editor::Placing()&&!core::Spawned().empty());
    const auto& prefab=core::PrefabIndex()[0];const auto objects=core::Spawned();
    const float expected=10+(prefab.hasCenter?std::max(0.f,prefab.sy)*.5f-prefab.cy:0.f);
    Check("fresh-browser-drop-uses-own-radius",!objects.empty()&&std::fabs(objects.back().pos.y-expected)<.001f);
    editor::CancelCarried();PumpAll();host::SetGroundRadius(0);core::GroundFrame();
    Check("raw-drop-ticket-cleanup",host::GroundTicketCount()==0);
}
