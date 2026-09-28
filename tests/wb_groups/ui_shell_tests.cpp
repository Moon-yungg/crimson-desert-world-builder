// Task16: actual editor.cpp Draw, real ImGui Win32 backend/hidden HWND, production CORE queues.
// Item hooks observe boxes/IDs only; actions use ImGui mouse/key input. Native memory/thumbnail service
// boundaries are supplied by production_host. No sleeps, polling delays, substitute UI or action dispatcher.
#include "production_host.h"
// Availability bits are engine-discovery inputs, not substitute time/weather implementations.
#include "../../asi/cdmodkit/environment.cpp"
// OS boundary only: the owner's physical keyboard must not race a deterministic camera fixture.
static SHORT WINAPI UiShellAsyncKeyState(int) { return 0; }
#define GetAsyncKeyState UiShellAsyncKeyState
#include "../../asi/cdmodkit/editor.cpp"
#undef GetAsyncKeyState
#include "drawdata_capture.h"
#include <imgui_internal.h>
#include <imgui_impl_win32.h>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <imm.h>
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"imm32.lib")
#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"dwmapi.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"comdlg32.lib")
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);
namespace {
struct Item { ImGuiID id=0; ImRect box,clip; std::string label; ImGuiWindow* window=nullptr; bool disabled=false; };
std::map<ImGuiID,Item> items;
std::string root,current;std::vector<std::string> cases,traces,captures;
int assertions=0,failures=0,creates=0,removes=0,refreshes=0;uintptr_t handle=100;
HWND hwnd=nullptr;int width=1920,height=1080;bool routed=false,manualPolicy=false;
struct Message { UINT msg; WPARAM key; };
std::vector<Message> gameMessages;
std::vector<std::string> routes;
HANDLE recipientEvent=nullptr;UINT awaitedMsg=0;WPARAM awaitedKey=0;
bool checkIncompleteDelivery=false,incompleteWasPublished=false;
void Policy(bool mouse,bool keys,bool placing=false,bool text=false,bool hover=true,bool play=false){
    core::g_menuOpen=true;core::g_uiWantsMouse=mouse;core::g_uiWantsKeyboard=keys;core::g_uiTextInput=text;core::g_uiMouseOverUi=hover;
    input::PublishOwnership({true,mouse,keys,placing,text,hover,play});
}
std::map<ImTextureID,drawcapture::Texture> textures;
const char* browser=ICON_MAGNIFYING_GLASS " Browser";const char* scene=ICON_CUBE " Scene";
const char* project=ICON_FLOPPY_DISK " Project";const char* npc=ICON_LOCATION_DOT " NPCs";
const char* weather=ICON_CLOCK_ROTATE_LEFT " Time & Weather";
const char* terrain=ICON_CUBE " Terrain";const char* dockPlace=ICON_LOCATION_CROSSHAIRS " PLACE ";
const char* history=ICON_CLOCK_ROTATE_LEFT " History";const char* dock=ICON_COPY " dock";const char* full=ICON_LIST " full editor";
const char* prefab="/object/stone.prefab";
std::string J(const std::string& s){std::string o;for(char c:s){if(c=='\\'||c=='\"')o+='\\';if(c=='\n')o+="\\n";else o+=c;}return o;}
void Write(const std::string& p,const std::string& s){drawcapture::Bytes(p,s.data(),s.size());}
void Array(const char* name,const std::vector<std::string>& rows){std::string s="[";for(size_t i=0;i<rows.size();++i){if(i)s+=',';s+=rows[i];}Write(root+"\\"+name,s+"]");}
void Check(bool ok,const char* msg){++assertions;if(!ok)++failures;printf("CHECK %s %s/%s\n",ok?"PASS":"FAIL",current.c_str(),msg);core::Log("CHECK %s %s/%s",ok?"PASS":"FAIL",current.c_str(),msg);}
void Require(bool ok,const char* msg){Check(ok,msg);if(!ok)throw std::runtime_error(msg);}
LRESULT CALLBACK Wnd(HWND w,UINT m,WPARAM a,LPARAM b){
    if(routed){
        gameMessages.push_back({m,a});
        if(recipientEvent&&m==awaitedMsg&&a==awaitedKey)SetEvent(recipientEvent);
        if(checkIncompleteDelivery&&m==WM_KEYDOWN){
            input::RouteEvent pending[input::kRouteCapacity];const int n=input::TakeRouted(pending,input::kRouteCapacity);
            for(int i=0;i<n;++i)if(pending[i].msg==m&&pending[i].wParam==a)incompleteWasPublished=true;
        }
    } else if(ImGui_ImplWin32_WndProcHandler(w,m,a,b))return 1;
    return DefWindowProcW(w,m,a,b);
}
void Trace(const char* event){std::ostringstream s;s<<"{\"case\":\""<<current<<"\",\"event\":\""<<event<<"\",\"frame\":"<<ImGui::GetFrameCount()<<",\"dock\":"<<editor::g_compact<<",\"tab\":"<<editor::g_mainTab<<",\"shared\":"<<editor::g_compactPage<<",\"creates\":"<<creates<<",\"removes\":"<<removes<<",\"refreshes\":"<<refreshes<<",\"nextUid\":"<<host::NextUid()<<",\"undo\":"<<editor::g_undo.size()<<",\"redo\":"<<editor::g_redo.size()<<",\"placing\":"<<editor::g_place.active<<",\"camera\":"<<editor::g_cameraMode<<",\"selection\":"<<editor::g_sel.size()<<'}';traces.push_back(s.str());}
void Frame(){
    items.clear();ImGui_ImplWin32_NewFrame();auto& io=ImGui::GetIO();io.DisplaySize={(float)width,(float)height};io.DeltaTime=1.f/60;
    // Fixture clock advances by frame, never wall-clock waits. OS mouse sampling is disabled for scripted input.
    ImGui::NewFrame();editor::Draw();ImGui::Render();
    if(routed&&!manualPolicy){
        const bool edit=editor::IsOpen()&&!editor::PlayMode(),gizmo=editor::Placing()&&editor::MouseMode();
        Policy(edit||gizmo,edit,editor::Placing(),io.WantTextInput,io.WantCaptureMouse,editor::PlayMode());
    }
    Trace("draw");
}
bool Has(const std::string& label){for(const auto& p:items)if(p.second.label==label)return true;return false;}
Item Find(const std::string& label){for(const auto& p:items)if(p.second.label==label)return p.second;
    // BeginCombo does not emit ItemInfo in this ImGui revision; its ItemAdd still exposes the stable ID.
    if(label.rfind("##",0)==0)for(const auto& p:items)if(p.second.window&&(p.first==ImHashStr(label.c_str(),0,p.second.window->ID)||p.first==ImHashStr(label.c_str(),0,ImHashStr("project-page")))){auto item=p.second;item.label=label;return item;}
    std::vector<ImGuiID> scopes{ImHashStr("project-page")};
    // ItemAdd exposes clipped IDs before ItemInfo. Resolve actual production scopes, never guessed boxes.
    if(!editor::g_librarySelected.path.empty()){
        const auto& file=editor::g_librarySelected;const bool group=file.kind==proj_codec::Kind::Group;
        const auto name=file.filename.substr(0,file.filename.size()-(group?8:7));
        scopes.push_back(ImHashStr(name.c_str(),0,ImHashStr(group?"group":"project",0,ImHashStr("project-page"))));
    }
    for(const auto& pair:items)if(pair.second.window){
        const auto window=pair.second.window->ID;scopes.push_back(window);scopes.push_back(ImHashStr("placement-bar",0,window));
        for(int page=0;page<=editor::TabTerrain;++page)scopes.push_back(ImHashData(&page,sizeof page,window));
    }
    for(const auto scope:scopes){const auto item=items.find(ImHashStr(label.c_str(),0,scope));if(item!=items.end())return item->second;}
    throw std::runtime_error("missing control: "+label);
}
void Mouse(const Item& item,int button,bool down){auto& io=ImGui::GetIO();io.AddMousePosEvent(item.box.GetCenter().x,item.box.GetCenter().y);
    if(routed)SendMessageW(hwnd,button==0?(down?WM_LBUTTONDOWN:WM_LBUTTONUP):(down?WM_RBUTTONDOWN:WM_RBUTTONUP),down?(button==0?MK_LBUTTON:MK_RBUTTON):0,0);
    else io.AddMouseButtonEvent(button,down);
}
void Click(const std::string& label){
    Frame();Item item=Find(label);
    if(!item.clip.Contains(item.box)){
        ImGui::SetScrollFromPosY(item.window,item.box.Min.y-item.window->Pos.y,0.5f);Frame();Frame();item=Find(label);
    }
    Require(item.clip.Contains(item.box), ("click-control-unclipped: "+label).c_str());
    auto& io=ImGui::GetIO();io.AddMousePosEvent(item.box.GetCenter().x,item.box.GetCenter().y);Frame();item=Find(label);
    Mouse(item,0,true);Frame();Mouse(item,0,false);Frame();Frame();Trace("click");
}
ImGuiWindow* LibraryWindow() {
    for(auto* w:GImGui->Windows)if(w->Active&&w->ChildId==ImHashStr("saved-library",0,ImHashStr("project-page")))return w;
    throw std::runtime_error("library child missing");
}
void LibraryView() {
    Frame();const auto input=Find("##library-search");
    ImGui::SetScrollFromPosY(input.window,input.box.Min.y-input.window->Pos.y,0.f);Frame();Frame();
}
void SelectLibraryFile(const std::string& filename,bool archived=false) {
    Frame();auto* child=LibraryWindow();auto* parent=child->ParentWindow;
    ImGui::SetScrollY(parent,std::max(0.f,parent->Scroll.y+child->Pos.y-parent->InnerClipRect.Min.y-ImGui::GetStyle().ItemSpacing.y));Frame();Frame();
    size_t index=0;for(;index<editor::g_libraryRows.size();++index) { const auto& f=editor::g_projectLibrary.entries[editor::g_libraryRows[index]].file;if(f.filename==filename&&f.archived==archived)break; }
    Require(index<editor::g_libraryRows.size(),"library-target-in-filtered-snapshot");const auto file=editor::g_projectLibrary.entries[editor::g_libraryRows[index]].file;
    ImGui::SetScrollY(child,(float)index*ImGui::GetTextLineHeightWithSpacing());Frame();Frame();
    const auto id=ImHashStr("###saved-file",0,ImHashStr(file.path.c_str(),0,child->ID));
    // Resolve the parent viewport after native child/nav scrolling, then click the observed row.
    const auto box=items.at(id).box;ImGui::SetScrollFromPosY(parent,box.Min.y-parent->Pos.y,0.25f);Frame();Frame();const Item item=items.at(id);
    Require(item.clip.Contains(item.box),"library-row-click-inside-clip");Mouse(item,0,true);Frame();Mouse(item,0,false);Frame();Frame();
    Require(editor::SameSavedFile(editor::g_librarySelected,file),"library-mouse-exact-selection");
}
void LibraryClick(const std::string& label) {
    Frame();auto item=Find(label);ImGui::SetScrollFromPosY(item.window,item.box.Min.y-item.window->Pos.y,0.25f);Frame();Frame();
    item=Find(label);Require(item.clip.Contains(item.box),"library-control-in-viewport");Click(label);
}
void SelectTab(const char* label){ // v0.97 uses wrapping buttons: exercise their actual mouse dispatch.
    Click(i18n::TStable(label));
}
void Drain(){int n=0;while(host::PumpGame())if(++n>4096)throw std::runtime_error("queue not drained");}
void Capture(const std::string& name,bool keepPointer=false){
    if(!keepPointer)ImGui::GetIO().AddMousePosEvent(0,(float)height-1);Frame();
    if(name.rfind("full-project-",0)==0&&name.find("bottom")==std::string::npos){const auto tab=Find(i18n::TStable(project));Check(tab.box.Min.x>=tab.clip.Min.x&&tab.box.Max.x<=tab.clip.Max.x,"selected-project-tab-in-capture");}
    const auto* data=ImGui::GetDrawData();Require(data&&data->Valid&&data->TotalVtxCount>0,"nonempty-actual-drawdata");
    drawcapture::Save(root+"\\"+name,*data,textures);
    std::ostringstream geometry;geometry<<'[';bool first=true;
    for(const auto& pair:items){const auto& item=pair.second;if(!item.window)continue;if(!first)geometry<<',';first=false;
        geometry<<"{\"id\":"<<item.id<<",\"label\":\""<<J(item.label)<<"\",\"window\":\""<<J(item.window->Name)<<"\",\"disabled\":"<<item.disabled
            <<",\"box\":["<<item.box.Min.x<<','<<item.box.Min.y<<','<<item.box.Max.x<<','<<item.box.Max.y<<"],\"clip\":["
            <<item.clip.Min.x<<','<<item.clip.Min.y<<','<<item.clip.Max.x<<','<<item.clip.Max.y<<"]}";
    }geometry<<']';Write(root+"\\"+name+".items.json",geometry.str());
    const auto* shell=ImGui::FindWindowByID(ImHashStr(editor::g_compact?"###cdmodkit_dock":"###cdmodkit"));
    std::string memberIds="[";if(editor::g_place.active)for(const auto& member:editor::g_place.m){if(memberIds.size()>1)memberIds+=',';memberIds+=std::to_string(member.uid);}memberIds+=']';
    captures.push_back("{\"name\":\""+name+"\",\"width\":"+std::to_string(width)+",\"height\":"+std::to_string(height)+",\"vertices\":"+std::to_string(data->TotalVtxCount)+",\"source\":\"production editor.cpp\",\"hiddenHwnd\":true,\"backend\":\"imgui_impl_win32.cpp\",\"language\":\""+i18n::ActiveLanguage()+"\",\"dock\":"+std::to_string(editor::g_compact)+",\"page\":"+std::to_string(editor::g_mainTab)+",\"carried\":"+std::to_string(editor::g_place.active)+",\"camera\":"+std::to_string(editor::g_cameraMode)+",\"preflightExpanded\":"+std::to_string(editor::g_exportOpen)+",\"collapsed\":"+std::to_string(shell&&shell->Collapsed)+",\"placementGeneration\":"+std::to_string(editor::g_place.generation)+",\"carriedUids\":"+memberIds+"}");
}
void Context(){
    if(ImGui::GetCurrentContext()){if(routed){input::Shutdown();routed=false;}ImGui_ImplWin32_Shutdown();ImGui::DestroyContext();}
    ImGui::CreateContext();GImGui->TestEngineHookItems=true;auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.ConfigFlags|=ImGuiConfigFlags_NoMouseCursorChange;
    io.ConfigInputTrickleEventQueue=false;Require(ImGui_ImplWin32_Init(hwnd),"real-win32-backend-init");
    // Same font size and system-font merge as the production overlay, plus deterministic test text glyphs.
    ImFontConfig cfg;cfg.SizePixels=17;auto* font=io.Fonts->AddFontDefault(&cfg);i18n::MergeSystemFonts(io.Fonts,17);icons::Register(io.Fonts,font,17);
    io.Fonts->Build();icons::Paint(io.Fonts);unsigned char* p;int w,h;io.Fonts->GetTexDataAsRGBA32(&p,&w,&h);io.Fonts->SetTexID(1);
    textures.clear();textures.emplace(1,drawcapture::Texture{w,h,std::vector<unsigned char>(p,p+(size_t)w*h*4)});editor::ApplyStyle(1);
}
void Reset(){
    editor::g_open=true;if(editor::g_cameraMode)editor::StopCameraMode();editor::ClearSceneAction(false);Drain();editor::host_seam::ResetPlacement();editor::host_seam::ResetHistory();
    editor::g_open=true;editor::g_compact=false;editor::g_mainTab=editor::g_compactPage=0;editor::g_selectMainTab=false;editor::g_playMode=false;
    editor::g_selPrefab=0;editor::g_filter[0]=0;editor::g_selCat=0;editor::g_selColl=-1;editor::g_tagFilter.clear();editor::g_favOnly=false;editor::g_lastKey.clear();
    editor::g_projectLibrary={};editor::g_librarySelected={};editor::g_libraryRows.clear();editor::g_librarySearch[0]=0;editor::g_libraryKind=editor::g_libraryLocation=0;editor::g_libraryFilterDirty=true;
    editor::g_libraryAction={};editor::g_projectRefresh=true;editor::g_exportOpen=false;editor::ResetExportApproval();editor::g_exportOverwrite=false;editor::g_projectReadValid=false;editor::g_projectPlacements.clear();editor::g_projectStatus.clear();
    // Independent host cases start a fresh session; production never resets its archives.
    editor::g_placementReports={};editor::g_groundReports={};editor::g_projectDetails.clear();editor::g_exportAttempts.clear();editor::g_projectFileFailures.clear();
    manualPolicy=false;editor::g_projTab=-1;editor::g_npcFilter[0]=0;editor::g_npcKey.clear();
    editor::g_sceneCards=false;editor::g_npcCards=false;editor::g_currentCam={};editor::g_log.clear();editor::g_clip.clear();host::Seam().ready=true;
    core::g_keyToggle=VK_F11;core::g_keyMode=VK_F2;
    core::g_timeAvailable=false;core::g_timeCurrentValid=false;core::ResetTimeControl();core::ResetWeatherControl();
    core::g_weatherAvailable=false;core::g_rainAvailable=false;core::g_cloudAvailable=false;core::g_windAvailable=false;core::g_snowEffectsAvailable=false;
    strcpy_s(editor::g_projName,"workshop");i18n::SetPreference("en");width=1920;height=1080;Context();Frame();Frame();
}
int Spawn(float x){int uid=core::SpawnAt(prefab,{x,0,0});Drain();return uid;}
int GroupOf(int uid){for(const auto& o:core::Spawned())if(o.uid==uid)return o.group;throw std::runtime_error("uid absent");}
void ShellSwitch(){
    Reset();host::SetTerrainAvailable(true);int uid=Spawn(1);editor::host_seam::SelectUid(uid);strcpy_s(editor::g_filter,"stone");strcpy_s(editor::g_npcFilter,"Guard");
    for(auto page:std::vector<std::pair<int,const char*>>{{0,browser},{1,scene},{2,project},{7,npc},{8,weather},{editor::TabTerrain,terrain}}){const int p=page.first;SelectTab(page.second);if(p==2){Click("Export Selection");Click("Preflight");Require(editor::g_exportApproval.valid,"actual-preflight-before-shell-switch");}
        Click(dock);Check(editor::g_compact&&editor::g_compactPage==p,"full-to-dock-active-page");
        for(const char* label:{browser,scene,project,npc,weather,terrain})Check(Has(i18n::TStable(label)),"all-six-dock-pages-present");
        Check(editor::g_sel.count(uid)&&std::string(editor::g_filter)=="stone"&&std::string(editor::g_npcFilter)=="Guard","search-selection-survive");
        Check(!Has(i18n::TStable(ICON_LOCATION_CROSSHAIRS " Travel")),"no-travel-navigation");
        if(p==2)Check(editor::g_exportOpen&&editor::g_exportApproval.valid&&Has("Preflight"),"project-preflight-survives-entry");
        Click(full);Check(!editor::g_compact&&editor::g_mainTab==p,"dock-to-full-active-page");
        if(p==2)Check(editor::g_exportOpen&&editor::g_exportApproval.valid&&Has("Preflight"),"project-preflight-survives-return");
    }
    Click(dock);Click(i18n::TStable(browser));Click(full);Check(editor::g_mainTab==0,"dock-page-change-inherited-by-full");
    Click(dock);Click(i18n::TStable(scene));Click(full);Check(editor::g_mainTab==1,"dock-scene-change-inherited-by-full");
    Click(dock);Click(i18n::TStable(project));Click(full);Check(editor::g_mainTab==2,"dock-project-change-inherited-by-full");
}
void Fallback(){
    Reset();SelectTab(history);Click(dock);Check(editor::g_compactPage==0,"initial-full-only-fallback-browser");Click(full);
    for(const char* label:{history,ICON_LIST " Settings",ICON_LIST " Log"}){
        SelectTab(project);SelectTab(label);Click(dock);Check(editor::g_compactPage==2,"full-only-uses-last-shared-project");Click(full);Check(editor::g_mainTab==2,"fallback-return-shared-project");
    }
}
void SwitchActions(){
    Reset();Click(dock);int n=host::NextUid(),c=creates;Click(dockPlace);Drain();
    Require(editor::g_place.active,"actual-place-active");Check(host::NextUid()==n+1&&creates==c+1,"place-one-engine-create");
    const auto generation=editor::g_place.generation;Click(full);Check(editor::g_place.active&&editor::g_place.generation==generation,"carried-placement-survives-shell");
    editor::DropCarried();Drain();int a=Spawn(2),b=Spawn(4);editor::host_seam::SelectUid(a);editor::host_seam::SelectAdd(b);
    SelectTab(scene);Click("Group");const int group=GroupOf(a);Require(group>0&&GroupOf(b)==group,"actual-group-action");
    const size_t undo=editor::g_undo.size();const int uid=host::NextUid();
    Frame();auto sw=Find(dock);ImGui::GetIO().AddMousePosEvent(sw.box.GetCenter().x,sw.box.GetCenter().y);Frame();Mouse(sw,0,true);Frame();
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl,true);ImGui::GetIO().AddKeyEvent(ImGuiKey_Z,true);Mouse(sw,0,false);Frame();
    Check(editor::g_compact,"switch-release-frame");Check(editor::g_undo.size()+1==undo&&editor::g_redo.size()==1,"switch-frame-undo-exactly-once");
    int shells=0;for(auto* w:GImGui->Windows)if(w->Active&&(w->ID==ImHashStr("###cdmodkit")||w->ID==ImHashStr("###cdmodkit_dock")))++shells;
    Check(shells==1,"at-most-one-active-shell-on-switch-frame");Check(host::NextUid()==uid&&GroupOf(a)==0&&GroupOf(b)==0,"no-double-action-side-effect");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Z,false);ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl,false);Frame();Frame();
    Check(editor::g_undo.size()+1==undo,"held-switch-does-not-replay-undo");
    Click("Redo");Frame();sw=Find(full);ImGui::GetIO().AddMousePosEvent(sw.box.GetCenter().x,sw.box.GetCenter().y);Frame();Mouse(sw,0,true);Frame();
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl,true);ImGui::GetIO().AddKeyEvent(ImGuiKey_Z,true);Mouse(sw,0,false);Frame();
    Check(!editor::g_compact&&editor::g_undo.size()+1==undo&&editor::g_redo.size()==1,"reverse-switch-frame-undo-exactly-once");
    Check(GroupOf(a)==0&&GroupOf(b)==0&&host::NextUid()==uid,"reverse-no-double-action-side-effect");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Z,false);ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl,false);Frame();Frame();
}
void GroupUndo(){
    Reset();int a=Spawn(1),b=Spawn(2);editor::host_seam::SelectUid(a);editor::host_seam::SelectAdd(b);SelectTab(scene);Click("Group");
    Require(editor::g_undo.size()==1&&editor::g_undo.back().acts.size()==2,"one-history-entry-per-group-action");
    Check(editor::g_undo.back().acts[0].kind==editor::Act::SetGroup,"upstream-setgroup-act");int gid=GroupOf(a);
    SelectTab(history);Click("Undo");Check(GroupOf(a)==0&&GroupOf(b)==0,"history-undo-membership");Click("Redo");Check(GroupOf(a)==gid&&GroupOf(b)==gid,"history-redo-membership");
    Check(editor::kHistoryLimit==1000,"upstream-shared-history-limit");
}
void CameraGrab(){
    Reset();int a=Spawn(1);editor::host_seam::SelectUid(a);SelectTab(scene);input::Init(hwnd);routed=true;Policy(true,true);
    SendMessageW(hwnd,WM_KEYDOWN,core::g_keyMode,1|(MapVirtualKeyW(core::g_keyMode,MAPVK_VK_TO_VSC)<<16));
    SendMessageW(hwnd,WM_KEYUP,core::g_keyMode,1|(MapVirtualKeyW(core::g_keyMode,MAPVK_VK_TO_VSC)<<16)|(LPARAM(3)<<30));
    bool down=false;Require(input::HotkeyPressed(core::g_keyMode,down),"configured-mode-through-production-router");editor::ToggleCameraMode();host::CaptureCamera({0,3,-8},0,-10);
    Require(editor::g_cameraMode&&core::FreeCamActive(),"configured-mode-enters-camera");Click(ICON_HAND " Grab");
    Check(editor::g_place.active&&editor::g_open&&editor::g_cameraMode&&!editor::g_place.reopen,"camera-aware-grab-keeps-editor-open");
    Click(dock);Check(editor::g_cameraMode&&editor::g_place.active,"camera-grab-survives-dock");Click(full);Check(editor::g_cameraMode&&editor::g_place.active,"camera-grab-survives-full");
    editor::DropCarried();Drain();editor::StopCameraMode();input::Shutdown();routed=false;
}
void Preview(){
    Reset();host::Ui().thumbnailsReady=true;int before=refreshes;
    Frame();Item row=Find("stone");ImGui::GetIO().AddMousePosEvent(row.box.GetCenter().x,row.box.GetCenter().y);Frame();Mouse(row,1,true);Frame();Mouse(row,1,false);Frame();Frame();
    Click("render preview again");Check(refreshes==before+1,"actual-context-menu-preview-refresh-once");
    Click(dock);Frame();Item card=Find("card");ImGui::GetIO().AddMousePosEvent(card.box.GetCenter().x,card.box.GetCenter().y);Frame();Mouse(card,1,true);Frame();Mouse(card,1,false);Frame();Frame();
    Click("render preview again");Check(refreshes==before+2,"dock-context-menu-preview-refresh-once");host::Ui().thumbnailsReady=false;
}
void Ime(){
    Reset();SelectTab(project);Click("project name");
    SendMessageW(hwnd,WM_CHAR,0xAC00,1);Frame();Check(std::string(editor::g_projName).find("\xEA\xB0\x80")!=std::string::npos,"win32-backend-CJK-commit-to-production-input");
    Check(ImGui::GetFont()->FindGlyphNoFallback(0xAC00)!=nullptr,"CJK-glyph-present-not-replacement");
}
void CheckHorizontal(const std::vector<std::string>& labels,std::set<std::string>& seen) {
    for(const auto& label:labels)for(const auto& pair:items){const auto& item=pair.second;
        if(!item.window||!item.box.Overlaps(item.clip))continue;
        const bool match=item.label==label||(label.rfind("##",0)==0&&item.id==ImHashStr(label.c_str(),0,item.window->ID));if(!match)continue;
        if(item.box.Min.y>=item.clip.Min.y&&item.box.Max.y<=item.clip.Max.y)seen.insert(label);
        Check(item.box.Min.x>=item.clip.Min.x&&item.box.Max.x<=item.clip.Max.x,("action-horizontal-unclipped: "+label).c_str());
    }
}
void ScrollControls(const std::vector<std::string>& labels,const std::string& captureName,const char* anchor) {
    std::set<std::string> seen;auto* window=Find(anchor).window;
    // NPC details have their own scroll range; first reveal the child in the shell.
    // Scrolling only the child cannot expose controls below the parent's viewport.
    if(window->Flags&ImGuiWindowFlags_ChildWindow){
        auto* parent=window->ParentWindow;
        ImGui::SetScrollFromPosY(parent,window->Pos.y+window->Size.y-parent->Pos.y,1.f);Frame();Frame();
    }
    // Enumerate overlapping viewport strips, not timed polling. This visits controls omitted by
    // ImGui's clipping early-return as well as controls between the initial and final scroll positions.
    const float stride=window->InnerRect.GetHeight()/2;
    const int strips=(int)ceilf(window->ScrollMax.y/stride);
    for(int strip=0;strip<=strips;++strip){ImGui::SetScrollY(window,std::min(strip*stride,window->ScrollMax.y));Frame();Frame();CheckHorizontal(labels,seen);}
    ImGui::SetScrollY(window,window->ScrollMax.y);Frame();Frame();Capture(captureName);
    for(const auto& label:labels)Check(seen.count(label)!=0,("action-reachable-with-scroll: "+label).c_str());
    ImGui::SetScrollY(window,0);Frame();Frame();
}
std::string NpcSpawnLabel(){return std::string(i18n::T(ICON_LOCATION_CROSSHAIRS "   SPAWN   "))+"  x"+std::to_string(editor::g_npcCount);}
std::vector<std::string> Labels(std::initializer_list<const char*> labels){std::vector<std::string> out;for(auto label:labels)out.push_back(i18n::T(label));return out;}
void InView(const std::string& label){
    const auto item=Find(label);Check(item.box.Min.x>=0&&item.box.Min.y>=0&&item.box.Max.x<=width&&item.box.Max.y<=height,("in-viewport: "+label).c_str());
    Check(item.clip.Contains(item.box),("unclipped: "+label).c_str());
}
void DismissSceneMenus(){
    // ImGui gates Escape dismissal on NavEnableKeyboard, which this production-style context does not enable.
    // A real outside click on the observed shell title closes menus without editing the Scene selection.
    auto* shell=ImGui::FindWindowByID(ImHashStr(editor::g_compact?"###cdmodkit_dock":"###cdmodkit"));
    Require(shell&&shell->Active,"menu-owner-shell-visible");
    Item title;title.id=shell->MoveId;title.window=shell;title.box=shell->TitleBarRect();
    ImGui::GetIO().AddMousePosEvent(title.box.GetCenter().x,title.box.GetCenter().y);Frame();
    Mouse(title,0,true);Frame();Mouse(title,0,false);Frame();Frame();
    Require(GImGui->OpenPopupStack.empty(),"context-menus-close-through-titlebar-click");
}
void OpenSceneContext(){
    if(!editor::g_compact)Click(i18n::T(ICON_COPY " cards"));
    Frame();ImGuiWindow* child=nullptr;
    for(auto* window:GImGui->Windows)if(window->Active&&window->ParentWindow&&window->ChildId==ImHashStr("scenecards",0,window->ParentWindow->ID))child=window;
    Require(child!=nullptr,"real-Scene-card-child");
    auto* parent=child->ParentWindow;ImGui::SetScrollFromPosY(parent,child->Pos.y-parent->Pos.y,0.f);Frame();Frame();
    Item card;float area=0;
    for(const auto& pair:items){const auto& item=pair.second;if(item.label!="entitycard"||!item.box.Overlaps(item.clip))continue;
        Item visible=item;visible.box.ClipWith(visible.clip);const float visibleArea=visible.box.GetWidth()*visible.box.GetHeight();
        if(visibleArea>area){area=visibleArea;card=visible;}
    }
    // A scrollable card may be partially visible; click its observed visible portion, never an off-body point.
    Require(card.id!=0&&area>0,"visible-native-card-context-target");
    ImGui::GetIO().AddMousePosEvent(card.box.GetCenter().x,card.box.GetCenter().y);Frame();
    Mouse(card,1,true);Frame();Mouse(card,1,false);Frame();Frame();
    Require(!GImGui->OpenPopupStack.empty(),"actual-right-click-opens-Scene-context");
}
void SceneTransformMenus(const std::string& capture){
    const auto selection=editor::g_sel;const auto undo=editor::g_undo.size(),redo=editor::g_redo.size();
    OpenSceneContext();Click(i18n::T("Rotate"));
    InView(i18n::T("Rotate left"));InView(i18n::T("Rotate right"));Capture(capture+"-rotate",true);DismissSceneMenus();
    OpenSceneContext();Click(i18n::T("Align to primary"));
    for(const char* axis:{"X","Y","Z"})InView(i18n::T(axis));Capture(capture+"-align",true);DismissSceneMenus();
    Check(editor::g_sel==selection&&editor::g_undo.size()==undo&&editor::g_redo.size()==redo,"context-reachability-does-not-edit-selection-or-History");
}
void Gallery(){
    Reset();int a=Spawn(1),b=Spawn(3);editor::host_seam::SelectUid(a);editor::host_seam::SelectAdd(b);editor::GroupSel(true);
    SelectTab(project);Click("Export Selection");Click("Preflight");Require(editor::g_exportApproval.included.size()==2,"gallery-real-two-object-preflight");
    for(const char* lang:{"en","ko"})for(auto size:std::vector<ImVec2>{{1920,1080},{1024,768},{480,900}}){
        width=(int)size.x;height=(int)size.y;i18n::SetPreference(lang);editor::g_compact=false;editor::g_open=true;editor::g_cameraMode=false;editor::g_playMode=false;
        core::g_timeAvailable=true;core::g_weatherAvailable=true;core::g_rainAvailable=true;core::g_cloudAvailable=true;core::g_windAvailable=true;core::g_snowEffectsAvailable=false;
        core::SetWeatherRainOverride(true,0.4f);core::SetWeatherCloudOverride(true,1.2f);core::SetWeatherWindOverride(true,1.5f);
        Context();Frame();Frame();
        const std::string suffix="-"+std::string(lang)+"-"+std::to_string(width)+"x"+std::to_string(height);
        // Rotate/Align are reached through the real context menu below; snap is tested on the real placement bar in DiagnosticsGallery.
        const auto sceneActions=Labels({"click selects group","show deleted","Undo","Redo",ICON_HAND " Grab","Focus","To ground",ICON_COPY " Duplicate","Group","Ungroup",ICON_TRASH " Delete","Forget","Remove duplicates"});
        const auto projectActions=Labels({"Approve existing-file overwrite","Preflight","Write group","Cancel export","Load","Reload","Save","Unload","Add unassigned","autoload"});
        auto projectControls=[&](const std::string& name) {
            SelectLibraryFile("workshop.cdproj");ScrollControls(projectActions,name,i18n::T("project name"));
            SelectLibraryFile("stones.cdgroup");ScrollControls(Labels({"Read","Place","Overwrite"}),name+"-group",i18n::T("project name"));
        };
        for(auto state:std::vector<std::pair<const char*,const char*>>{{browser,"browser"},{scene,"scene"},{project,"project"},{npc,"npc"},{weather,"weather"},{history,"history"}}){
            SelectTab(state.first);if(state.first==npc){editor::g_npcSel=0;editor::g_npcCount=19;Frame();}
            Capture("full-"+std::string(state.second)+suffix);
            if(state.first==scene){ScrollControls(sceneActions,"full-scene-bottom"+suffix,i18n::T("Undo"));SceneTransformMenus("full-scene-context"+suffix);}
            if(state.first==project)projectControls("full-project-bottom"+suffix);
            if(state.first==weather)ScrollControls(Labels({"time of day","Dawn 06:00","Noon 12:00","Sunset 18:00","Midnight 00:00","freeze time (lighting only)","use native time","override rain","rain intensity","override snow","override clouds","cloud amount","override wind","wind multiplier","use native weather"}),"full-weather-bottom"+suffix,i18n::T("time of day"));
            if(state.first==npc)ScrollControls(Labels({"distance","count","##npcformation","spacing",NpcSpawnLabel().c_str()}),"full-npc-bottom"+suffix,i18n::T("count"));
        }
        SelectTab(browser);Click(i18n::T(dock));
        for(auto state:std::vector<std::pair<const char*,const char*>>{{browser,"browser"},{scene,"scene"},{project,"project"},{npc,"npc"},{weather,"weather"}}){
            Click(i18n::TStable(state.first));Capture("dock-"+std::string(state.second)+suffix);
            for(auto page:{browser,scene,project,npc,weather})InView(i18n::TStable(page));InView(i18n::T(full));
            if(state.first==scene){ScrollControls(sceneActions,"dock-scene-bottom"+suffix,i18n::T("Undo"));SceneTransformMenus("dock-scene-context"+suffix);}
            if(state.first==weather)ScrollControls(Labels({"time of day","Dawn 06:00","Noon 12:00","Sunset 18:00","Midnight 00:00","freeze time (lighting only)","use native time","override rain","rain intensity","override snow","override clouds","cloud amount","override wind","wind multiplier","use native weather"}),"dock-weather-bottom"+suffix,i18n::T("time of day"));
            if(state.first==npc)ScrollControls(Labels({"distance","count","##npcformation","spacing",NpcSpawnLabel().c_str()}),"dock-npc-bottom"+suffix,i18n::T("count"));
            if(state.first==project){
                const auto button=Find(i18n::T("Preflight"));ImGui::SetScrollFromPosY(button.window,button.box.Min.y-button.window->Pos.y,0.25f);Frame();Frame();
                Check(editor::g_exportOpen&&Has(i18n::T("Preflight"))&&Find(i18n::T("Preflight")).clip.Contains(Find(i18n::T("Preflight")).box),"expanded-preflight-accessible");projectControls("dock-project-bottom"+suffix);
            }
        }
    }
    i18n::SetPreference("en");
}
void MainRuntimePages(){
    Reset();host::SetTravelStage([](uint32_t,uint32_t,uint32_t,const float*){Check(false,"gallery-does-not-request-native-travel");});host::SetTerrainAvailable(true);
    core::TerrainStroke stroke{};stroke.r=8;stroke.strength=1;core::TerrainReplaceProject(0,{stroke});
    for(const char* lang:{"en","ko"})for(auto size:std::vector<ImVec2>{{1920,1080},{480,900}}){
        width=(int)size.x;height=(int)size.y;i18n::SetPreference(lang);editor::g_compact=false;editor::g_open=true;
        Context();Frame();Frame();
        const std::string suffix="-"+std::string(lang)+"-"+std::to_string(width)+"x"+std::to_string(height);
        SelectTab(ICON_LOCATION_CROSSHAIRS " Travel");
        Check(editor::g_mainTab==editor::TabTravel&&!editor::g_compact,"main-native-travel-page-dispatched");
        Capture("main-travel"+suffix);
        SelectTab(ICON_CUBE " Terrain");
        Check(editor::g_mainTab==editor::TabTerrain&&!editor::g_compact,"main-terrain-page-dispatched");
        Capture("main-terrain"+suffix);
        Click(i18n::T(dock));Check(editor::g_compactPage==editor::TabTerrain,"terrain-is-sixth-shared-Dock-page");
        Capture("dock-terrain"+suffix);Click(i18n::T(full));
    }
    core::TerrainClear();host::SetTerrainAvailable(false);host::SetTravelStage({});i18n::SetPreference("en");
}
#include "ui_diagnostics_cases.h"
void DiagnosticsPresence(){
    Reset();Check(ImGui::GetDrawData()->TotalVtxCount>0&&!ImGui::FindWindowByName("##focushud"),"open-status-renders-in-shell-not-overlapping-window");
    SelectTab(ICON_LIST " Log");Check(Has("camera trace (16 s)")&&Has("##replayprefab"),"live-log-controls-present");
    SelectTab(project);Click("Export Selection");Check(Has("Preflight")&&Find("Write group").disabled,"preflight-required-before-write");
    Click("Preflight");Check(!editor::g_exportStatus.empty()&&editor::g_projectStatus.empty()&&!editor::g_exportApproval.valid,"empty-selection-has-action-scoped-refusal-status");
    SelectTab(browser);Click(dock);Click(dockPlace);Drain();Frame();
    Check(editor::MouseMode()&&Has("drop")&&Has("Cancel")&&Has("To ground")&&Has("level"),"real-mouse-placement-actions-present");
    Click("Cancel");Drain();Check(!editor::Placing(),"cancel-action-operates");
}
void DiagnosticsGallery(){
    Reset();int uid=Spawn(1),second=Spawn(3);editor::host_seam::SelectUid(uid);editor::host_seam::SelectAdd(second);editor::GroupSel(true);
    for(auto size:std::vector<ImVec2>{{1920,1080},{1024,768},{480,900}}){
        width=(int)size.x;height=(int)size.y;editor::g_compact=false;editor::g_open=true;Context();BeginRoutes();manualPolicy=false;Frame();SelectTab(scene);
        editor::ToggleCameraMode();host::CaptureCamera({0,3,-8},30,-10);editor::StartGrab(editor::SelUids(),false,"selection");
        Frame();Key('W',true);Frame();std::string suffix="-"+std::to_string(width)+"x"+std::to_string(height);Capture("diagnostics-full-camera"+suffix);
        Check(Event(WM_KEYDOWN,'W').recipients==(input::RecipientUi|input::RecipientCamera),"rendered-camera-real-recipient");Key('W',false);Frame();
        for(const char* action:{"Cancel","drop","To ground","level","snap"})InView(action);
        Click(dock);Capture("diagnostics-dock-camera"+suffix);for(const char* action:{"Cancel","drop","To ground","level","snap"})InView(action);
        Check(editor::g_cameraMode&&editor::g_place.active,"dock-preserves-camera-and-carry");
        i18n::SetPreference("ko");Frame();Frame();Capture("diagnostics-dock-cjk"+suffix);
        for(const char* action:{"Cancel","drop","To ground","level","snap"})InView(i18n::T(action));
        Click(i18n::T(full));Frame();Capture("diagnostics-full-cjk"+suffix);
        for(const char* action:{"Cancel","drop","To ground","level","snap"})InView(i18n::T(action));i18n::SetPreference("en");
        editor::DropCarried();Drain();editor::StopCameraMode();editor::StartGrab(editor::SelUids(),false,"selection");
        manualPolicy=true;Policy(true,false,true);const auto center=editor::g_place.center;Key(VK_NUMPAD8,true);Frame();Key(VK_NUMPAD8,false);Frame();Capture("diagnostics-closed-placement"+suffix);
        Check(editor::g_place.center.x==center.x&&editor::g_place.center.y==center.y&&editor::g_place.center.z==center.z,"numpad-does-not-transform-carried-object");
        auto* hud=ImGui::FindWindowByName("##focushud");Require(hud!=nullptr,"closed-placement-HUD-present");Check(hud->Pos.x>=0&&hud->Pos.y>=0&&hud->Pos.x+hud->Size.x<=width&&hud->Pos.y+hud->Size.y<=height,"closed-HUD-fully-in-viewport");
        InView("Cancel");i18n::SetPreference("ko");Frame();Capture("diagnostics-closed-placement-ko"+suffix);
        for(const char* action:{"Cancel","drop","To ground","level","snap"})InView(i18n::T(action));
        i18n::SetPreference("en");Frame();Click("Cancel");Drain();Check(!editor::g_place.active&&core::Spawned().size()==2,"visible-closed-cancel-restores-existing-objects");
    }
}
void ClickCollapse(ImGuiWindow* shell, bool collapsed){
    Frame();const auto found=items.find(ImHashStr("#COLLAPSE",0,shell->ID));
    Require(found!=items.end(),"native-collapse-button-observed");const Item arrow=found->second;
    ImGui::GetIO().AddMousePosEvent(arrow.box.GetCenter().x,arrow.box.GetCenter().y);Frame();
    Mouse(arrow,0,true);Frame();Mouse(arrow,0,false);Frame();Frame();
    Require(shell->Collapsed==collapsed&&editor::g_open,"real-collapse-input-not-close");
}
void CollapsedCarry(const char* lang,int w,int h,bool dockShell){
    Reset();width=w;height=h;i18n::SetPreference(lang);Context();Frame();Frame();
    if(dockShell){Click(i18n::T(dock));Click(i18n::T(dockPlace));Drain();}
    else {
        int a=Spawn(1),b=Spawn(5);editor::host_seam::SelectUid(a);editor::host_seam::SelectAdd(b);SelectTab(scene);
        editor::ToggleCameraMode();host::CaptureCamera({0,3,-8},0,-10);Click(i18n::T(ICON_HAND " Grab"));
    }
    Require(editor::g_open&&editor::g_place.active&&editor::g_cameraMode==!dockShell,"reachable-open-carried-state");
    auto* shell=ImGui::FindWindowByID(ImHashStr(dockShell?"###cdmodkit_dock":"###cdmodkit"));
    Require(shell&&shell->Active&&!shell->Collapsed,"expanded-shell-before-collapse");
    const auto generation=editor::g_place.generation;const auto members=editor::g_place.m;const auto selection=editor::g_sel;
    const auto world=core::Spawned();const auto center=editor::g_place.center;const int next=host::NextUid(),made=creates,removed=removes;
    const auto undo=editor::g_undo.size(),redo=editor::g_redo.size();
    auto unchanged=[&](){
        if(!editor::g_place.active||editor::g_place.generation!=generation||editor::g_place.m.size()!=members.size()||editor::g_sel!=selection)return false;
        for(size_t i=0;i<members.size();++i)if(editor::g_place.m[i].uid!=members[i].uid)return false;
        const auto now=core::Spawned();if(now.size()!=world.size())return false;
        for(const auto& old:world){const auto* obj=editor::Find(now,old.uid);if(!obj||obj->pos.x!=old.pos.x||obj->pos.y!=old.pos.y||obj->pos.z!=old.pos.z||obj->rot.yaw!=old.rot.yaw||obj->rot.pitch!=old.rot.pitch||obj->rot.roll!=old.rot.roll||obj->scale!=old.scale||obj->group!=old.group||obj->proj!=old.proj||obj->hidden!=old.hidden)return false;}
        return editor::g_place.center.x==center.x&&editor::g_place.center.y==center.y&&editor::g_place.center.z==center.z&&host::NextUid()==next&&creates==made&&removes==removed&&editor::g_undo.size()==undo&&editor::g_redo.size()==redo;
    };
    ClickCollapse(shell,true);Check(unchanged(),"collapse-preserves-carried-identities-world-and-history");
    const std::string suffix=std::string(dockShell?"dock":"full")+"-"+lang+"-"+std::to_string(w)+"x"+std::to_string(h);
    Capture("collapsed-"+suffix);
    Require(Has(i18n::T("Cancel"))&&Has(i18n::T("drop")),"collapsed-carried-cancel-and-drop-present");
    for(auto label:{"Cancel","drop","To ground","level","snap"})InView(i18n::T(label));
    auto* hud=Find(i18n::T("Cancel")).window;
    Check(hud->ID==ImHashStr("##placehud")&&hud->Pos.y>=shell->Pos.y+shell->Size.y,"same-standalone-bar-below-collapsed-title");
    ClickCollapse(shell,false);Check(unchanged(),"expand-preserves-carried-identities");
    Check(!hud->Active&&Find(i18n::T("Cancel")).window==shell,"expanded-shell-reuses-embedded-bar-only");
    ClickCollapse(shell,true);Check(unchanged(),"recollapse-preserves-carried-identities-until-cancel");
    const Item cancel=Find(i18n::T("Cancel"));ImGui::GetIO().AddMousePosEvent(cancel.box.GetCenter().x,cancel.box.GetCenter().y);Frame();
    Check(GImGui->HoveredWindow==cancel.window&&!cancel.disabled,"collapsed-cancel-receives-mouse");
    Check(unchanged(),"hover-does-not-mutate-carried-identities");Click(i18n::T("Cancel"));Drain();Frame();
    Check(!editor::g_place.active&&editor::g_open&&shell->Collapsed,"cancel-finishes-carry-without-expanding-or-closing");
    Check(host::NextUid()==next,"cancel-allocates-no-new-object-uids");
    if(dockShell)Check(core::Spawned().empty()&&creates==made&&removes==removed+1,"cancel-removes-only-new-dock-placement");
    else {
        // Cancel uses production final MoveMany: with the default recreate-on-move policy native
        // handles are replaced, but logical UIDs and their original transforms must survive.
        const auto now=core::Spawned();bool restored=now.size()==world.size();
        for(const auto& old:world){const auto* obj=editor::Find(now,old.uid);restored&=obj&&obj->pos.x==old.pos.x&&obj->pos.y==old.pos.y&&obj->pos.z==old.pos.z&&obj->rot.yaw==old.rot.yaw&&obj->rot.pitch==old.rot.pitch&&obj->rot.roll==old.rot.roll&&obj->scale==old.scale;}
        Check(restored&&creates==made+2&&removes==removed+2,"cancel-restores-existing-uids-through-final-native-replacement");
    }
    // Drop is independently clickable too, rather than merely painted beside Cancel.
    ClickCollapse(shell,false);
    if(dockShell){Click(i18n::T(dockPlace));Drain();}
    else Click(i18n::T(ICON_HAND " Grab"));
    Require(editor::g_place.active,"second-carry-for-drop-positive-control");const auto dropMembers=editor::g_place.m;
    ClickCollapse(shell,true);InView(i18n::T("drop"));Click(i18n::T("drop"));Drain();Frame();
    Check(!editor::g_place.active&&shell->Collapsed,"collapsed-drop-finishes-carry");
    const auto dropped=core::Spawned();for(const auto& member:dropMembers)Check(editor::Find(dropped,member.uid)!=nullptr,"drop-retains-carried-object-identity");
    Capture("collapsed-after-drop-"+suffix);
}
void UpstreamContracts(){
    Reset();SelectTab(weather);core::SetWeatherClearSky(true);core::SetWeatherRainOverride(true,1);core::SetWeatherSnowOverride(true,1);core::SetWeatherCloudOverride(true,2);core::SetWeatherWindOverride(true,2);
    Check(!core::WeatherClearSky()&&!core::WeatherRainOverride(nullptr)&&!core::WeatherSnowOverride(nullptr)&&!core::WeatherCloudOverride(nullptr)&&!core::WeatherWindOverride(nullptr),"unresolved-weather-setters-fail-closed");
    Check(!Has("clear sky")&&!Has("time of day"),"unresolved-controls-not-actionable");
    core::g_timeAvailable=true;core::g_weatherAvailable=true;core::g_rainAvailable=true;core::g_cloudAvailable=true;core::g_windAvailable=true;Frame();
    Check(Find("override snow").disabled&&!Find("override rain").disabled&&!Find("override clouds").disabled&&!Find("override wind").disabled,"per-control-availability-not-global-disable");
    Click("Noon 12:00");Check(core::TimeTargetHour()==12&&core::g_timeRun.load()&&!core::TimeFrozen(),"setting-hour-keeps-time-running");
    Click("freeze time (lighting only)");Check(core::TimeFrozen()&&!core::g_timeRun.load(),"freeze-holds-only-time-control");
    Click(dock);Click(full);Check(core::TimeFrozen()&&core::TimeTargetHour()==12,"weather-state-survives-shell");
    Click("freeze time (lighting only)");Check(!core::TimeFrozen()&&core::g_timeRun.load(),"unfreeze-resumes-from-target");
    Click("use native time");Check(!core::TimeFrozen()&&!core::g_timeRun.load()&&!core::g_timeApply.load(),"native-time-resets-all-override-flags");
    Click("override rain");Click("override wind");Check(core::WeatherRainOverride(nullptr)&&core::WeatherWindOverride(nullptr),"available-weather-actions-work");
    Click("clear sky");Check(core::WeatherClearSky()&&Find("override rain").disabled&&!Find("override wind").disabled,"clear-sky-keeps-independent-wind");
    Click("use native weather");Check(!core::WeatherClearSky()&&!core::WeatherRainOverride(nullptr)&&!core::WeatherWindOverride(nullptr),"native-weather-resets-overrides");
    SelectTab(npc);editor::g_npcSel=0;
    for(int formation=0;formation<3;++formation){editor::g_npcFormation=formation;editor::g_npcCount=501;Frame();Check(editor::g_npcCount==500,"formation-upper-count-clamp");editor::g_npcCount=-1;Frame();Check(editor::g_npcCount==1,"formation-lower-count-clamp");}
    Check(Find(NpcSpawnLabel()).disabled,"unresolved-npc-spawn-disabled");
    host::Seam().playerWorldPos=[](Vec3* out){*out={0,37,0};return true;};editor::ToggleCameraMode();host::CaptureCamera({0,45,-8},0,-40);Frame();
    // CameraBasis normally reads a native scene object. Supply that immutable sampled frame at the boundary;
    // NpcDropPoint and its ray/plane/fallback math remain the actual editor implementation.
    editor::g_currentCam={{0,45,-8},{1,0,0},{0,0.8f,0.6f},{0,-0.6f,0.8f},true,1,(float)width/height,(float)width,(float)height};
    Vec3 at{};bool plane=false;Check(editor::NpcDropPoint({(float)width/2,(float)height/2},&at,&plane)&&plane&&fabsf(at.y-37)<0.001f&&fabsf(at.z-2.666667f)<0.001f,"npc-drag-uses-target-height-not-camera-height");
    editor::g_currentCam.fwd={0,0,1};editor::g_currentCam.up={0,1,0};editor::g_npcDist=25;
    Check(editor::NpcDropPoint({(float)width/2,(float)height/2},&at,&plane)&&plane&&at.y==37&&at.z==17,"npc-horizon-fallback-uses-player-height-and-distance");
    editor::g_currentCam={};
    editor::StopCameraMode();host::Seam().playerWorldPos=[](Vec3* out){*out={0,0,0};return true;};
}
// Task9: drive actual admission, GroundStep, reconciliation and shell actions. No test calls Admit/Observe.
namespace rp = report_projection;
std::vector<std::string> reportTrace;
struct ReportNative {
    host::Engine saved = host::Seam(); bool recreate = core::g_recreateOnMove, failNext = false;
    std::map<uintptr_t, Vec3> poses; uintptr_t refused = 0; std::function<void(uintptr_t)> atMove;
    ReportNative() {
        core::g_recreateOnMove = false; auto& e = host::Seam(); e.probeReady = true;
        e.createGeneric = [this](const std::string&, Vec3 p, Rot, float) -> uintptr_t {
            if (failNext) { failNext = false; return 0; }
            ++creates; const auto h = handle++; poses[h] = p; return h;
        };
        e.remove = [this](uintptr_t h) { ++removes; return poses.erase(h) != 0; };
        e.moveInPlace = [this](uintptr_t h, Vec3 p, Rot, float) {
            if (atMove) atMove(h);
            if (!poses.count(h) || h == refused) return false;
            poses[h] = p; return true;
        };
        e.liveMove = e.moveInPlace;
        e.groundCast = [](Vec3 p, float length, core::GroundHit* hit) {
            // A physical hole must cover the full +/-0.36m query footprint, not just one exact coordinate.
            hit->done = true; hit->hit = std::fabs(p.x-20.0f)>1.0f; hit->centerY = 0; hit->center={p.x,0,p.z}; hit->fraction = p.y / length; return true;
        };
    }
    ~ReportNative() { host::Seam() = saved; core::g_recreateOnMove = recreate; }
};
SpawnedObj ReportRecord(int uid) {
    for (const auto& o : core::Spawned()) if (o.uid == uid) return o;
    throw std::runtime_error("report-native-record-missing");
}
rp::Placements PlacementReports() {
    return editor::g_placementReports.Snapshot(editor::g_place.active ? editor::g_place.req : core::PlaceRequestHandle{});
}
rp::Placement PlacementReport(const core::PlaceRequestHandle& req) {
    for (const auto& p : PlacementReports().reports) if (p.request == req) return p;
    throw std::runtime_error("report-editor-placement-admission-missing");
}
rp::Ground GroundReport(uint64_t id) {
    for (const auto& g : editor::g_groundReports.Snapshot().reports) if (g.receipt.id == id) return g;
    throw std::runtime_error("report-editor-ground-admission-missing");
}
bool ReportHas(const std::vector<uint64_t>& ids, uint64_t id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); }
std::string ReportHistory() {
    std::ostringstream s; s << std::hexfloat << editor::g_historySerial << ':' << editor::g_historyBranch << ':' << bool(editor::g_deferredEditor.action);
    for (const auto* stack : { &editor::g_undo, &editor::g_redo }) {
        s << '[';
        for (const auto& e : *stack) { s << e.serial << ':' << e.branch << '{';
            for (const auto& a : e.acts) s << a.kind << ':' << a.uid << ':' << a.prefab << ':' << a.group << ':' << a.group1 << ':' << a.proj
                << ':' << a.pos0.x << ':' << a.pos0.y << ':' << a.pos0.z << ':' << a.pos1.x << ':' << a.pos1.y << ':' << a.pos1.z
                << ':' << a.rot0.yaw << ':' << a.rot0.pitch << ':' << a.rot0.roll << ':' << a.rot1.yaw << ':' << a.rot1.pitch << ':' << a.rot1.roll
                << ':' << a.sc0 << ':' << a.sc1 << ';';
            s << '}';
        } s << ']';
    }
    return s.str();
}
void GroundReportTrace(uint64_t id, const char* event) {
    const auto g = GroundReport(id);
    reportTrace.push_back("{\"case\":\""+current+"\",\"event\":\""+event+"\",\"id\":"+std::to_string(id)+
        ",\"state\":"+std::to_string(g.receipt.state)+",\"accepted\":"+std::to_string(g.accepted)+",\"notApplied\":"+std::to_string(g.notApplied)+
        ",\"pending\":"+std::to_string(g.pending)+",\"reason\":\""+J(g.receipt.reason)+"\"}");
}
proj_codec::Document ReportGroup(const char* path) {
    proj_codec::Document doc; doc.kind = proj_codec::Kind::Group; doc.hasBounds = true;
    doc.bounds = {{0,0,0},{-1,-1,-1},{1,1,1},true}; doc.envelopes.push_back({1,doc.bounds});
    proj_codec::Record row; row.prefab = path; row.envelope = 1; doc.records.push_back(row); return doc;
}
void ReportPlacements() {
    Reset(); ReportNative native; Click(dock); // real open Dock keeps the carried controls available
    const auto count = PlacementReports().reports.size(); const auto doc = ReportGroup(prefab);
    Require(editor::PlaceGroupCopy(doc,{1,10,0},0,1,"same"),"report-first-editor-admission");
    const auto old = editor::g_place.req; const auto oldId = PlacementReport(old).id;
    Require(editor::PlaceGroupCopy(doc,{5,10,0},0,1,"same"),"report-second-editor-admission");
    const auto newer = editor::g_place.req; const auto newId = PlacementReport(newer).id;
    Check(oldId != newId && PlacementReports().reports.size() == count + 2,"report-same-name-distinct-admission-identities");
    Require(host::PumpGameAt(1),"report-newer-native-completion-first");
    auto p = PlacementReport(newer); auto order = PlacementReports().order;
    Check(p.final() && p.carrying && p.receipt.attached == 1 && editor::g_place.req == newer,"report-final-while-actually-carried");
    Check(order.newest == newId && ReportHas(order.olderActive,oldId),"report-old-active-not-hidden-by-new-completion");
    native.failNext = true; Require(host::PumpGame(),"report-older-native-failure-last"); Drain();
    order = PlacementReports().order;
    Check(order.newest == newId && PlacementReport(old).receipt.failed == 1 && ReportHas(order.earlierCompleted,oldId),"report-late-callback-keeps-newest-and-failure");
    Check(order.earlierAttentionCount > 0 && !PlacementReport(old).receipt.rows[0].reason.empty(),"report-old-exact-failure-retained");
    const auto excludedDoc = ReportGroup("/missing/report-excluded.prefab");
    Require(!editor::PlaceGroupCopy(excludedDoc,{0,10,0},0,1,"all-excluded"),"report-all-excluded-editor-refusal");
    const auto excluded = editor::g_projectPlacements.back().request; const auto excludedReport = PlacementReport(excluded);
    Check(excludedReport.final() && excludedReport.receipt.requested == 1 && excludedReport.displayedExcluded() == 1 && excludedReport.receipt.attached == 0 && excludedReport.attention(),"report-all-excluded-retained-despite-refusal");
    Check(excludedReport.receipt.rows[0].prefab == excludedDoc.records[0].prefab && excludedReport.receipt.rows[0].reason == core::PlaceRequestState(excluded).rows[0].reason,"report-all-excluded-exact-path-and-reason");
    Check(ReportHas(PlacementReports().order.olderActive,newId),"report-older-final-carried-still-active");
    const auto before = ReportHistory(); Click(full); Click(dock);
    Check(ReportHistory() == before && PlacementReport(old).id == oldId && PlacementReport(newer).id == newId && PlacementReports().order.newest == excludedReport.id,"report-full-dock-retains-identities-and-history");
    editor::CancelProjectPlacement(newer); const auto lag = PlacementReport(newer);
    Check(lag.receipt.attached == 1 && lag.removedAfterAttachment == 1 && lag.receipt.cleanupPending && !lag.final(),"report-editor-cancel-cleanup-not-final");
    Drain(); p = PlacementReport(newer);
    Check(p.final() && p.receipt.attached == 1 && !p.carrying && !p.receipt.cleanupPending && PlacementReport(old).receipt.failed == 1,"report-historical-attached-and-request-specific-cancel");
    Trace("report-placements-retained");
}
void ReportGroundAdmission() {
    Reset(); ReportNative native;
    const int uid = core::SpawnAt(prefab,{0,10,0});
    const auto refused = editor::BeginGrounding({uid},true); Require(refused.size() == 1,"report-immediate-refusal-admitted");
    const auto id = core::GroundStateOf(refused[0]).id; const auto report = GroundReport(id);
    Check(report.final() && report.receipt.reason == "refused" && report.notApplied == 1,"report-refusal-captured-before-any-frame");
    Drain(); editor::PumpGroundHistory();
    const auto first = editor::BeginGrounding({uid},true); const auto firstId = core::GroundStateOf(first[0]).id;
    const auto second = editor::BeginGrounding({uid},true); const auto secondId = core::GroundStateOf(second[0]).id;
    Check(GroundReport(firstId).receipt.reason == "canceled" && GroundReport(secondId).active(),"report-overlap-admission-captures-older-cancel");
    Require(editor::ReconcileGroundBatch(true),"report-real-cancel-barrier-reconciles"); Drain(); Frame();
    Check(GroundReport(secondId).receipt.state == core::GroundCanceled && editor::g_pendingGround.empty(),"report-original-canceled-state-before-reconciled");
    GroundReportTrace(id,"immediate-refusal");
}
void ReportGroundSiblings() {
    Reset(); ReportNative native;
    std::vector<int> uids;
    for (float x : {0.f,4.f,20.f,30.f,40.f}) uids.push_back(core::SpawnAt(prefab,{x,10,0}));
    Drain(); const int a = uids[0], b = uids[1], failed = uids[2], success = uids[3], later = uids[4];
    const auto partial = editor::BeginGrounding({a,b},true).front();
    const auto noSurface = editor::BeginGrounding({failed},true).front();
    const auto ok = editor::BeginGrounding({success},true).front();
    const auto pid = core::GroundStateOf(partial).id, fid = core::GroundStateOf(noSurface).id, sid = core::GroundStateOf(ok).id;
    Check(GroundReport(sid).active() && editor::g_groundReports.Snapshot().order.newest == sid,"report-ground-admission-before-frame");
    host::PumpPhysics(); Drain(); Frame(); // real Draw -> PumpSnapJobs -> GroundStep, not a copied reason policy
    Check(core::GroundStateOf(noSurface).reason == "no-surface" && core::GroundStateOf(partial).state == core::GroundQueued,"report-real-no-surface-and-queued-sibling");
    Require(host::PumpGameAt(1),"report-success-sibling-completes-first");
    const auto historyBefore = ReportHistory(); const auto lastBefore = editor::g_groundLastResults.size();
    editor::PumpGroundHistory(); // observe BEFORE a following frame can conceal a missing barrier capture
    Check(GroundReport(sid).accepted == 1 && GroundReport(sid).final() && GroundReport(fid).receipt.reason == "no-surface" && GroundReport(pid).active(),"report-barrier-captures-all-siblings-before-early-return");
    Check(ReportHistory() == historyBefore && editor::g_groundLastResults.size() == lastBefore,"report-active-batch-does-not-reconcile-history-or-last-results");
    native.refused = ReportRecord(b).obj; bool applyingObserved = false;
    native.atMove = [&](uintptr_t h) {
        if (h != native.refused) return;
        applyingObserved = true; const auto notice = core::GroundNotice();
        Check(notice == editor::g_seenGroundNotice,"report-member-progress-without-new-terminal-notice");
        editor::PumpSnapJobs(); const auto during = GroundReport(pid);
        Check(during.receipt.state == core::GroundApplying && during.accepted == 1 && during.pending == 1 && during.notApplied == 0 && !during.final(),"report-frame-captures-applying-member-progress");
        Check(ReportHistory() == historyBefore && core::GroundNotice() == notice,"report-frame-observation-does-not-mutate-history-or-notice");
    };
    Require(host::PumpGame(),"report-partial-native-dispatch"); native.atMove = {};
    Require(applyingObserved,"report-exact-native-member-event-fired"); Frame();
    const auto p = GroundReport(pid);
    Check(p.receipt.state == core::GroundSettled && p.accepted == 1 && p.notApplied == 1 && p.receipt.reason == "partial" && p.receipt.members[1].reason == "move-refused","report-terminal-partial-reasons-before-reconcile");
    Check(editor::g_pendingGround.empty() && editor::g_groundLastResults.size() == 3 && editor::g_undo.size() == 2 && editor::g_redo.empty(),"report-existing-batch-history-and-last-result-contract");
    Check(editor::g_undo[0].acts.size() == 1 && editor::g_undo[0].acts[0].uid == a && editor::g_undo[1].acts[0].uid == success,"report-only-accepted-members-enter-history");
    Check(native.poses.at(ReportRecord(a).obj).y == 1 && native.poses.at(ReportRecord(b).obj).y == 10,"report-native-partial-outcomes");
    GroundReportTrace(pid,"partial-reconciled"); GroundReportTrace(fid,"no-surface-sibling"); GroundReportTrace(sid,"successful-sibling");
    const auto next = editor::BeginGrounding({later},true).front(); const auto nextId = core::GroundStateOf(next).id;
    host::PumpPhysics(); Drain(); Frame(); Drain(); Frame();
    Check(editor::g_groundLastResults.size() == 1 && editor::g_groundLastResults[0].id == nextId,"report-operational-last-results-replaced-only-by-current-batch");
    Check(GroundReport(pid).receipt.reason == "partial" && GroundReport(fid).receipt.reason == "no-surface" && GroundReport(sid).accepted == 1 && editor::g_groundReports.Snapshot().order.newest == nextId,"report-earlier-siblings-survive-later-batch");
    editor::Undo(); Drain(); Frame(); Require(editor::g_undo.size() == 2 && editor::g_redo.size() == 1,"report-real-undo-and-redo-fixture");
    const auto beforeRead = ReportHistory();
    GroundReportTrace(pid,"read-again"); PlacementReports(); Frame();
    SelectTab(project); Click(dock); GroundReportTrace(fid,"dock"); Capture("reports-retained-dock");
    Click(full); GroundReportTrace(pid,"full"); Capture("reports-retained-full");
    Check(ReportHistory() == beforeRead && editor::g_groundLastResults.size() == 1 && editor::g_groundLastResults[0].id == nextId,"report-snapshot-shell-and-render-leave-entire-undo-redo-unchanged");
    Check(GroundReport(pid).receipt.members[1].reason == "move-refused" && GroundReport(fid).receipt.reason == "no-surface","report-full-dock-keeps-exact-old-reasons");
    editor::Redo(); Drain(); Frame();
    Check(editor::g_undo.size() == 3 && editor::g_redo.empty() && native.poses.at(ReportRecord(later).obj).y == 1,"report-redo-still-operates-after-read-and-shell-switch");
}
Item ReportKey(const std::string& key) {
    const auto id=ImHashStr(("###"+key).c_str(),0,ImHashStr("project-page"));
    const auto found=items.find(id);if(found==items.end())throw std::runtime_error("missing-report-key: "+key);return found->second;
}
void Reveal(Item item,float ratio=0.25f) {
    ImGui::SetScrollFromPosY(item.window,item.box.Min.y-item.window->Pos.y,ratio);Frame();Frame();
}
void ClickKey(const std::string& key) {
    Frame();Reveal(ReportKey(key));const bool wasOpen=editor::g_projectDetails.count(key)!=0;
    auto item=ReportKey(key);ImGui::GetIO().AddMousePosEvent(item.box.GetCenter().x,item.box.GetCenter().y);Frame();
    item=ReportKey(key);Require(item.clip.Contains(item.box),"disclosure-click-inside-clip");
    Mouse(item,0,true);Frame();Mouse(item,0,false);Frame();Frame();
    if((editor::g_projectDetails.count(key)!=0)==wasOpen) {
        Capture("task10-click-failure-"+current+"-"+key);
        printf("DISCLOSURE key=%s window=%s y=%g scroll=%g clip=%g,%g hovered=%s\n",key.c_str(),item.window->Name,item.box.Min.y,item.window->Scroll.y,item.clip.Min.y,item.clip.Max.y,GImGui->HoveredWindow?GImGui->HoveredWindow->Name:"none");
    }
    Require((editor::g_projectDetails.count(key)!=0)!=wasOpen,"disclosure-mouse-toggle");
}
void ReportCapture(const std::string& name,const std::string& key) {
    Frame();Reveal(ReportKey(key),0.f);const auto item=ReportKey(key);
    Check(item.clip.Contains(item.box),"report-header-fully-reachable");
    Check(item.box.GetHeight()==ImGui::GetFrameHeight(),"report-summary-single-line");Capture(name);
}
void ProjectTop() {auto* w=ReportKey("placement-earlier").window;ImGui::SetScrollY(w,0);Frame();Frame();}
void RoutedToggle() {
    bool down=false;const auto key=core::g_keyToggle;
    SendMessageW(hwnd,WM_KEYDOWN,key,1|(MapVirtualKeyW(key,MAPVK_VK_TO_VSC)<<16));
    SendMessageW(hwnd,WM_KEYUP,key,1|(MapVirtualKeyW(key,MAPVK_VK_TO_VSC)<<16)|(LPARAM(3)<<30));
    Require(input::HotkeyPressed(key,down),"F11-routed-toggle-edge");editor::Toggle();Frame();Frame();
}
void ReportSurface(const char* lang,int w,int h,bool compact) {
    Reset();ReportNative native;width=w;height=h;i18n::SetPreference(lang);Context();Frame();Frame();
    SelectTab(project);Click(i18n::T(dock));
    const int failed=core::SpawnAt(prefab,{20,10,0}),other=core::SpawnAt(prefab,{30,10,0});Drain();
    const auto ground=editor::BeginGrounding({failed},true).front();const auto gid=core::GroundStateOf(ground).id;
    host::PumpPhysics();Drain();Frame();Drain();Frame();
    Require(GroundReport(gid).receipt.reason=="no-surface","surface-real-ground-failure");
    const std::string longPath="/missing/library/rocks/volcanic/very_long_exact_prefab_name_with_no_spaces_for_wrapping_validation.prefab";
    Require(!editor::PlaceGroupCopy(ReportGroup(longPath.c_str()),{0,10,0},0,1,"earlier-warning"),"surface-excluded-attempt");
    const auto warning=editor::g_projectPlacements.back().request;const auto wid=PlacementReport(warning).id;
    Require(editor::PlaceGroupCopy(ReportGroup(prefab),{1,10,0},0,1,"older-active"),"surface-older-active-admitted");
    const auto older=editor::g_place.req;const auto oid=PlacementReport(older).id;editor::DropCarried();
    Require(editor::PlaceGroupCopy(ReportGroup(prefab),{5,10,0},0,1,"newest-request"),"surface-newest-admitted");
    const auto newest=editor::g_place.req;const auto nid=PlacementReport(newest).id;
    const auto activeGround=editor::BeginGrounding({other},true).front();const auto activeGid=core::GroundStateOf(activeGround).id;
    if(!compact)Click(i18n::T(full));Frame();Frame();
    const std::string nkey="placement-"+std::to_string(nid),okey="placement-"+std::to_string(oid),wkey="placement-"+std::to_string(wid),gkey="ground-"+std::to_string(gid);
    const std::string suffix="-"+std::string(compact?"dock":"full")+"-"+lang+"-"+std::to_string(w)+"x"+std::to_string(h);
    const auto order=PlacementReports().order;
    Check(order.newest==nid&&order.olderActive==std::vector<uint64_t>{oid}&&order.earlierCompleted==std::vector<uint64_t>{wid}&&order.earlierAttentionCount==1,"surface-classification-counts");
    Check(ReportKey(okey).id!=ReportKey(nkey).id&&!editor::g_projectDetails.count(nkey)&&!editor::g_projectDetails.count("placement-earlier"),"surface-older-active-visible-details-initially-collapsed");
    const auto stable=ReportKey(nkey).id,earlierId=ReportKey("placement-earlier").id;
    ReportCapture("task10-newest-older-active"+suffix,nkey);
    for(const char* key:{"placement-earlier","ground-earlier"}) {
        size_t completed=0,attention=0;const auto label=ReportKey(key).label;
        Check(sscanf_s(label.c_str(),"%zu / !%zu",&completed,&attention)==2&&completed==1&&attention==1,"surface-rendered-completed-and-attention-counts-not-truncated");
    }
    // End the ground probe explicitly; no frame-budget timeout is used to produce this result.
    core::GroundCancel(activeGround);Frame();const auto before=ReportHistory();
    ClickKey("placement-earlier");ClickKey(wkey);
    const auto reason=PlacementReport(warning).receipt.rows[0].reason;
    Check(PlacementReport(warning).receipt.rows[0].prefab==longPath&&!reason.empty()&&reason==core::PlaceRequestState(warning).rows[0].reason,"surface-exact-long-path-and-machine-reason");
    ReportCapture("task10-earlier-warning"+suffix,wkey);
    ProjectTop();Click(i18n::T(compact?full:dock));Frame();
    Check(ReportKey(nkey).id==stable&&ReportKey("placement-earlier").id==earlierId&&editor::g_projectDetails.count(wkey)&&editor::g_projectDetails.count("placement-earlier"),"surface-identical-widget-ids-and-open-state-across-shells");
    ProjectTop();Click(i18n::T(compact?dock:full));Frame();
    input::Init(hwnd);routed=true;Policy(true,true);RoutedToggle();Check(!editor::IsOpen(),"surface-F11-closed");RoutedToggle();
    Check(editor::IsOpen()&&editor::g_projectDetails.count(wkey)&&ReportKey(nkey).id==stable,"surface-F11-restores-report-state-and-id");input::Shutdown();routed=false;
    ClickKey(wkey);ClickKey(wkey);Check(editor::g_projectDetails.count(wkey)&&PlacementReport(warning).receipt.rows[0].reason==reason,"surface-collapse-reopen-preserves-exact-receipt");
    ClickKey("placement-earlier");ClickKey("ground-earlier");ClickKey(gkey);
    ReportCapture("task10-ground-warning"+suffix,gkey);ClickKey("ground-earlier");
    const auto cancelId=ImHashStr(i18n::T("Cancel placement"),0,ImHashStr(nkey.c_str(),0,ImHashStr("project-page")));
    Frame();Reveal(items.at(cancelId));Frame();const auto cancel=items.at(cancelId);
    Check(cancel.clip.Contains(cancel.box)&&!cancel.disabled&&!editor::g_projectDetails.count(nkey),"surface-current-request-cancel-visible-while-collapsed");
    Check(ReportHistory()==before,"surface-disclosure-does-not-mutate-history");
    Capture("task10-cancel-collapsed"+suffix);Mouse(cancel,0,true);Frame();Mouse(cancel,0,false);Frame();Frame();
    Check(core::PlaceRequestState(newest).requestCanceled&&!core::PlaceRequestState(older).requestCanceled&&!editor::g_place.active,"surface-collapsed-cancel-targets-current-only");
    Drain();Frame();
    Check(ReportKey(nkey).id==stable&&ReportKey("placement-earlier").id==earlierId&&PlacementReport(older).receipt.attached==1,"surface-count-changes-retain-widget-ids-and-older-receipt");
    Check(GroundReport(gid).receipt.reason=="no-surface"&&GroundReport(activeGid).receipt.reason=="canceled","surface-old-ground-reasons-retained");
    reportTrace.push_back("{\"case\":\""+current+"\",\"newest\":"+std::to_string(nid)+",\"olderActive\":"+std::to_string(oid)+",\"earlierWarning\":"+std::to_string(wid)+",\"widgetId\":"+std::to_string(stable)+",\"earlierId\":"+std::to_string(earlierId)+",\"reason\":\""+J(reason)+"\",\"path\":\""+J(longPath)+"\"}");
}
void ReportActions(const char* lang,int w,int h,bool compact) {
    Reset();ReportNative native;width=w;height=h;i18n::SetPreference(lang);Context();Frame();Frame();SelectTab(project);
    if(compact)Click(i18n::T(dock));
    const std::string suffix="-"+std::string(compact?"dock":"full")+"-"+lang+"-"+std::to_string(w)+"x"+std::to_string(h);
    const int uid=Spawn(3);editor::host_seam::SelectUid(uid);core::HideUid(uid);Drain();
    Click(i18n::T("Export Selection"));Click(i18n::T("Preflight"));
    Require(!editor::g_exportApproval.valid&&editor::g_exportApproval.excluded.size()==1,"surface-failed-preflight-exclusion");
    const auto exclusion=editor::g_exportApproval.excluded.front();
    Require(core::RestoreUid(uid),"surface-restore-selected-record");Drain();Click(i18n::T("Preflight"));
    Require(editor::g_exportApproval.valid,"surface-success-after-failed-preflight");Click(i18n::T("Cancel export"));
    Check(editor::g_exportStatus.empty()&&!editor::g_exportApproval.valid&&editor::g_exportAttempts.size()==2,"surface-cancel-has-no-stale-ready-and-keeps-attempts");
    ClickKey("export-attempts");ClickKey("export-1");
    const auto& old=editor::g_exportAttempts.front().approval.excluded.front();
    Check(old.uid==exclusion.uid&&old.prefab==exclusion.prefab&&old.reason==exclusion.reason&&!old.reason.empty(),"surface-old-exact-exclusion-after-success-and-cancel");
    ReportCapture("task10-export-prior-failure"+suffix,"export-1");ClickKey("export-attempts");
    Require(editor::DispatchProjectAction({"stones",proj_codec::Kind::Group},editor::ProjectAction::Read),"surface-real-file-read");Frame();
    Check(editor::g_projectReadValid&&editor::g_projectRead.records.size()==1&&editor::g_projectReadPath==root+"\\Groups\\stones.cdgroup"&&!editor::g_projectDetails.count("read-preview"),"surface-read-named-and-initially-collapsed");
    ClickKey("read-preview");ReportCapture("task10-read-preview"+suffix,"read-preview");
}
void LibrarySearch(const char* value) {
    LibraryClick("##library-search");auto& io=ImGui::GetIO();io.AddKeyEvent(ImGuiMod_Ctrl,true);io.AddKeyEvent(ImGuiKey_A,true);Frame();
    io.AddKeyEvent(ImGuiKey_A,false);io.AddKeyEvent(ImGuiMod_Ctrl,false);Frame();io.AddInputCharactersUTF8(value);Frame();
    if(!*value){io.AddKeyEvent(ImGuiKey_Backspace,true);Frame();io.AddKeyEvent(ImGuiKey_Backspace,false);Frame();}
    io.AddKeyEvent(ImGuiKey_Enter,true);Frame();io.AddKeyEvent(ImGuiKey_Enter,false);Frame();
    Require(std::string(editor::g_librarySearch)==value,"surface-search-real-text-input");
}
void LibraryChoice(const char* key,const char* choice) {LibraryClick(key);Click(i18n::T(choice));}
size_t LibraryViewportRows() {
    auto* child=LibraryWindow();std::vector<Item> rows;
    for(const auto& p:items)if(p.second.window==child&&p.second.label.find("###saved-file")!=std::string::npos)rows.push_back(p.second);
    const float stride=ImGui::GetTextLineHeightWithSpacing();
    Check(rows.size()<=(size_t)ceilf(child->InnerClipRect.GetHeight()/stride)+2,"surface-viewport-only-submitted-rows");
    std::sort(rows.begin(),rows.end(),[](const Item& a,const Item& b){return a.box.Min.y<b.box.Min.y;});
    for(size_t i=1;i<rows.size();++i)Check(fabsf(rows[i].box.Min.y-rows[i-1].box.Min.y-stride)<0.1f&&rows[i].box.Min.y>=rows[i-1].box.Max.y,"surface-row-stride-no-overlap");
    for(const auto& row:rows)Check(row.box.Min.x>=row.clip.Min.x&&row.box.Max.x<=row.clip.Max.x,"surface-row-horizontal-bounds");
    return rows.size();
}
void LibrarySurface(const char* lang,int w,int h,bool compact) {
    Reset();const std::string lib=root+"\\library-fixture";
    struct RestoreModDir { ~RestoreModDir(){host::SetModDir(root);editor::g_projectRefresh=true;} } restore;
    static bool prepared=false;
    if(!prepared) {
        CreateDirectoryA(lib.c_str(),nullptr);
        for(const char* folder:{"projects","Groups"}) {
            const bool group=std::string(folder)=="Groups";const std::string base=lib+"\\"+folder;
            CreateDirectoryA(base.c_str(),nullptr);CreateDirectoryA((base+"\\.archive").c_str(),nullptr);
            for(bool archive:{false,true})for(int n=1023;n>=0;--n){char name[32];sprintf_s(name,"item-%04d",n);Write(base+"\\"+(archive?".archive\\":"")+name+(group?".cdgroup":".cdproj"),"library bytes\r\n");}
        }
        for(const char* name:{"Alpha.cdproj","tie.cdproj","long_exact_saved_filename_for_narrow_library_selection.cdproj"})Write(lib+"\\projects\\"+name,"active bytes\r\n");
        for(const char* name:{"Alpha.cdproj","Tie.cdproj"})Write(lib+"\\projects\\.archive\\"+name,"archived bytes\r\n");
        Write(lib+"\\Groups\\kit.cdgroup","active group\r\n");Write(lib+"\\Groups\\.archive\\kit.cdgroup","archived group\r\n");prepared=true;
    }
    host::SetModDir(lib);width=w;height=h;i18n::SetPreference(lang);Context();Frame();Frame();SelectTab(project);if(compact)Click(i18n::T(dock));
    const std::string suffix="-"+std::string(compact?"dock":"full")+"-"+lang+"-"+std::to_string(w)+"x"+std::to_string(h);
    Require(editor::g_projectLibrary.entries.size()==4103&&editor::g_projectLibrary.active.projects==1027&&editor::g_projectLibrary.active.groups==1025&&editor::g_projectLibrary.archived.projects==1026&&editor::g_projectLibrary.archived.groups==1025,"surface-all-four-large-library-totals");
    LibraryView();const float fixedHeight=LibraryWindow()->Size.y;Check(LibraryViewportRows()>0,"surface-large-library-visible");Capture("task11-active"+suffix);
    SelectLibraryFile("long_exact_saved_filename_for_narrow_library_selection.cdproj");const auto selected=editor::g_librarySelected;
    auto* child=LibraryWindow();ImGui::SetScrollY(child,child->ScrollMax.y);Frame();Frame();LibraryView();
    Check(LibraryWindow()->Scroll.y>0&&LibraryWindow()->Size.y==fixedHeight&&LibraryViewportRows()>0,"surface-end-scroll-still-fixed-and-visible");Capture("task11-scrolled"+suffix);
    // Real Refresh keeps the exact selected path even after another sorted row appears before it.
    Write(lib+"\\projects\\000-first.cdproj","new bytes");LibraryClick(i18n::T("Refresh"));
    Check(editor::SameSavedFile(selected,editor::g_librarySelected)&&editor::g_projectLibrary.entries.size()==4104,"surface-refresh-keeps-selected-file-after-index-shift");
    Require(DeleteFileA((lib+"\\projects\\000-first.cdproj").c_str())!=0,"surface-remove-only-inserted-fixture");LibraryClick(i18n::T("Refresh"));
    LibraryChoice("##library-kind","Groups");LibraryChoice("##library-location","Archived");
    Check(editor::g_libraryRows.size()==1025,"surface-archived-group-filter");LibraryView();ImGui::SetScrollY(LibraryWindow(),LibraryWindow()->ScrollMax.y);Frame();Frame();Check(LibraryViewportRows()>0,"surface-archive-tail-visible");Capture("task11-archives"+suffix);
    LibraryChoice("##library-kind","Projects");LibraryChoice("##library-location","All locations");LibrarySearch("tIe");
    Require(editor::g_libraryRows.size()==2,"surface-mixed-case-query");SelectLibraryFile("Tie.cdproj",true);const auto exact=editor::g_librarySelected;
    Check(editor::g_projectLibrary.entries[editor::g_libraryRows[0]].file.filename=="Tie.cdproj"&&editor::g_projectLibrary.entries[editor::g_libraryRows[1]].file.filename=="tie.cdproj","surface-case-order-not-location-order");
    LibraryView();Capture("task11-case-ties"+suffix);
    auto* parent=LibraryWindow()->ParentWindow;ImGui::SetScrollY(parent,0);Frame();Frame();Click(i18n::T(compact?full:dock));LibraryView();
    Check(editor::g_libraryKind==1&&editor::g_libraryLocation==2&&std::string(editor::g_librarySearch)=="tIe"&&editor::SameSavedFile(exact,editor::g_librarySelected)&&editor::g_libraryRows.size()==2,"surface-full-dock-shares-search-filter-exact-selection");
    ImGui::SetScrollY(LibraryWindow()->ParentWindow,0);Frame();Frame();Click(i18n::T(compact?dock:full));LibraryView();
    LibrarySearch("no-matching-file");LibraryView();Check(editor::g_libraryRows.empty()&&LibraryViewportRows()==0&&LibraryWindow()->Size.y==fixedHeight,"surface-empty-has-zero-rows-same-height");Capture("task11-empty"+suffix);
    const auto groups=lib+"\\Groups",parked=lib+"\\Groups-parked";Require(MoveFileA(groups.c_str(),parked.c_str())!=0,"surface-park-groups");Write(groups,"invalid directory");LibraryClick(i18n::T("Refresh"));
    Check(editor::g_libraryResult.reason==core::FileReason::UnsafePath&&editor::g_projectLibrary.entries.size()==4103&&editor::SameSavedFile(exact,editor::g_librarySelected),"surface-error-not-empty-success");LibraryView();Capture("task11-refresh-error"+suffix);
    Require(DeleteFileA(groups.c_str())&&MoveFileA(parked.c_str(),groups.c_str()),"surface-restore-fixture-directory");
    traces.push_back("{\"case\":\""+current+"\",\"libraryRows\":4103,\"listHeight\":"+std::to_string(fixedHeight)+",\"selectedPath\":\""+J(exact.path)+"\"}");
}
std::string LifecycleRead(const std::string& path) { std::ifstream f(path,std::ios::binary);return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()}; }
void LifecycleType(const std::string& text) {
    Click("##file-confirm-name");auto& io=ImGui::GetIO();io.AddKeyEvent(ImGuiMod_Ctrl,true);io.AddKeyEvent(ImGuiKey_A,true);Frame();
    io.AddKeyEvent(ImGuiKey_A,false);io.AddKeyEvent(ImGuiMod_Ctrl,false);Frame();io.AddInputCharactersUTF8(text.c_str());Frame();
    io.AddKeyEvent(ImGuiKey_Enter,true);Frame();io.AddKeyEvent(ImGuiKey_Enter,false);Frame();
    Require(std::string(editor::g_libraryAction.typed)==text,"lifecycle-real-confirmation-text");
}
void LifecycleModalBounds() {
    const auto input=Find("##file-confirm-name");const auto cancel=Find(i18n::T("Cancel file action")),confirm=Find(i18n::T("Confirm permanent delete"));
    for(const auto& item:{input,cancel,confirm})Require(item.clip.Contains(item.box),"lifecycle-modal-control-unclipped");
    Check(!cancel.box.Overlaps(confirm.box)&&input.box.Max.y<=cancel.box.Min.y,"lifecycle-modal-controls-no-overlap");
    const auto* win=input.window;Check(win->Pos.x>=0&&win->Pos.y>=0&&win->Pos.x+win->Size.x<=width&&win->Pos.y+win->Size.y<=height,"lifecycle-modal-inside-display");
}
void LifecycleAction(const char* label) {
    Frame();auto* parent=LibraryWindow()->ParentWindow;
    ImGui::SetScrollY(parent,parent->ScrollMax.y);Frame();Frame();LibraryClick(i18n::T(label));
}
void LifecycleSurface(const char* lang,int w,int h,bool compact) {
    Reset();const std::string lib=root+"\\delete-"+lang+"-"+std::to_string(w)+(compact?"d":"f");
    struct RestoreModDir { ~RestoreModDir(){host::SetModDir(root);editor::g_projectRefresh=true;core::g_fileMutationFault=core::FileMutationFault::None;} } restore;
    CreateDirectoryA(lib.c_str(),nullptr);CreateDirectoryA((lib+"\\projects").c_str(),nullptr);CreateDirectoryA((lib+"\\Groups").c_str(),nullptr);
    const std::string name="Exact full saved filename with spaces.CDPROJ",path=lib+"\\projects\\"+name,archived=lib+"\\projects\\.archive\\"+name;
    const std::string original="opaque saved project bytes\r\n",neighbor=lib+"\\projects\\keep.cdproj";
    Write(path,original);Write(neighbor,"neighbor remains");host::SetModDir(lib);
    const int uid=Spawn(4);editor::host_seam::SelectUid(uid);editor::Act act;act.kind=editor::Act::Spawn;act.uid=uid;editor::Push({act});
    width=w;height=h;i18n::SetPreference(lang);Context();Frame();Frame();SelectTab(project);if(compact)Click(i18n::T(dock));
    const auto historyBefore=ReportHistory();const int made=creates,removed=removes,next=host::NextUid();const auto sceneBefore=ReportRecord(uid);
    const std::string suffix="-"+std::string(compact?"dock":"full")+"-"+lang+"-"+std::to_string(w)+"x"+std::to_string(h);
    SelectLibraryFile(name);LifecycleAction("Read"); // opaque bytes fail visibly, never load or rewrite
    Reveal(Find(i18n::T("Archive"))); // the error can shift the action below the previous viewport
    InView(i18n::T("Archive"));
    Check(Has(i18n::T("Archive"))&&!Has(i18n::T("Permanent delete")),"lifecycle-archive-default-delete-not-default");
    LifecycleAction("More file actions");Capture("task13-active-menu"+suffix);Click(i18n::T("Permanent delete"));
    Require(editor::g_libraryAction.selection&&core::SelectedFile(editor::g_libraryAction.selection).path==path,"lifecycle-modal-captures-exact-file");
    LifecycleModalBounds();Capture("task13-delete-modal"+suffix);
    LifecycleType("wrong.CDPROJ");Click(i18n::T("Confirm permanent delete"));
    Require(editor::g_libraryAction.result.reason==core::FileReason::ConfirmationMismatch&&LifecycleRead(path)==original,"lifecycle-wrong-name-visible-refusal");
    Capture("task13-wrong-name"+suffix);LifecycleModalBounds();Click(i18n::T("Cancel file action"));
    core::g_fileMutationFault=core::FileMutationFault::Move;LifecycleAction("Archive");
    Require(editor::g_libraryAction.result.reason==core::FileReason::MoveFailed&&LifecycleRead(path)==original&&editor::g_projectLibrary.active.projects==2,"lifecycle-move-failure-preserves-active-list");
    LifecycleAction("Read");Capture("task13-move-failure"+suffix);
    LifecycleAction("Archive");Require(GetFileAttributesA(path.c_str())==INVALID_FILE_ATTRIBUTES,"lifecycle-source-moved");
    Check(LifecycleRead(archived)==original&&editor::g_projectLibrary.active.projects==1&&editor::g_projectLibrary.archived.projects==1,"lifecycle-archive-refreshes-shared-snapshot");
    LibraryChoice("##library-location","Archived");SelectLibraryFile(name,true);LifecycleAction("Read");
    Check(Has(i18n::T("Restore"))&&Has(i18n::T("Purge"))&&!Has(i18n::T("Archive")),"lifecycle-archived-separate-restore-purge");Capture("task13-archived-actions"+suffix);
    Write(path,"destination must survive");LifecycleAction("Restore");
    Require(editor::g_libraryAction.result.reason==core::FileReason::Collision&&LifecycleRead(archived)==original&&LifecycleRead(path)=="destination must survive","lifecycle-restore-no-replace");
    LifecycleAction("Read");Capture("task13-restore-collision"+suffix);
    LifecycleAction("Purge");LifecycleType(name);LifecycleModalBounds();Capture("task13-purge-modal"+suffix);Click(i18n::T("Confirm permanent delete"));
    Check(editor::g_libraryAction.result.ok()&&GetFileAttributesA(archived.c_str())==INVALID_FILE_ATTRIBUTES&&LifecycleRead(path)=="destination must survive"&&LifecycleRead(neighbor)=="neighbor remains","lifecycle-purge-only-exact-archive");
    LibraryChoice("##library-location","Active");SelectLibraryFile(name);LifecycleAction("More file actions");Click(i18n::T("Permanent delete"));LifecycleType(name);Click(i18n::T("Confirm permanent delete"));
    Check(editor::g_libraryAction.result.ok()&&GetFileAttributesA(path.c_str())==INVALID_FILE_ATTRIBUTES&&LifecycleRead(neighbor)=="neighbor remains","lifecycle-direct-delete-only-exact-active-file");
    Check(ReportHistory()==historyBefore&&creates==made&&removes==removed&&host::NextUid()==next&&core::Spawned().size()==1&&ReportRecord(uid).obj==sceneBefore.obj&&ReportRecord(uid).pos.x==sceneBefore.pos.x,"lifecycle-no-scene-or-history-effect");
    Check(editor::g_projectLibrary.active.projects==1&&editor::g_projectLibrary.archived.projects==0&&editor::g_librarySelected.path.empty(),"lifecycle-success-clears-only-missing-file-selection");
}
void Run(const char* name,void(*fn)()){ current=name;int a=assertions,f=failures;try{fn();}catch(const std::exception& e){Check(false,e.what());}Trace("case-end");cases.push_back("{\"id\":\""+current+"\",\"assertions\":"+std::to_string(assertions-a)+",\"failures\":"+std::to_string(failures-f)+"}");}
}
void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx,ImGuiID id,const ImRect& bb,const ImGuiLastItemData*){if(id){auto& item=items[id];item.id=id;item.box=bb;item.window=ctx->CurrentWindow;item.clip=ctx->CurrentWindow->ClipRect;item.disabled=(ctx->CurrentItemFlags&ImGuiItemFlags_Disabled)!=0;}}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*,ImGuiID id,const char* label,ImGuiItemStatusFlags){items[id].label=label;}
void ImGuiTestEngineHook_Log(ImGuiContext*,const char*,...){}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*,ImGuiID id){auto i=items.find(id);return i==items.end()?nullptr:i->second.label.c_str();}
int main(int argc,char** argv){
    if(argc!=2)return 2;root=argv[1];host::SetModDir(root);host::OpenLog(root+"\\ui_shell.log");
    WNDCLASSW wc{};wc.lpfnWndProc=Wnd;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"WB079UiShell";if(!RegisterClassW(&wc))return 2;
    hwnd=CreateWindowW(wc.lpszClassName,L"UiShell hidden",WS_OVERLAPPEDWINDOW,0,0,1920,1080,nullptr,nullptr,wc.hInstance,nullptr);if(!hwnd)return 2;
    auto& e=host::Seam();e.ready=true;e.playerWorldPos=[](Vec3* out){*out={0,0,0};return true;};e.createGeneric=[](const std::string&,Vec3,Rot,float){++creates;return handle++;};e.remove=[](uintptr_t){++removes;return true;};e.moveInPlace=[](uintptr_t,Vec3,Rot,float){return true;};e.liveMove=e.moveInPlace;
    core::PrefabInfo p;p.path=prefab;p.name="stone";p.meshes=1;p.hasCenter=true;p.sx=p.sy=p.sz=2;host::SetPrefabIndex({p});host::SetCameraAvailable(true);
    host::Ui().refreshPreview=[](const std::string& path){Check(path==prefab,"refresh-actual-selected-prefab");++refreshes;};
    host::Ui().characters=std::make_shared<const std::vector<thumbgen::CharInfo>>(std::vector<thumbgen::CharInfo>{{17,"NHM_Guard","Guard", ""}});
    CreateDirectoryA((root+"\\projects").c_str(),nullptr);CreateDirectoryA((root+"\\Groups").c_str(),nullptr);Write(root+"\\projects\\workshop.cdproj",std::string(prefab)+"|0|0|0\n");
    proj_codec::Document doc;doc.kind=proj_codec::Kind::Group;doc.hasBounds=true;doc.bounds={{0,0,0},{-1,-1,-1},{1,1,1},true};doc.envelopes.push_back({1,doc.bounds});proj_codec::Record row;row.prefab=prefab;row.envelope=1;doc.records.push_back(row);std::string text,error;if(!proj_codec::Serialize(doc,text,error))return 2;Write(root+"\\Groups\\stones.cdgroup",text);
    // The host EXE has no ASI resources. Exercise the real read-only locale loader using shipped bytes in scratch.
    { std::ifstream f("asi\\cdmodkit\\data\\locales.tsv",std::ios::binary);if(!f)return 2;
      Write(root+"\\locales.tsv",std::string(std::istreambuf_iterator<char>(f),{})); }
    i18n::Initialize();i18n::SetPreference("en");i18n::AddGlyphText("\xEA\xB0\x80");i18n::RebuildGlyphRanges();
    Run("HOST-SETUP",Context);
    {
        Run("HUD-EPOCH",HudEpoch);Run("DELIVERY-COMPLETION",DeliveryCompletion);Run("STALE-SKIPPED",StaleSkipped);Run("FOCUS-RELEASE",FocusRelease);
        Run("DIAG-CLICK",DiagClick);Run("CLIPBOARD-ROUNDTRIP",ClipboardRoundtrip);Run("CLIPBOARD-FAIL",ClipboardFail);
        Run("SHELL-SWITCH",ShellSwitch);Run("FULL-ONLY-FALLBACK",Fallback);Run("SWITCH-FRAME-DOUBLE-ACTION",SwitchActions);
        Run("GROUP-UNDO",GroupUndo);Run("CAMERA-GRAB",CameraGrab);Run("PREVIEW-ACTION",Preview);Run("IME",Ime);Run("NARROW-CLIP",Gallery);Run("DIAG-CLICK-PRESENCE",DiagnosticsPresence);Run("HUD-VISUAL",DiagnosticsGallery);Run("V095-ENVIRONMENT-NPC",UpstreamContracts);
        Run("COLLAPSED-FULL-EN-DESKTOP",[](){CollapsedCarry("en",1920,1080,false);});
        Run("COLLAPSED-DOCK-EN-DESKTOP",[](){CollapsedCarry("en",1920,1080,true);});
        Run("COLLAPSED-FULL-KO-DESKTOP",[](){CollapsedCarry("ko",1920,1080,false);});
        Run("COLLAPSED-DOCK-KO-DESKTOP",[](){CollapsedCarry("ko",1920,1080,true);});
        Run("COLLAPSED-FULL-EN-NARROW",[](){CollapsedCarry("en",480,900,false);});
        Run("COLLAPSED-DOCK-EN-NARROW",[](){CollapsedCarry("en",480,900,true);});
        Run("COLLAPSED-FULL-KO-NARROW",[](){CollapsedCarry("ko",480,900,false);});
        Run("COLLAPSED-DOCK-KO-NARROW",[](){CollapsedCarry("ko",480,900,true);});
        Run("REPORT-EDITOR-PLACEMENT",ReportPlacements);
        Run("REPORT-EDITOR-ADMISSION",ReportGroundAdmission);
        Run("REPORT-EDITOR-SIBLINGS",ReportGroundSiblings);
        Run("REPORT-SURFACE-FULL-EN-DESKTOP",[](){ReportSurface("en",1920,1080,false);ReportActions("en",1920,1080,false);});
        Run("REPORT-SURFACE-DOCK-EN-DESKTOP",[](){ReportSurface("en",1920,1080,true);ReportActions("en",1920,1080,true);});
        Run("REPORT-SURFACE-FULL-KO-DESKTOP",[](){ReportSurface("ko",1920,1080,false);ReportActions("ko",1920,1080,false);});
        Run("REPORT-SURFACE-DOCK-KO-DESKTOP",[](){ReportSurface("ko",1920,1080,true);ReportActions("ko",1920,1080,true);});
        Run("REPORT-SURFACE-FULL-EN-NARROW",[](){ReportSurface("en",480,900,false);ReportActions("en",480,900,false);});
        Run("REPORT-SURFACE-DOCK-EN-NARROW",[](){ReportSurface("en",480,900,true);ReportActions("en",480,900,true);});
        Run("REPORT-SURFACE-FULL-KO-NARROW",[](){ReportSurface("ko",480,900,false);ReportActions("ko",480,900,false);});
        Run("REPORT-SURFACE-DOCK-KO-NARROW",[](){ReportSurface("ko",480,900,true);ReportActions("ko",480,900,true);});
    }
    Run("LIBRARY-FULL-EN-DESKTOP",[](){LibrarySurface("en",1920,1080,false);});
    Run("LIBRARY-DOCK-EN-DESKTOP",[](){LibrarySurface("en",1920,1080,true);});
    Run("LIBRARY-FULL-KO-DESKTOP",[](){LibrarySurface("ko",1920,1080,false);});
    Run("LIBRARY-DOCK-KO-DESKTOP",[](){LibrarySurface("ko",1920,1080,true);});
    Run("LIBRARY-FULL-EN-NARROW",[](){LibrarySurface("en",480,900,false);});
    Run("LIBRARY-DOCK-EN-NARROW",[](){LibrarySurface("en",480,900,true);});
    Run("LIBRARY-FULL-KO-NARROW",[](){LibrarySurface("ko",480,900,false);});
    Run("LIBRARY-DOCK-KO-NARROW",[](){LibrarySurface("ko",480,900,true);});
    Run("DELETE-FULL-EN-DESKTOP",[](){LifecycleSurface("en",1920,1080,false);});
    Run("DELETE-DOCK-EN-DESKTOP",[](){LifecycleSurface("en",1920,1080,true);});
    Run("DELETE-FULL-KO-DESKTOP",[](){LifecycleSurface("ko",1920,1080,false);});
    Run("DELETE-DOCK-KO-DESKTOP",[](){LifecycleSurface("ko",1920,1080,true);});
    Run("DELETE-FULL-EN-NARROW",[](){LifecycleSurface("en",480,900,false);});
    Run("DELETE-DOCK-EN-NARROW",[](){LifecycleSurface("en",480,900,true);});
    Run("DELETE-FULL-KO-NARROW",[](){LifecycleSurface("ko",480,900,false);});
    Run("DELETE-DOCK-KO-NARROW",[](){LifecycleSurface("ko",480,900,true);});
    Run("MAIN-TRAVEL-TERRAIN-PAGES",MainRuntimePages);
    Array("cases.json",cases);Array("action-trace.json",traces);Array("captures.json",captures);
    Array("routes.json",routes); Array("report-projections.json",reportTrace);
    if(routed)input::Shutdown();ImGui_ImplWin32_Shutdown();ImGui::DestroyContext();DestroyWindow(hwnd);UnregisterClassW(wc.lpszClassName,wc.hInstance);
    printf("ASSERTIONS=%d\nFAILURES=%d\n",assertions,failures);return failures?1:0;
}
