// UiShell v0.95 migration: real router observations and live editor status/log/copy contracts.
// The removed v0.94 diagnostics JSON/OS clipboard feature is not recreated by this fixture.
struct Signal {
    HANDLE h=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    ~Signal(){if(h)CloseHandle(h);}
    bool Wait(){return h&&WaitForSingleObject(h,5000)==WAIT_OBJECT_0;}
};
int GameCount(UINT msg,WPARAM key){int n=0;for(auto e:gameMessages)if(e.msg==msg&&e.key==key)++n;return n;}
void Key(int vk,bool down){LPARAM lp=1|(LPARAM(MapVirtualKeyW(vk,MAPVK_VK_TO_VSC))<<16);if(!down)lp|=(LPARAM(3)<<30);SendMessageW(hwnd,down?WM_KEYDOWN:WM_KEYUP,vk,lp);}
std::vector<input::RouteEvent> Events(){input::RouteEvent buf[input::kRouteCapacity];int n=input::TakeRouted(buf,input::kRouteCapacity);return {buf,buf+n};}
input::RouteEvent Event(UINT msg,WPARAM key){
    auto events=Events();for(auto it=events.rbegin();it!=events.rend();++it)if(it->msg==msg&&it->wParam==key){
        routes.push_back("{\"case\":\""+current+"\",\"epoch\":"+std::to_string(it->epoch)+",\"message\":"+std::to_string(msg)+",\"recipients\":"+std::to_string(it->recipients)+"}");return *it;
    }throw std::runtime_error("expected delivered route absent");
}
void BeginRoutes(){input::Init(hwnd);routed=true;manualPolicy=true;input::SetFreeCam(false);Policy(true,true);SendMessageW(hwnd,WM_SETFOCUS,0,0);Frame();gameMessages.clear();Events();}
void Shortcut(ImGuiKey key){
    ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl,true);ImGui::GetIO().AddKeyEvent(key,true);Frame();
    ImGui::GetIO().AddKeyEvent(key,false);ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl,false);Frame();
}
void HudEpoch(){
    Reset();BeginRoutes();auto own=input::CurrentOwnership();Key('J',true);Frame();auto ui=Event(WM_KEYDOWN,'J');
    Check(ImGui::IsKeyDown(ImGuiKey_J)&&GameCount(WM_KEYDOWN,'J')==0,"real-backend-only-ui-key");
    Check(ui.recipients==input::RecipientUi&&input::Matches(own,ui),"delivered-ui-epoch-matches");Key('J',false);Frame();Events();
    Policy(false,false,false,false,false,true);Signal delivered;recipientEvent=delivered.h;awaitedMsg=WM_KEYDOWN;awaitedKey='J';Key('J',true);
    Require(delivered.Wait(),"subscribed-original-recipient-arrived");recipientEvent=nullptr;Frame();auto game=Event(WM_KEYDOWN,'J');
    Check(GameCount(WM_KEYDOWN,'J')==1&&!ImGui::IsKeyDown(ImGuiKey_J)&&game.recipients==input::RecipientGame,"actual-game-delivery");
    Check(!input::Matches(own,game),"old-policy-does-not-match-new-delivery");Key('J',false);Frame();Events();
    Policy(true,false,true);Key(VK_NUMPAD8,true);Frame();auto placing=Event(WM_KEYDOWN,VK_NUMPAD8);
    Check(placing.recipients==input::RecipientGame&&GameCount(WM_KEYDOWN,VK_NUMPAD8)==1,"mouse-placement-does-not-consume-numpad");Key(VK_NUMPAD8,false);Frame();Events();
    editor::ToggleCameraMode();host::CaptureCamera({0,3,-8},0,-10);Policy(true,true,false,false,false);Key('W',true);Frame();auto camera=Event(WM_KEYDOWN,'W');
    Check(camera.recipients==(input::RecipientCamera|input::RecipientUi)&&ImGui::IsKeyDown(ImGuiKey_W)&&GameCount(WM_KEYDOWN,'W')==0,"camera-and-backend-not-game");Key('W',false);Frame();Events();
    Policy(true,true,false,true,true);Key('A',true);Frame();Check(Event(WM_KEYDOWN,'A').recipients==input::RecipientUi&&ImGui::IsKeyDown(ImGuiKey_A),"typing-overrides-camera");Key('A',false);Frame();Events();
    SendMessageW(hwnd,WM_IME_COMPOSITION,0,GCS_COMPSTR);Check(Event(WM_IME_COMPOSITION,0).recipients==input::RecipientSystem&&GameCount(WM_IME_COMPOSITION,0)==0,"IME-system-not-game");editor::StopCameraMode();
    Policy(false,false);Events();const auto drops=input::RoutedDropped();for(int i=0;i<300;++i)SendMessageW(hwnd,WM_CHAR,0x1000+i,0);
    auto burst=Events();Check(burst.size()==256&&input::RoutedDropped()==drops+44,"bounded-ring-explicit-drops");
    Check(burst.front().wParam==0x1000+44&&burst.back().wParam==0x1000+299,"ring-retains-newest-in-order");Check(Events().empty(),"event-drain-not-replayed");
}
void DeliveryCompletion(){
    Reset();BeginRoutes();Policy(false,false);Events();checkIncompleteDelivery=true;incompleteWasPublished=false;Key('K',true);checkIncompleteDelivery=false;
    Check(!incompleteWasPublished,"in-flight-recipient-not-published");auto e=Event(WM_KEYDOWN,'K');
    Check(e.recipients==input::RecipientGame&&GameCount(WM_KEYDOWN,'K')==1,"published-once-after-original-returns");Key('K',false);Frame();
}
void StaleSkipped(){
    Reset();BeginRoutes();auto old=input::CurrentOwnership();Key('J',true);Frame();auto event=Event(WM_KEYDOWN,'J');Key('J',false);Frame();
    Signal locked,release;bool workerReleased=false;const int frame=ImGui::GetFrameCount();
    std::thread holder([&](){input::FrameScope lock;Policy(false,false);SetEvent(locked.h);workerReleased=release.Wait();});
    bool ready=locked.Wait(),admitted=false;if(ready){input::FrameScope attempt(std::try_to_lock);admitted=attempt.OwnsLock();}
    SetEvent(release.h);holder.join();Check(ready&&workerReleased&&!admitted,"barrier-proves-skipped-frame-admission");
    Check(ImGui::GetFrameCount()==frame,"rejected-admission-does-not-draw");auto next=input::CurrentOwnership();
    Check(input::Matches(old,event)&&!input::Matches(next,event)&&next.epoch>old.epoch,"old-delivery-remains-stale-not-predicted");
    Key('Y',true);Frame();Check(input::Matches(next,Event(WM_KEYDOWN,'Y')),"new-delivery-matches-new-epoch");Key('Y',false);Frame();
}
void FocusRelease(){
    Reset();BeginRoutes();Key('A',true);Frame();Check(ImGui::IsKeyDown(ImGuiKey_A),"backend-holds-before-loss");Policy(false,false);Key(VK_SPACE,true);Frame();auto press=input::CurrentOwnership().epoch;Events();
    Signal released;recipientEvent=released.h;awaitedMsg=WM_KEYUP;awaitedKey=VK_SPACE;SendMessageW(hwnd,WM_KILLFOCUS,0,0);
    Require(released.Wait(),"subscribed-game-cleanup-release");recipientEvent=nullptr;Frame();
    Check(!ImGui::IsKeyDown(ImGuiKey_A)&&!input::ScanDown(MapVirtualKeyW(VK_SPACE,MAPVK_VK_TO_VSC),false)&&GameCount(WM_KEYUP,VK_SPACE)==1,"backend-game-and-scan-holds-cleared");
    bool cleanup=false;for(auto e:Events())if(e.cleanup&&e.wParam==VK_SPACE&&e.pressEpoch==press&&(e.recipients&input::RecipientGame))cleanup=true;
    Check(cleanup&&!input::CurrentOwnership().focused&&input::CurrentOwnership().epoch>press,"focus-cleanup-retains-press-identity");
    SendMessageW(hwnd,WM_SETFOCUS,0,0);Frame();Check(input::CurrentOwnership().focused,"focus-restored");
}
void DiagClick(){
    Reset();int calls=0;host::Services().camTrace=[&](int seconds){Check(seconds==16,"live-log-action-machine-argument");++calls;};
    SelectTab(ICON_LIST " Log");const size_t logs=editor::g_log.size();Click("camera trace (16 s)");
    Check(calls==1&&editor::g_log.size()==logs+1&&!editor::g_log.back().empty(),"log-action-records-current-result-once");
    const auto log=editor::g_log.back();SelectTab(project);Check(editor::DispatchProjectAction({"stones",proj_codec::Kind::Group},editor::ProjectAction::Read),"real-project-read");
    Check(editor::g_projectReadValid&&editor::g_projectRead.records.size()==1,"read-status-current-document");
    const auto count=core::Spawned().size();Check(!editor::DispatchProjectAction({"absent",proj_codec::Kind::Group},editor::ProjectAction::Read),"missing-read-refuses");
    Check(!editor::g_projectStatus.empty()&&core::Spawned().size()==count,"refusal-reason-no-world-side-effect");
    SelectTab(ICON_LIST " Log");Check(editor::g_log.back()==log,"log-persists-across-pages");host::Services().camTrace={};
}
void ClipboardRoundtrip(){
    Reset();int a=Spawn(1),b=Spawn(5);editor::host_seam::SelectUid(a);editor::host_seam::SelectAdd(b);SelectTab(scene);Click(dock);
    Shortcut(ImGuiKey_C);Check(editor::g_clip.size()==2&&editor::g_clip[0].rel.x==-2&&editor::g_clip[1].rel.x==2,"real-copy-preserves-relative-geometry");
    const int before=creates;Shortcut(ImGuiKey_V);Drain();Require(editor::g_place.active&&editor::g_place.isNew&&editor::g_place.m.size()==2,"real-paste-carries-new-pair");
    Check(creates==before+2&&editor::g_place.m[0].uid!=a&&editor::g_place.m[1].uid!=b,"paste-fresh-identities-once");
    Check(editor::g_place.m[1].origPos.x-editor::g_place.m[0].origPos.x==4,"copy-paste-spacing-roundtrip");
    Click("Cancel");Drain();Check(!editor::g_place.active&&core::Spawned().size()==2,"mouse-cancel-removes-only-pasted-copy");
}
void ClipboardFail(){
    Reset();SelectTab(scene);Click(dock);editor::g_clip.clear();const int before=creates;Shortcut(ImGuiKey_V);
    Check(creates==before&&!editor::g_place.active,"empty-paste-no-world-or-carried-mutation");
    int uid=Spawn(2);editor::host_seam::SelectUid(uid);Shortcut(ImGuiKey_C);Require(editor::g_clip.size()==1,"positive-copy-before-refusal");
    host::Seam().ready=false;Shortcut(ImGuiKey_V);Check(creates==before+1&&!editor::g_place.active,"unavailable-engine-refuses-paste");host::Seam().ready=true;
    Shortcut(ImGuiKey_V);Drain();Require(editor::g_place.active&&creates==before+2,"paste-recovers-after-refusal");Click("Cancel");Drain();
    editor::host_seam::ClearSelection();Shortcut(ImGuiKey_C);Shortcut(ImGuiKey_V);
    Check(editor::g_clip.empty()&&creates==before+2&&!editor::g_place.active,"empty-copy-cannot-replay-stale-payload");
}
