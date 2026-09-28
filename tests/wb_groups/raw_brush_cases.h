// Runs the actual BrushTick raw-ticket consumer, not a substitute brush dispatcher.
#pragma once
struct RawBrushWorker {
    Gate& barrier; std::thread worker;
    ~RawBrushWorker() { barrier.Release(); if (worker.joinable()) worker.join(); }
};
void RawBrushScenario(int mode, bool painted) {
    Reset(); host::SetTerrainAvailable(true); host::SetGroundRadius(.25f);
    const auto originalCast=host::Seam().groundCast;
    editor::g_open=true;editor::g_compact=false;editor::g_playMode=false;editor::g_mainTab=editor::TabTerrain;
    editor::g_brushOn=true;editor::g_shapePreview=false;
    editor::g_currentCam={{0,30,0},{1,0,0},{0,1,0},{0,0,1},true,1,1280.0f/720.0f,1280,720};
    auto& io=ImGui::GetIO();io.MousePos={640,360};io.MouseDown[0]=false;
    for(int i=0;i<2;++i){ImGui::NewFrame();ImGui::Render();} // clear prior windows' hover ownership, no wall-clock wait
    auto brushFrame=[](){ImGui::NewFrame();editor::BrushTick({},false);ImGui::Render();};
    const int old=core::RayProbe({0,30,0},{0,0,1},300);
    Require(old!=0,"raw-brush-ticket-admitted");
    editor::g_brushTicket=old;editor::g_brushPainting=editor::g_brushHave=editor::g_brushLastHit=true;
    editor::g_brushHitTick=GetTickCount();io.MouseDown[0]=true;
    editor::g_brushHistoryMark=core::TerrainStrokes().size();
    const size_t history=editor::g_undo.size();
    if(painted){editor::g_brushAt={2,10,3};editor::g_brushAx=2;editor::g_brushAz=3;editor::AddBrushStroke();}
    const auto accepted=core::TerrainStrokes();const size_t strokes=accepted.size();
    if(mode==1)host::PumpPhysics(); // ready result, still unconsumed by the editor
    if(mode==2){
        Gate held;castGate=&held;
        RawBrushWorker worker{held,std::thread([](){host::PumpPhysics();})};
        Require(held.Entered(),"raw-brush-cast-inflight-barrier");
        core::InvalidateGroundWorld();brushFrame();held.Release();worker.worker.join();castGate=nullptr;
    }else{
        core::InvalidateGroundWorld();
        if(mode==3){core::GroundHit ignored;Require(core::GroundResultState(old,&ignored)==core::GroundProbeStatus::Invalidated,"consume-before-brush-to-expose-unknown");}
        brushFrame();
    }
    Check(!editor::g_brushLastHit&&!editor::g_brushHave&&!editor::g_brushPainting&&editor::g_brushHitTick==0,"stale-brush-discards-hit-and-continuation");
    Check(core::TerrainStrokes().size()==strokes&&editor::g_undo.size()==history+(painted?1:0),"stale-brush-never-paints-and-commits-only-accepted-strokes");
    Check(editor::g_brushTicket==0,"terminal-brush-does-not-rearm-in-invalidated-frame");
    if(painted){
        const auto& act=editor::g_undo.back().acts.front();
        Check(act.kind==editor::Act::TerrainBatch&&act.terrain.size()==1&&act.terrain[0].x==accepted[0].x&&act.terrain[0].y==accepted[0].y,"accepted-brush-stroke-enters-shared-history-once");
        editor::Undo();Check(core::TerrainStrokes().empty(),"accepted-stroke-undo-after-invalidation");
        editor::Redo();Check(core::TerrainStrokes().size()==strokes&&core::TerrainStrokes()[0].x==accepted[0].x,"accepted-stroke-redo-after-invalidation");
    }
    brushFrame(); // fresh cursor input frame, still held: query may rearm, painting may not.
    const int fresh=editor::g_brushTicket;Vec3 direction{};
    Require(fresh!=0&&fresh!=old,"brush-rearms-with-new-cursor-ticket");
    Check(host::RawGroundDirection(fresh,&direction)&&Near(direction.z,1)&&Near(direction.y,0),"rearmed-brush-keeps-current-directional-ray");
    core::GroundHit ignored;Check(core::GroundResultState(old,&ignored)==core::GroundProbeStatus::Unknown,"late-old-cast-cannot-resurrect-consumed-ticket");
    host::Seam().groundCast=[](Vec3,float,core::GroundHit* hit){hit->done=hit->hit=true;hit->fraction=.5f;hit->center={8,10.25f,9};hit->centerY=10.25f;return true;};
    host::PumpPhysics();host::SetGroundRadius(.75f);host::Seam().probeReady=false;brushFrame();
    Check(editor::g_brushTicket==0&&Near(editor::g_brushAt.x,8)&&Near(editor::g_brushAt.y,10)&&Near(editor::g_brushAt.z,9),"fresh-brush-uses-consumed-center-and-result-radius");
    Check(core::TerrainStrokes().size()==strokes&&!editor::g_brushPainting,"fresh-hit-while-held-does-not-resume-old-stroke");
    io.MouseDown[0]=false;brushFrame();io.MouseDown[0]=true;brushFrame();
    Check(core::TerrainStrokes().size()==strokes+1&&editor::g_brushPainting,"fresh-click-starts-new-stroke-against-current-hit");
    io.MouseDown[0]=false;brushFrame();
    Check(editor::g_undo.size()==history+(painted?2:1),"fresh-stroke-is-separate-history-entry");
    host::Seam().groundCast=originalCast;host::Seam().probeReady=true;host::SetGroundRadius(0);
    host::SetTerrainAvailable(false);editor::g_brushOn=false;editor::g_brushTicket=0;editor::g_currentCam={};editor::g_mainTab=editor::TabBrowser;
    NoLeaks();
}
void RawBrushInvalidation(int mode) { RawBrushScenario(mode,false); RawBrushScenario(mode,true); }
