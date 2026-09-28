// Core-only mixed-project coverage: real registry/codec/terrain/queues, real files, native NPC services only.
#pragma once
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include "../../asi/cdmodkit/proj_codec.h"

namespace v097_project {
static std::map<uintptr_t,std::array<unsigned char,0x200>> transforms;
static std::set<uintptr_t> nativeLive;
static std::vector<uintptr_t> nativeRemoved;
static std::vector<uint32_t> toggles;
static std::function<void()> duringNpc;
static uintptr_t nextActor = 0x900000;
static std::vector<int> projects;
static std::string Path(const std::string& name) { return g_fixtureDir + "/projects/" + name + ".cdproj"; }
static std::string Read(const std::string& path) {
    std::ifstream file(path, std::ios::binary); Require(file.is_open(), "mixed-file-open", path);
    std::string bytes{std::istreambuf_iterator<char>(file), {}};
    Require(!file.bad(), "mixed-file-read", path); return bytes;
}
static void Write(const std::string& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary); file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close(); Require(!file.fail(), "mixed-fixture-write", path);
}
static proj_codec::Document Document(const std::string& name) {
    proj_codec::Document doc; std::string error;
    Require(proj_codec::Parse(Read(Path(name)),Path(name),proj_codec::Kind::Project,doc,error), "mixed-parse-real-file",error);
    return doc;
}
static SpawnedObj Object(int uid) {
    for (const auto& row : core::Spawned()) if (row.uid == uid) return row;
    throw std::runtime_error("mixed object missing: " + std::to_string(uid));
}
static ManagedNpc Npc(int uid) {
    for (const auto& row : core::ManagedNpcs()) if (row.uid == uid) return row;
    throw std::runtime_error("mixed NPC missing: " + std::to_string(uid));
}
static std::string State() {
    std::ostringstream out; out << std::hexfloat;
    for (const auto& o : core::Spawned()) out << "o " << o.uid << ' ' << o.gen << ' ' << o.poseGen << ' ' << o.obj << ' ' << o.actor << ' '
        << std::quoted(o.prefab) << ' ' << o.hidden << ' ' << o.proj << ' ' << o.group << ' ' << std::quoted(core::GroupName(o.group)) << ' '
        << o.pos.x << ' ' << o.pos.y << ' ' << o.pos.z << ' ' << o.rot.yaw << ' ' << o.rot.pitch << ' ' << o.rot.roll << ' ' << o.scale << ' ' << std::quoted(o.note) << '\n';
    for (const auto& n : core::ManagedNpcs()) out << "n " << n.uid << ' ' << n.gen << ' ' << n.actor << ' ' << n.actorId << ' ' << n.transform << ' '
        << n.hidden << ' ' << n.proj << ' ' << n.group << ' ' << std::quoted(core::GroupName(n.group)) << ' ' << n.key << ' ' << n.type << ' ' << n.extra << ' '
        << n.pos.x << ' ' << n.pos.y << ' ' << n.pos.z << ' ' << n.aiEnabled << ' ' << n.aiApplied << ' ' << n.behavior << ' ' << n.spawnPending << ' '
        << n.spawnRequestTick << ' ' << n.editMoving << ' ' << n.liveMovePending << ' ' << std::quoted(n.label) << ' ' << std::quoted(n.note) << '\n';
    for (const auto& t : core::TerrainStrokes()) out << "t " << t.proj << ' ' << t.mode << ' ' << t.x << ' ' << t.z << ' ' << t.r << ' ' << t.amount << ' ' << t.strength << ' ' << t.ax << ' ' << t.az << ' ' << t.y << '\n';
    for (int project : projects) out << "p " << project << ' ' << core::ProjectDirty(project) << '\n';
    return out.str();
}
static void Stroke(int project, float x) {
    core::TerrainStroke stroke{}; stroke.x=x; stroke.z=4; stroke.r=2; stroke.amount=-3; stroke.strength=1;
    core::TerrainReplaceProject(project,{stroke});
}
static void Setup(const char* id) {
    BeginCase(id); duringNpc = {}; host::SetBeforeReplace({}); host::SetSaveFault(host::SaveFault::None);
    Require(core::ClearScene(), "mixed-clear-prior-scene"); PumpRounds(16); core::GroundFrame();
    InstallSeam(); transforms.clear(); nativeLive.clear(); nativeRemoved.clear(); toggles.clear(); projects={0};
    host::SetModDir(g_fixtureDir); std::filesystem::create_directories(g_fixtureDir + "/projects");
    core::PrefabInfo prefab; prefab.path="/object/v097.prefab"; prefab.sx=prefab.sy=prefab.sz=2; prefab.hasCenter=true;
    host::SetPrefabIndex({prefab});
    const auto removeOther = host::Seam().removeActor;
    host::Seam().removeActor = [removeOther](uintptr_t actor) {
        if (!transforms.count(actor)) return removeOther(actor);
        const bool live = nativeLive.erase(actor) == 1;
        if (live) nativeRemoved.push_back(actor);
        return live;
    };
    core::g_managedNpcNativeTest.state = [] { return 2; };
    core::g_managedNpcNativeTest.spawn = [](uint32_t,Vec3 pos,int,uint32_t,uintptr_t* actor,uint32_t* actorId) {
        Check("NPC-native-spawn-registry-lock-free", g_probe.CheckFree());
        const uintptr_t handle=nextActor++; auto& memory=transforms[handle]; memory.fill(0);
        std::memcpy(memory.data()+host::NpcPositionOffset(), &pos, sizeof pos);
        nativeLive.insert(handle); *actor=handle; *actorId=static_cast<uint32_t>(handle);
        if (duringNpc) { auto callback=std::move(duringNpc); duringNpc={}; callback(); }
        return true;
    };
    core::g_managedNpcNativeTest.transform = [](uintptr_t actor) { return reinterpret_cast<uintptr_t>(transforms.at(actor).data()); };
    core::g_managedNpcNativeTest.toggleAi = [](uint32_t actorId) { toggles.push_back(actorId); return true; };
}
struct Trio { int object, npc; };
static Trio Add(int project, int group, float x) {
    const int object=core::SpawnAt("/object/v097.prefab",{x,10,3},{},1,group,project);
    const int npc=core::SpawnManagedNpc(30191,{x+1,10,6},12,17,false,1,group,project,"label","npc note");
    core::SetObjectNote(object,"object note"); Stroke(project,x); PumpRounds();
    Require(object != 0 && npc != 0 && Npc(npc).actor != 0, "mixed-native-admission"); return {object,npc};
}
static int Project(const char* name) { const int id=core::ProjectId(name); projects.push_back(id); return id; }
static void Cleanup() {
    duringNpc={}; host::SetBeforeReplace({}); Require(core::ClearScene(),"mixed-final-logical-clear"); PumpRounds(16);
    Check("mixed-native-actors-disposed",nativeLive.empty());
    core::g_managedNpcNativeTest={}; InstallSeam(); core::GroundFrame();
}
static void Scopes() {
    Setup("V097-MIXED-SAVE-SCOPES");
    const int a=Project("v097-a"), b=Project("v097-b"), group=core::NewGroupId(); core::SetGroupName(group,"shared group");
    const auto ta=Add(a,group,1), tb=Add(b,0,7), fresh=Add(0,0,10);
    Check("composite-count-includes-objects-NPCs-and-terrain",core::ProjectObjectCount(a)==3 && core::ProjectObjectCount(b)==3 && core::ProjectObjectCount(0)==3);
    Check("NPC-native-hold-control",!Npc(ta.npc).aiEnabled && !Npc(ta.npc).aiApplied && Npc(ta.npc).behavior==1);
    Require(core::SaveProject("v097-a",core::SaveProjectOnly),"mixed-SaveOnly"); const auto doc=Document("v097-a");
    Require(doc.records.size()==1 && doc.npcs.size()==1 && doc.terrain.size()==1,"SaveOnly-exact-three-kinds");
    Check("object-note-and-NPC-values-persist",doc.records[0].note=="object note" && doc.npcs[0].note=="npc note" && doc.npcs[0].label=="label" && doc.npcs[0].extra==17 && doc.npcs[0].type==12 && !doc.npcs[0].aiEnabled && doc.npcs[0].behavior==1);
    Check("object-NPC-shared-group-namespace",doc.records[0].group==doc.npcs[0].group && doc.groupNames.at(doc.records[0].group)=="shared group");
    Check("SaveOnly-does-not-adopt-New",Object(fresh.object).proj==0 && Npc(fresh.npc).proj==0 && core::ProjectObjectCount(0)==3);
    Require(core::SaveProject("v097-b",core::SaveProjectOnly),"mixed-unrelated-save"); const auto otherBytes=Read(Path("v097-b"));
    Require(core::SaveProject("v097-new",core::SaveNewOnly),"mixed-NewOnly"); const int newProject=Project("v097-new");
    Check("NewOnly-adopts-all-three-new-kinds",Object(fresh.object).proj==newProject && Npc(fresh.npc).proj==newProject && core::ProjectObjectCount(newProject)==3 && core::ProjectObjectCount(0)==0);
    const auto more=Add(0,0,14);
    Require(core::SaveProject("v097-a",core::SaveProjectAndNew),"mixed-Add-New"); const auto added=Document("v097-a");
    Check("Add-writes-exact-target-plus-New",added.records.size()==2 && added.npcs.size()==2 && added.terrain.size()==2 && Object(more.object).proj==a && Npc(more.npc).proj==a && core::ProjectObjectCount(a)==6);
    Check("other-project-records-and-bytes-untouched",Object(tb.object).proj==b && Npc(tb.npc).proj==b && core::ProjectObjectCount(b)==3 && Read(Path("v097-b"))==otherBytes);
    Require(core::SaveProject("v097-whole",core::SaveWholeScene),"mixed-WholeScene"); const int whole=Project("v097-whole");
    const auto all=Document("v097-whole");
    Check("WholeScene-adopts-all-three-kinds",all.records.size()==4 && all.npcs.size()==4 && all.terrain.size()==4 && core::ProjectObjectCount(whole)==12 && Object(ta.object).proj==whole && Npc(ta.npc).proj==whole);
    Cleanup();
}
static void Transactions() {
    Setup("V097-MIXED-TRANSACTIONS"); const int a=Project("v097-tx"), group=core::NewGroupId();
    core::SetGroupName(group,"before"); const auto owned=Add(a,group,1); const auto fresh=Add(0,0,3);
    Require(core::SaveProject("v097-tx",core::SaveProjectOnly),"mixed-tx-positive-control");
    for (auto fault : {host::SaveFault::WriteAbort,host::SaveFault::ShortWrite,host::SaveFault::FlushAbort,host::SaveFault::CloseAbort,host::SaveFault::ReplaceAbort}) {
        core::SetObjectNote(owned.object,"dirty object"); core::SetManagedNpcNote(owned.npc,"dirty NPC");
        const auto before=State(), bytes=Read(Path("v097-tx")); host::SetSaveFault(fault);
        Check("mixed-write-fault-refused",!core::SaveProject("v097-tx",core::SaveProjectAndNew));
        Check("mixed-write-fault-no-bytes-or-state-change",Read(Path("v097-tx"))==bytes && State()==before);
        Check("mixed-write-fault-never-adopts-New",Object(fresh.object).proj==0 && Npc(fresh.npc).proj==0 && core::ProjectObjectCount(0)==3);
    }
    host::SetBeforeReplace([=] { core::SetObjectNote(owned.object,"later object"); core::SetManagedNpcNote(owned.npc,"later NPC"); core::SetGroupName(group,"later group"); });
    Require(core::SaveProject("v097-tx",core::SaveProjectOnly),"mixed-metadata-write-completed");
    Check("newer-metadata-kept-dirty",Object(owned.object).note=="later object" && Npc(owned.npc).note=="later NPC" && core::GroupName(group)=="later group" && core::ProjectDirty(a));
    host::SetBeforeReplace([=] { Check("NPC-generation-change-during-save",core::HideManagedNpc(fresh.npc)); });
    Require(core::SaveProject("v097-tx",core::SaveProjectAndNew),"mixed-stale-generation-save"); PumpRounds();
    Check("save-never-adopts-newer-hidden-NPC-incarnation",Npc(fresh.npc).hidden && Npc(fresh.npc).proj==0 && core::ProjectDirty(a));
    host::SetBeforeReplace([] { Require(core::SaveProject("v097-adopter",core::SaveWholeScene),"nested-real-adoption"); });
    Require(core::SaveProject("v097-tx",core::SaveProjectOnly),"older-write-finishes"); const int adopter=Project("v097-adopter");
    Check("newer-object-and-NPC-memberships-survive-older-write",Object(owned.object).proj==adopter && Npc(owned.npc).proj==adopter && core::ProjectDirty(a));
    Cleanup();
}
static void ValidationReload() {
    Setup("V097-MIXED-VALIDATION-RELOAD"); const int a=Project("v097-reload"), b=Project("v097-keep"), group=core::NewGroupId();
    core::SetGroupName(group,"loaded group"); const auto old=Add(a,group,1), kept=Add(b,0,7);
    Require(core::SaveProject("v097-reload",core::SaveProjectOnly) && core::SaveProject("v097-keep",core::SaveProjectOnly),"reload-files-saved");
    const auto valid=Read(Path("v097-reload")), otherBytes=Read(Path("v097-keep")); const auto before=State(); const auto allocation=host::Allocators();
    for (const char* bad : {"#npc|30191|nan|2|3|1|0|1|0|0\n", "#npc|30191|1e39|2|3|1|0|1|0|0\n", "#npc|30191|40000000|2|3|1|0|1|0|0\n", "#terrain|0|nan|2|3|1|1|0|0|0\n"}) {
        Write(Path("v097-reload"),valid+bad);
        Check("malformed-mixed-Load-Replace-Reload-refused",!core::LoadProject("v097-reload",false) && !core::LoadProject("v097-reload",true) && !core::ReloadProject("v097-reload"));
        const auto after=host::Allocators();
        Check("validation-preserves-all-registry-and-allocator-state",State()==before && after.object==allocation.object && after.npc==allocation.npc && after.group==allocation.group && after.projects==allocation.projects);
        Check("validation-preserves-other-file",Read(Path("v097-keep"))==otherBytes);
    }
    Write(Path("v097-reload"),valid);
    Require(core::ReloadProject("v097-reload"),"validated-one-snapshot-Reload");
    Check("Reload-logical-outcome-before-native-drain",core::IndexOfUid(old.object)<0 && core::ProjectObjectCount(a)==3 && Object(kept.object).proj==b && Npc(kept.npc).proj==b);
    const auto report=core::ProjectLoadReportFor("v097-reload");
    Check("load-report-separates-three-kinds",report.valid && report.requested==1 && report.queued==1 && report.excluded==0 && report.requestedNpcs==1 && report.queuedNpcs==1 && report.excludedNpcs==0 && report.terrainStrokes==1);
    PumpRounds(); std::vector<SpawnedObj> objects; std::vector<ManagedNpc> npcs;
    for (const auto& o : core::Spawned()) if (o.proj==a) objects.push_back(o);
    for (const auto& n : core::ManagedNpcs()) if (n.proj==a) npcs.push_back(n);
    Require(objects.size()==1 && npcs.size()==1,"reloaded-mixed-registries");
    Check("reload-remaps-one-shared-group-and-preserves-notes",objects[0].group!=group && objects[0].group==npcs[0].group && core::GroupName(objects[0].group)=="loaded group" && objects[0].note=="object note" && npcs[0].note=="npc note" && npcs[0].uid!=old.npc && npcs[0].actor!=0);
    Require(core::ImportProjectFile(Path("v097-reload")),"same-path-v4-import-closes-source-before-replace");
    std::filesystem::create_directories(g_fixtureDir+"/shared"); const auto shared=g_fixtureDir+"/shared/v097-reload.cdproj"; Write(shared,valid);
    Require(core::ImportProjectFile(shared),"external-v4-import");
    Require(core::UnloadProject(a),"mixed-logical-Unload");
    Check("Unload-removes-target-not-other-project",core::ProjectObjectCount(a)==0 && core::ProjectObjectCount(b)==3 && Read(Path("v097-keep"))==otherBytes); PumpRounds();
    Require(core::LoadProject("v097-reload",true),"validated-mixed-Replace");
    Check("Replace-removes-other-logical-scene",core::ProjectObjectCount(a)==3 && core::ProjectObjectCount(b)==0); PumpRounds();
    Cleanup(); Check("empty-Clear-is-success",core::ClearScene());
}
static void NpcLifecycle() {
    Setup("V097-NPC-NATIVE-LIFETIME"); const int project=Project("v097-npc");
    const int uid=core::SpawnManagedNpc(30191,{1,2,3},1,0,true,0,0,project,"native label","native note"); PumpRounds();
    Require(uid!=0 && Npc(uid).actor!=0,"NPC-bound-through-real-server-job");
    Require(core::SaveProject("v097-npc",core::SaveProjectOnly),"NPC-only-save"); core::SavedLibrarySnapshot library;
    Require(core::RefreshSavedLibrary(library).ok(),"NPC-library-refresh");
    const auto row=std::find_if(library.entries.begin(),library.entries.end(),[](const auto& entry) { return entry.file.filename=="v097-npc.cdproj"; });
    Require(row!=library.entries.end(),"NPC-file-row");
    Check("NPC-ownership-not-double-counted-as-object",row->ownership.visible==0 && row->ownership.visibleNpcs==1 && row->ownership.pendingNpcs==0 && row->ownership.terrain==0 && core::ProjectObjectCount(project)==1);
    core::FileSelectionHandle selection; Require(core::SelectSavedFile(row->file,selection).ok(),"NPC-file-selected");
    auto archive=[&] { return core::ExecuteFileAction(selection,core::FileAction::Archive,"",[](const auto&) { return core::FileReason::None; }); };
    Check("live-NPC-file-guard",archive().reason==core::FileReason::VisibleReference);
    Require(core::HideManagedNpc(uid),"NPC-hide"); PumpRounds(); Check("hidden-NPC-file-guard",archive().reason==core::FileReason::HiddenReference);
    Require(core::RestoreManagedNpc(uid),"NPC-restore"); Check("pending-NPC-file-guard",archive().reason==core::FileReason::PendingOperation); PumpRounds();
    Check("bound-NPC-clears-readiness-gate",!core::ProjectMutationPending(project)); const auto actor=Npc(uid).actor; const auto toggleCount=toggles.size();
    Require(core::BeginManagedNpcMove(uid) && core::MoveManagedNpcLive(uid,{8,9,10}),"NPC-real-live-move");
    Check("NPC-live-edit-blocks-autosave",core::ProjectMutationPending(project)); PumpRounds();
    Check("native-AI-paused-not-saved-desired-AI",Npc(uid).aiEnabled && !Npc(uid).aiApplied && toggles.size()==toggleCount+1);
    Require(core::CommitManagedNpcMove(uid,{8,9,10}) && core::EndManagedNpcMove(uid,{1011,12,-2013}),"NPC-commit-and-end"); PumpRounds();
    struct PositionTile { float x,y,z; int16_t tx,tz; } position{};
    static_assert(sizeof(position)==16,"native TransformSync packed position");
    std::memcpy(&position,transforms.at(actor).data()+host::NpcPositionOffset(),sizeof position);
    Check("actual-native-transform-write-tiled-position",position.x==11 && position.y==12 && position.z==-13 && position.tx==1 && position.tz==-2);
    Check("NPC-AI-restored-and-readiness-cleared",Npc(uid).actor==actor && Npc(uid).aiApplied && Npc(uid).pos.x==1011 && !Npc(uid).editMoving && !core::ProjectMutationPending(project));
    Require(core::SetManagedNpcBehavior(uid,1),"NPC-hold-behavior"); PumpRounds();
    Check("hold-persists-desired-control",Npc(uid).behavior==1 && !Npc(uid).aiEnabled && !Npc(uid).aiApplied);
    Require(core::UnloadProject(project),"NPC-only-logical-unload"); Check("native-teardown-file-lease",archive().reason==core::FileReason::PendingOperation); PumpRounds();
    Check("archive-only-after-real-native-teardown",archive().ok());
    const int late=core::SpawnManagedNpc(30192,{1,2,3}); const auto gen=Npc(late).gen; const auto removed=nativeRemoved.size();
    duringNpc=[=] { Check("hide-restore-inside-native-NPC-create",core::HideManagedNpc(late) && core::RestoreManagedNpc(late)); }; PumpRounds();
    Check("stale-NPC-native-generation-disposed-once",Npc(late).gen!=gen && Npc(late).actor!=0 && nativeRemoved.size()==removed+1 && nativeLive.size()==1);
    Cleanup();
}
static void BusyOutcomes() {
    Setup("V097-PROJECT-BUSY-OUTCOME"); const int a=Project("v097-busy"); Add(a,0,1);
    Require(core::SaveProject("v097-busy",core::SaveProjectOnly),"busy-valid-mixed-file");
    host::SetDirectGimmick(true); const int uid=core::SpawnAt("/object/cd_gimmick/leased.prefab",{0,20,0},{},1,0,a); PumpRounds();
    const auto operation=core::BeginGround({uid},5000,1);
    Require(core::GroundApply(operation,{{uid,{0,19,0},{},1}}) && host::PumpGame(),"actual-server-lane-Applying");
    Require(core::GroundStateOf(operation).state==core::GroundApplying && host::GroundLeaseCount()==1,"actual-Applying-lease-held");
    const auto before=State(); const auto epoch=host::GroundEpoch(); const auto deferred=host::GroundDeferredCount();
    Check("busy-bools-reject-Load-Replace-Reload-Unload-Clear",!core::LoadProject("v097-busy",false) && !core::LoadProject("v097-busy",true) && !core::ReloadProject("v097-busy") && !core::UnloadProject(a) && !core::ClearScene());
    core::DeleteAllSpawned();
    Check("busy-refusal-preserves-scene-epoch-and-deferred-count",State()==before && host::GroundEpoch()==epoch && host::GroundDeferredCount()==deferred && core::GroundStateOf(operation).state==core::GroundApplying);
    PumpRounds(); Check("settlement-never-executes-rejected-project-action",core::GroundStateOf(operation).state==core::GroundSettled && core::IndexOfUid(uid)>=0 && core::ProjectObjectCount(a)==4);
    Require(core::ReloadProject("v097-busy"),"explicit-retry-completes-logical-reload"); Check("retry-replaces-old-logical-scene",core::IndexOfUid(uid)<0 && core::ProjectObjectCount(a)==3); PumpRounds();
    int stages=0;
    host::SetTravelStage([&](uint32_t,uint32_t,uint32_t,const float*) {
        ++stages; const auto state=State(); const auto count=host::GroundDeferredCount();
        Check("native-world-reservation-refuses-all-project-bools",!core::LoadProject("v097-busy",false) && !core::LoadProject("v097-busy",true) && !core::ReloadProject("v097-busy") && !core::UnloadProject(a) && !core::ClearScene());
        Check("native-world-refusals-have-no-side-effects",State()==state && host::GroundDeferredCount()==count);
    });
    Require(core::TravelTo({100,20,300},0),"actual-TravelTo-busy-check"); PumpGame_(); host::SetTravelStage({});
    Check("native-stage-executed-and-reservation-released",stages==1 && host::GroundWorldWriters()==0);
    Cleanup();
}
} // namespace v097_project
