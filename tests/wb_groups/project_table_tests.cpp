// WB080 Task 7: actual v0.97 editor/ImGui controls, DrawData and CORE queues. Item hooks OBSERVE only.
// Legacy repeat-append/whole-scene Replace remain explicit production-dispatcher checks where U has no row button.
// Tests release exact queue/physics work; no sleeps, timing guesses, substitute dispatcher or UI mock.
#include "../../asi/cdmodkit/editor.cpp"
#include "production_host.h"
#include <imgui_internal.h>
#include <sstream>
#include <stdexcept>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")

namespace {
struct Item { ImGuiID id = 0; ImRect box, clip; std::string label; bool disabled = false; std::vector<ImGuiID> stack; ImGuiWindow* window = nullptr; };
std::map<ImGuiID, Item> items;
int assertions = 0, failures = 0, creates = 0, removes = 0, moves = 0, casts = 0;
std::string root, current, dir;
std::vector<std::string> traces, cases;
bool full = false, sceneProjects = false, libraryOnly = false; float screenW = 1280, screenH = 900;
uintptr_t nextHandle = 100;
struct Object { Vec3 pos; bool live = true; };
std::map<uintptr_t, Object> objects;
const char* good = "/object/good.prefab";
void Check(bool ok, const char* what) {
    ++assertions; if (!ok) ++failures;
    printf("CHECK %s %s/%s\n", ok ? "PASS" : "FAIL", current.c_str(), what);
    core::Log("CHECK %s %s/%s", ok ? "PASS" : "FAIL", current.c_str(), what);
}
void Require(bool ok, const char* what) { Check(ok, what); if (!ok) throw std::runtime_error(what); }
void Write(const std::string& path, const std::string& bytes) {
    FILE* f = fopen(path.c_str(), "wb"); if (!f) throw std::runtime_error("file open failed");
    bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size(); ok = fclose(f) == 0 && ok;
    if (!ok) throw std::runtime_error("file write failed");
}
std::string Read(const std::string& path) { std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()}; }
void Array(const char* name, const std::vector<std::string>& rows) {
    std::string text = "[\n"; for (size_t i=0;i<rows.size();++i) text += rows[i] + (i+1==rows.size()?"\n":",\n");
    Write(root+"\\"+name,text+"]\n");
}
void Trace(const char* event) {
    std::ostringstream s;s<<"{\"case\":\""<<current<<"\",\"event\":\""<<event<<"\",\"creates\":"<<creates<<",\"removes\":"<<removes<<",\"moves\":"<<moves<<",\"casts\":"<<casts
      <<",\"queue\":"<<core::PendingSpawns()<<",\"registry\":"<<host::RegistrySize()<<",\"undo\":"<<editor::g_undo.size()<<",\"redo\":"<<editor::g_redo.size()<<",\"requests\":[";
    for(size_t i=0;i<editor::g_projectPlacements.size();++i){if(i)s<<',';const auto& r=editor::g_projectPlacements[i];const auto v=core::PlaceRequestState(r.request);
        s<<"{\"handle\":"<<(uintptr_t)r.request.get()<<",\"requested\":"<<v.requested<<",\"attached\":"<<v.attached<<",\"excluded\":"<<v.excluded<<",\"failed\":"<<v.failed<<",\"canceled\":"<<v.canceled<<",\"pending\":"<<v.pending
         <<",\"settled\":"<<v.settled<<",\"requestCanceled\":"<<v.requestCanceled<<",\"cleanupPending\":"<<v.cleanupPending<<",\"rows\":[";
        for(size_t j=0;j<v.rows.size();++j){if(j)s<<',';const auto& x=v.rows[j];s<<"{\"id\":"<<x.rowId<<",\"uid\":"<<x.uid<<",\"prefab\":\""<<x.prefab<<"\",\"state\":"<<x.state<<",\"reason\":\""<<x.reason<<"\",\"removed\":"<<x.removedAfterAttach<<",\"cleanup\":"<<x.cleanupPending<<'}';}s<<"]}";
    }
    s<<"],\"ground\":[";for(size_t i=0;i<editor::g_pendingGround.size();++i){if(i)s<<',';const auto v=core::GroundStateOf(editor::g_pendingGround[i]);s<<"{\"id\":"<<v.id<<",\"state\":"<<v.state<<",\"reason\":\""<<v.reason<<"\",\"members\":"<<v.members.size()<<'}';}s<<"]}";traces.push_back(s.str());
}
void Frame() {
    items.clear(); ImGui::GetIO().DisplaySize = {screenW,screenH}; ImGui::NewFrame();
    if(full) editor::Draw();
    else { if(!libraryOnly)editor::PumpSnapJobs(); ImGui::SetNextWindowPos({0,150}); ImGui::SetNextWindowSize({screenW,screenH-150});
        ImGui::Begin("ProjectTableHost",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoDecoration);
        if(libraryOnly) { ImGui::PushOverrideID(ImHashStr("project-page")); editor::RefreshProjectLibrary(); editor::DrawSavedLibrary(); ImGui::PopID(); }
        else if(sceneProjects) editor::DrawProjectTabs(core::Spawned()); else editor::DrawProject();
        ImGui::End();
        if(editor::g_place.active) editor::DrawPlaceHud();
        editor::PublishProjectContext(); }
    ImGui::Render();
}
bool InRow(const Item& item,const std::string& name,bool group) {
    if(name.empty())return true;const auto& s=item.stack;size_t n=s.size();if(n<3)return false;
    return s[n-2]==ImHashStr(group?"group":"project",0,s[n-3]) && s[n-1]==ImHashStr(name.c_str(),0,s[n-2]);
}
bool Has(const std::string& label) { for(const auto& x:items) if(x.second.label==label)return true;return false; }
Item FindItem(const std::string& label,const std::string& row="",bool group=false,const core::PlaceRequestHandle& request={}) {
    ImGuiID requested=0;
    if(request){
        for(const auto& report:editor::g_placementReports.Snapshot().reports)if(report.request==request)
            requested=ImHashStr(label.c_str(),0,ImHashStr(("placement-"+std::to_string(report.id)).c_str(),0,ImHashStr("project-page")));
        Require(requested!=0,"requested-report-has-stable-identity");
    }
    for(const auto& x:items){const auto& item=x.second;
        if(!InRow(item,row,group)||(request&&item.id!=requested))continue;
        // ItemAdd records even clipped controls, before ItemInfo may return early.
        if(item.label==label||(!item.stack.empty()&&item.id==ImHashStr(label.c_str(),0,item.stack.back())))return item;
    }
    throw std::runtime_error("control missing: "+label+" / "+row);
}
ImGuiWindow* LibraryWindow() {
    for(auto* w:GImGui->Windows)if(w->Active&&w->ChildId==ImHashStr("saved-library",0,ImHashStr("project-page")))return w;
    throw std::runtime_error("library child missing");
}
void SelectLibraryFile(const std::string& filename,bool archived=false) {
    Frame();auto* child=LibraryWindow();auto* parent=child->ParentWindow;
    ImGui::SetScrollY(parent,std::max(0.f,parent->Scroll.y+child->Pos.y-parent->InnerClipRect.Min.y-ImGui::GetStyle().ItemSpacing.y));Frame();Frame();
    size_t index=0;for(;index<editor::g_libraryRows.size();++index) { const auto& f=editor::g_projectLibrary.entries[editor::g_libraryRows[index]].file;if(f.filename==filename&&f.archived==archived)break; }
    Require(index<editor::g_libraryRows.size(),"selected-file-is-in-filtered-snapshot");
    const auto file=editor::g_projectLibrary.entries[editor::g_libraryRows[index]].file;
    ImGui::SetScrollY(child,(float)index*ImGui::GetTextLineHeightWithSpacing());Frame();Frame();
    const auto id=ImHashStr("###saved-file",0,ImHashStr(file.path.c_str(),0,child->ID));
    const auto box=items.at(id).box;ImGui::SetScrollFromPosY(parent,box.Min.y-parent->Pos.y,0.25f);Frame();Frame();
    const Item item=items.at(id);Require(item.clip.Contains(item.box),"library-selection-row-in-viewport");
    auto& io=ImGui::GetIO();io.AddMousePosEvent(item.box.GetCenter().x,item.box.GetCenter().y);Frame();io.AddMouseButtonEvent(0,true);Frame();io.AddMouseButtonEvent(0,false);Frame();Frame();
    Require(editor::SameSavedFile(editor::g_librarySelected,file),"mouse-selected-exact-saved-file");
}
void Click(const std::string& label,const std::string& row="",bool group=false,const core::PlaceRequestHandle& request={}) {
    Frame(); // layout reflects the last released producer callback, not stale pre-attachment widths
    if(!row.empty() && (editor::g_librarySelected.filename!=row+(group?".cdgroup":".cdproj") || editor::g_librarySelected.archived)) SelectLibraryFile(row+(group?".cdgroup":".cdproj"));
    Item item=FindItem(label,row,group,request); auto& io=ImGui::GetIO();
    if(item.window&&!item.clip.Contains(item.box)) {
        ImGui::SetScrollFromPosY(item.window,item.box.Min.y-item.window->Pos.y,0.5f);Frame();Frame();item=FindItem(label,row,group,request);
    }
    Require(item.window&&item.clip.Contains(item.box),"control-in-viewport");
    io.AddMousePosEvent(item.box.GetCenter().x,item.box.GetCenter().y); Frame();item=FindItem(label,row,group,request);
    io.AddMousePosEvent(item.box.GetCenter().x,item.box.GetCenter().y);
    io.AddMouseButtonEvent(0,true);Frame();io.AddMouseButtonEvent(0,false);Frame();Frame(); // render the state changed by release
    traces.push_back("{\"case\":\""+current+"\",\"controlId\":"+std::to_string(item.id)+",\"disabled\":"+(item.disabled?"true":"false")+"}");Trace("click");
}
void Name(const char* value) {
    Click("project name");auto& io=ImGui::GetIO();io.AddKeyEvent(ImGuiMod_Ctrl,true);io.AddKeyEvent(ImGuiKey_A,true);Frame();
    io.AddKeyEvent(ImGuiKey_A,false);io.AddKeyEvent(ImGuiMod_Ctrl,false);Frame();io.AddInputCharactersUTF8(value);Frame();
    io.AddKeyEvent(ImGuiKey_Enter,true);Frame();io.AddKeyEvent(ImGuiKey_Enter,false);Frame();
    Require(std::string(editor::g_projName)==value,"actual-text-input-committed");
}
void Capture(const char* name) {
    const ImDrawData* data=ImGui::GetDrawData();Require(data && data->Valid && data->TotalVtxCount>0,"actual-DrawData");
    std::ostringstream s;s<<"{\"width\":"<<data->DisplaySize.x<<",\"height\":"<<data->DisplaySize.y<<",\"lists\":[";
    for(int i=0;i<data->CmdListsCount;++i){if(i)s<<',';const auto* l=data->CmdLists[i];s<<"{\"vertices\":[";
        for(int j=0;j<l->VtxBuffer.Size;++j){if(j)s<<',';const auto& v=l->VtxBuffer[j];s<<'['<<v.pos.x<<','<<v.pos.y<<','<<v.uv.x<<','<<v.uv.y<<','<<v.col<<']';}
        s<<"],\"indices\":[";for(int j=0;j<l->IdxBuffer.Size;++j){if(j)s<<',';s<<l->IdxBuffer[j];}s<<"],\"commands\":[";
        for(int j=0;j<l->CmdBuffer.Size;++j){if(j)s<<',';const auto& c=l->CmdBuffer[j];s<<'['<<c.ElemCount<<','<<c.IdxOffset<<','<<c.VtxOffset<<','<<c.ClipRect.x<<','<<c.ClipRect.y<<','<<c.ClipRect.z<<','<<c.ClipRect.w<<']';}s<<"]}";
    }s<<"]}";Write(root+"\\"+name+".draw.json",s.str());Trace(name);
}
void Drain() {int n=0;while(host::PumpGame())if(++n>4096)throw std::runtime_error("CORE queue did not drain");}
void Install() {
    auto& e=host::Seam();e.ready=e.probeReady=true;
    e.createGeneric=[](const std::string& p,Vec3 pos,Rot,float){++creates;if(p=="/object/fail.prefab")return uintptr_t(0);auto h=nextHandle++;objects[h]={pos,true};return h;};
    e.remove=[](uintptr_t h){++removes;auto it=objects.find(h);if(it==objects.end()||!it->second.live)return false;it->second.live=false;return true;};
    e.moveInPlace=[](uintptr_t h,Vec3 p,Rot,float){++moves;auto it=objects.find(h);if(it==objects.end()||!it->second.live)return false;it->second.pos=p;return true;};e.liveMove=e.moveInPlace;
    e.groundCast=[](Vec3 p,float length,core::GroundHit* hit){++casts;hit->done=hit->hit=true;hit->centerY=p.x>10?3.0f:0.0f;hit->fraction=(p.y-hit->centerY)/length;return true;};
    std::vector<core::PrefabInfo> idx;for(const char* p:{good,"/object/fail.prefab","/object/pending.prefab"}){core::PrefabInfo x;x.path=p;x.name=editor::ShortName(p);x.hasCenter=true;x.meshes=1;x.sx=x.sy=x.sz=2;idx.push_back(x);}host::SetPrefabIndex(idx);
    core::g_recreateOnMove=false;
}
std::string ProjectPath(){return dir+"\\projects\\home.cdproj";}
std::string GroupPath(){return dir+"\\Groups\\kit.cdgroup";}
proj_codec::Document Group(const std::vector<std::string>& prefabs) {
    proj_codec::Document doc;doc.kind=proj_codec::Kind::Group;doc.hasBounds=true;doc.bounds={{0,10,0},{-1,9,-1},{21,16,1},true};doc.envelopes.push_back({1,doc.bounds});
    for(size_t i=0;i<prefabs.size();++i){proj_codec::Record r;r.prefab=prefabs[i];r.pos={(double)i*4,10,0};r.envelope=1;doc.records.push_back(r);}return doc;
}
void GroupFile(const std::vector<std::string>& prefabs) {std::string text,error;Require(proj_codec::Serialize(Group(prefabs),text,error),"serialize-group-fixture");Write(GroupPath(),text);editor::g_projectRefresh=true;}
void Reset() {
    full=false;sceneProjects=false;libraryOnly=false;screenW=1280;screenH=900;
    ImGui::ClosePopupsOverWindow(nullptr,false);editor::g_libraryAction={};core::g_fileMutationFault=core::FileMutationFault::None;
    editor::g_projectLibrary={};editor::g_librarySelected={};editor::g_libraryRows.clear();editor::g_librarySearch[0]=0;editor::g_libraryKind=editor::g_libraryLocation=0;editor::g_libraryFilterDirty=true;editor::ClearSceneAction(false);Drain();editor::PumpSnapJobs();editor::host_seam::ResetPlacement();editor::host_seam::ResetHistory();
    editor::g_projectPlacements.clear();editor::g_groundLastResults.clear();editor::g_projectStatus.clear();editor::g_exportOpen=false;editor::g_exportApproval={};editor::g_exportOverwrite=false;editor::g_projectReadValid=false;editor::g_groundMode=0;
    editor::g_projectOverwriteName.clear();editor::ResetExportApproval();editor::g_exportAttempts.clear();editor::g_projectDetails.clear();editor::g_projectFileFailures.clear();
    editor::g_placementReports={};editor::g_groundReports={};
    editor::g_open=true;editor::g_playMode=false;editor::g_compact=false;editor::g_primary=0;editor::g_sel.clear();editor::g_selPrefab=-1;strcpy_s(editor::g_projName,"mybuild");
    dir=root+"\\"+current;CreateDirectoryA(dir.c_str(),nullptr);CreateDirectoryA((dir+"\\projects").c_str(),nullptr);CreateDirectoryA((dir+"\\Groups").c_str(),nullptr);host::SetModDir(dir);
    Write(ProjectPath(),std::string(good)+"|0|10|0\n");GroupFile({good});
    editor::g_projectRefresh=true;ImGui::GetIO().AddMousePosEvent(-100,-100);Frame();Frame();
}
SpawnedObj Rec(int uid){for(const auto& o:core::Spawned())if(o.uid==uid)return o;throw std::runtime_error("missing uid");}
int Spawn(float x=0,float y=10){int uid=core::SpawnAt(good,{x,y,0});Drain();return uid;}
void Select(const std::vector<int>& uids){editor::host_seam::ClearSelection();for(int uid:uids)editor::host_seam::SelectAdd(uid);}
std::string World() {
    std::ostringstream s;s<<host::NextUid()<<'/'<<core::PendingSpawns()<<'/'<<creates<<'/'<<removes<<'/'<<moves;
    for(const auto& o:core::Spawned())s<<'|'<<o.uid<<','<<o.obj<<','<<o.gen<<','<<o.poseGen<<','<<o.hidden<<','<<o.proj<<','<<o.group<<','<<o.pos.x<<','<<o.pos.y<<','<<o.pos.z;
    for(const auto& e:editor::g_undo)s<<"U"<<e.serial<<':'<<e.acts.size();for(const auto& e:editor::g_redo)s<<"R"<<e.serial<<':'<<e.acts.size();
    for(int uid:editor::g_sel)s<<"S"<<uid;return s.str();
}
void Equations(const core::PlaceRequestHandle& handle) {
    const auto v=core::PlaceRequestState(handle);
    Check(v.requested==v.attached+v.excluded+v.failed+v.canceled+v.pending,"five-state-conservation");
    int attached=0,excluded=0,failed=0,canceled=0,pending=0;
    for(const auto& row:v.rows) { attached+=row.state==core::PlaceAttached;excluded+=row.state==core::PlaceExcluded;
        failed+=row.state==core::PlaceFailed;canceled+=row.state==core::PlaceCanceled;pending+=row.state==core::PlacePending; }
    Check(v.attached==attached&&v.excluded==excluded&&v.failed==failed&&v.canceled==canceled&&v.pending==pending,"raw-counts-match-one-snapshot-rows");
    Check(v.settled==(v.pending==0&&!v.cleanupPending),"final-only-after-rows-and-cleanup");
}
void Actions() {
    Reset();Check(Has("Refresh"),"refresh-control");Check(Has("Export Selection"),"export-control");
    Click("Read","kit",true);Check(editor::g_projectReadValid&&editor::g_projectRead.kind==proj_codec::Kind::Group,"read-group");
    Click("Read","home");Check(editor::g_projectRead.kind==proj_codec::Kind::Project,"read-project");
    const int createBefore=creates;Click("Load","home");Check(core::PendingSpawns()>0&&creates==createBefore,"load-not-predictive-success");Drain();
    Check(core::ProjectObjectCount(core::ProjectId("home"))==1,"load-project-producer");
    Click("autoload","home");Check(core::Autoload()==std::vector<std::string>{"home"},"project-autoload-producer");
    Check(FindItem("Load","home").disabled,"U-loaded-row-disables-repeat-Load");
    // No missing-widget fallback: repeat append is an explicit real editor API contract.
    Require(editor::DispatchProjectAction({"home"},editor::ProjectAction::Load),"repeat-append-production-dispatch");Drain();
    Check(core::ProjectObjectCount(core::ProjectId("home"))==2,"append-not-replace");
    int fresh=Spawn();core::SetObjectNote(core::Spawned().front().uid,"SaveOnly control");Click("Refresh");Click("Save","home");
    Check(Rec(fresh).proj==0&&core::ProjectObjectCount(0)==1,"ordinary-Save-never-adopts-New");
    Click("Add unassigned","home");Check(Rec(fresh).proj==core::ProjectId("home")&&!core::ProjectDirty(core::ProjectId("home")),"save-back-adopts-new-and-clears-dirty");
    std::string saved=Read(ProjectPath());proj_codec::Document savedDocument;std::string savedError;
    Check(proj_codec::Parse(saved,ProjectPath(),proj_codec::Kind::Project,savedDocument,savedError)&&savedDocument.records.size()==3,"saved-project-kind");
    int unrelated=Spawn(100);
    Require(editor::DispatchProjectAction({"home"},editor::ProjectAction::Replace),"whole-scene-Replace-production-dispatch");Drain();
    Check(core::ProjectObjectCount(0)==0&&editor::g_undo.empty()&&editor::g_sel.empty(),"replace-clears-only-after-validation");
    bool oldVisible=false;for(const auto& o:core::Spawned())oldVisible|=o.uid==unrelated&&!o.hidden;Check(!oldVisible,"replace-dispatched-deletion");
    int selected=Spawn();Select({selected});Name("exported");Click("Export Selection");Click("Preflight");
    Check(editor::g_exportApproval.valid&&editor::g_exportApproval.included==std::vector<int>{selected},"preflight-actual-selection");
    const int adopted=Rec(selected).proj;Click("Write group");Check(Rec(selected).proj==adopted,"export-does-not-adopt-project");
    std::string text=Read(dir+"\\Groups\\exported.cdgroup");proj_codec::Document doc;std::string error;
    Check(proj_codec::Parse(text,dir+"\\Groups\\exported.cdgroup",proj_codec::Kind::Group,doc,error)&&doc.records.size()==1,"write-group-real-file");
    Click("Cancel export");Click("Refresh");Frame();Check(editor::g_projectLibrary.entries.size()==3,"refresh-both-kinds");
    Click("Place","kit",true);Require(!editor::g_projectPlacements.empty(),"place-created-caller-owned-receipt");
    auto request=editor::g_projectPlacements.back().request;Check(core::PlaceRequestState(request).pending==1,"place-pending-not-placed");Drain();Frame();Equations(request);
    Check(core::PlaceRequestState(request).attached==1&&editor::g_place.req==request,"actual-attachment-owned-by-ui");
    Click("drop");Drain();Frame();Check(!editor::Placing(),"mouse-bar-drop-finishes");
    std::vector<int> oldHome;for(const auto& object:core::Spawned())if(object.proj==core::ProjectId("home"))oldHome.push_back(object.uid);
    Click("Reload","home");Drain();
    Check(core::ProjectObjectCount(core::ProjectId("home"))==3&&std::all_of(oldHome.begin(),oldHome.end(),[](int uid){return core::IndexOfUid(uid)<0;}),"real-Reload-replaces-target-identities-once");
    Check(Rec(selected).proj==0,"Reload-keeps-unrelated-New-record");
    Click("Unload","home");Drain();Check(core::ProjectObjectCount(core::ProjectId("home"))==0&&Rec(selected).proj==0,"real-Unload-removes-only-target-project");
    Capture("actions-desktop");
}
void WrongKind() {
    Reset();int a=Spawn();Select({a});const auto before=World();const auto autoBefore=core::Autoload();
    const editor::ProjectFile group{"kit",proj_codec::Kind::Group},project{"home",proj_codec::Kind::Project};
    for(auto action:{editor::ProjectAction::Load,editor::ProjectAction::Replace,editor::ProjectAction::SaveBack,editor::ProjectAction::Autoload})
        Check(!editor::DispatchProjectAction(group,action,true),"group-project-action-refused-at-dispatch");
    for(auto action:{editor::ProjectAction::Place,editor::ProjectAction::Overwrite})Check(!editor::DispatchProjectAction(project,action),"project-group-action-refused-at-dispatch");
    Check(before==World()&&autoBefore==core::Autoload(),"wrong-kind-no-producer-mutation");
    // Replace content after row discovery: extension/badge alone cannot authorize dispatch.
    Write(ProjectPath(),Read(GroupPath()));Click("Load","home");Check(before==World(),"disk-kind-rechecked-before-visible-Load");
    Check(!editor::DispatchProjectAction(project,editor::ProjectAction::Replace),"wrong-kind-Replace-production-dispatch-refused");
    Check(before==World(),"disk-kind-rechecked-before-replace");
    Write(dir+"\\autoload.txt","kit.cdgroup\n");host::ResetAutoloadDone();host::AutoloadRun();Check(before==World(),"group-never-autoloads");Trace("wrong-kind-authority");
}
void DeclinedOverwrite() {
    Reset();int a=Spawn();Select({a});const auto bytes=Read(GroupPath());const auto before=World();
    Click("Overwrite","kit",true);Check(editor::g_exportOpen&&!editor::g_exportOverwrite,"overwrite-opens-preflight-not-approval");
    Click("Preflight");Check(!editor::g_exportApproval.valid,"existing-file-without-approval-refused");
    Click("Write group");Check(Read(GroupPath())==bytes&&World()==before,"disabled-write-no-effects");
    Check(!editor::ProjectExportWrite()&&Read(GroupPath())==bytes,"bypassed-disabled-still-refused-by-producer");
    Click("Cancel export");Check(Read(GroupPath())==bytes,"declined-overwrite-byte-identical");
    Click("Overwrite","kit",true);Click("Approve existing-file overwrite");Click("Preflight");Require(editor::g_exportApproval.valid,"explicit-approval-preflight");
    Click("Write group");Check(Read(GroupPath())!=bytes,"approved-overwrite-writes");Capture("export-approved");
}
void Empty() {
    Reset();Name("empty");Click("Export Selection");const auto before=World();Click("Preflight");
    Check(!editor::g_exportApproval.valid&&before==World(),"empty-selection-no-preflight-mutation");
    Check(!editor::ProjectExportWrite()&&Read(dir+"\\Groups\\empty.cdgroup").empty(),"empty-producer-write-refused");
    GroupFile({"/object/missing.prefab"});Click("Cancel export");Click("Refresh");Frame();Click("Place","kit",true);
    Require(!editor::g_projectPlacements.empty(),"all-excluded-retains-report");auto r=editor::g_projectPlacements.back().request;Equations(r);
    Check(core::PlaceRequestState(r).excluded==1&&!editor::g_place.active,"all-excluded-no-false-grab");Capture("all-excluded");
}
void Reports() {
    Reset();GroupFile({good,"/object/fail.prefab","/object/missing.prefab","/object/pending.prefab","/object/pending.prefab"});Frame();Click("Place","kit",true);
    Require(editor::g_projectPlacements.size()==1,"one-request-not-basename-map");auto r=editor::g_projectPlacements[0].request;
    Equations(r);Check(core::PlaceRequestState(r).requested==5&&core::PlaceRequestState(r).excluded==1,"fixed-requested-with-named-preflight-exclusion");
    Require(host::PumpGame(),"release-first-attach");Require(host::PumpGame(),"release-second-engine-failure");Frame();auto v=core::PlaceRequestState(r);
    Check(v.attached==1&&v.failed==1&&v.pending==2&&!v.settled,"partial-actual-engine-results");
    Check(!v.rows[1].reason.empty()&&!v.rows[2].reason.empty()&&v.rows[1].reason!=v.rows[2].reason,"engine-failure-not-missing-prefab");Equations(r);Capture("reports-pending");
    Click("Cancel placement");v=core::PlaceRequestState(r);Equations(r);
    Check(v.requestCanceled&&v.cleanupPending&&!v.settled&&v.attached==1&&v.canceled==2&&v.excluded==1&&v.failed==1,"cancel-retains-attached-receipt-pending-cleanup");Capture("reports-cleanup");
    Drain();Frame();v=core::PlaceRequestState(r);Equations(r);
    Check(v.settled&&v.rows[0].removedAfterAttach&&!v.cleanupPending&&v.attached==1,"cleanup-settles-without-rewriting-attachment");Capture("reports-final");
    screenW=480;Frame();Frame();Capture("reports-480x900");
    for(const char* label:{"Refresh","Export Selection","Rigid","Per-object","Start grounding","Cancel grounding","Place","Overwrite"}) {
        const auto item=FindItem(label);Check(item.box.Min.x>=0&&item.box.Max.x<=screenW,"narrow-control-within-horizontal-viewport");
    }
    SelectLibraryFile("home.cdproj");
    for(const char* label:{"Load","Reload","Save","Unload","Add unassigned","autoload"}) { const auto item=FindItem(label);Check(item.box.Min.x>=0&&item.box.Max.x<=screenW,"narrow-project-control-within-horizontal-viewport"); }
    screenW=1280;
}
void MissingPrefab() {
    Reset();GroupFile({good,"/object/missing.prefab"});Frame();Click("Place","kit",true);auto r=editor::g_projectPlacements.back().request;
    auto v=core::PlaceRequestState(r);Check(v.requested==2&&v.pending==1&&v.excluded==1&&v.rows[1].prefab=="/object/missing.prefab"&&!v.rows[1].reason.empty(),"missing-prefab-named-and-not-parse-error");
    Drain();Frame();Equations(r);Check(core::PlaceRequestState(r).attached==1&&editor::g_place.req==r&&!editor::g_place.haveCenter&&editor::g_place.prefabIdx==-1,"single-survivor-saved-pivot");Capture("missing-prefab");
}
void StaleResult() {
    Reset();Click("Place","kit",true);auto first=editor::g_projectPlacements.back().request;Click("Cancel placement");
    Click("Place","kit",true);auto second=editor::g_projectPlacements.back().request;Require(first!=second,"same-name-independent-handles");
    Click("Refresh");Frame();Check(editor::g_projectPlacements[0].request==first&&editor::g_projectPlacements[1].request==second,"refresh-preserves-identities");
    Check(core::PlaceRequestState(second).pending==1,"refresh-remains-truthfully-pending");
    Click("drop");full=true;editor::g_playMode=false;editor::g_open=true;editor::g_mainTab=1;editor::g_selectMainTab=true;Frame();Frame();
    Click(ICON_FLOPPY_DISK " Project");Frame();Capture("full-project");
    Click(ICON_COPY " dock");Frame();Check(editor::g_compact,"actual-shell-dock-switch");Capture("dock-retained");
    Click(ICON_LIST " full editor");Frame();Check(!editor::g_compact,"actual-shell-full-switch");Click(ICON_FLOPPY_DISK " Project");Frame();
    Check(editor::g_projectPlacements[0].request==first&&editor::g_projectPlacements[1].request==second,"shell-switch-keeps-same-request-handles");
    Check(core::PlaceRequestState(second).pending==1,"shell-switch-remains-truthfully-pending");Equations(first);Equations(second);Capture("full-return");full=false;
    Drain();Frame();Check(core::PlaceRequestState(first).canceled==1&&core::PlaceRequestState(second).attached==1,"late-first-cannot-steal-second");
    editor::Undo();Drain();Frame();Check(core::PlaceRequestState(second).attached==1,"undo-does-not-rewrite-attachment-receipt");
}
void MalformedReplace() {
    Reset();Click("Load","home");Drain();int a=Spawn();Select({a});editor::Act act;act.kind=editor::Act::Spawn;act.uid=a;editor::Push({act});
    const auto before=World();const auto file=Read(ProjectPath());Write(ProjectPath(),file+std::string(good)+"|bad|1|0\n");
    Click("Reload","home");Check(World()==before,"malformed-Reload-preserves-scene-history-selection-queue-allocator");
    Check(!editor::DispatchProjectAction({"home"},editor::ProjectAction::Replace),"malformed-Replace-production-dispatch-refused");
    Check(World()==before,"malformed-last-row-preserves-scene-history-selection-queue-allocator");
    Check(!editor::g_projectStatus.empty(),"malformed-reason-visible");Capture("malformed-replace");
}
void StaleApproval() {
    Reset();int a=Spawn();Select({a});Click("Overwrite","kit",true);Click("Approve existing-file overwrite");Click("Preflight");const auto before=Read(GroupPath());
    Require(editor::g_exportApproval.valid,"preflight-before-live-move");core::MoveMany({{a,{4,20,0},{},1}},false);Drain();Click("Write group");
    Check(Read(GroupPath())==before&&!editor::g_exportApproval.valid,"live-move-rejects-stale-ui-approval");
    Click("Preflight");Require(editor::g_exportApproval.valid,"preflight-before-selection-change");Select({});Click("Write group");Check(Read(GroupPath())==before,"same-frame-selection-published-before-write");
}
void GroundModes() {
    Reset();Check(Has("Rigid"),"rigid-control");Check(Has("Per-object"),"per-object-control");Check(Has("Start grounding"),"explicit-start-control");
    int a=Spawn(0,10),b=Spawn(20,15);Select({a,b});const int castBefore=casts;Frame();Frame();Check(editor::g_pendingGround.empty()&&casts==castBefore,"never-automatic-grounding");
    Click("Start grounding");Require(editor::g_pendingGround.size()==1,"rigid-one-operation");auto rigid=editor::g_pendingGround.front();
    Check(core::GroundStateOf(rigid).members.size()==2&&core::GroundStateOf(rigid).state==core::GroundProbing,"rigid-pending-authority");Capture("ground-pending");
    host::PumpPhysics();Frame();Check(core::GroundStateOf(rigid).state==core::GroundQueued,"queued-not-final");Drain();Frame();
    Check(core::GroundStateOf(rigid).reason=="applied"&&std::fabs(Rec(a).pos.y-1)<.001f&&std::fabs(Rec(b).pos.y-6)<.001f,"rigid-shared-Y-actual");
    core::MoveMany({{a,{0,10,0},{},1},{b,{20,15,0},{},1}},true);Drain();
    Click("Per-object");Click("Start grounding");Require(editor::g_pendingGround.size()==2,"per-object-two-operations");auto per=editor::g_pendingGround;
    host::PumpPhysics(true);Frame();Drain();Frame();
    Check(core::GroundStateOf(per[0]).reason=="applied"&&std::fabs(Rec(a).pos.y-1)<.001f&&std::fabs(Rec(b).pos.y-4)<.001f,"per-object-independent-Y-actual");Capture("ground-settled");
    Click("Start grounding");auto canceled=editor::g_pendingGround;Click("Cancel grounding");
    for(const auto& op:canceled)Check(core::GroundStateOf(op).reason=="canceled","explicit-ground-cancel-reason");Check(host::GroundLeaseCount()==0,"cancel-no-leases");Capture("ground-canceled");
}
void MouseCancelIdentity() {
    Reset();Click("Place","kit",true);auto first=editor::g_place.req;
    Click("drop");Check(!editor::Placing()&&core::PlaceRequestState(first).pending==1,"drop-retains-pending-request");
    Click("Place","kit",true);auto second=editor::g_place.req;
    Require(first!=second,"mouse-two-independent-requests");
    Click("Cancel placement","",false,first);
    Check(editor::g_place.active&&editor::g_place.req==second&&!core::PlaceRequestState(second).requestCanceled,"older-request-cancel-not-current-carry");
    Click("Cancel");Check(!editor::Placing()&&core::PlaceRequestState(second).requestCanceled,"mouse-bar-cancels-current-only");
    Drain();Frame();Equations(first);Equations(second);
    bool live=false;for(const auto& o:objects)live|=o.second.live;
    Check(!live&&core::PlaceRequestState(first).canceled==1&&core::PlaceRequestState(second).canceled==1,"queued-cancel-no-late-survivors");
    editor::Undo();editor::Redo();Drain();Frame();live=false;for(const auto& o:objects)live|=o.second.live;
    Check(!live,"undo-redo-cannot-revive-forgotten-cancel");
    Check(editor::g_projectPlacements.size()==2,"both-canceled-reports-retained");Capture("mouse-cancel");
}
void FailedPreflightReasons() {
    Reset();int a=Spawn(),b=Spawn(4);Select({a,b});core::HideUid(a);core::ForgetUid(b);Drain();
    Name("excluded");Click("Export Selection");Click("Preflight");
    const auto& approval=editor::g_exportApproval;
    Check(!approval.valid&&approval.included.empty()&&approval.excluded.size()==2,"all-excluded-export-reasons-retained");
    bool hidden=false,forgotten=false;for(const auto& row:approval.excluded){hidden|=row.uid==a&&!row.reason.empty();forgotten|=row.uid==b&&!row.reason.empty();}
    Check(hidden&&forgotten,"failed-preflight-identifies-each-selected-row");Capture("failed-preflight-reasons");
    Click("Write group");Check(Read(dir+"\\Groups\\excluded.cdgroup").empty(),"all-excluded-no-file");
}
void UiFileFaults() {
    Reset();int a=Spawn();Select({a});const auto original=Read(GroupPath());
    Click("Overwrite","kit",true);Click("Approve existing-file overwrite");
    for(auto fault:{host::SaveFault::ShortWrite,host::SaveFault::FlushAbort,host::SaveFault::CloseAbort,host::SaveFault::ReplaceAbort}) {
        Click("Preflight");Require(editor::g_exportApproval.valid,"ui-fault-fresh-approval");const auto before=World();
        host::SetSaveFault(fault);Click("Write group");
        Check(Read(GroupPath())==original&&World()==before&&!editor::g_exportApproval.valid,"ui-fault-keeps-original-scene-and-clears-approval");
        WIN32_FIND_DATAA fd;HANDLE h=FindFirstFileA((dir+"\\Groups\\wbv*").c_str(),&fd);
        Check(h==INVALID_HANDLE_VALUE,"ui-fault-no-owned-temp");if(h!=INVALID_HANDLE_VALUE)FindClose(h);
    }
    host::SetSaveFault(host::SaveFault::None);Click("Preflight");Click("Write group");Check(Read(GroupPath())!=original,"ui-recovery-new-preflight-writes");
}
void ProjectHistoryScope() {
    Reset();Click("Load","home");Drain();int a=core::Spawned().back().uid;
    Write(dir+"\\projects\\other.cdproj",std::string(good)+"|30|10|0\n");Click("Refresh");Click("Load","other");Drain();
    int b=core::Spawned().back().uid;const auto otherBytes=Read(dir+"\\projects\\other.cdproj");
    Select({a});editor::DeleteSel();Drain();editor::Undo();Drain();
    Check(!Rec(a).hidden&&Rec(a).proj==core::ProjectId("home")&&Rec(b).proj==core::ProjectId("other"),"undo-preserves-separate-projects");
    int fresh=Spawn(8);Click("Add unassigned","home");
    Check(Rec(fresh).proj==core::ProjectId("home")&&Rec(b).proj==core::ProjectId("other")&&Read(dir+"\\projects\\other.cdproj")==otherBytes,"saveback-never-adopts-other-project");
    editor::Redo();Drain();Check(Rec(a).hidden&&!Rec(b).hidden,"redo-only-own-record");
    editor::Undo();Drain();Check(!Rec(a).hidden&&Rec(a).proj==core::ProjectId("home"),"undo-keeps-saveback-adoption");
    Write(dir+"\\projects\\inactive.cdproj",std::string(good)+"|99|10|0\n");Click("Refresh");
    const auto inactive=Read(dir+"\\projects\\inactive.cdproj");Name("inactive");Click(ICON_FLOPPY_DISK " Save whole scene as project");
    Check(Read(dir+"\\projects\\inactive.cdproj")==inactive&&!editor::g_projectOverwriteName.empty(),"inactive-project-needs-explicit-approval");
    Click("Cancel overwrite");Check(Read(dir+"\\projects\\inactive.cdproj")==inactive,"project-overwrite-cancel-no-loss");
    Name("inactive");Click(ICON_FLOPPY_DISK " Save whole scene as project");Click("Approve project overwrite");
    Check(Read(dir+"\\projects\\inactive.cdproj")!=inactive,"project-explicit-overwrite-writes");
}
void DropFailedRows() {
    Reset();GroupFile({good,"/object/fail.prefab"});Click("Place","kit",true);const auto request=editor::g_place.req;
    Drain();Frame();const auto before=core::PlaceRequestState(request);Require(before.attached==1&&before.failed==1,"drop-fixture-mixed-result");
    Click("drop");Drain();Frame();
    for(const auto& entry:editor::g_undo)for(const auto& act:entry.acts)
        printf("HISTORY serial=%llu kind=%d uid=%d attachedUid=%d failedUid=%d\n",static_cast<unsigned long long>(entry.serial),static_cast<int>(act.kind),act.uid,before.rows[0].uid,before.rows[1].uid);
    Check(editor::g_undo.size()==1&&editor::g_undo.back().acts.size()==1&&editor::g_undo.back().acts[0].uid==before.rows[0].uid,"drop-history-excludes-failed-row");
    Equations(request);Check(core::PlaceRequestState(request).failed==1,"drop-does-not-relabel-failure");
    editor::Undo();Drain();const int beforeRedo=creates;editor::Redo();Drain();
    Check(creates==beforeRedo+1&&core::PlaceRequestState(request).failed==1,"redo-never-retries-a-failed-placement-row");
}
void PaddedProjectOverwrite() {
    Reset();
    const std::string destination=dir+"\\projects\\inactive.cdproj", other=dir+"\\projects\\other.cdproj";
    const std::string original=std::string(good)+"|99|10|0\n", otherOriginal=std::string(good)+"|199|10|0\n";
    Write(destination,original);Write(other,otherOriginal);const int uid=Spawn(8);
    const auto before=World();Write(dir+"\\original.bytes",Read(destination));
    Name(" inactive ");Click(ICON_FLOPPY_DISK " Save whole scene as project");
    Write(dir+"\\after-save.bytes",Read(destination));
    Check(Read(destination)==original,"padded-save-preserves-existing-bytes-before-approval");
    Check(World()==before,"padded-save-no-adoption-before-approval");
    Require(editor::g_projectOverwriteName=="inactive"&&Has("Approve project overwrite"),"padded-save-captures-resolved-destination");
    Capture("padded-overwrite-prompt");
    Click("Cancel overwrite");Write(dir+"\\after-cancel.bytes",Read(destination));
    Check(editor::g_projectOverwriteName.empty()&&Read(destination)==original&&World()==before,"padded-cancel-preserves-file-and-scene");
    Name(" inactive ");Click(ICON_FLOPPY_DISK " Save whole scene as project");
    Require(editor::g_projectOverwriteName=="inactive"&&Read(destination)==original,"padded-second-save-still-requires-approval");
    // Approval owns the captured resolved destination, not a later edit of the Save As text box.
    Name(" other ");Check(editor::g_projectOverwriteName=="inactive","padded-confirmation-not-retargeted-by-text-edit");
    Capture("padded-overwrite-changed-input");Click("Approve project overwrite");
    const auto saved=Read(destination);Write(dir+"\\after-approval.bytes",saved);
    proj_codec::Document doc;std::string error;
    Check(saved!=original&&proj_codec::Parse(saved,destination,proj_codec::Kind::Project,doc,error)&&doc.records.size()==1&&doc.records[0].pos.x==8,"padded-explicit-approval-writes-intended-project");
    Check(Read(other)==otherOriginal,"padded-approval-does-not-write-edited-name");
    Check(editor::g_projectOverwriteName.empty()&&std::string(editor::g_projName)=="inactive"&&Rec(uid).proj==core::ProjectId("inactive"),"padded-approved-name-and-membership-agree");
    Check(GetFileAttributesA((dir+"\\projects\\ inactive .cdproj").c_str())==INVALID_FILE_ATTRIBUTES,"padded-name-not-a-second-file");
}
void ProjectNameWhitespaceBoundary() {
    Reset();const std::string destination=dir+"\\projects\\inactive.cdproj";
    const std::string original=std::string(good)+"|99|10|0\n";Write(destination,original);Spawn(8);
    const auto before=World();
    // InputText filters Tab; exercise the same production action directly for core's tab-trim rule.
    for(const char* name:{"\tinactive\t"," \t inactive \t ","inactive"}) {
        Check(!editor::SaveProjectAction(name,core::SaveWholeScene),"whitespace-boundary-requires-approval");
        Check(Read(destination)==original&&World()==before,"whitespace-boundary-preserves-bytes-and-scene");
        Require(editor::g_projectOverwriteName=="inactive","whitespace-boundary-canonical-confirmation");
        Click("Cancel overwrite");
    }
    Check(!editor::SaveProjectAction(" \t ",core::SaveWholeScene)&&editor::g_projectOverwriteName.empty(),"blank-name-refused-without-approval");
    Check(Read(destination)==original&&World()==before,"blank-name-does-not-mutate");
}
void SceneProject(int pid) {
    sceneProjects=true;Frame();Frame();
    const std::string suffix="###pt"+std::to_string(pid);
    std::string label;
    for(const auto& entry:items) { const auto& text=entry.second.label;
        if(text.size()>=suffix.size()&&text.compare(text.size()-suffix.size(),suffix.size(),suffix)==0) {label=text;break;} }
    Require(!label.empty(),"scene-project-tab-found");Click(label);
    Require(editor::g_projTab==pid,"actual-scene-project-tab-selected");
}
void LoadedPaddedSave(bool scene, bool dirty) {
    Reset();
    const std::string paddedName=" inactive ", canonicalName="inactive";
    const std::string paddedPath=dir+"\\projects\\"+paddedName+".cdproj", canonicalPath=dir+"\\projects\\inactive.cdproj";
    const std::string paddedBytes=std::string(good)+"|10|10|0\n", canonicalBytes=std::string(good)+"|100|10|0\n";
    Write(paddedPath,paddedBytes);Write(canonicalPath,canonicalBytes);Click("Refresh");
    Click("Load",paddedName);Drain();const int paddedUid=core::Spawned().back().uid;
    Click("Load",canonicalName);Drain();const int canonicalUid=core::Spawned().back().uid;
    const int paddedPid=Rec(paddedUid).proj, canonicalPid=Rec(canonicalUid).proj;
    const int fresh=Spawn(8);Click("Refresh"); // rebuild the cached library snapshot after the New object exists, so cached row actions observe it
    Require(paddedPid!=canonicalPid&&core::ProjectNameOf(paddedPid)==paddedName&&Rec(fresh).proj==0,"two-distinct-loaded-identities-and-new");
    if(dirty) {Require(core::MoveMany({{paddedUid,{15,10,0},{},1}},true),"dirty-padded-move-admitted");Drain();}
    if(scene) SceneProject(paddedPid);
    const auto before=World();const bool paddedDirty=core::ProjectDirty(paddedPid), canonicalDirty=core::ProjectDirty(canonicalPid);
    Write(dir+"\\padded-before.bytes",Read(paddedPath));Write(dir+"\\canonical-before.bytes",Read(canonicalPath));Write(dir+"\\before.world",before);
    editor::g_projectStatus.clear();
    if(scene) Click(dirty?ICON_FLOPPY_DISK " Save the changes":ICON_FLOPPY_DISK " Overwrite this project");
    else Click("Add unassigned",paddedName);
    Write(dir+"\\padded-after.bytes",Read(paddedPath));Write(dir+"\\canonical-after.bytes",Read(canonicalPath));Write(dir+"\\after.world",World());
    Check(Read(paddedPath)==paddedBytes,"padded-source-bytes-preserved");
    Check(Read(canonicalPath)==canonicalBytes,"canonical-neighbor-bytes-preserved");
    Check(Rec(paddedUid).proj==paddedPid&&Rec(canonicalUid).proj==canonicalPid&&Rec(fresh).proj==0,"refused-save-no-retarget-or-new-adoption");
    Check(World()==before&&core::ProjectDirty(paddedPid)==paddedDirty&&core::ProjectDirty(canonicalPid)==canonicalDirty,"refused-save-world-and-dirty-unchanged");
    Check(!editor::g_projectStatus.empty()&&editor::g_projectOverwriteName.empty(),"refusal-reason-without-retarget-approval");
    Capture(scene?(dirty?"padded-scene-dirty-refused":"padded-scene-clean-refused"):"padded-row-saveback-refused");
    if(scene) {
        SceneProject(canonicalPid);Click(ICON_FLOPPY_DISK " Overwrite this project");
        proj_codec::Document saved;std::string error;
        Check(proj_codec::Parse(Read(canonicalPath),canonicalPath,proj_codec::Kind::Project,saved,error)&&saved.records.size()==1&&saved.records[0].pos.x==100,"canonical-scene-save-only-own-record");
        Check(Rec(fresh).proj==0&&Read(paddedPath)==paddedBytes,"canonical-scene-save-keeps-new-and-padded-source");
    }
    sceneProjects=false;Frame();Click("Add unassigned",canonicalName);
    proj_codec::Document saved;std::string error;
    Check(proj_codec::Parse(Read(canonicalPath),canonicalPath,proj_codec::Kind::Project,saved,error)&&saved.records.size()==2&&saved.records[0].pos.x==100&&saved.records[1].pos.x==8,"canonical-saveback-still-writes-own-plus-new");
    Check(Rec(paddedUid).proj==paddedPid&&Rec(canonicalUid).proj==canonicalPid&&Rec(fresh).proj==canonicalPid&&Read(paddedPath)==paddedBytes,"canonical-saveback-adopts-only-new");
    if(!scene) {
        const auto beforeChoice=Read(canonicalPath);const auto worldBeforeChoice=World();
        Name("inactive");Click(ICON_FLOPPY_DISK " Save whole scene as project");
        Check(editor::g_projectOverwriteName==canonicalName&&Read(canonicalPath)==beforeChoice&&World()==worldBeforeChoice,"explicit-canonical-top-save-still-needs-separate-approval");
        Click("Cancel overwrite");Check(Read(canonicalPath)==beforeChoice&&Read(paddedPath)==paddedBytes&&World()==worldBeforeChoice,"explicit-destination-cancel-preserves-both-identities");
    }
}
std::string KeyLabel(const std::string& key) {
    const auto id=ImHashStr(("###"+key).c_str(),0,ImHashStr("project-page"));
    const auto found=items.find(id);if(found==items.end())throw std::runtime_error("missing disclosure: "+key);return found->second.label;
}
void ExportActionState() {
    Reset();const int a=Spawn();Select({a});Name("fresh");Click("Export Selection");Click("Preflight");
    Require(editor::g_exportApproval.valid&&editor::g_exportStatus=="preflight-ready","action-preflight-ready");
    Name("renamed");Check(!editor::g_exportApproval.valid&&FindItem("Write group").disabled&&editor::g_exportStatus!="preflight-ready","name-change-clears-ready-before-write");
    Click("Preflight");Require(editor::g_exportApproval.valid,"name-change-new-preflight");
    core::MoveMany({{a,{4,20,0},{},1}},false);Drain();Frame();
    Check(!editor::g_exportApproval.valid&&FindItem("Write group").disabled&&editor::g_exportStatus!="preflight-ready","pose-change-clears-ready-before-write");
    Click("Preflight");Click("Export Selection");Check(!editor::g_exportApproval.valid&&editor::g_exportStatus.empty(),"reset-clears-active-ready");
    Click("Preflight");Click("Cancel export");Check(!editor::g_exportOpen&&!editor::g_exportApproval.valid&&editor::g_exportStatus.empty(),"cancel-clears-active-ready");
    // The actual Write button reaches core's guarded replacement; inject a live pose change at that boundary.
    Click("Export Selection");Click("Preflight");bool boundary=false;
    host::SetBeforeReplace([&](){boundary=true;core::MoveMany({{a,{8,22,0},{},1}},false);Drain();});
    Click("Write group");host::SetBeforeReplace({});
    Check(boundary&&!editor::g_exportApproval.valid&&editor::g_exportStatus!="preflight-ready"&&Read(dir+"\\Groups\\renamed.cdgroup").empty(),"actual-write-revalidates-at-replace-boundary");
    Require(!editor::g_exportAttempts.back().result.empty(),"write-failure-retained-in-attempt");const auto failed=editor::g_exportAttempts.back();
    Click("Preflight");Click("Write group");Check(!editor::g_exportApproval.valid&&editor::g_exportStatus=="written"&&!Read(dir+"\\Groups\\renamed.cdgroup").empty(),"write-success-clears-ready");
    Click("Cancel export");Click(KeyLabel("export-attempts"));
    Check(editor::g_projectDetails.count("export-attempts")&&!failed.result.empty()&&editor::g_exportAttempts[editor::g_exportAttempts.size()-2].result==failed.result,"prior-write-failure-accessible-after-success-and-cancel");
    Click(KeyLabel("export-"+std::to_string(editor::g_exportAttempts.size()-1)));Capture("export-prior-failure-detail");
    const auto history=World();Click(KeyLabel("export-attempts"));Click(KeyLabel("export-attempts"));Check(World()==history,"export-disclosure-readonly");
}
void MalformedAfterSuccess() {
    Reset();Click("Read","kit",true);Require(editor::g_projectReadValid&&!editor::g_projectDetails.count("read-preview"),"named-read-preview-collapsed-by-default");
    const auto previewId=ImHashStr("###read-preview",0,ImHashStr("project-page"));
    Click(KeyLabel("read-preview"));Check(editor::g_projectDetails.count("read-preview")&&editor::g_projectReadPath==GroupPath(),"read-preview-opens-exact-file");
    Click(KeyLabel("read-preview"));Click("Place","kit",true);const auto request=editor::g_place.req;Drain();Frame();Click("drop");
    const auto before=World();const auto prior=editor::g_placementReports.Snapshot();
    Write(GroupPath(),"not a valid group\n");Click("Place","kit",true);
    Check(World()==before&&editor::g_placementReports.Snapshot().order.newest==prior.order.newest&&core::PlaceRequestState(request).attached==1,"malformed-attempt-no-fake-admission-or-world-change");
    Require(editor::g_projectFileFailures.size()==1&&!editor::g_projectStatus.empty(),"malformed-new-attempt-recorded-after-success");
    const auto reason=editor::g_projectFileFailures.back().reason;
    Check(editor::g_projectFileFailures.back().path==GroupPath()&&reason==editor::g_projectStatus,"malformed-exact-path-and-core-reason-retained");
    Click(KeyLabel("file-attempts"));screenW=480;Frame();Frame();Capture("malformed-after-success-480");
    Check(editor::g_projectDetails.count("file-attempts"),"malformed-details-expanded");
    Click("Read","kit",true);Check(!editor::g_projectReadValid&&items.count(previewId)==0,"malformed-read-does-not-show-stale-success-preview");
    GroupFile({good});Click("Read","kit",true);Check(editor::g_projectReadValid&&editor::g_projectFileFailures.front().reason==reason,"new-read-preserves-prior-failure-details");
}
void LibraryInput(const char* value) {
    Click("##library-search");auto& io=ImGui::GetIO();io.AddKeyEvent(ImGuiMod_Ctrl,true);io.AddKeyEvent(ImGuiKey_A,true);Frame();
    io.AddKeyEvent(ImGuiKey_A,false);io.AddKeyEvent(ImGuiMod_Ctrl,false);Frame();io.AddInputCharactersUTF8(value);Frame();
    if(!*value){io.AddKeyEvent(ImGuiKey_Backspace,true);Frame();io.AddKeyEvent(ImGuiKey_Backspace,false);Frame();}
    io.AddKeyEvent(ImGuiKey_Enter,true);Frame();io.AddKeyEvent(ImGuiKey_Enter,false);Frame();
    Require(std::string(editor::g_librarySearch)==value,"library-real-search-input");
}
void LibraryCombo(const char* key,const char* choice) { Click(key);Click(choice); }
size_t LibraryDrawn() {
    std::vector<Item> rows;auto* child=LibraryWindow();
    for(const auto& p:items)if(p.second.window==child&&p.second.label.find("###saved-file")!=std::string::npos)rows.push_back(p.second);
    std::sort(rows.begin(),rows.end(),[](const Item& a,const Item& b){return a.box.Min.y<b.box.Min.y;});
    const float stride=ImGui::GetTextLineHeightWithSpacing();
    Check(rows.size()<=(size_t)ceilf(child->InnerClipRect.GetHeight()/stride)+2,"clipper-submits-only-viewport-rows");
    for(size_t i=1;i<rows.size();++i)Check(rows[i].box.Min.y>=rows[i-1].box.Max.y&&fabsf(rows[i].box.Min.y-rows[i-1].box.Min.y-stride)<0.1f,"uniform-nonoverlapping-row-stride");
    return rows.size();
}
void LibraryScale() {
    Reset();libraryOnly=true;screenW=480;
    const std::string projectBytes=Read(ProjectPath()),groupBytes=Read(GroupPath());
    for(const char* folder:{"projects","Groups"}) {
        const bool group=std::string(folder)=="Groups";const std::string ext=group?".cdgroup":".cdproj";
        CreateDirectoryA((dir+"\\"+folder+"\\.archive").c_str(),nullptr);
        for(bool archive:{false,true})for(int n=1023;n>=0;--n) {
            char stem[32];sprintf_s(stem,"item-%04d",n);
            Write(dir+"\\"+folder+"\\"+(archive?".archive\\":"")+stem+ext,group?groupBytes:projectBytes);
        }
    }
    Write(dir+"\\projects\\tie.cdproj",projectBytes);Write(dir+"\\projects\\Alpha.cdproj",projectBytes);
    Write(dir+"\\projects\\.archive\\Tie.cdproj",projectBytes);Write(dir+"\\projects\\.archive\\home.cdproj",projectBytes);
    Write(dir+"\\Groups\\.archive\\kit.cdgroup",groupBytes);
    const int a=Spawn(),hidden=Spawn();Spawn();const int pid=core::ProjectId("home");
    core::AssignProject(a,pid);core::AssignProject(hidden,pid);core::HideUid(hidden);Drain();
    const size_t records=core::Spawned().size();
    editor::g_projectRefresh=true;core::g_sceneEnumerationStats={};Frame();Frame();
    const auto refresh=core::g_sceneEnumerationStats;
    Require(refresh.calls==1&&refresh.records==records,"one-real-scene-enumeration-per-explicit-refresh");
    Check(editor::g_projectLibrary.active.projects==1027&&editor::g_projectLibrary.active.groups==1025&&editor::g_projectLibrary.archived.projects==1026&&editor::g_projectLibrary.archived.groups==1025,"all-four-library-totals-no-cap");
    Check(editor::g_projectLibrary.entries.size()==4103&&editor::g_libraryRows.size()==2052,"all-files-and-active-filter");
    const float height=LibraryWindow()->Size.y;const size_t drawn=LibraryDrawn();Require(drawn>0&&drawn<20,"large-library-actually-clipped");
    SelectLibraryFile("home.cdproj");const auto selected=editor::g_librarySelected;
    auto selectedIndex=[&](){for(size_t i=0;i<editor::g_projectLibrary.entries.size();++i)if(editor::SameSavedFile(editor::g_projectLibrary.entries[i].file,selected))return i;throw std::runtime_error("selected identity absent");};
    const size_t oldIndex=selectedIndex();const auto counts=editor::g_projectLibrary.entries[oldIndex].ownership;
    Check(counts.visible==1&&counts.hidden==1&&editor::g_projectLibrary.unassigned.visible==1,"cached-visible-hidden-and-unassigned-ownership");
    core::AssignProject(a,0);Write(dir+"\\projects\\000-first.cdproj",projectBytes);
    core::g_sceneEnumerationStats={};Frame();Frame();
    printf("ENUM render-only calls=%zu records=%zu\n",core::g_sceneEnumerationStats.calls,core::g_sceneEnumerationStats.records);
    Check(core::g_sceneEnumerationStats.calls==0&&editor::g_projectLibrary.entries.size()==4103&&editor::g_projectLibrary.entries[oldIndex].ownership.visible==1,"frames-do-not-poll-files-or-rescan-scene");
    editor::g_projectRefresh=true;Frame();Frame();
    printf("ENUM after-explicit-refresh calls=%zu records=%zu\n",core::g_sceneEnumerationStats.calls,core::g_sceneEnumerationStats.records);
    Check(core::g_sceneEnumerationStats.calls==1&&selectedIndex()==oldIndex+1&&editor::SameSavedFile(selected,editor::g_librarySelected),"refresh-shifts-index-not-exact-selected-file");
    Check(editor::g_projectLibrary.entries[selectedIndex()].ownership.visible==0&&editor::g_projectLibrary.unassigned.visible==2,"refresh-publishes-new-cached-ownership");
    core::g_sceneEnumerationStats={};
    LibraryCombo("##library-location","All locations");LibraryInput("tIe.CDPROJ");
    Require(editor::g_libraryRows.size()==2,"case-insensitive-full-filename-search");
    Check(editor::g_projectLibrary.entries[editor::g_libraryRows[0]].file.filename=="Tie.cdproj"&&editor::g_projectLibrary.entries[editor::g_libraryRows[0]].file.archived&&editor::g_projectLibrary.entries[editor::g_libraryRows[1]].file.filename=="tie.cdproj"&&!editor::g_projectLibrary.entries[editor::g_libraryRows[1]].file.archived,"exact-case-tie-precedes-active-location-tiebreak");
    LibraryCombo("##library-kind","Groups");Check(editor::g_libraryRows.empty(),"kind-filter-empty-not-wrong-file");
    LibraryInput("");Check(editor::g_libraryRows.size()==2050,"all-groups-kind-filter");
    LibraryCombo("##library-location","Archived");Check(editor::g_libraryRows.size()==1025,"archived-groups-filter");
    ImGui::SetScrollY(LibraryWindow(),LibraryWindow()->ScrollMax.y);Frame();Frame();LibraryDrawn();
    SelectLibraryFile("kit.cdgroup",true);Click("Read");Check(editor::g_projectReadPath==dir+"\\Groups\\.archive\\kit.cdgroup"&&!Has("Place")&&!Has("Overwrite"),"archived-read-exact-path-without-active-actions");
    LibraryCombo("##library-kind","Projects");Check(editor::g_libraryRows.size()==1026,"archived-projects-filter");
    LibraryCombo("##library-location","Active");Check(editor::g_libraryRows.size()==1028,"active-projects-filter-after-insertion");
    LibraryCombo("##library-kind","All kinds");Check(editor::g_libraryRows.size()==2053,"active-all-filter-after-insertion");
    LibraryCombo("##library-location","All locations");Check(editor::g_libraryRows.size()==4104,"all-kinds-all-locations-filter");
    LibraryInput("HOME");Require(editor::g_libraryRows.size()==2,"same-exact-name-in-two-locations");SelectLibraryFile("home.cdproj",true);const auto archived=editor::g_librarySelected;
    editor::g_projectRefresh=true;Frame();Frame();Check(editor::SameSavedFile(archived,editor::g_librarySelected)&&editor::g_librarySelected.archived,"refresh-retains-exact-archived-not-active-namesake");
    core::g_sceneEnumerationStats={};LibraryInput("no-matching-file");Check(editor::g_libraryRows.empty()&&LibraryDrawn()==0&&LibraryWindow()->Size.y==height,"empty-results-retain-fixed-height-and-submit-no-rows");
    Check(core::g_sceneEnumerationStats.calls==0,"search-filter-render-zero-scene-enumerations");
    // Deterministic refresh-failure injection (r2 flake fix): the previous mechanism renamed the real 2051-entry
    // Groups directory (an exclusive-subtree rename, transiently blockable by an external holder - the one-off
    // E/10 r2 1306/2 failure) and dropped a file named Groups in its place. The injected production fault is
    // unchanged - the active mod root's Groups path is not a directory, which OpenFileDirectory rejects as
    // UnsafePath - but the precondition is now created without renaming any occupied tree: a parked root holding
    // that file, selected through the existing host::SetModDir seam.
    const std::string parked=dir+"\\parked-root";
    const bool parkedRoot=CreateDirectoryA(parked.c_str(),nullptr)!=0;const DWORD parkError=parkedRoot?0:GetLastError();
    Require(parkedRoot,"park-synthetic-groups");
    Write(parked+"\\Groups","not a directory");
    const DWORD groupsAttr=GetFileAttributesA((parked+"\\Groups").c_str());
    printf("PARK root=%s parkError=%lu parkedGroupsAttr=0x%lX realGroupsAttr=0x%lX\n",parked.c_str(),(unsigned long)parkError,(unsigned long)groupsAttr,(unsigned long)GetFileAttributesA((dir+"\\Groups").c_str()));
    Require(groupsAttr!=INVALID_FILE_ATTRIBUTES&&(groupsAttr&FILE_ATTRIBUTE_DIRECTORY)==0,"park-groups-path-is-file");
    host::SetModDir(parked);editor::g_projectRefresh=true;Frame();Frame();
    Check(editor::g_libraryResult.reason==core::FileReason::UnsafePath&&editor::g_projectLibrary.entries.size()==4104&&editor::SameSavedFile(archived,editor::g_librarySelected),"failed-refresh-preserves-snapshot-selection-and-core-error");
    Check(core::g_sceneEnumerationStats.calls==0,"failed-refresh-no-scene-pass-or-automatic-retry");
    host::SetModDir(dir);
    editor::g_projectRefresh=true;Frame();Frame();Check(editor::g_libraryResult.ok(),"explicit-retry-clears-refresh-error");
    Require(DeleteFileA(archived.path.c_str())!=0,"remove-selected-synthetic-file");editor::g_projectRefresh=true;Frame();Frame();Check(editor::g_librarySelected.path.empty(),"missing-exact-selection-cleared-not-retargeted");
    Check(Read(ProjectPath())==projectBytes&&Read(GroupPath())==groupBytes,"browsing-preserves-fixture-document-bytes");
    Write(root+"\\library-scale.json","{\"activeProjects\":1027,\"activeGroups\":1025,\"archivedProjects\":1026,\"archivedGroups\":1025,\"rows\":4103,\"refreshScenePasses\":"+std::to_string(refresh.calls)+",\"refreshSceneRecords\":"+std::to_string(refresh.records)+",\"viewportRows\":"+std::to_string(drawn)+",\"height\":"+std::to_string(height)+",\"oldSelectedIndex\":"+std::to_string(oldIndex)+",\"newSelectedIndex\":"+std::to_string(oldIndex+1)+"}");
    libraryOnly=false;
}
std::vector<std::string> lifecycleAudit,lifecycleDeletes;
std::string Json(const std::string& s) { std::string out="\"";for(char c:s){if(c=='\\'||c=='\"')out+='\\';if(c=='\n')out+="\\n";else out+=c;}return out+'\"'; }
std::string LifecycleWorld() {
    std::ostringstream s;s<<World()<<std::hexfloat<<editor::g_historySerial<<'/'<<editor::g_historyBranch;
    for(const auto* stack:{&editor::g_undo,&editor::g_redo}) {s<<'[';for(const auto& entry:*stack){s<<entry.serial<<':'<<entry.branch;
        for(const auto& a:entry.acts)s<<a.kind<<':'<<a.uid<<':'<<a.proj<<':'<<a.prefab<<':'<<a.group<<':'<<a.group1<<':'<<a.pos0.x<<':'<<a.pos0.y<<':'<<a.pos0.z<<':'<<a.pos1.x<<':'<<a.pos1.y<<':'<<a.pos1.z<<':'<<a.rot0.yaw<<':'<<a.rot0.pitch<<':'<<a.rot0.roll<<':'<<a.rot1.yaw<<':'<<a.rot1.pitch<<':'<<a.rot1.roll<<':'<<a.sc0<<':'<<a.sc1;}s<<']';}
    for(const auto& o:core::Spawned())s<<o.rot.yaw<<':'<<o.rot.pitch<<':'<<o.rot.roll<<':'<<o.scale;
    s<<editor::g_primary<<':'<<editor::g_place.active<<':'<<editor::g_place.generation<<':'<<editor::g_place.center.x<<':'<<editor::g_place.center.y<<':'<<editor::g_place.center.z;
    return s.str();
}
std::string LibraryIdentity() {std::string s;for(const auto& row:editor::g_projectLibrary.entries)s+=row.file.path+'\n';return s;}
void FileType(const std::string& value) {
    Click("##file-confirm-name");auto& io=ImGui::GetIO();io.AddKeyEvent(ImGuiMod_Ctrl,true);io.AddKeyEvent(ImGuiKey_A,true);Frame();
    io.AddKeyEvent(ImGuiKey_A,false);io.AddKeyEvent(ImGuiMod_Ctrl,false);Frame();io.AddInputCharactersUTF8(value.c_str());Frame();
    io.AddKeyEvent(ImGuiKey_Enter,true);Frame();io.AddKeyEvent(ImGuiKey_Enter,false);Frame();
    Require(std::string(editor::g_libraryAction.typed)==value,"exact-text-typed-through-ImGui");
}
void DeletePrompt(const char* filename,bool archived=false) {
    SelectLibraryFile(filename,archived);
    if(archived)Click("Purge");else {Check(!Has("Permanent delete"),"direct-delete-not-default-action");Click("More file actions");Click("Permanent delete");}
    Require(Has("##file-confirm-name")&&core::SelectedFile(editor::g_libraryAction.selection).filename==filename,"separate-confirmation-owns-exact-selection");
}
void FileRefusal(const char* id,const char* button,core::FileReason expected,const std::string& path) {
    const auto bytes=Read(path),world=LifecycleWorld(),listing=LibraryIdentity();
    const auto selected=editor::g_librarySelected;const auto autoload=Read(dir+"\\autoload.txt");
    const std::string proof=dir+"\\proof-"+std::to_string(lifecycleAudit.size());Write(proof+"-before.bytes",bytes);
    const bool destructive=std::string(button)=="Confirm permanent delete";
    if(destructive&&expected!=core::FileReason::DeleteFailed)core::g_fileMutationFault=core::FileMutationFault::Delete;
    Click(button);Frame(); // native popup auto-fit lays out the added reason before inspecting it
    const auto result=editor::g_libraryAction.result;
    Check(!result.ok()&&result.reason==expected,"live-guard-machine-refusal");
    const bool exists=GetFileAttributesA(path.c_str())!=INVALID_FILE_ATTRIBUTES;
    Check(exists&&Read(path)==bytes,"refusal-original-bytes-preserved");
    Check(LibraryIdentity()==listing&&!editor::g_projectRefresh&&editor::SameSavedFile(selected,editor::g_librarySelected),"refusal-keeps-snapshot-and-selection");
    Check(LifecycleWorld()==world&&Read(dir+"\\autoload.txt")==autoload,"refusal-no-scene-history-or-autoload-change");
    std::vector<core::SavedFile> files;const bool group=path.find("\\Groups\\")!=std::string::npos,archive=path.find("\\.archive\\")!=std::string::npos;
    Require(core::ListSavedFiles(group?proj_codec::Kind::Group:proj_codec::Kind::Project,archive,files).ok(),"refusal-real-file-list-readable");
    const bool listed=std::any_of(files.begin(),files.end(),[&](const core::SavedFile& file){return file.path==path;});Check(listed,"refusal-source-still-discoverable");
    if(destructive&&expected!=core::FileReason::DeleteFailed)Check(core::g_fileMutationFault==core::FileMutationFault::Delete,"refusal-never-reached-delete-boundary");
    core::g_fileMutationFault=core::FileMutationFault::None;Write(proof+"-after.bytes",Read(path));
    lifecycleAudit.push_back("{\"case\":"+Json(current)+",\"id\":"+Json(id)+",\"source\":"+Json(path)+",\"expected\":"+Json(core::FileReasonCode(expected))+",\"actual\":"+Json(core::FileReasonCode(result.reason))+",\"before\":"+Json(proof+"-before.bytes")+",\"after\":"+Json(proof+"-after.bytes")+",\"listed\":"+(listed?"true":"false")+",\"worldUnchanged\":"+(LifecycleWorld()==world?"true":"false")+",\"snapshotUnchanged\":"+(LibraryIdentity()==listing?"true":"false")+"}");
}
void LifecycleWrongAndStale() {
    Reset();libraryOnly=true;DeletePrompt("home.cdproj");const auto selection=editor::g_libraryAction.selection;
    for(const char* wrong:{"home","HOME.cdproj","home.cdproj "}) {FileType(wrong);FileRefusal(wrong,"Confirm permanent delete",core::FileReason::ConfirmationMismatch,ProjectPath());}
    FileType("home.cdproj");const auto selected=editor::g_librarySelected;
    // Deterministic selection/index invalidation AFTER modal opening. The modal remains the real UI.
    for(const auto& row:editor::g_projectLibrary.entries)if(row.file.filename=="kit.cdgroup")editor::g_librarySelected=row.file;
    FileRefusal("selected-row-changed","Confirm permanent delete",core::FileReason::SelectionChanged,ProjectPath());editor::g_librarySelected=selected;
    size_t index=0;for(;index<editor::g_projectLibrary.entries.size();++index)if(editor::SameSavedFile(editor::g_projectLibrary.entries[index].file,selected))break;
    auto row=editor::g_projectLibrary.entries.at(index);editor::g_projectLibrary.entries[index].file=core::SavedFile{};
    FileRefusal("stale-row-index","Confirm permanent delete",core::FileReason::SelectionChanged,ProjectPath());editor::g_projectLibrary.entries[index]=row;
    const auto original=Read(ProjectPath());Write(ProjectPath(),original+"changed\n");
    FileRefusal("file-rewritten-after-prompt","Confirm permanent delete",core::FileReason::StaleTarget,ProjectPath());
    Check(editor::g_libraryAction.selection==selection,"confirmation-never-recaptures-stale-source");Click("Cancel file action");
}
void LifecycleReference(const std::string& mode) {
    Reset();libraryOnly=true;DeletePrompt("home.cdproj");FileType("home.cdproj");core::FileReason expected=core::FileReason::None;
    const int pid=core::ProjectId("home");
    if(mode=="autoload") {Require(core::SetAutoload("home",true).ok(),"enable-autoload-after-modal-open");expected=core::FileReason::AutoloadEnabled;}
    else if(mode=="pending") {Require(core::LoadProject("home",false),"admit-load-after-modal-open");expected=core::FileReason::PendingOperation;}
    else if(mode=="dirty") {int uid=Spawn();core::AssignProject(uid,pid);core::MoveMany({{uid,{8,10,0},{},1}},true);Drain();core::ForgetUid(uid);Require(core::ProjectDirty(pid),"dirty-only-reference-without-record");expected=core::FileReason::DirtyProject;}
    else if(mode=="deferred") {editor::g_deferredEditor.action=[](){};expected=core::FileReason::PendingOperation;}
    else {
        const int uid=Spawn();core::AssignProject(uid,pid);
        if(mode=="visible")expected=core::FileReason::VisibleReference;
        else if(mode=="hidden"){core::HideUid(uid);Drain();expected=core::FileReason::HiddenReference;}
        else if(mode=="ground"){editor::BeginGrounding({uid},true);expected=core::FileReason::PendingOperation;}
        else {
            editor::Act act;act.kind=editor::Act::Delete;act.uid=uid;act.proj=pid;editor::Push({act});
            if(mode=="adopted-undo"){editor::g_undo.back().acts[0].proj=0;expected=core::FileReason::UndoReference;}
            else {core::ForgetUid(uid);if(mode=="redo"){editor::g_redo=std::move(editor::g_undo);editor::g_undo.clear();expected=core::FileReason::RedoReference;}else expected=core::FileReason::UndoReference;}
        }
    }
    FileRefusal(mode.c_str(),"Confirm permanent delete",expected,ProjectPath());Click("Cancel file action");
    SelectLibraryFile("home.cdproj");FileRefusal((mode+"-archive").c_str(),"Archive",expected,ProjectPath());
    editor::g_deferredEditor={};editor::ReconcileGroundBatch(true);Drain();
}
void LifecycleAutoloadWrite() {
    Reset();libraryOnly=true;SelectLibraryFile("home.cdproj");Click("autoload");Require(core::Autoload()==std::vector<std::string>{"home"},"autoload-on-through-checkbox");
    Write(dir+"\\projects\\other.cdproj",std::string(good)+"|1|10|0\n");Require(core::SetAutoload("other",true).ok(),"unrelated-autoload-fixture");Click("autoload");
    Check(core::Autoload()==std::vector<std::string>{"other"},"explicit-checkbox-off-only-selected-project");Click("autoload");
    const auto original=Read(dir+"\\autoload.txt"),listing=LibraryIdentity();host::SetSaveFault(host::SaveFault::ReplaceAbort);Click("autoload");
    Check(Read(dir+"\\autoload.txt")==original&&LibraryIdentity()==listing&&std::find(editor::g_projectAutoload.begin(),editor::g_projectAutoload.end(),"home")!=editor::g_projectAutoload.end(),"failed-off-preserves-disk-and-cached-checkbox");
    Check(!editor::g_projectStatus.empty(),"failed-off-reason-visible");host::SetSaveFault(host::SaveFault::None);
    FileRefusal("failed-off-still-protected","Archive",core::FileReason::AutoloadEnabled,ProjectPath());Click("autoload");
    Check(core::Autoload()==std::vector<std::string>{"other"},"successful-explicit-off-reopened");Click("Archive");
    Check(editor::g_libraryAction.result.ok()&&Read(dir+"\\projects\\.archive\\home.cdproj")==std::string(good)+"|0|10|0\n","off-then-archive-succeeds");
}
void LifecycleMoves(bool group) {
    Reset();libraryOnly=true;const char* filename=group?"kit.cdgroup":"home.cdproj";
    const auto path=group?GroupPath():ProjectPath(),bytes=Read(path),neighbor=Read(group?ProjectPath():GroupPath());
    const auto archive=dir+(group?"\\Groups\\.archive\\":"\\projects\\.archive\\")+filename;
    const int uid=Spawn();Select({uid});editor::Act act;act.kind=editor::Act::Spawn;act.uid=uid;editor::Push({act});const auto world=LifecycleWorld();
    SelectLibraryFile(filename);core::g_fileMutationFault=core::FileMutationFault::Move;FileRefusal("archive-move-failure","Archive",core::FileReason::MoveFailed,path);
    Click("Archive");Require(editor::g_libraryAction.result.ok()&&Read(archive)==bytes&&GetFileAttributesA(path.c_str())==INVALID_FILE_ATTRIBUTES,"archive-exact-bytes-and-name");
    LibraryCombo("##library-location","Archived");SelectLibraryFile(filename,true);
    core::g_fileMutationFault=core::FileMutationFault::Move;FileRefusal("restore-move-failure","Restore",core::FileReason::MoveFailed,archive);
    Write(path,"existing destination");FileRefusal("restore-collision","Restore",core::FileReason::Collision,archive);Check(Read(path)=="existing destination","collision-never-replaces-destination");
    Require(DeleteFileA(path.c_str())!=0,"remove-only-synthetic-collision");Click("Restore");
    Require(editor::g_libraryAction.result.ok()&&Read(path)==bytes&&GetFileAttributesA(archive.c_str())==INVALID_FILE_ATTRIBUTES,"restore-exact-bytes-and-name");
    Write(dir+"\\roundtrip-before.bytes",bytes);Write(dir+"\\roundtrip-restored.bytes",Read(path));
    LibraryCombo("##library-location","Active");SelectLibraryFile(filename);Click("Archive");LibraryCombo("##library-location","Archived");DeletePrompt(filename,true);
    FileType("wrong");FileRefusal("purge-wrong-name","Confirm permanent delete",core::FileReason::ConfirmationMismatch,archive);
    FileType(filename);core::g_fileMutationFault=core::FileMutationFault::Delete;FileRefusal("purge-delete-failure","Confirm permanent delete",core::FileReason::DeleteFailed,archive);
    Click("Confirm permanent delete");Require(editor::g_libraryAction.result.ok()&&GetFileAttributesA(archive.c_str())==INVALID_FILE_ATTRIBUTES,"exact-purge-only-selected-file");
    Check(Read(group?ProjectPath():GroupPath())==neighbor&&LifecycleWorld()==world,"roundtrip-purge-keeps-neighbor-scene-and-full-history");
}
void LifecycleExactDelete(bool group) {
    Reset();libraryOnly=true;const char* filename=group?"kit.cdgroup":"home.cdproj";const auto path=group?GroupPath():ProjectPath();
    const auto neighborPath=group?ProjectPath():GroupPath(),neighbor=Read(neighborPath);
    const int uid=Spawn();Select({uid});editor::Act act;act.kind=editor::Act::Spawn;act.uid=uid;editor::Push({act});
    DeletePrompt(filename);FileType(filename);const auto selection=editor::g_libraryAction.selection;
    const std::string inserted=dir+"\\projects\\000-before.cdproj";Write(inserted,"inserted neighbor");editor::g_projectRefresh=true;Frame();Frame();
    Require(editor::g_libraryAction.selection==selection&&editor::g_librarySelected.filename==filename,"resort-preserves-immutable-target-not-row-index");
    const auto world=LifecycleWorld();Write(dir+"\\deleted-before.bytes",Read(path));Write(dir+"\\neighbor-before.bytes",neighbor);Click("Confirm permanent delete");
    Check(editor::g_libraryAction.result.ok()&&GetFileAttributesA(path.c_str())==INVALID_FILE_ATTRIBUTES&&Read(neighborPath)==neighbor&&Read(inserted)=="inserted neighbor","direct-delete-exact-inactive-file-only");
    Check(LifecycleWorld()==world&&editor::g_librarySelected.path.empty(),"direct-delete-no-scene-history-or-retarget");
    Write(dir+"\\neighbor-after.bytes",Read(neighborPath));Write(dir+"\\before.world",world);Write(dir+"\\after.world",LifecycleWorld());
    lifecycleDeletes.push_back("{\"case\":"+Json(current)+",\"source\":"+Json(path)+",\"sourceBefore\":"+Json(dir+"\\deleted-before.bytes")+",\"deleted\":"+(GetFileAttributesA(path.c_str())==INVALID_FILE_ATTRIBUTES?"true":"false")+",\"neighborBefore\":"+Json(dir+"\\neighbor-before.bytes")+",\"neighborAfter\":"+Json(dir+"\\neighbor-after.bytes")+",\"worldBefore\":"+Json(dir+"\\before.world")+",\"worldAfter\":"+Json(dir+"\\after.world")+"}");
    editor::Undo();Drain();Check(GetFileAttributesA(path.c_str())==INVALID_FILE_ATTRIBUTES,"scene-undo-does-not-restore-file");
}
void LifecyclePurgeLive() {
    Reset();libraryOnly=true;SelectLibraryFile("home.cdproj");Click("Archive");LibraryCombo("##library-location","Archived");DeletePrompt("home.cdproj",true);FileType("home.cdproj");
    const auto path=dir+"\\projects\\.archive\\home.cdproj";const int uid=Spawn();core::AssignProject(uid,core::ProjectId("home"));
    FileRefusal("purge-live-visible-after-modal","Confirm permanent delete",core::FileReason::VisibleReference,path);
    core::HideUid(uid);Drain();FileRefusal("purge-live-hidden-after-modal","Confirm permanent delete",core::FileReason::HiddenReference,path);Click("Cancel file action");
}
void LifecycleGroups() {
    Reset();Click("Place","kit",true);const auto request=editor::g_place.req;libraryOnly=true;
    DeletePrompt("kit.cdgroup");FileType("kit.cdgroup");FileRefusal("group-pending","Confirm permanent delete",core::FileReason::InFlightPlace,GroupPath());
    Drain();FileRefusal("group-carried-after-settlement","Confirm permanent delete",core::FileReason::InFlightPlace,GroupPath());Click("Cancel file action");
    editor::DropCarried();Drain();Frame();const auto world=LifecycleWorld();SelectLibraryFile("kit.cdgroup");Click("Archive");
    Check(editor::g_libraryAction.result.ok()&&LifecycleWorld()==world&&core::PlaceRequestState(request).attached==1,"archive-leaves-placed-copy-and-history");
    LibraryCombo("##library-location","Archived");DeletePrompt("kit.cdgroup",true);FileType("kit.cdgroup");Click("Confirm permanent delete");
    Check(editor::g_libraryAction.result.ok()&&LifecycleWorld()==world&&core::Spawned().size()==1,"purge-leaves-placed-copy-and-history");
    GroupFile({good});editor::g_projectRefresh=true;LibraryCombo("##library-location","Active");DeletePrompt("kit.cdgroup");FileType("kit.cdgroup");
    strcpy_s(editor::g_projName,"kit");editor::g_exportOpen=true;FileRefusal("group-export-pending","Confirm permanent delete",core::FileReason::InFlightExport,GroupPath());
    editor::g_exportOpen=false;Click("Confirm permanent delete");Check(editor::g_libraryAction.result.ok()&&LifecycleWorld()==world,"delete-after-export-cancel-keeps-copy");
}
void TerrainProjectActions() {
    Reset();
    const int target=core::ProjectId("home"), other=core::ProjectId("other-terrain");
    core::TerrainStroke stroke{};stroke.r=2;stroke.strength=1;stroke.x=10;
    core::TerrainReplaceProject(target,{stroke});stroke.x=30;core::TerrainReplaceProject(other,{stroke});
    stroke.x=50;core::TerrainReplaceProject(0,{stroke});editor::g_projectRefresh=true;
    Check(core::ProjectObjectCount(target)==1&&core::ProjectObjectCount(0)==1&&core::Spawned().empty()&&core::ManagedNpcs().empty(),"terrain-only-composite-count-without-object-or-NPC-records");
    Click("Add unassigned","home");
    proj_codec::Document document;std::string error;
    Require(proj_codec::Parse(Read(ProjectPath()),ProjectPath(),proj_codec::Kind::Project,document,error),"parse-real-terrain-ui-saveback");
    Check(document.records.empty()&&document.terrain.size()==2&&document.terrain[0].x==10&&document.terrain[1].x==50,"terrain-ui-saveback-scope-not-whole-scene");
    auto strokes=core::TerrainStrokes();
    Check(std::count_if(strokes.begin(),strokes.end(),[other](const auto& s){return s.proj==other&&s.x==30;})==1,"terrain-ui-saveback-preserves-unrelated-membership");
    stroke.x=70;core::TerrainReplaceProject(0,{stroke});Name("terrain-ui-new");
    Frame();Require(!FindItem("Save unassigned entities as project").disabled,"new-only-save-enabled-by-unassigned-terrain");
    Click("Save unassigned entities as project");
    const std::string path=dir+"\\projects\\terrain-ui-new.cdproj";
    Require(proj_codec::Parse(Read(path),path,proj_codec::Kind::Project,document,error),"parse-real-new-only-ui-terrain-save");
    Check(document.records.empty()&&document.terrain.size()==1&&document.terrain[0].x==70,"new-only-ui-saves-only-unassigned-stroke");
    Frame();Check(FindItem("Save unassigned entities as project").disabled,"new-only-save-disabled-after-last-new-stroke-adopted");
    Check(!FindItem(ICON_FLOPPY_DISK " Save whole scene as project").disabled,"whole-scene-save-still-enabled-by-owned-terrain");Capture("terrain-project-actions-desktop");
}
void Run(const char* name, const std::function<void()>& fn) {
    current=name;int before=assertions,failed=failures;
    try{fn();}catch(const std::exception& e){Check(false,e.what());}
    Trace("case-end");cases.push_back("{\"id\":\""+current+"\",\"assertions\":"+std::to_string(assertions-before)+",\"failures\":"+std::to_string(failures-failed)+"}");
}
}
void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData*) {
    if(id) {auto& item=items[id];item.id=id;item.box=bb;item.window=ctx->CurrentWindow;item.clip=ctx->CurrentWindow->ClipRect;item.disabled=(ctx->CurrentItemFlags & ImGuiItemFlags_Disabled)!=0;
        if(ctx->CurrentWindow)item.stack.assign(ctx->CurrentWindow->IDStack.begin(),ctx->CurrentWindow->IDStack.end());}
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags) {items[id].label=label;}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID id) {auto it=items.find(id);return it==items.end()?nullptr:it->second.label.c_str();}
int main(int argc, char** argv) {
    if(argc!=2)return 2;root=argv[1];host::SetModDir(root);host::OpenLog(root+"\\project_table.log");
    ImGui::CreateContext();GImGui->TestEngineHookItems=true;auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DeltaTime=1.0f/60;io.ConfigInputTrickleEventQueue=false;
    auto* font=io.Fonts->AddFontDefault();icons::Register(io.Fonts,font,13);io.Fonts->Build();icons::Paint(io.Fonts);
    unsigned char* pixels;int width,height;io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    Write(root+"\\atlas.rgba",std::string((char*)pixels,(size_t)width*height*4));
    Write(root+"\\atlas.json","{\"width\":"+std::to_string(width)+",\"height\":"+std::to_string(height)+"}");
    editor::ApplyStyle(1);Install();
    Run("ACTIONS",Actions);Run("REPORTS",Reports);Run("GROUND-MODES",GroundModes);Run("WRONG-KIND",WrongKind);
    Run("DECLINED-OVERWRITE",DeclinedOverwrite);Run("EMPTY",Empty);Run("MISSING-PREFAB",MissingPrefab);Run("STALE-RESULT",StaleResult);
    Run("MALFORMED-REPLACE",MalformedReplace);Run("STALE-APPROVAL",StaleApproval);
    Run("MOUSE-CANCEL-IDENTITY",MouseCancelIdentity);Run("FAILED-PREFLIGHT-REASONS",FailedPreflightReasons);Run("UI-FILE-FAULTS",UiFileFaults);
    Run("PROJECT-HISTORY-SCOPE",ProjectHistoryScope);Run("DROP-FAILED-ROWS",DropFailedRows);
    Run("PADDED-PROJECT-OVERWRITE",PaddedProjectOverwrite);Run("PROJECT-NAME-WHITESPACE",ProjectNameWhitespaceBoundary);
    Run("LOADED-PADDED-SAVEBACK",[]{LoadedPaddedSave(false,false);});
    Run("LOADED-PADDED-SCENE-CLEAN",[]{LoadedPaddedSave(true,false);});
    Run("LOADED-PADDED-SCENE-DIRTY",[]{LoadedPaddedSave(true,true);});
    Run("EXPORT-ACTION-STATE",ExportActionState);Run("MALFORMED-AFTER-SUCCESS",MalformedAfterSuccess);
    Run("LIBRARY-SCALE",LibraryScale);
    Run("LIFECYCLE-WRONG-STALE",LifecycleWrongAndStale);
    for(const char* mode:{"visible","hidden","pending","dirty","undo","redo","adopted-undo","ground","deferred","autoload"})Run((std::string("LIFECYCLE-REFERENCE-")+mode).c_str(),[=](){LifecycleReference(mode);});
    Run("LIFECYCLE-AUTOLOAD-WRITE",LifecycleAutoloadWrite);
    Run("LIFECYCLE-PROJECT-MOVES",[](){LifecycleMoves(false);});Run("LIFECYCLE-GROUP-MOVES",[](){LifecycleMoves(true);});
    Run("LIFECYCLE-DELETE-PROJECT",[](){LifecycleExactDelete(false);});Run("LIFECYCLE-DELETE-GROUP",[](){LifecycleExactDelete(true);});
    Run("LIFECYCLE-PURGE-LIVE",LifecyclePurgeLive);Run("LIFECYCLE-GROUP-COPIES",LifecycleGroups);
    Run("MAIN-TERRAIN-PROJECT-ACTIONS",TerrainProjectActions);
    Array("delete-audit.json",lifecycleAudit);Array("delete-success.json",lifecycleDeletes);
    Array("cases.json",cases);Array("action-trace.json",traces);
    editor::CancelCarried();editor::ClearSceneAction(false);Drain();host::PumpServer();Drain();
    printf("ASSERTIONS=%d\nFAILURES=%d\n",assertions,failures);ImGui::DestroyContext();return failures?1:0;
}
