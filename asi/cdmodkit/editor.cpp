// In-game editor UI (Dear ImGui): category tree + filtered prefab list + details, scene objects (selection, groups, undo),
// placement mode (single objects and groups, snapping), copy/paste, line/circle tools, projects.
#define NOMINMAX
#include "core.h"
#include <imgui.h>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <set>
#include <map>
#include <cmath>
#include <commdlg.h>
#include <shellapi.h>
#include "icons.h"
#include "overlay.h"
#include "thumbgen.h"
#include "input.h"
#include "http_api.h"
#include "i18n.h"

namespace editor {
    // UI text goes through T("english") (translated, English fallback) - labels of tabs, popups and headers through
    // TStable, which keeps the English ImGui ID so state and OpenPopup names do not depend on the language
    using i18n::T; using i18n::TStable;
    static bool InputTextI18n(const char* label, const char* hint, char* buf, size_t cap, ImGuiInputTextFlags flags = 0) {
        const bool changed = ImGui::InputTextWithHint(label, hint, buf, cap, flags);
        if (changed) i18n::AddGlyphText(buf);   // committed CJK text may not be in the atlas yet; rebuild on the next frame
        if (ImGui::IsItemActive()) {
            const std::string comp = input::ImeComposition();
            if (!comp.empty()) {
                i18n::AddGlyphText(comp);
                const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                const float x = std::min(max.x - 8.0f, min.x + ImGui::GetStyle().FramePadding.x + ImGui::CalcTextSize(buf).x + 2.0f);
                const ImVec2 at = { x, min.y + ImGui::GetStyle().FramePadding.y };
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->PushClipRect(min, max, true);
                dl->AddText(at, ImGui::GetColorU32(ImGuiCol_Text), comp.c_str());
                const ImVec2 sz = ImGui::CalcTextSize(comp.c_str());
                dl->AddLine({ at.x, at.y + sz.y }, { std::min(max.x - 3.0f, at.x + sz.x), at.y + sz.y }, ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
                dl->PopClipRect();
            }
        }
        return changed;
    }
    static bool ComboT(const char* label, int* cur, const char* const items[], int count) {
        const char* shown[16]; if (count > 16) count = 16;
        for (int i = 0; i < count; i++) shown[i] = T(items[i]);   // stays valid: T keeps its last 16 decorated results
        return ImGui::Combo(label, cur, shown, count);
    }
    // Sliders/drags keep their native interaction. A separate small ">" control beside them expands a numeric field;
    // direct entry never takes over the slider itself, so double-clicking/dragging the slider cannot corrupt the value.
    static ImGuiID g_numericEditId = 0, g_numericEditEndedId = 0;
    static bool g_numericEditFocus = false, g_numericEditWasActive = false;
    static int g_numericEditStartedFrame = -1, g_numericEditLastSeenFrame = -1, g_numericEditEndedFrame = -1;
    static void CancelNumericEdit() {
        g_numericEditId = 0; g_numericEditFocus = g_numericEditWasActive = false;
        g_numericEditStartedFrame = g_numericEditLastSeenFrame = -1;
    }
    static bool NumericEditInput(const char* label, float* value, float min, float max, const char* format, bool vec3, bool* changed) {
        const ImGuiID id = ImGui::GetID(label); if (g_numericEditId != id) return false;
        g_numericEditLastSeenFrame = ImGui::GetFrameCount(); *changed = false;
        if (g_numericEditFocus) ImGui::SetKeyboardFocusHere();
        float before[3] = { value[0], vec3 ? value[1] : 0.0f, vec3 ? value[2] : 0.0f };
        ImGui::PushID("##numeric_edit");
        const bool entered = vec3 ? ImGui::InputFloat3("##value", value, format) : ImGui::InputFloat("##value", value, 0.0f, 0.0f, format);
        ImGui::PopID();
        const ImGuiIO& io = ImGui::GetIO(); const bool active = ImGui::IsItemActive() || io.WantTextInput;
        if (active) { g_numericEditWasActive = true; g_numericEditFocus = false; }
        const bool clickedOutside = ImGui::GetFrameCount() > g_numericEditStartedFrame && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsItemHovered();
        const bool stableDeactivation = g_numericEditWasActive && ImGui::GetFrameCount() > g_numericEditStartedFrame + 1 && ImGui::IsItemDeactivated() && !ImGui::IsItemActive();
        const bool submit = g_numericEditWasActive && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
        if (stableDeactivation || submit || ((g_numericEditFocus || g_numericEditWasActive) && clickedOutside)) {
            const int count = vec3 ? 3 : 1; for (int i = 0; i < count; ++i) value[i] = std::max(min, std::min(max, value[i]));
            g_numericEditEndedId = id; g_numericEditEndedFrame = ImGui::GetFrameCount(); CancelNumericEdit();
        }
        for (int i = 0; i < (vec3 ? 3 : 1); ++i) value[i] = std::max(min, std::min(max, value[i]));   // a half-typed 0 must not reach the object
        *changed = entered; for (int i = 0; i < (vec3 ? 3 : 1); ++i) *changed |= before[i] != value[i];
        return true;
    }
    static bool NumericEditFoldout(const char* label, float* value, float min, float max, const char* format, bool vec3) {
        const ImGuiID id = ImGui::GetID(label);
        const float gap = ImGui::GetStyle().ItemSpacing.x, buttonW = ImGui::GetFrameHeight();
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
        if (ImGui::GetItemRectMax().x + gap + buttonW <= right) ImGui::SameLine();
        ImGui::PushID(label);
        const bool clicked = ImGui::SmallButton(g_numericEditId == id ? "v##number" : ">##number");
        ImGui::PopID();
        if (clicked) {
            if (g_numericEditId == id) {
                const int count = vec3 ? 3 : 1; for (int i = 0; i < count; ++i) value[i] = std::max(min, std::min(max, value[i]));
                g_numericEditEndedId = id; g_numericEditEndedFrame = ImGui::GetFrameCount(); CancelNumericEdit();
            } else {
                g_numericEditId = id; g_numericEditFocus = true; g_numericEditWasActive = false;
                g_numericEditStartedFrame = g_numericEditLastSeenFrame = ImGui::GetFrameCount();
            }
        }
        if (g_numericEditId != id) return false;
        const float inputW = vec3 ? 240.0f : 120.0f;
        if (ImGui::GetItemRectMax().x + gap + inputW <= right) ImGui::SameLine();
        ImGui::SetNextItemWidth(std::min(inputW, ImGui::GetContentRegionAvail().x));
        bool edited = false; NumericEditInput(label, value, min, max, format, vec3, &edited); return edited;
    }
    static bool NumericEditEnded(const char* label) { return g_numericEditEndedFrame == ImGui::GetFrameCount() && g_numericEditEndedId == ImGui::GetID(label); }
    // the ">" button comes after the slider, so a caller's IsItemDeactivatedAfterEdit() sees the button: the slider's own end
    // (drag released) is recorded here as an ended edit, and callers check NumericEditEnded() for both ways
    static void NoteSliderEnd(const char* label) { if (ImGui::IsItemDeactivatedAfterEdit()) { g_numericEditEndedId = ImGui::GetID(label); g_numericEditEndedFrame = ImGui::GetFrameCount(); } }
    static bool SliderFloatEdit(const char* label, float* value, float min, float max, const char* format, ImGuiSliderFlags flags = 0) {
        const bool changed = ImGui::SliderFloat(label, value, min, max, format, flags); NoteSliderEnd(label);
        return NumericEditFoldout(label, value, min, max, format, false) || changed;
    }
    static bool DragFloatEdit(const char* label, float* value, float speed, float min, float max, const char* format, ImGuiSliderFlags flags = 0) {
        const bool changed = ImGui::DragFloat(label, value, speed, min, max, format, flags); NoteSliderEnd(label);
        return NumericEditFoldout(label, value, min, max, format, false) || changed;
    }
    static bool DragFloat3Edit(const char* label, float value[3], float speed, float min, float max, const char* format) {
        const bool changed = ImGui::DragFloat3(label, value, speed, min, max, format); NoteSliderEnd(label);
        return NumericEditFoldout(label, value, min, max, format, true) || changed;
    }
    static void SameLineOrWrap(bool compact, float nextWidth = 80.0f, float spacing = -1.0f) {
        const float gap = spacing >= 0 ? spacing : ImGui::GetStyle().ItemSpacing.x;
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
        if (!compact || ImGui::GetItemRectMax().x + gap + nextWidth <= right) ImGui::SameLine(0, spacing);
    }
    static constexpr const char* kEditorVersion = "0.96";

    static bool g_open = false;
    // browser state
    static char  g_filter[128] = "";
    static int   g_selCat = 0;            // 0 = all
    static int   g_selPrefab = -1;
    static bool  g_favOnly = false; static bool g_meshOnly = true;
    enum MainTabId {
        TabBrowser = 0, TabScene = 1, TabProject = 2, TabTravel = 3, TabSettings = 4,
        TabLog = 5, TabHistory = 6, TabNpcs = 7, TabEnvironment = 8
    };
    static bool  g_compact = false; static int g_dockCols = 2;   // narrow dock window; supported pages reuse the full editor functions
    static int g_mainTab = TabBrowser, g_compactPage = TabBrowser; static bool g_selectMainTab = false;
    static bool  g_showSpawnOpts = false, g_showMass = false; static int g_hoverUid = 0, g_hoverNpcUid = 0;
    static bool  g_cardView = false; static float g_cardSize = 96.0f;   // browser: tile view instead of the list (same matches / filters)
    static int   g_browserDragPrefab = -1;   // browser row/card being dragged out into the game view
    struct BrowserDropJob { int prefab = -1, ticket = 0; Vec3 center{}; float yaw = 0, scale = 1; };
    static std::vector<BrowserDropJob> g_browserDropJobs;   // ground is probed before spawning, so a new object's own collision cannot be mistaken for the surface
    static int   g_npcDragIndex = -1;        // character row/card being dragged out into the game view
    struct NpcDropJob { uint32_t key = 0; int ticket = 0; Vec3 at{}; int count = 1, formation = 0; float spacing = 1.5f, radius = 8.0f, fx = 0, fz = 1; bool ai = true; int behavior = 0; };
    static std::vector<NpcDropJob> g_npcDropJobs;
    static std::set<std::string> g_tagFilter;
    static std::vector<int> g_matches; static std::string g_lastKey;
    static float g_off[3] = { 0.0f, 0.0f, 0.0f };
    static float g_spawnYaw = 0, g_spawnScale = 1;
    static std::vector<int> g_recent;
    // variants: sibling prefabs that only differ by a trailing variant token are folded into one expandable row
    struct Row { int prefab; int head; int count; std::string base; };   // head 0 = plain, 1 = group header, 2 = child
    static std::vector<Row> g_rows; static std::set<std::string> g_openVar; static bool g_groupVariants = false; static bool g_rowsDirty = true;
    // collections: named prefab lists kept in bin64\cdmodkit\collections.txt (name, then paths, tab separated)
    struct Collection { std::string name; std::vector<std::string> paths; };
    static std::vector<Collection> g_colls; static int g_selColl = -1; static bool g_collsLoaded = false; static char g_newColl[48] = "";
    // scene state: object and managed-NPC selections share one Scene workflow.
    // g_rightUid and g_sceneLastEntity use positive object UIDs and negative NPC UIDs.
    static std::set<int> g_sel; static int g_primary = 0; static int g_lastClicked = 0;
    static int g_sceneLastEntity = 0;
    static bool g_boxSelecting = false, g_boxMoved = false, g_boxAdd = false; static ImVec2 g_boxStart{}, g_boxCurrent{}; static std::set<int> g_boxBase, g_boxNpcBase;
    static bool g_rightGesture = false, g_rightMoved = false, g_worldPopupRequested = false, g_worldPopupOpen = false; static ImVec2 g_rightStart{}, g_worldPopupPos{}; static int g_rightUid = 0;
    static bool  g_selectGroups = true, g_showDeleted = false;
    static float g_edit[3] = { 0, 0, 0 }, g_editScale = 1; static Rot g_editRot, g_editRot0; static Vec3 g_editPos0{}; static float g_editScale0 = 1;
    static int   g_editUid = 0; static bool g_live = true;
    static std::vector<std::string> g_log;
    static std::set<int> g_managedNpcSel; static int g_managedNpcPrimary = 0, g_managedNpcLast = 0;
    static int g_projTab = -1;   // -1 = whole scene, 0/new = unassigned, >0 = loaded project
    static const ManagedNpc* FindManagedNpc(const std::vector<ManagedNpc>& list, int uid);
    static void SelectAllManagedNpcs(const std::vector<ManagedNpc>& list, int projectFilter = -1);
    static void DeleteSelectedNpcs();
    static void GroupSelectedNpcs(bool makeGroup);
    static void SelectAllSceneEntities(const std::vector<SpawnedObj>& objects, const std::vector<ManagedNpc>& npcs, int projectFilter = -1);
    static void DeleteSceneSelection();
    static void GroupSceneSelection(bool makeGroup);
    static void MoveSceneSelection(Vec3 delta);
    // snapping
    static bool  g_snap = false; static int g_snapPosIdx = 3, g_snapYawIdx = 2;
    static const float kSnapPos[] = { 0.1f, 0.25f, 0.5f, 1.0f, 2.0f }; static const char* kSnapPosNames[] = { "0.1 m", "0.25 m", "0.5 m", "1 m", "2 m" };
    static const float kSnapYaw[] = { 5.0f, 15.0f, 30.0f, 45.0f, 90.0f }; static const char* kSnapYawNames[] = { "5 deg", "15 deg", "30 deg", "45 deg", "90 deg" };
    static float g_rotationStep = 25.0f;
    static float g_moveStep = 0.5f, g_scaleUpPct = 10.0f, g_scaleDownPct = 10.0f;

    static bool  g_preview = false; static bool g_previewShown = false; static bool g_previewSuppressed = false;
    static float g_catW = 260.0f;
    static bool  g_playMode = false;      // menu visible but every input goes to the game (Home toggles)
    static bool  g_cameraMode = false;    // free camera keeps the upstream editor/input UI; no full-screen input window
    static int   g_cameraViewMode = 0;    // compact 4-state orientation tool: free, level, straight down, straight up
    static DWORD g_cameraStartAt = 0; static bool g_cameraEverActive = false;
    static bool  g_cameraShortcutDown[256] = {};
    static float g_fx = 1, g_fz = 0; static Vec3 g_lastPlayer{}; static bool g_havePlayer = false;   // "in front": camera view (default) or last movement direction
    static bool  g_useCamera = true; static float g_camSign = 0;   // camSign: +1/-1 once the camera axis was compared with a walking direction
    static float g_mx = 1, g_mz = 0;                                // last movement direction

    // ---- undo / redo ----
    struct Act {
        enum Kind { Spawn, Move, Delete, SetGroup, NpcSpawn, NpcMove, NpcDelete, NpcControl, NpcGroup, ObjectNote, NpcNote, NpcLabel, GroupName } kind;
        int uid = 0; std::string prefab; Vec3 pos0{}, pos1{}; Rot rot0, rot1; float sc0 = 1, sc1 = 1;
        int group = 0, group1 = 0; int proj = 0; bool flag0 = false, flag1 = false; int behavior0 = 0, behavior1 = 0; std::string text0, text1;
    };   // object and managed-NPC committed edits share one chronological undo/redo stack
    struct HistoryEntry { unsigned long long serial = 0; std::vector<Act> acts; };
    static constexpr size_t kHistoryLimit = 1000;
    static unsigned long long g_historySerial = 0;
    static std::vector<HistoryEntry> g_undo, g_redo;
    static bool VecChanged(Vec3 a, Vec3 b) { return a.x != b.x || a.y != b.y || a.z != b.z; }
    static bool RotChanged(Rot a, Rot b) { return a.yaw != b.yaw || a.pitch != b.pitch || a.roll != b.roll; }
    static bool ActChanged(const Act& a) {
        if (a.kind == Act::Spawn || a.kind == Act::Delete || a.kind == Act::NpcSpawn || a.kind == Act::NpcDelete) return true;
        if (a.kind == Act::SetGroup || a.kind == Act::NpcGroup) return a.group != a.group1;
        if (a.kind == Act::NpcMove) return VecChanged(a.pos0, a.pos1);
        if (a.kind == Act::NpcControl) return a.flag0 != a.flag1 || a.behavior0 != a.behavior1;
        if (a.kind == Act::ObjectNote || a.kind == Act::NpcNote || a.kind == Act::NpcLabel || a.kind == Act::GroupName) return a.text0 != a.text1;
        // History is intentionally exact: even a sub-millimetre move or a tiny typed rotation/scale change counts.
        return VecChanged(a.pos0, a.pos1) || RotChanged(a.rot0, a.rot1) || a.sc0 != a.sc1;
    }
    static void Push(std::vector<Act> acts) {
        acts.erase(std::remove_if(acts.begin(), acts.end(), [](const Act& a) { return !ActChanged(a); }), acts.end());
        if (acts.empty()) return;
        g_undo.push_back({ ++g_historySerial, std::move(acts) });
        if (g_undo.size() > kHistoryLimit) g_undo.erase(g_undo.begin());
        g_redo.clear();
    }

    // ---- placement mode: one or many objects carried as a rigid set around a center; the gizmo edits the set ----
    struct Member { int uid; std::string prefab; Vec3 rel{}; Rot rot0; float scale0 = 1; Vec3 origPos{}; Rot origRot; float origScale = 1; };
    struct Place {
        bool active = false, isNew = false, reopen = false;
        std::vector<Member> m; std::string name;
        Vec3 center{}; float yaw = 0, scale = 1;     // yaw = rotation delta about the center, scale = multiplier
        float pitch = 0, roll = 0;                   // tilt deltas added to every member (no position change for sets)
        float radius = 1.0f;
        DWORD lastSend = 0; bool dirty = true; bool touched = false; Vec3 lastCenter{}; float lastYaw = 1e9f, lastScale = 1e9f, lastPitch = 1e9f, lastRoll = 1e9f;
        bool haveCenter = false; int prefabIdx = -1;   // single object: bbox center known (rotation about it) or still being measured
        float snapAx = 0, snapAz = 1;                  // fixed world-axis frame for optional keyboard placement
        bool mouse = false;                            // optional keyboard placement: mouse to gizmo while menu is closed
        bool keyTransformHeld = false;
        int hover = 0, drag = 0;   // gizmo: 1 X, 2 Y, 3 Z, 4 yaw ring, 5 center dot, 6 scale cube, 7 pitch ring, 8 roll ring
        float drag0 = 0; Vec3 dragCenter0{}; float dragYaw0 = 0, dragScale0 = 1, dragPitch0 = 0, dragRoll0 = 0;
        std::vector<core::MoveReq> historyBase;
        size_t historyMark = 0;
        int groundTicket = 0, groundIter = 0; float groundBottom = 0, groundTop = 0, groundStartY = 0;   // snap to ground in flight (see GroundStep)
    };
    static Place g_place;
    static void StopCameraMode(bool forceFreeCamOff = false) {
        if (!g_cameraMode && !(forceFreeCamOff && core::FreeCamActive())) return;
        g_cameraMode = false; g_cameraViewMode = 0; g_cameraStartAt = 0; g_cameraEverActive = false; memset(g_cameraShortcutDown, 0, sizeof g_cameraShortcutDown);
        g_rightGesture = g_rightMoved = g_worldPopupRequested = false; g_rightUid = 0;
        core::SetFreeCam(false); core::g_fcHoldMove = false; input::ClearKeys(); ImGui::GetIO().ClearInputKeys();
    }
    static void FinishCloseEditor() {
        StopCameraMode(true);
        g_playMode = false;
        g_boxSelecting = g_boxMoved = g_boxAdd = false; g_boxBase.clear(); g_boxNpcBase.clear();
        g_rightGesture = g_rightMoved = g_worldPopupRequested = false; g_rightUid = 0;
        g_browserDragPrefab = -1; g_browserDropJobs.clear();
        g_npcDragIndex = -1; g_npcDropJobs.clear();
        ImGui::GetIO().ClearInputKeys();
        if (g_previewShown) { core::PreviewClear(); g_previewShown = false; }
    }
    bool IsOpen() { return g_open; }
    bool PlayMode() { return g_playMode; }
    void TogglePlay() {
        if (!g_open) return;
        if (g_cameraMode) StopCameraMode();
        g_boxSelecting = g_boxMoved = g_boxAdd = false; g_boxBase.clear(); g_boxNpcBase.clear(); g_rightGesture = g_rightMoved = g_worldPopupRequested = false; g_rightUid = 0;
        g_playMode = !g_playMode; ImGui::GetIO().ClearInputKeys();
        if (g_playMode) {
            core::g_uiWantsMouse = core::g_uiWantsKeyboard = false;
            core::g_uiTextInput = core::g_uiMouseOverUi = false;
        }
    }
    void ToggleCameraMode() {
        if (!g_open) return;
        if (g_cameraMode) { StopCameraMode(); return; }
        if (!core::FreeCamAvailable()) { core::Log("[editor] camera mode: the free camera is not available in this game build"); return; }   // the header button says so too
        g_boxSelecting = g_boxMoved = g_boxAdd = false; g_boxBase.clear(); g_boxNpcBase.clear(); g_rightGesture = g_rightMoved = g_worldPopupRequested = false; g_rightUid = 0;
        g_playMode = false; g_cameraMode = true; g_cameraStartAt = GetTickCount(); g_cameraEverActive = false; memset(g_cameraShortcutDown, 0, sizeof g_cameraShortcutDown);
        input::TakeMouseDelta(nullptr, nullptr);
        input::ClearKeys(); ImGui::GetIO().ClearInputKeys(); core::SetFreeCam(true);
    }
    bool Placing() { return g_place.active; }
    // Mouse placement remains the default. The restored keyboard scheme is opt-in and can hand the mouse back to the game.
    bool MouseMode() { return g_place.active && (!core::g_keyboardPlacement || (g_open ? !g_playMode : g_place.mouse)); }
    static void DrawCameraViewTool() {
        const char* labels[4] = { "F", "H", "D", "U" };
        const float side = ImGui::GetFrameHeight();
        ImGui::BeginDisabled(!core::FreeCamAvailable());
        if (ImGui::Button(labels[g_cameraViewMode], ImVec2(side, side))) {
            g_cameraViewMode = (g_cameraViewMode + 1) & 3;
            if (!g_cameraMode) ToggleCameraMode();
            if (g_cameraMode && g_cameraViewMode > 0) core::FreeCamViewPreset(g_cameraViewMode);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", T("camera view: F free, H level, D straight down, U straight up; click to cycle"));
    }
    void Toggle() {
        g_open = !g_open;
        if (!g_open) FinishCloseEditor();
        else {
            g_playMode = false; ImGui::GetIO().ClearInputKeys();
            if (core::g_autoFreeCamOnOpen && core::FreeCamAvailable()) ToggleCameraMode();
        }
    }

    void ApplyStyle(float scale) {
        ImGuiStyle& s = ImGui::GetStyle();
        ImGui::StyleColorsDark();
        s.WindowRounding = 8; s.FrameRounding = 5; s.GrabRounding = 5; s.TabRounding = 5; s.PopupRounding = 6; s.ScrollbarRounding = 6; s.ChildRounding = 6;
        s.WindowPadding = ImVec2(12, 10); s.FramePadding = ImVec2(8, 5); s.ItemSpacing = ImVec2(8, 6); s.IndentSpacing = 18; s.ScrollbarSize = 14;
        s.WindowBorderSize = 1; s.FrameBorderSize = 0;
        ImVec4* c = s.Colors;
        c[ImGuiCol_WindowBg]        = ImVec4(0.09f, 0.09f, 0.10f, 0.96f);
        c[ImGuiCol_ChildBg]         = ImVec4(0.11f, 0.11f, 0.13f, 1.00f);
        c[ImGuiCol_PopupBg]         = ImVec4(0.10f, 0.10f, 0.12f, 0.98f);
        c[ImGuiCol_Border]          = ImVec4(0.30f, 0.26f, 0.22f, 0.60f);
        c[ImGuiCol_FrameBg]         = ImVec4(0.17f, 0.17f, 0.20f, 1.00f);
        c[ImGuiCol_FrameBgHovered]  = ImVec4(0.24f, 0.23f, 0.26f, 1.00f);
        c[ImGuiCol_FrameBgActive]   = ImVec4(0.30f, 0.27f, 0.28f, 1.00f);
        c[ImGuiCol_TitleBg]         = ImVec4(0.13f, 0.12f, 0.12f, 1.00f);
        c[ImGuiCol_TitleBgActive]   = ImVec4(0.38f, 0.18f, 0.14f, 1.00f);
        c[ImGuiCol_Header]          = ImVec4(0.55f, 0.24f, 0.18f, 0.55f);
        c[ImGuiCol_HeaderHovered]   = ImVec4(0.65f, 0.30f, 0.22f, 0.80f);
        c[ImGuiCol_HeaderActive]    = ImVec4(0.72f, 0.34f, 0.25f, 1.00f);
        c[ImGuiCol_Button]          = ImVec4(0.30f, 0.28f, 0.30f, 1.00f);
        c[ImGuiCol_ButtonHovered]   = ImVec4(0.62f, 0.30f, 0.22f, 1.00f);
        c[ImGuiCol_ButtonActive]    = ImVec4(0.75f, 0.36f, 0.26f, 1.00f);
        c[ImGuiCol_Tab]             = ImVec4(0.17f, 0.16f, 0.17f, 1.00f);
        c[ImGuiCol_TabHovered]      = ImVec4(0.62f, 0.30f, 0.22f, 1.00f);
        c[ImGuiCol_TabSelected]     = ImVec4(0.45f, 0.21f, 0.16f, 1.00f);
        c[ImGuiCol_SliderGrab]      = ImVec4(0.80f, 0.42f, 0.30f, 1.00f);
        c[ImGuiCol_SliderGrabActive]= ImVec4(0.92f, 0.52f, 0.36f, 1.00f);
        c[ImGuiCol_CheckMark]       = ImVec4(0.92f, 0.52f, 0.36f, 1.00f);
        c[ImGuiCol_Separator]       = ImVec4(0.30f, 0.26f, 0.22f, 0.60f);
        c[ImGuiCol_TableHeaderBg]   = ImVec4(0.18f, 0.16f, 0.16f, 1.00f);
        c[ImGuiCol_TableRowBgAlt]   = ImVec4(1, 1, 1, 0.03f);
        c[ImGuiCol_TextDisabled]    = ImVec4(0.55f, 0.53f, 0.50f, 1.00f);
        s.ScaleAllSizes(scale);
    }

    static void Note(const char* fmt, ...) {
        char b[512]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
        g_log.push_back(b); if (g_log.size() > 40) g_log.erase(g_log.begin());
        core::Log("[editor] %s", b);
    }
    static void AutoSaveTick() {
        static ULONGLONG retryAt = 0;
        if (!core::g_projectAutoSave || GetTickCount64() < retryAt) return;
        bool failed = false;
        std::set<int> ids;
        for (const auto& o : core::Spawned()) if (o.proj > 0) ids.insert(o.proj);
        for (const auto& n : core::ManagedNpcs()) if (n.proj > 0) ids.insert(n.proj);
        for (int id : ids) {
            if (!core::ProjectDirty(id)) continue;
            const std::string name = core::ProjectNameOf(id);
            if (!name.empty()) {
                if (core::SaveProject(name, core::SaveProjectOnly)) core::Log("[editor] autosave: %s", name.c_str());
                else { core::Log("[editor] autosave failed: %s", name.c_str()); failed = true; }
            }
        }
        retryAt = failed ? GetTickCount64() + 1000 : 0;
    }
    static unsigned char SearchFold(unsigned char c) { return c < 0x80 ? (unsigned char)tolower(c) : c; }
    static bool ContainsCI(const std::string& s, const std::string& w) {
        if (w.empty()) return true;
        for (size_t k = 0; k + w.size() <= s.size(); k++) { size_t j = 0; while (j < w.size() && SearchFold((unsigned char)s[k + j]) == SearchFold((unsigned char)w[j])) j++; if (j == w.size()) return true; }
        return false;
    }
    static bool HasTag(const std::string& tags, const std::string& tag) {
        size_t p = 0;
        while (p <= tags.size()) { size_t q = tags.find(',', p); std::string t = tags.substr(p, q == std::string::npos ? std::string::npos : q - p); size_t c = t.find(':'); if (c != std::string::npos) t = t.substr(0, c); if (t == tag) return true; if (q == std::string::npos) break; p = q + 1; }
        return false;
    }
    static bool InCat(int cat, int sel) { const auto& cats = core::Categories(); for (int n = cat; n >= 0; n = cats[n].parent) if (n == sel) return true; return false; }
    static bool IsVariantToken(const std::string& t) {
        if (t.empty()) return false;
        bool digits = true; for (char c : t) if (!isdigit((unsigned char)c)) { digits = false; break; }
        if (digits) return true;
        static const char* kv[] = { "a", "b", "c", "d", "e", "f", "snow", "broken", "dem", "venus", "kwe", "old", "new", "dirty", "clean", "wet", "dry", "lod0", "lod1", "lod2", "lod3", "lo", "hi", "s", "m", "l", "xl", "left", "right", "top", "bottom", "front", "back", "open", "close", "closed", "on", "off", "lit", "unlit", "day", "night", "red", "blue", "green", "white", "black", "brown", "grey", "gray", "dark", "light", nullptr };
        for (int i = 0; kv[i]; i++) if (t == kv[i]) return true;
        if (t.size() >= 2 && t.size() <= 5 && isalpha((unsigned char)t[0]) && isdigit((unsigned char)t.back())) { bool ok = true; for (size_t i = 1; i < t.size(); i++) if (!isdigit((unsigned char)t[i])) { ok = false; break; } if (ok) return true; }
        return false;
    }
    static std::string BaseName(const std::string& n) {   // strips up to three trailing variant tokens: wall_01_broken_a -> wall
        std::string b = n;
        for (int k = 0; k < 3; k++) { size_t u = b.rfind('_'); if (u == std::string::npos || u == 0) break; if (!IsVariantToken(b.substr(u + 1))) break; b = b.substr(0, u); }
        return b;
    }
    static std::string CollPath() { return core::ModDir() + "\\collections.txt"; }
    static void LoadColls() {
        if (g_collsLoaded) return; g_collsLoaded = true; g_colls.clear();
        FILE* f = fopen(CollPath().c_str(), "r"); if (!f) return;
        char line[8192];
        while (fgets(line, sizeof line, f)) {
            std::string l = line; while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            if (l.empty()) continue; Collection c; size_t p = 0;
            while (true) { size_t t = l.find('\t', p); std::string tok = l.substr(p, t == std::string::npos ? std::string::npos : t - p); if (c.name.empty()) c.name = tok; else if (!tok.empty()) c.paths.push_back(tok); if (t == std::string::npos) break; p = t + 1; }
            if (!c.name.empty()) g_colls.push_back(c);
        }
        fclose(f);
    }
    static void SaveColls() {
        FILE* f = fopen(CollPath().c_str(), "w"); if (!f) return;
        for (auto& c : g_colls) { fputs(c.name.c_str(), f); for (auto& p : c.paths) { fputc('\t', f); fputs(p.c_str(), f); } fputc('\n', f); }
        fclose(f);
    }
    static bool InColl(const Collection& c, const std::string& path) { return std::find(c.paths.begin(), c.paths.end(), path) != c.paths.end(); }
    static std::string ShortName(const std::string& prefab) { size_t sl = prefab.rfind('/'); std::string n = sl == std::string::npos ? prefab : prefab.substr(sl + 1); size_t dot = n.rfind('.'); if (dot != std::string::npos) n = n.substr(0, dot); return n; }
    static int IndexOfPrefab(const std::string& path) { const auto& idx = core::PrefabIndex(); for (int i = 0; i < (int)idx.size(); i++) if (idx[i].path == path) return i; return -1; }
    static const SpawnedObj* Find(const std::vector<SpawnedObj>& list, int uid) { for (auto& o : list) if (o.uid == uid) return &o; return nullptr; }
    static float SnapV(float v, float step) { return step > 0 ? roundf(v / step) * step : v; }
    static float WrapYaw(float y) { while (y > 180) y -= 360; while (y < -180) y += 360; return y; }

    // in-game names (gimmicks, in the UI language) replace the file-derived name wherever the user reads it; snapshot per frame
    static std::shared_ptr<const std::unordered_map<std::string, std::string>> g_gameNames;
    static const std::string& ShownName(const core::PrefabInfo& pi) {
        if (g_gameNames) { auto it = g_gameNames->find(pi.path); if (it != g_gameNames->end()) return it->second; }
        return pi.name;
    }
    static void RefreshMatches() {
        std::string key = std::string(g_filter) + "|" + std::to_string(g_selCat) + "|" + (g_favOnly ? "f" : "") + (g_meshOnly ? "m" : "") + "|" + std::to_string(g_selColl) + "|" + (g_groupVariants ? "v" : "") + "|" + std::to_string((uintptr_t)g_gameNames.get()) + "|";   // names arrive later: search again
        for (auto& t : g_tagFilter) key += t + ",";
        if (key == g_lastKey && !g_rowsDirty) return;
        const bool sameMatches = key == g_lastKey;
        g_lastKey = key; g_rowsDirty = false;
        if (!sameMatches) { g_matches.clear();
        std::set<std::string> collSet; if (g_selColl >= 0 && g_selColl < (int)g_colls.size()) for (auto& p : g_colls[g_selColl].paths) collSet.insert(p);
        std::vector<std::string> words; { std::string w; for (const char* p = g_filter; ; p++) { if (*p == ' ' || *p == 0) { if (!w.empty()) words.push_back(w); w.clear(); if (!*p) break; } else w += (char)SearchFold((unsigned char)*p); } }
        const auto& idx = core::PrefabIndex();
        for (int i = 0; i < (int)idx.size(); i++) {
            const auto& pi = idx[i];
            if (g_favOnly && !core::IsFavorite(i)) continue;
            // skinned-only / empty prefabs never show a visible spawn (tested in game: NPC and armour prefabs create an invisible
            // scene object); sets made of other prefabs do, their meshes are just not counted in the index
            if (g_meshOnly && pi.meshes == 0 && pi.children == 0 && pi.tags.find("SubPrefab") == std::string::npos) continue;   // appearances list their parts as children
            if (g_selColl >= 0 && !collSet.count(pi.path)) continue;
            if (g_selCat > 0 && !InCat(pi.cat, g_selCat)) continue;
            bool ok = true;
            const std::string& gn = ShownName(pi);
            for (auto& w : words) if (!ContainsCI(pi.path, w) && !ContainsCI(pi.tags, w) && !ContainsCI(pi.name, w) && !ContainsCI(gn, w)) { ok = false; break; }
            if (!ok) continue;
            for (auto& t : g_tagFilter) if (!HasTag(pi.tags, t)) { ok = false; break; }
            if (!ok) continue;
            g_matches.push_back(i);
            if (g_matches.size() >= 5000) break;
        } }
        const auto& idx = core::PrefabIndex();
        // rows: fold runs of siblings with the same base name (same folder) into one expandable row
        g_rows.clear();
        for (size_t a = 0; a < g_matches.size(); ) {
            const auto& pa = idx[g_matches[a]]; std::string base = pa.name; size_t b = a + 1;
            if (g_groupVariants) {   // extend the run while the common prefix (cut at an underscore) stays long enough
                while (b < g_matches.size() && idx[g_matches[b]].cat == pa.cat) {
                    const std::string& nb = idx[g_matches[b]].name; size_t k = 0; while (k < base.size() && k < nb.size() && base[k] == nb[k]) k++;
                    size_t cut = base.substr(0, k).rfind('_'); if (k == base.size() && k == nb.size()) cut = k; if (cut == std::string::npos) break;
                    if (cut < 8 || cut < (size_t)(0.6f * std::min(base.size(), nb.size()))) break;
                    base = base.substr(0, cut); b++;
                }
            }
            if (b - a >= 2) {
                std::string key2 = std::to_string(pa.cat) + "/" + base + "/" + std::to_string(g_matches[a]);
                g_rows.push_back({ g_matches[a], 1, (int)(b - a), key2 });
                if (g_openVar.count(key2)) for (size_t k = a; k < b; k++) g_rows.push_back({ g_matches[k], 2, 0, key2 });
            } else g_rows.push_back({ g_matches[a], 0, 0, "" });
            a = b;
        }
    }

    static void DrawCatNode(int n) {
        const auto& cats = core::Categories(); const auto& c = cats[n];
        ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | (c.children.empty() ? ImGuiTreeNodeFlags_Leaf : 0) | (g_selCat == n ? ImGuiTreeNodeFlags_Selected : 0);
        if (n == 0) fl |= ImGuiTreeNodeFlags_DefaultOpen;
        char label[160]; snprintf(label, sizeof label, "%s  (%d)###cat%d", c.name.c_str(), c.total, n);
        bool open = ImGui::TreeNodeEx(label, fl);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) g_selCat = n;
        if (open) {
            std::vector<int> kids = c.children;
            std::sort(kids.begin(), kids.end(), [&](int a, int b) { return cats[a].name < cats[b].name; });
            for (int k : kids) DrawCatNode(k);
            ImGui::TreePop();
        }
    }

    static void TrackFacing(const PosInfo& p, bool havePos) {
        { Vec3 fp, ff; if (core::FreeCamPose(&fp, &ff)) {   // flying: spawn spots and the placement start in front of the camera, not the character
            g_lastPlayer = fp; g_havePlayer = true; const float l = sqrtf(ff.x * ff.x + ff.z * ff.z); if (l > 0.05f) { g_fx = ff.x / l; g_fz = ff.z / l; } return; } }
        if (!havePos) return;
        bool moved = false;
        if (g_havePlayer) { float dx = p.world.x - g_lastPlayer.x, dz = p.world.z - g_lastPlayer.z; float l2 = dx * dx + dz * dz; if (l2 > 0.0004f) { float l = sqrtf(l2); g_mx = dx / l; g_mz = dz / l; moved = true; } }
        g_lastPlayer = p.world; g_havePlayer = true;
        Vec3 cam, camPos; const bool haveCam = core::CameraPose(&cam, &camPos);
        if (haveCam) {   // the camera looks from behind the character towards it: that fixes the sign of the view axis at all times
            const float vx = p.world.x - camPos.x, vz = p.world.z - camPos.z; const float vl = sqrtf(vx * vx + vz * vz);
            if (vl > 0.8f) { const float d = (cam.x * vx + cam.z * vz) / vl; if (fabsf(d) > 0.3f) g_camSign = d > 0 ? 1.0f : -1.0f; }
        }
        if (g_useCamera && haveCam) { const float sgn = g_camSign != 0 ? g_camSign : 1.0f; g_fx = cam.x * sgn; g_fz = cam.z * sgn; }
        else if (moved) { g_fx = g_mx; g_fz = g_mz; }
    }
    static Vec3 InFront(float radius, float height) {   // spot in front of the character, far enough for the object's footprint
        const float dist = std::max(2.0f, radius + 1.5f);
        return { g_lastPlayer.x + g_fx * dist, g_lastPlayer.y + height, g_lastPlayer.z + g_fz * dist };
    }
    // applies an object's rotation to a local offset: Ry(yaw) * Rx(pitch) * Rz(roll), the order MakeTransform builds the quaternion in
    static Vec3 RotLocal(const Rot& r, float x, float y, float z) {
        const float k = 3.14159265f / 180.0f; float c, s;
        c = cosf(r.roll * k);  s = sinf(r.roll * k);  { const float x1 = c * x - s * y, y1 = s * x + c * y; x = x1; y = y1; }
        c = cosf(r.pitch * k); s = sinf(r.pitch * k); { const float y1 = c * y - s * z, z1 = s * y + c * z; y = y1; z = z1; }
        c = cosf(r.yaw * k);   s = sinf(r.yaw * k);   { const float x1 = c * x + s * z, z1 = -s * x + c * z; x = x1; z = z1; }
        return { x, y, z };
    }
    static Vec3 LocalToWorld(const SpawnedObj& o, float x, float y, float z) { const Vec3 v = RotLocal(o.rot, x * o.scale, y * o.scale, z * o.scale); return { o.pos.x + v.x, o.pos.y + v.y, o.pos.z + v.z }; }
    // bbox center of a prefab instance (world) from its pivot, with the full rotation (yaw, pitch, roll)
    static Vec3 BboxCenter(const SpawnedObj& o) {
        int pi = IndexOfPrefab(o.prefab); if (pi < 0) return o.pos;
        const auto& info = core::PrefabIndex()[pi]; if (!info.hasCenter) { if (thumbgen::Ready()) thumbgen::Refresh(o.prefab); return o.pos; }
        return LocalToWorld(o, info.cx, info.cy, info.cz);
    }
    static float Footprint(const SpawnedObj& o) { int pi = IndexOfPrefab(o.prefab); if (pi < 0) return 1.0f; const auto& info = core::PrefabIndex()[pi]; return std::max(info.sx, info.sz) * 0.5f * o.scale; }
    static float ObjectRadius(const SpawnedObj& o) { int pi = IndexOfPrefab(o.prefab); if (pi < 0) return 1.0f; const auto& info = core::PrefabIndex()[pi]; return std::max(info.sx, std::max(info.sy, info.sz)) * 0.5f * o.scale; }

    static void RecordSpawn(std::vector<Act>& acts, int uid, const std::string& prefab, Vec3 pos, Rot rot, float sc, int group) {
        Act a; a.kind = Act::Spawn; a.uid = uid; a.prefab = prefab; a.pos1 = pos; a.rot1 = rot; a.sc1 = sc; a.group = group; acts.push_back(a);
    }
    // spawn spot for a prefab: in front of the character, footprint away, bounding box center on that spot; offsets are forward / up / sideways
    static Vec3 SpawnSpot(const core::PrefabInfo& pi, float yaw, float scale) {
        const float radius = std::max(pi.sx, pi.sz) * 0.5f * scale;
        const float dist = std::max(2.0f, radius + 1.5f) + g_off[0];
        Vec3 at = { g_lastPlayer.x + g_fx * dist + g_fz * g_off[2], g_lastPlayer.y + g_off[1], g_lastPlayer.z + g_fz * dist - g_fx * g_off[2] };
        if (pi.hasCenter) { const float t = yaw * 3.14159265f / 180.0f, cs = cosf(t), sn = sinf(t); at.x -= scale * (cs * pi.cx + sn * pi.cz); at.y -= scale * pi.cy; at.z -= scale * (-sn * pi.cx + cs * pi.cz); }
        return at;
    }
    static bool IsAppearance(const core::PrefabInfo& pi) { return pi.tags.rfind("Appearance", 0) == 0; }   // .app_xml rows: preview only
    static void SpawnSelected(const PosInfo& p) {
        if (g_selPrefab < 0) return;
        const auto& pi = core::PrefabIndex()[g_selPrefab];
        if (IsAppearance(pi)) { Note(T("these appearances can only be previewed; living characters are spawned from the NPCs tab")); return; }
        Vec3 at = SpawnSpot(pi, g_spawnYaw, g_spawnScale);
        int committedUid = g_previewShown ? core::PreviewCommit() : 0;
        if (committedUid) {
            Vec3 cpos = at; Rot crot{ g_spawnYaw }; float cscale = g_spawnScale;
            { const auto list = core::Spawned(); if (const SpawnedObj* o = Find(list, committedUid)) { cpos = o->pos; crot = o->rot; cscale = o->scale; } }
            g_previewShown = false; g_previewSuppressed = true; std::vector<Act> acts; RecordSpawn(acts, committedUid, pi.path, cpos, crot, cscale, 0); Push(std::move(acts)); Note(T("placed %s"), ShownName(pi).c_str()); }
        else { int uid = core::SpawnAt(pi.path, at, Rot{ g_spawnYaw }, g_spawnScale); std::vector<Act> acts; RecordSpawn(acts, uid, pi.path, at, Rot{ g_spawnYaw }, g_spawnScale, 0); Push(std::move(acts)); Note(T("spawn %s"), ShownName(pi).c_str()); }
        g_recent.erase(std::remove(g_recent.begin(), g_recent.end(), g_selPrefab), g_recent.end());
        g_recent.insert(g_recent.begin(), g_selPrefab); if (g_recent.size() > 12) g_recent.pop_back();
    }

    // ---- world -> screen (perspective from the camera object's frame; fov and mirror are user-calibrated settings) ----
    struct CamFrame { Vec3 pos, right, up, fwd; bool ok = false; float f = 1, aspect = 1, w = 1, h = 1; };
    static CamFrame LiveCam() {
        CamFrame c; ImGuiIO& io = ImGui::GetIO(); c.w = io.DisplaySize.x; c.h = io.DisplaySize.y; c.aspect = c.w / std::max(1.0f, c.h);
        // pose from the camera scene object (live every frame, verified against the renderer's view matrix to a few centimetres);
        // the projection from the renderer's own block when it is known, since the game changes the field of view with the
        // situation (50 degrees outdoors, 40 in town were measured); until then the manual / traced value
        // while flying the camera scene object carries the free camera's pose (cdmodkit.cpp FreeCamSceneXf), so the same read serves both
        if (!core::CameraBasis(&c.pos, &c.right, &c.up, &c.fwd)) return c;
        const float sgn = g_camSign != 0 ? g_camSign : 1.0f;   // forward sign as calibrated from the camera -> character vector
        c.fwd = { c.fwd.x * sgn, c.fwd.y * sgn, c.fwd.z * sgn };
        if (core::g_camMirror) c.right = { -c.right.x, -c.right.y, -c.right.z };
        float m00 = 0, m11 = 0; Vec3 rp;
        if (core::g_fovAuto && core::RenderCamera(&rp, nullptr, nullptr, nullptr, &m00, &m11)) { c.f = m11; c.aspect = m11 / m00; c.ok = true; return c; }
        float fov = core::g_fovDeg; if (core::g_fovAuto) { float live; if (core::CameraFov(&live)) fov = live; }
        c.f = 1.0f / tanf(fov * 3.14159265f / 360.0f); c.ok = true; return c;
    }
    static CamFrame g_currentCam;
    static void SampleCamera() { g_currentCam = LiveCam(); }
    static CamFrame CurrentCam() { return g_currentCam.ok ? g_currentCam : LiveCam(); }
    static bool WorldToScreen(const CamFrame& c, const Vec3& p, ImVec2* out) {
        const float dx = p.x - c.pos.x, dy = p.y - c.pos.y, dz = p.z - c.pos.z;
        const float x = dx * c.right.x + dy * c.right.y + dz * c.right.z, y = dx * c.up.x + dy * c.up.y + dz * c.up.z, z = dx * c.fwd.x + dy * c.fwd.y + dz * c.fwd.z;
        if (z < 0.05f) return false;
        out->x = (0.5f + 0.5f * (x / z) * c.f / c.aspect) * c.w; out->y = (0.5f - 0.5f * (y / z) * c.f) * c.h; return true;
    }
    static bool g_gizmo = true;
    static void DrawAxis(ImDrawList* dl, const CamFrame& c, const Vec3& a, const Vec3& b, ImU32 col, const char* label) {
        ImVec2 pa, pb; if (!WorldToScreen(c, a, &pa) || !WorldToScreen(c, b, &pb)) return;
        dl->AddLine(pa, pb, IM_COL32(0, 0, 0, 160), 5.0f); dl->AddLine(pa, pb, col, 3.0f);
        ImVec2 d(pb.x - pa.x, pb.y - pa.y); float l = sqrtf(d.x * d.x + d.y * d.y); if (l > 4) { d.x /= l; d.y /= l; ImVec2 n(-d.y, d.x);
            dl->AddTriangleFilled(pb, ImVec2(pb.x - d.x * 12 + n.x * 6, pb.y - d.y * 12 + n.y * 6), ImVec2(pb.x - d.x * 12 - n.x * 6, pb.y - d.y * 12 - n.y * 6), col); }
        dl->AddText(ImVec2(pb.x + 6, pb.y - 6), col, label);
    }
    static Vec3 MouseRay(const CamFrame& c, ImVec2 m) {   // world direction of the pixel under the mouse
        const float nx = (m.x / c.w * 2 - 1) * c.aspect / c.f, ny = (1 - m.y / c.h * 2) / c.f;
        Vec3 d = { c.right.x * nx + c.up.x * ny + c.fwd.x, c.right.y * nx + c.up.y * ny + c.fwd.y, c.right.z * nx + c.up.z * ny + c.fwd.z };
        const float l = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z); return { d.x / l, d.y / l, d.z / l };
    }
    // Keep every gizmo handle reachable even for very large prefabs. In world units the gizmo may be proportional to the object,
    // but on screen it is capped by both a useful pixel size and the distance from its center to the nearest viewport edge.
    static float GizmoScreenSize(const CamFrame& c, const Vec3& center, float radius) {
        float wanted = std::max(1.0f, radius * 0.8f);
        if (!c.ok || c.h <= 1.0f || c.f <= 0.01f) return wanted;
        const Vec3 d = { center.x - c.pos.x, center.y - c.pos.y, center.z - c.pos.z };
        const float depth = d.x * c.fwd.x + d.y * c.fwd.y + d.z * c.fwd.z;
        if (depth <= 0.05f) return std::min(wanted, 1.0f);
        ImVec2 sc; if (!WorldToScreen(c, center, &sc)) return std::min(wanted, std::max(0.25f, depth * 0.08f));
        const float edge = std::min(std::min(sc.x, c.w - sc.x), std::min(sc.y, c.h - sc.y));
        const float maxPx = std::max(12.0f, std::min(120.0f, edge - 12.0f));
        const float worldPerPx = (2.0f * depth) / (c.h * c.f);
        const float cap = std::max(0.18f, (maxPx / 1.25f) * worldPerPx);   // scale cubes sit at 1.25 * size
        return std::min(wanted, cap);
    }
    static float SegDist(ImVec2 p, ImVec2 a, ImVec2 b) { float vx = b.x - a.x, vy = b.y - a.y, l2 = vx * vx + vy * vy; float t = l2 > 0 ? ((p.x - a.x) * vx + (p.y - a.y) * vy) / l2 : 0; t = std::max(0.0f, std::min(1.0f, t)); float dx = a.x + vx * t - p.x, dy = a.y + vy * t - p.y; return sqrtf(dx * dx + dy * dy); }
    // parameter t of the point on the line (o + a*t) closest to the ray (ro + rd*s)
    static float LineRayParam(const Vec3& o, const Vec3& a, const Vec3& ro, const Vec3& rd) {
        const float wx = o.x - ro.x, wy = o.y - ro.y, wz = o.z - ro.z;
        const float aa = a.x * a.x + a.y * a.y + a.z * a.z, ab = a.x * rd.x + a.y * rd.y + a.z * rd.z, bb = 1.0f;
        const float aw = a.x * wx + a.y * wy + a.z * wz, bw = rd.x * wx + rd.y * wy + rd.z * wz;
        const float den = aa * bb - ab * ab; if (fabsf(den) < 1e-6f) return 0;
        return (ab * bw - bb * aw) / den;
    }
    static bool RayPlaneY(const Vec3& ro, const Vec3& rd, float y, Vec3* hit) { if (fabsf(rd.y) < 1e-4f) return false; float t = (y - ro.y) / rd.y; if (t < 0) return false; *hit = { ro.x + rd.x * t, y, ro.z + rd.z * t }; return true; }
    struct Ring { Vec3 n, u, v; float r; int id; };   // points: c + r*(cos a*u + sin a*v); a rotation about n by +d adds d to the angle
    struct GizmoGeo { Vec3 c, ax, ay, az; float size; ImVec2 sc, sx, sy, sz, cube[3]; bool ok = false, cubeOk[3] = {}; Ring ring[3]; };
    static GizmoGeo GizmoAt(const CamFrame& cf, const Vec3& center, float yawDeg, float pitchDeg, float size) {
        GizmoGeo g; g.c = center; g.size = size;
        const float t = yawDeg * 3.14159265f / 180.0f, cs = cosf(t), sn = sinf(t), tp = pitchDeg * 3.14159265f / 180.0f, cp = cosf(tp), sp = sinf(tp);
        g.ax = { cs, 0, -sn }; g.ay = { 0, 1, 0 }; g.az = { sn, 0, cs };   // yaw frame (move arrows), MakeTransform convention
        // gimbal rings matching Ry(yaw)*Rx(pitch)*Rz(roll): yaw about world Y, pitch about the yawed X axis, roll about the yawed+pitched Z axis
        const Vec3 lz = { g.az.x * cp, -sp, g.az.z * cp }, ly = { g.az.x * sp, cp, g.az.z * sp };
        g.ring[0] = { g.ay, g.az, g.ax, size * 0.8f, 4 };
        g.ring[1] = { g.ax, g.ay, g.az, size * 0.7f, 7 };
        g.ring[2] = { lz, g.ax, ly, size * 0.6f, 8 };
        g.ok = WorldToScreen(cf, center, &g.sc) && WorldToScreen(cf, { center.x + g.ax.x * size, center.y, center.z + g.ax.z * size }, &g.sx)
            && WorldToScreen(cf, { center.x, center.y + size, center.z }, &g.sy) && WorldToScreen(cf, { center.x + g.az.x * size, center.y, center.z + g.az.z * size }, &g.sz);
        const float e = size * 1.25f;   // uniform scale handles (cubes) beyond the arrow tips
        g.cubeOk[0] = WorldToScreen(cf, { center.x + g.ax.x * e, center.y, center.z + g.ax.z * e }, &g.cube[0]);
        g.cubeOk[1] = WorldToScreen(cf, { center.x, center.y + e, center.z }, &g.cube[1]);
        g.cubeOk[2] = WorldToScreen(cf, { center.x + g.az.x * e, center.y, center.z + g.az.z * e }, &g.cube[2]);
        return g;
    }
    static Vec3 RingPt(const Vec3& c, const Ring& r, float a) { const float ca = cosf(a) * r.r, sa = sinf(a) * r.r; return { c.x + r.u.x * ca + r.v.x * sa, c.y + r.u.y * ca + r.v.y * sa, c.z + r.u.z * ca + r.v.z * sa }; }
    static bool RayPlane(const Vec3& ro, const Vec3& rd, const Vec3& c, const Vec3& n, Vec3* hit) {
        const float den = rd.x * n.x + rd.y * n.y + rd.z * n.z; if (fabsf(den) < 1e-4f) return false;
        const float t = ((c.x - ro.x) * n.x + (c.y - ro.y) * n.y + (c.z - ro.z) * n.z) / den; if (t < 0) return false;
        *hit = { ro.x + rd.x * t, ro.y + rd.y * t, ro.z + rd.z * t }; return true;
    }
    static bool RingAngle(const CamFrame& cf, const Vec3& rd, const Vec3& c, const Ring& r, float* ang) {   // angle of the cursor on the ring's plane
        Vec3 h; if (!RayPlane(cf.pos, rd, c, r.n, &h)) return false;
        const Vec3 d = { h.x - c.x, h.y - c.y, h.z - c.z };
        *ang = atan2f(d.x * r.v.x + d.y * r.v.y + d.z * r.v.z, d.x * r.u.x + d.y * r.u.y + d.z * r.u.z); return true;
    }
    static int GizmoHover(const CamFrame& cf, const GizmoGeo& g, ImVec2 m) {
        if (!g.ok) return 0;
        float dc = sqrtf((m.x - g.sc.x) * (m.x - g.sc.x) + (m.y - g.sc.y) * (m.y - g.sc.y)); if (dc < 14) return 5;
        for (int i = 0; i < 3; i++) if (g.cubeOk[i] && fabsf(m.x - g.cube[i].x) < 9 && fabsf(m.y - g.cube[i].y) < 9) return 6;
        if (SegDist(m, g.sc, g.sx) < 10) return 1; if (SegDist(m, g.sc, g.sy) < 10) return 2; if (SegDist(m, g.sc, g.sz) < 10) return 3;
        for (int k = 0; k < 3; k++) {
            ImVec2 prev; bool hp = false;
            for (int i = 0; i <= 48; i++) { float a = i * 6.28318531f / 48; ImVec2 p; if (WorldToScreen(cf, RingPt(g.c, g.ring[k], a), &p)) { if (hp && SegDist(m, prev, p) < 8) return g.ring[k].id; prev = p; hp = true; } else hp = false; }
        }
        return 0;
    }
    // axis cross, three gimbal rings, scale cubes at a world point; hover highlights the handle under the mouse
    static void DrawGizmo(const Vec3& center, float yawDeg, float pitchDeg, float size, int hover = 0) {
        CamFrame c = CurrentCam(); if (!c.ok) return;
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        auto hi = [&](int h, ImU32 col) { return hover == h ? IM_COL32(255, 240, 120, 255) : col; };
        GizmoGeo g = GizmoAt(c, center, yawDeg, pitchDeg, size);
        const ImU32 cols[3] = { IM_COL32(230, 70, 60, 255), IM_COL32(90, 210, 80, 255), IM_COL32(70, 120, 240, 255) };
        const ImU32 ringCols[3] = { IM_COL32(90, 210, 80, 200), IM_COL32(230, 70, 60, 200), IM_COL32(70, 120, 240, 200) };
        for (int k = 0; k < 3; k++) {
            ImVec2 prev; bool havePrev = false; const int id = g.ring[k].id;
            for (int i = 0; i <= 48; i++) { float a = i * 6.28318531f / 48; ImVec2 p; if (WorldToScreen(c, RingPt(center, g.ring[k], a), &p)) { if (havePrev) dl->AddLine(prev, p, hi(id, ringCols[k]), hover == id ? 3.5f : 2.0f); prev = p; havePrev = true; } else havePrev = false; }
        }
        DrawAxis(dl, c, center, { center.x + g.ax.x * size, center.y, center.z + g.ax.z * size }, hi(1, cols[0]), "X");
        DrawAxis(dl, c, center, { center.x, center.y + size, center.z }, hi(2, cols[1]), "Y");
        DrawAxis(dl, c, center, { center.x + g.az.x * size, center.y, center.z + g.az.z * size }, hi(3, cols[2]), "Z");
        ImVec2 pc; if (WorldToScreen(c, center, &pc)) dl->AddCircleFilled(pc, hover == 5 ? 8.0f : 5.0f, hi(5, IM_COL32(255, 255, 255, 230)));
        for (int i = 0; i < 3; i++) if (g.cubeOk[i]) { const float h = hover == 6 ? 7.0f : 5.0f; dl->AddRectFilled({ g.cube[i].x - h, g.cube[i].y - h }, { g.cube[i].x + h, g.cube[i].y + h }, hi(6, cols[i])); dl->AddRect({ g.cube[i].x - h, g.cube[i].y - h }, { g.cube[i].x + h, g.cube[i].y + h }, IM_COL32(0, 0, 0, 160)); }
    }
    static bool g_calib = false;
    static void DrawCalibrationMarker(const PosInfo& p, bool havePos) {   // marker at the character's feet plus a 1 m cross: tune fov / mirror until it sits on the character
        if (!g_calib || !havePos) return;
        CamFrame c = CurrentCam(); if (!c.ok) return;
        ImDrawList* dl = ImGui::GetForegroundDrawList(); ImVec2 s;
        if (WorldToScreen(c, p.world, &s)) { dl->AddCircle(s, 14, IM_COL32(255, 220, 40, 255), 24, 3); dl->AddText(ImVec2(s.x + 18, s.y - 8), IM_COL32(255, 220, 40, 255), T("feet")); }
        ImVec2 h; if (WorldToScreen(c, { p.world.x, p.world.y + 1.8f, p.world.z }, &h)) { dl->AddCircle(h, 10, IM_COL32(255, 220, 40, 255), 24, 2); dl->AddText(ImVec2(h.x + 14, h.y - 8), IM_COL32(255, 220, 40, 255), T("head (1.8 m)")); }
        DrawGizmo(p.world, 0, 0, 1.0f);
    }
    static void DropCarried();
    static void CancelCarried(bool notify = true);
    static void StartGroundSnap(Place& P);
    static void CommitPlaceHistory(Place& P);
    static void DrawPlaceHud() {
        Place& P = g_place; ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 28.0f), ImGuiCond_Always, ImVec2(0.5f, 0));
        ImGui::SetNextWindowBgAlpha(0.75f);
        if (ImGui::Begin("##placehud", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
            ImGui::Text(ICON_CUBE "  %s  %s%s", T(P.isNew ? "Place" : "Grab"), P.name.c_str(), P.m.size() > 1 ? T("  (group)") : "");
            ImGui::TextDisabled(T("center %.1f  %.1f  %.1f    rotation %+.0f    tilt %+.0f, %+.0f    scale x%.2f    snap %s"), P.center.x, P.center.y, P.center.z, P.yaw, P.pitch, P.roll, P.scale,
                g_snap ? (std::string(T(kSnapPosNames[g_snapPosIdx])) + ", " + T(kSnapYawNames[g_snapYawIdx])).c_str() : T("off"));
            if (ImGui::Button(T("drop"))) DropCarried();
            ImGui::SameLine(); if (ImGui::Button(T("Cancel"))) CancelCarried();
            ImGui::SameLine(); if (ImGui::Button(T("To ground"))) StartGroundSnap(P);
            ImGui::SameLine(); if (ImGui::Button(T("level"))) { if (P.m.size() == 1) { P.pitch = -P.m[0].rot0.pitch; P.roll = -P.m[0].rot0.roll; } else { P.pitch = P.roll = 0; } P.dirty = P.touched = true; CommitPlaceHistory(P); }
            ImGui::SameLine(); ImGui::Checkbox(T("snap"), &g_snap);
            if (core::g_keyboardPlacement && !g_compact) {
                auto kn = [](int pk) { return core::KeyName(core::g_placeKeys[pk]); };
                ImGui::TextDisabled(T("Move: %s %s %s %s%s     Rotate: %s %s     Height: %s %s     Size: %s %s"), kn(core::PK_FWD), kn(core::PK_BACK), kn(core::PK_LEFT), kn(core::PK_RIGHT),
                    g_snap ? T(" (one grid step per press)") : (std::string(" (") + kn(core::PK_FAST) + " = " + T("fast") + ")").c_str(), kn(core::PK_ROT_L), kn(core::PK_ROT_R), kn(core::PK_UP), kn(core::PK_DOWN), kn(core::PK_SCALE_UP), kn(core::PK_SCALE_DOWN));
                ImGui::TextDisabled(T("%s = bring in front     %s = snap     %s = mouse gizmo and game     %s = level     %s = ground     %s = drop     %s = %s"),
                    kn(core::PK_FETCH), kn(core::PK_SNAP), kn(core::PK_MOUSE), kn(core::PK_LEVEL), kn(core::PK_GROUND), kn(core::PK_DROP), kn(core::PK_CANCEL), T(P.isNew ? "cancel" : "put back"));
            } else if (!g_compact && !g_cameraMode) ImGui::TextDisabled(T("To look around: %s opens the editor, %s there switches to the free camera"), core::KeyName(core::g_keyToggle), core::KeyName(core::g_keyMode));
        }
        ImGui::End();
    }

    // ---- placement ----
    static bool IsCarried(int uid);
    static void StartGrab(const std::vector<int>& uids, bool isNew, const std::string& name) {
        if (uids.empty() || !core::GameThreadReady()) return;
        if (g_place.active) DropCarried();   // whatever is in the hands is left where it is (undo step included)
        const bool keepCamera = g_cameraMode;
        auto list = core::Spawned();
        Place P; P.active = true; P.isNew = isNew; P.name = name;
        Vec3 c{ 0, 0, 0 };
        for (int uid : uids) { const SpawnedObj* o = Find(list, uid); if (!o || o->hidden) continue; Member m; m.uid = uid; m.prefab = o->prefab; m.rot0 = o->rot; m.scale0 = o->scale; m.origPos = o->pos; m.origRot = o->rot; m.origScale = o->scale; P.m.push_back(m); }
        if (P.m.empty()) return;
        if (P.m.size() == 1) { const SpawnedObj* o = Find(list, P.m[0].uid); c = BboxCenter(*o); P.radius = Footprint(*o); P.prefabIdx = IndexOfPrefab(o->prefab); P.haveCenter = P.prefabIdx >= 0 && core::PrefabIndex()[P.prefabIdx].hasCenter; }
        else { for (auto& m : P.m) { c.x += m.origPos.x; c.y += m.origPos.y; c.z += m.origPos.z; } const float inv = 1.0f / (float)P.m.size(); c.x *= inv; c.y *= inv; c.z *= inv;
               for (auto& m : P.m) { float dx = m.origPos.x - c.x, dz = m.origPos.z - c.z; P.radius = std::max(P.radius, sqrtf(dx * dx + dz * dz) + 1.0f); } }
        for (auto& m : P.m) m.rel = { m.origPos.x - c.x, m.origPos.y - c.y, m.origPos.z - c.z };
        P.center = c; P.lastCenter = c; P.lastYaw = 0; P.lastScale = 1; P.dirty = isNew;
        P.historyMark = g_undo.size();
        for (const auto& m : P.m) P.historyBase.push_back({ m.uid, m.origPos, m.origRot, m.origScale });
        if (isNew) {
            std::vector<Act> spawned; spawned.reserve(P.m.size());
            for (const auto& m : P.m) {
                Act a; a.kind = Act::Spawn; a.uid = m.uid; a.prefab = m.prefab; a.pos1 = m.origPos; a.rot1 = m.origRot; a.sc1 = m.origScale;
                const SpawnedObj* o = Find(list, m.uid); if (o) { a.group = o->group; a.proj = o->proj; }
                spawned.push_back(std::move(a));
            }
            Push(std::move(spawned));
        }
        if (fabsf(g_fx) >= fabsf(g_fz)) { P.snapAx = g_fx > 0 ? 1.0f : -1.0f; P.snapAz = 0; } else { P.snapAx = 0; P.snapAz = g_fz > 0 ? 1.0f : -1.0f; }
        if (keepCamera) { P.reopen = false; }                             // camera mode stays live while the gizmo is shown
        else if (g_compact && g_open) { P.reopen = false; }               // the dock stays where it is
        else { P.reopen = g_open; g_open = false; }
        if (!keepCamera) StopCameraMode();
        g_playMode = false; core::g_placing = true; input::ClearKeys();
        g_place = P;
        if (core::g_keyboardPlacement) Note(T("%s: %s | %d objects | %s = done, %s %s"), T(isNew ? "placing" : "grabbed"), name.c_str(), (int)P.m.size(), core::KeyName(core::g_placeKeys[core::PK_DROP]), core::KeyName(core::g_placeKeys[core::PK_CANCEL]), T(isNew ? "cancels" : "puts back"));
        else Note("%s: %s", T(isNew ? "placing" : "grabbed"), name.c_str());
    }
    static void StartPlaceNew(const PosInfo& p, bool havePos) {
        CamFrame cf = CurrentCam();
        if (g_selPrefab < 0 || (!havePos && !cf.ok) || !core::GameThreadReady()) return;
        const auto& pi = core::PrefabIndex()[g_selPrefab];
        if (IsAppearance(pi)) { Note(T("these appearances can only be previewed; living characters are spawned from the NPCs tab")); return; }
        if (g_place.active) {   // PLACE / double-click while something is already carried
            const Place& P = g_place;
            if (P.isNew && !P.touched && P.m.size() == 1 && P.m[0].prefab == pi.path) { Note("%s: %s", T("placing"), ShownName(pi).c_str()); return; }   // a repeated double-click, not a second copy
        }
        if (g_previewShown) { core::PreviewClear(); g_previewShown = false; }
        Vec3 at;
        if (cf.ok) {
            // Double-click placement must be visible immediately, even when the free camera is far from the character or pitched up/down.
            // Fit the prefab's largest half-extent inside ~75% of the narrower camera half-FOV, then place its bbox center on the view axis.
            const float extent = std::max(0.5f, std::max(pi.sx, std::max(pi.sy, pi.sz)) * 0.5f * g_spawnScale);
            const float tanHalfV = 1.0f / std::max(0.1f, cf.f), tanHalfH = cf.aspect / std::max(0.1f, cf.f);
            const float fitTan = std::max(0.08f, std::min(tanHalfV, tanHalfH) * 0.75f);
            const float dist = std::max(2.5f, extent / fitTan + 0.75f);
            Vec3 center = { cf.pos.x + cf.fwd.x * dist, cf.pos.y + cf.fwd.y * dist, cf.pos.z + cf.fwd.z * dist };
            at = center;
            if (pi.hasCenter) { const float t = g_spawnYaw * 3.14159265f / 180.0f, cs = cosf(t), sn = sinf(t); at.x -= g_spawnScale * (cs * pi.cx + sn * pi.cz); at.y -= g_spawnScale * pi.cy; at.z -= g_spawnScale * (-sn * pi.cx + cs * pi.cz); }
        } else at = SpawnSpot(pi, g_spawnYaw, g_spawnScale);
        int uid = core::SpawnAt(pi.path, at, Rot{ g_spawnYaw }, g_spawnScale);
        if (uid) StartGrab({ uid }, true, ShownName(pi));
    }
    static void FinishPlace() {
        g_place.active = false; core::g_placing = false; input::ClearKeys();
        if (g_place.reopen) { g_open = true; g_playMode = true; }   // visible again, but the game keeps the input (Home to edit)
    }
    static void CancelCarried(bool notify) {
        Place& P = g_place; if (!P.active) return;
        if (P.isNew) {
            for (auto& m : P.m) { core::HideUid(m.uid); core::ForgetUid(m.uid); }
            if (g_undo.size() > P.historyMark) g_undo.resize(P.historyMark);
            g_redo.clear();
        }
        else {
            std::vector<core::MoveReq> r; for (auto& m : P.m) r.push_back({ m.uid, m.origPos, m.origRot, m.origScale }); core::MoveMany(r, true);
            if (g_undo.size() > P.historyMark) g_undo.resize(P.historyMark);
            g_redo.clear();
        }
        if (notify) Note(T("placement cancelled")); FinishPlace();
    }
    // pivot of a member relative to the placement center for the given deltas. A single object with a measured box turns about
    // that box center for yaw, pitch and roll alike (the gizmo sits in the middle); sets keep their layout and turn about the +y axis.
    static Vec3 MemberRel(const Place& P, const Member& m, const Rot& fr, float yawDelta, float scaleMul) {
        if (P.m.size() == 1 && P.haveCenter && P.prefabIdx >= 0) {
            const auto& info = core::PrefabIndex()[P.prefabIdx]; const float sc = m.scale0 * scaleMul;
            const Vec3 off = RotLocal(fr, info.cx * sc, info.cy * sc, info.cz * sc); return { -off.x, -off.y, -off.z };
        }
        const float t = yawDelta * 3.14159265f / 180.0f, cs = cosf(t), sn = sinf(t);
        return { (cs * m.rel.x + sn * m.rel.z) * scaleMul, m.rel.y * scaleMul, (-sn * m.rel.x + cs * m.rel.z) * scaleMul };
    }
    static void MembersTo(std::vector<core::MoveReq>& out, const Place& P, Vec3 center, float yawDelta, float scaleMul) {
        for (auto& m : P.m) {
            const Rot fr{ WrapYaw(m.rot0.yaw + yawDelta), WrapYaw(m.rot0.pitch + P.pitch), WrapYaw(m.rot0.roll + P.roll) };
            const Vec3 rel = MemberRel(P, m, fr, yawDelta, scaleMul);
            out.push_back({ m.uid, { center.x + rel.x, center.y + rel.y, center.z + rel.z }, fr, m.scale0 * scaleMul });
        }
    }
    static void CommitPlaceHistory(Place& P) {
        if (P.historyBase.size() != P.m.size()) return;
        std::vector<core::MoveReq> now; MembersTo(now, P, P.center, P.yaw, P.scale);
        std::vector<Act> acts; acts.reserve(now.size());
        for (size_t i = 0; i < now.size(); ++i) {
            const auto& before = P.historyBase[i]; const auto& after = now[i];
            Act a; a.kind = Act::Move; a.uid = P.m[i].uid; a.prefab = P.m[i].prefab;
            a.pos0 = before.pos; a.rot0 = before.rot; a.sc0 = before.scale;
            a.pos1 = after.pos; a.rot1 = after.rot; a.sc1 = after.scale;
            acts.push_back(std::move(a));
        }
        core::MoveMany(now, true);
        Push(std::move(acts));
        P.historyBase = std::move(now);
        P.lastCenter = P.center; P.lastYaw = P.yaw; P.lastScale = P.scale; P.lastPitch = P.pitch; P.lastRoll = P.roll; P.dirty = false;
    }
    // vertical extent of a member's (rotated, scaled) box relative to the placement center; objects without a box count as pivot .. pivot + 2 m
    static void MemberExtentY(const Place& P, const Member& m, float* lo, float* hi) {
        const Rot fr{ WrapYaw(m.rot0.yaw + P.yaw), WrapYaw(m.rot0.pitch + P.pitch), WrapYaw(m.rot0.roll + P.roll) };
        const Vec3 rel = MemberRel(P, m, fr, P.yaw, P.scale); const int pi = IndexOfPrefab(m.prefab);
        if (pi < 0 || !core::PrefabIndex()[pi].hasCenter) { *lo = rel.y; *hi = rel.y + 2.0f; return; }
        const auto& info = core::PrefabIndex()[pi]; const float sc = m.scale0 * P.scale; *lo = 1e30f; *hi = -1e30f;
        for (int k = 0; k < 8; k++) {
            const Vec3 v = RotLocal(fr, (info.cx + ((k & 1) ? info.sx : -info.sx) * 0.5f) * sc, (info.cy + ((k & 2) ? info.sy : -info.sy) * 0.5f) * sc, (info.cz + ((k & 4) ? info.sz : -info.sz) * 0.5f) * sc);
            *lo = std::min(*lo, rel.y + v.y); *hi = std::max(*hi, rel.y + v.y);
        }
    }
    // lowest point of the carried set (bounding boxes when known), relative to the placement center
    static float SetBottomOffset(const Place& P) {
        float bottom = 1e30f;
        for (const auto& m : P.m) { float lo, hi; MemberExtentY(P, m, &lo, &hi); bottom = std::min(bottom, lo); }
        return bottom < 1e29f ? bottom : 0.0f;
    }
    static float SetTopOffset(const Place& P) {
        float top = -1e30f;
        for (const auto& m : P.m) { float lo, hi; MemberExtentY(P, m, &lo, &hi); top = std::max(top, hi); }
        return top > -1e29f ? top : 2.0f;
    }
    // One cast from startY straight down. Returns 1 = ground found (groundY set), 0 = cast again from the updated startY,
    // -1 = give up. The object's own collision is in the way: hits inside its vertical range are stepped through (a cast that
    // starts inside a body reports fraction 0, so the start is lowered in 0.5 m steps until it leaves the body).
    static int GroundStep(const core::GroundHit& gh, float x, float z, float bottom, float top, float& startY, int& iter, float* groundY) {
        const float cy = gh.centerY - core::g_probeRadius;
        core::Log("[ground] cast from y %.2f: %s fraction %.4f center %.2f (object %.2f..%.2f)", startY, gh.hit ? "hit" : "no hit", gh.fraction, gh.centerY, bottom, top);
        if (++iter > 40) return -1;
        if (!gh.hit) return -1;
        const bool inside = gh.fraction <= 0.0005f;   // the sphere started inside a body (Havok reports a negative fraction)
        if (iter == 1 && inside) { startY = top + 150.0f; return 0; }   // buried: something solid sits above the top, so the surface is found from far above
        if (startY > bottom - 0.1f) {                                    // still beside the object: hits here are the object itself (a 36 m tower needs one jump, not 0.5 m steps)
            if (inside || (cy > bottom - 0.03f && cy < top + 0.03f)) { startY = bottom - 0.1f; return 0; }
            *groundY = cy; return 1;                                     // a surface above the object (far-above cast) or already below its bottom
        }
        if (inside) { startY -= 0.5f; return 0; }                        // collision reaches below the bounding box: step out of it
        *groundY = cy; return 1;
    }
    static void StartGroundSnap(Place& P) {
        if (!core::GroundProbeReady()) { Note(T("snap to ground: not ready yet (walk a step first)")); return; }
        P.groundBottom = P.center.y + SetBottomOffset(P); P.groundTop = P.center.y + SetTopOffset(P);
        P.groundStartY = P.groundTop + 0.5f; P.groundIter = 0;
        P.groundTicket = core::GroundProbe({ P.center.x, P.groundStartY, P.center.z }, 400.0f);
    }
    // drops the carried set at its current transform: fresh objects (collision + render state match the final transform), one undo step
    static void DropCarried() {
        Place& P = g_place; if (!P.active) return;
        CommitPlaceHistory(P);
        Note(T(P.isNew ? "placed %s" : "dropped %s"), P.name.c_str()); FinishPlace();
    }
    // Returns true when the placement ended. This is deliberately dormant unless the user enables keyboard placement in Settings.
    static bool KeyboardPlaceTick(Place& P) {
        if (!core::g_keyboardPlacement) return false;
        using namespace core;
        const float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
        auto held = [](int pk) { return input::VkDown(g_placeKeys[pk]); };
        static bool prev[PK_COUNT] = { false };
        auto pressed = [&](int pk) { const bool now = held(pk); const bool fire = now && !prev[pk]; prev[pk] = now; return fire; };
        const bool confirm = pressed(PK_DROP), cancel = pressed(PK_CANCEL), fetch = pressed(PK_FETCH);
        if (pressed(PK_SNAP)) g_snap = !g_snap;
        if (pressed(PK_MOUSE)) {
            if (P.drag) CommitPlaceHistory(P);
            if (g_open) { g_playMode = !g_playMode; ImGui::GetIO().ClearInputKeys(); }
            else P.mouse = !P.mouse;
            P.drag = P.hover = 0;
        }
        if (confirm) { DropCarried(); return true; }
        if (cancel) { CancelCarried(); return true; }
        if (pressed(PK_GROUND)) StartGroundSnap(P);
        if (pressed(PK_LEVEL)) {
            if (P.m.size() == 1) { P.pitch = -P.m[0].rot0.pitch; P.roll = -P.m[0].rot0.roll; }
            else P.pitch = P.roll = 0;
            P.dirty = P.touched = true; Note(T("levelled"));
            CommitPlaceHistory(P);
        }

        const float speed = held(PK_FAST) ? 4.0f : 1.5f;
        float ax = P.center.x - g_lastPlayer.x, az = P.center.z - g_lastPlayer.z, al = sqrtf(ax * ax + az * az);
        if (al > 0.3f) { ax /= al; az /= al; } else { ax = g_fx; az = g_fz; }
        const float rx = az, rz = -ax;
        float fwd = 0, side = 0, up = 0, rot = 0;
        if (g_snap) {
            static DWORD rep[8] = { 0 }; static bool was[8] = { false };
            auto step = [&](int i, int pk) { const bool h = held(pk); bool fire = false; const DWORD now = GetTickCount();
                if (h && !was[i]) { fire = true; rep[i] = now + 350; } else if (h && now >= rep[i]) { fire = true; rep[i] = now + 120; }
                was[i] = h; return fire ? 1.0f : 0.0f; };
            fwd += step(0, PK_FWD); fwd -= step(1, PK_BACK); side += step(2, PK_RIGHT); side -= step(3, PK_LEFT);
            up += step(4, PK_UP); up -= step(5, PK_DOWN); rot -= step(6, PK_ROT_L); rot += step(7, PK_ROT_R);
            const float ps = kSnapPos[g_snapPosIdx], ys = kSnapYaw[g_snapYawIdx];
            if (fwd || side) { const float sx = P.snapAz, sz = -P.snapAx; P.center.x += (P.snapAx * fwd + sx * side) * ps; P.center.z += (P.snapAz * fwd + sz * side) * ps; }
            if (up) P.center.y += up * ps;
            if (rot) P.yaw += rot * ys;
            P.center = { SnapV(P.center.x, ps), SnapV(P.center.y, ps), SnapV(P.center.z, ps) }; P.yaw = WrapYaw(SnapV(P.yaw, ys));
        } else {
            if (held(PK_FWD)) fwd += 1; if (held(PK_BACK)) fwd -= 1;
            if (held(PK_RIGHT)) side += 1; if (held(PK_LEFT)) side -= 1;
            if (held(PK_UP)) up += 1; if (held(PK_DOWN)) up -= 1;
            if (held(PK_ROT_L)) P.yaw -= 60.0f * dt; if (held(PK_ROT_R)) P.yaw += 60.0f * dt;
            P.yaw = WrapYaw(P.yaw);
            if (fwd || side || up) { P.center.x += (ax * fwd + rx * side) * speed * dt; P.center.z += (az * fwd + rz * side) * speed * dt; P.center.y += up * speed * dt; }
        }
        if (held(PK_SCALE_UP)) P.scale = std::min(20.0f, P.scale * (1.0f + dt));
        if (held(PK_SCALE_DOWN)) P.scale = std::max(0.05f, P.scale / (1.0f + dt));
        if (fetch && g_havePlayer) { P.center = InFront(P.radius, P.center.y - g_lastPlayer.y); CommitPlaceHistory(P); }
        const bool transformHeld = held(PK_FWD) || held(PK_BACK) || held(PK_LEFT) || held(PK_RIGHT) || held(PK_UP) || held(PK_DOWN) ||
            held(PK_ROT_L) || held(PK_ROT_R) || held(PK_SCALE_UP) || held(PK_SCALE_DOWN);
        if (P.keyTransformHeld && !transformHeld) CommitPlaceHistory(P);
        P.keyTransformHeld = transformHeld;
        return false;
    }
    static void PlaceTick() {
        if (!g_place.active) return;
        Place& P = g_place;
        if (P.m.size() == 1 && !P.haveCenter && P.prefabIdx >= 0 && core::PrefabIndex()[P.prefabIdx].hasCenter) {   // center measured meanwhile: keep the pivot, move the rotation center
            const auto& info = core::PrefabIndex()[P.prefabIdx]; Member& m = P.m[0];
            Vec3 pivot = { P.center.x + m.rel.x, P.center.y + m.rel.y, P.center.z + m.rel.z };   // current pivot (yaw/scale deltas are still 0 at this point in practice)
            const Vec3 off = RotLocal(m.rot0, info.cx * m.scale0, info.cy * m.scale0, info.cz * m.scale0);
            Vec3 bc = { pivot.x + off.x, pivot.y + off.y, pivot.z + off.z };
            m.rel = { pivot.x - bc.x, pivot.y - bc.y, pivot.z - bc.z }; P.center = bc; P.lastCenter = bc; P.haveCenter = true;
        }
        if (KeyboardPlaceTick(P)) return;
        const bool gizmoMouse = core::g_keyboardPlacement ? (g_open ? !g_playMode : P.mouse) : (g_open ? !g_playMode : true);
        if (gizmoMouse) {   // gizmo dragging with the virtual cursor (the game does not see the mouse while this is on)
            ImGuiIO& io = ImGui::GetIO(); CamFrame cf = CurrentCam();
            const float gsize = GizmoScreenSize(cf, P.center, P.radius);
            const float objYaw = P.m.size() == 1 ? WrapYaw(P.m[0].rot0.yaw + P.yaw) : P.yaw, objPitch = P.m.size() == 1 ? WrapYaw(P.m[0].rot0.pitch + P.pitch) : P.pitch;
            GizmoGeo g = GizmoAt(cf, P.center, objYaw, objPitch, gsize);
            if (cf.ok && g.ok) {
                const Vec3 rd = MouseRay(cf, io.MousePos);
                auto ringOf = [&](int id) -> const Ring& { return id == 7 ? g.ring[1] : id == 8 ? g.ring[2] : g.ring[0]; };
                if (!P.drag) {
                    P.hover = GizmoHover(cf, g, io.MousePos);
                    if (P.hover && ImGui::IsMouseClicked(0) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
                        P.drag = P.hover; P.dragCenter0 = P.center; P.dragYaw0 = P.yaw; P.dragPitch0 = P.pitch; P.dragRoll0 = P.roll; P.dragScale0 = P.scale;
                        if (P.drag <= 3) { const Vec3& a = P.drag == 1 ? g.ax : P.drag == 2 ? g.ay : g.az; P.drag0 = LineRayParam(P.center, a, cf.pos, rd); }
                        else if (P.drag == 4 || P.drag == 7 || P.drag == 8) { if (!RingAngle(cf, rd, P.center, ringOf(P.drag), &P.drag0)) P.drag = 0; }
                        else if (P.drag == 6) P.drag0 = std::max(8.0f, sqrtf((io.MousePos.x - g.sc.x) * (io.MousePos.x - g.sc.x) + (io.MousePos.y - g.sc.y) * (io.MousePos.y - g.sc.y)));
                        else { Vec3 h; if (RayPlaneY(cf.pos, rd, P.center.y, &h)) P.dragCenter0 = { h.x - P.center.x, 0, h.z - P.center.z }; else P.drag = 0; }
                    }
                } else if (!ImGui::IsMouseDown(0)) { CommitPlaceHistory(P); P.drag = 0; }
                else {
                    if (P.drag <= 3) { const Vec3& a = P.drag == 1 ? g.ax : P.drag == 2 ? g.ay : g.az; float t = LineRayParam(P.dragCenter0, a, cf.pos, rd) - P.drag0; if (fabsf(t) < 200) P.center = { P.dragCenter0.x + a.x * t, P.dragCenter0.y + a.y * t, P.dragCenter0.z + a.z * t }; }
                    else if (P.drag == 4 || P.drag == 7 || P.drag == 8) {
                        // the ring axes follow the current yaw/pitch, so recompute the frame the drag started in for a stable angle reference
                        GizmoGeo g0 = GizmoAt(cf, P.center, P.m.size() == 1 ? WrapYaw(P.m[0].rot0.yaw + P.dragYaw0) : P.dragYaw0, P.m.size() == 1 ? WrapYaw(P.m[0].rot0.pitch + P.dragPitch0) : P.dragPitch0, gsize);
                        const Ring& r0 = P.drag == 7 ? g0.ring[1] : P.drag == 8 ? g0.ring[2] : g0.ring[0];
                        float ang; if (RingAngle(cf, rd, P.center, r0, &ang)) { const float d = (ang - P.drag0) * 180.0f / 3.14159265f;
                            if (P.drag == 4) P.yaw = WrapYaw(P.dragYaw0 + d); else if (P.drag == 7) P.pitch = WrapYaw(P.dragPitch0 + d); else P.roll = WrapYaw(P.dragRoll0 + d); }
                    }
                    else if (P.drag == 6) { float d = sqrtf((io.MousePos.x - g.sc.x) * (io.MousePos.x - g.sc.x) + (io.MousePos.y - g.sc.y) * (io.MousePos.y - g.sc.y)); P.scale = std::max(0.05f, std::min(20.0f, P.dragScale0 * d / P.drag0)); }
                    else { Vec3 h; if (RayPlaneY(cf.pos, rd, P.center.y, &h)) P.center = { h.x - P.dragCenter0.x, P.center.y, h.z - P.dragCenter0.z }; }
                    if (g_snap) { const float ps = kSnapPos[g_snapPosIdx], ys = kSnapYaw[g_snapYawIdx]; P.center = { SnapV(P.center.x, ps), SnapV(P.center.y, ps), SnapV(P.center.z, ps) }; P.yaw = WrapYaw(SnapV(P.yaw, ys)); P.pitch = WrapYaw(SnapV(P.pitch, ys)); P.roll = WrapYaw(SnapV(P.roll, ys)); }
                }
            }
        } else { if (P.drag) CommitPlaceHistory(P); P.hover = 0; P.drag = 0; }
        if (P.groundTicket) {
            core::GroundHit gh;
            if (core::GroundResult(P.groundTicket, &gh)) {
                P.groundTicket = 0; float groundY = 0;
                const int r = GroundStep(gh, P.center.x, P.center.z, P.groundBottom, P.groundTop, P.groundStartY, P.groundIter, &groundY);
                if (r == 1) { P.center.y += groundY - P.groundBottom; Note(T("snapped to the ground (%+.2f m)"), groundY - P.groundBottom); CommitPlaceHistory(P); }
                else if (r == 0) P.groundTicket = core::GroundProbe({ P.center.x, P.groundStartY, P.center.z }, 400.0f);
                else Note(T("snap to ground: no surface found below"));
            }
        }
        Vec3 c = P.center; float yaw = P.yaw, sc = P.scale;
        const DWORD now = GetTickCount();
        const bool changed = fabsf(c.x - P.lastCenter.x) > 0.005f || fabsf(c.y - P.lastCenter.y) > 0.005f || fabsf(c.z - P.lastCenter.z) > 0.005f
            || fabsf(yaw - P.lastYaw) > 0.01f || fabsf(sc - P.lastScale) > 0.001f || fabsf(P.pitch - P.lastPitch) > 0.01f || fabsf(P.roll - P.lastRoll) > 0.01f;
        if (changed) { P.dirty = true; P.touched = true; }
        if (!P.dirty || now - P.lastSend < 60) return;
        std::vector<core::MoveReq> r; MembersTo(r, P, c, yaw, sc);
        if (core::MoveMany(r, false)) { P.dirty = false; P.lastSend = now; P.lastCenter = c; P.lastYaw = yaw; P.lastScale = sc; P.lastPitch = P.pitch; P.lastRoll = P.roll; }   // dropped: try again next frame
    }
    // ---- selection helpers, undo, copy/paste ----
    static std::vector<int> SelUids() { return std::vector<int>(g_sel.begin(), g_sel.end()); }
    static bool CtrlHeld(const ImGuiIO& io) { return io.KeyCtrl || (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0; }
    static bool ShiftHeld(const ImGuiIO& io) { return io.KeyShift || (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0; }
    static void ClearSceneSelection() {
        g_sel.clear(); g_primary = g_lastClicked = 0;
        g_managedNpcSel.clear(); g_managedNpcPrimary = g_managedNpcLast = 0;
        g_sceneLastEntity = 0; g_editUid = 0;
    }
    static size_t SceneSelectionCount() { return g_sel.size() + g_managedNpcSel.size(); }
    static bool SceneHasSelection() { return !g_sel.empty() || !g_managedNpcSel.empty(); }
    static void SelectSingleUid(int uid) {
        if (g_place.active) DropCarried();
        ClearSceneSelection(); g_sel.insert(uid); g_primary = g_lastClicked = uid; g_sceneLastEntity = uid; g_editUid = 0;
    }
    static bool FocusSelection() {
        if (!SceneHasSelection() || !core::FreeCamAvailable()) return false;
        const auto list = core::Spawned(); const auto npcs = core::ManagedNpcs(); Vec3 c{}; int n = 0;
        for (int uid : g_sel) { const SpawnedObj* o = Find(list, uid); if (!o || o->hidden) continue; const Vec3 bc = BboxCenter(*o); c.x += bc.x; c.y += bc.y; c.z += bc.z; n++; }
        for (int uid : g_managedNpcSel) { const ManagedNpc* m = FindManagedNpc(npcs, uid); if (!m || m->hidden) continue; c.x += m->pos.x; c.y += m->pos.y + 0.9f; c.z += m->pos.z; n++; }
        if (!n) return false; c.x /= n; c.y /= n; c.z /= n;
        float radius = 1.0f;
        for (int uid : g_sel) { const SpawnedObj* o = Find(list, uid); if (!o || o->hidden) continue; const Vec3 bc = BboxCenter(*o); const float dx = bc.x - c.x, dy = bc.y - c.y, dz = bc.z - c.z; radius = std::max(radius, sqrtf(dx * dx + dy * dy + dz * dz) + ObjectRadius(*o)); }
        for (int uid : g_managedNpcSel) { const ManagedNpc* m = FindManagedNpc(npcs, uid); if (!m || m->hidden) continue; const float dx = m->pos.x - c.x, dy = (m->pos.y + 0.9f) - c.y, dz = m->pos.z - c.z; radius = std::max(radius, sqrtf(dx * dx + dy * dy + dz * dz) + 1.2f); }
        if (!g_cameraMode) ToggleCameraMode();
        if (!g_cameraMode) return false;
        g_cameraViewMode = 0;   // the focus sets its own direction
        core::FreeCamFocus(c, radius); return true;
    }
    struct SelectionUnit { int key = 0; std::vector<const SpawnedObj*> objects; Vec3 center{}; };
    static std::vector<SelectionUnit> SelectionUnits(const std::set<int>& selection, const std::vector<SpawnedObj>& all) {
        std::set<int> candidates;
        for (int uid : selection) { const SpawnedObj* o = Find(all, uid); if (o && !o->hidden && o->group > 0) candidates.insert(o->group); }
        std::set<int> wholeGroups;
        for (int gid : candidates) {
            bool whole = true;
            for (const auto& o : all) if (!o.hidden && o.group == gid && !selection.count(o.uid)) { whole = false; break; }
            if (whole) wholeGroups.insert(gid);
        }
        std::map<int, SelectionUnit> grouped;
        std::vector<SelectionUnit> units;
        for (int uid : selection) {
            const SpawnedObj* o = Find(all, uid); if (!o || o->hidden) continue;
            if (o->group > 0 && wholeGroups.count(o->group)) { auto& u = grouped[o->group]; u.key = o->group; u.objects.push_back(o); }
            else { SelectionUnit u; u.key = -o->uid; u.objects.push_back(o); u.center = o->pos; units.push_back(std::move(u)); }
        }
        for (auto& pair : grouped) {
            SelectionUnit& u = pair.second;
            for (const SpawnedObj* o : u.objects) { u.center.x += o->pos.x; u.center.y += o->pos.y; u.center.z += o->pos.z; }
            const float n = static_cast<float>(u.objects.size());
            if (n > 0) { u.center.x /= n; u.center.y /= n; u.center.z /= n; }
            units.push_back(std::move(u));
        }
        return units;
    }
    static void SelectUid(int uid, bool add, const std::vector<SpawnedObj>& list) {
        if (g_place.active) DropCarried();   // selecting something else ends the placement
        if (!add) ClearSceneSelection();
        auto addOne = [&](int u) { if (g_sel.count(u)) { if (add) g_sel.erase(u); } else g_sel.insert(u); };
        const SpawnedObj* o = Find(list, uid);
        if (g_selectGroups && o && o->group > 0 && !add) {
            for (auto& x : list) if (x.group == o->group && !x.hidden) g_sel.insert(x.uid);
            for (const auto& n : core::ManagedNpcs()) if (n.group == o->group && !n.hidden) g_managedNpcSel.insert(n.uid);
        }
        else addOne(uid);
        g_primary = uid; g_lastClicked = uid; g_sceneLastEntity = uid;
    }
    // snap to ground for placed objects: one probe per object, results applied as they arrive (undoable move)
    struct SnapJob { int uid, ticket, iter; float bottom, top, startY; };
    static std::vector<SnapJob> g_snapJobs;
    static void SnapSelToGround() {
        if (!core::GroundProbeReady()) { Note(T("snap to ground: not ready yet (walk a step first)")); return; }
        auto list = core::Spawned(); int n = 0;
        for (int uid : g_sel) {
            const SpawnedObj* o = Find(list, uid); if (!o || o->hidden) continue;
            float bottom = o->pos.y, top = o->pos.y + 2.0f; int pi = IndexOfPrefab(o->prefab);
            if (pi >= 0) { const auto& info = core::PrefabIndex()[pi]; if (info.hasCenter) { bottom = o->pos.y + (info.cy - info.sy * 0.5f) * o->scale; top = o->pos.y + (info.cy + info.sy * 0.5f) * o->scale; } }
            const float startY = top + 0.5f;
            int t = core::GroundProbe({ o->pos.x, startY, o->pos.z }, 400.0f); if (t) { g_snapJobs.push_back({ uid, t, 0, bottom, top, startY }); n++; }
        }
        if (n) Note(T("snapping %d objects to the ground"), n);
    }
    static void PumpSnapJobs() {
        if (g_snapJobs.empty()) return;
        auto list = core::Spawned(); std::vector<Act> acts; std::vector<core::MoveReq> moves;
        for (size_t i = 0; i < g_snapJobs.size(); ) {
            SnapJob& j = g_snapJobs[i]; core::GroundHit gh;
            if (!core::GroundResult(j.ticket, &gh)) { i++; continue; }
            const SpawnedObj* o = Find(list, j.uid);
            if (!o) { g_snapJobs.erase(g_snapJobs.begin() + i); continue; }
            float groundY = 0; const int r = GroundStep(gh, o->pos.x, o->pos.z, j.bottom, j.top, j.startY, j.iter, &groundY);
            if (r == 0) { j.ticket = core::GroundProbe({ o->pos.x, j.startY, o->pos.z }, 400.0f); if (j.ticket) { i++; continue; } }
            if (r == 1) {
                Act a; a.kind = Act::Move; a.uid = o->uid; a.prefab = o->prefab; a.pos0 = o->pos; a.rot0 = o->rot; a.sc0 = o->scale;
                a.pos1 = { o->pos.x, o->pos.y + (groundY - j.bottom), o->pos.z }; a.rot1 = o->rot; a.sc1 = o->scale; acts.push_back(a);
                moves.push_back({ o->uid, a.pos1, o->rot, o->scale });
            }
            g_snapJobs.erase(g_snapJobs.begin() + i);
        }
        if (!moves.empty()) { core::MoveMany(moves, true); Push(acts); Note(T("%d objects snapped to the ground"), (int)moves.size()); }
    }
    static void DeleteSel() {
        auto list = core::Spawned(); std::vector<Act> acts;
        for (int uid : g_sel) { const SpawnedObj* o = Find(list, uid); if (!o || o->hidden) continue; Act a; a.kind = Act::Delete; a.uid = uid; a.prefab = o->prefab; a.pos0 = o->pos; a.rot0 = o->rot; a.sc0 = o->scale; a.group = o->group; a.proj = o->proj; a.text0 = o->note; acts.push_back(a); core::HideUid(uid); }
        if (!acts.empty()) { Note(T("deleted %d objects"), (int)acts.size()); Push(acts); }
        g_sel.clear(); g_primary = 0;
    }
    // objects that sit exactly on an earlier identical one (same prefab, position, rotation and scale), as after a project
    // loaded twice: selects the later copies and returns how many
    static int SelectDuplicates() {
        auto list = core::Spawned(); std::vector<int> ord; for (int i = 0; i < (int)list.size(); i++) if (!list[i].hidden) ord.push_back(i);
        std::sort(ord.begin(), ord.end(), [&](int a, int b) { return list[a].uid < list[b].uid; });
        g_sel.clear(); g_primary = 0;
        for (size_t a = 0; a < ord.size(); a++) {
            const SpawnedObj& o = list[ord[a]]; if (g_sel.count(o.uid)) continue;
            for (size_t b = a + 1; b < ord.size(); b++) {
                const SpawnedObj& q = list[ord[b]]; if (q.prefab != o.prefab) continue;
                if (fabsf(q.pos.x - o.pos.x) > 0.01f || fabsf(q.pos.y - o.pos.y) > 0.01f || fabsf(q.pos.z - o.pos.z) > 0.01f) continue;
                if (fabsf(WrapYaw(q.rot.yaw - o.rot.yaw)) > 0.1f || fabsf(q.rot.pitch - o.rot.pitch) > 0.1f || fabsf(q.rot.roll - o.rot.roll) > 0.1f || fabsf(q.scale - o.scale) > 0.001f) continue;
                g_sel.insert(q.uid);
            }
        }
        return (int)g_sel.size();
    }
    static void RemapUid(int from, int to) {
        if (!from || !to || from == to) return;
        for (auto* stack : { &g_undo, &g_redo }) for (auto& entry : *stack) for (auto& a : entry.acts) if (a.uid == from) a.uid = to;
        if (g_sel.erase(from)) g_sel.insert(to);
        if (g_primary == from) g_primary = to;
        if (g_lastClicked == from) g_lastClicked = to;
    }
    static void RemoveSelectionUid(int uid) {
        g_sel.erase(uid);
        if (g_primary == uid) g_primary = g_sel.empty() ? 0 : *g_sel.begin();
        if (g_lastClicked == uid) g_lastClicked = 0;
    }
    static void Undo() {
        if (g_undo.empty()) return;
        HistoryEntry entry = std::move(g_undo.back()); g_undo.pop_back();
        std::vector<Act>& acts = entry.acts;
        std::vector<core::MoveReq> moves;
        for (auto& a : acts) {
            if (a.kind == Act::Spawn) { core::HideUid(a.uid); RemoveSelectionUid(a.uid); }
            else if (a.kind == Act::Delete) { const int oldUid = a.uid, nu = core::SpawnAt(a.prefab, a.pos0, a.rot0, a.sc0, a.group, a.proj); if (nu) { a.uid = nu; RemapUid(oldUid, nu); if (!a.text0.empty()) core::SetObjectNote(nu, a.text0); g_sel.insert(nu); g_primary = nu; } }
            else if (a.kind == Act::SetGroup) core::SetGroup(a.uid, a.group);
            else if (a.kind == Act::Move) moves.push_back({ a.uid, a.pos0, a.rot0, a.sc0 });
            else if (a.kind == Act::NpcSpawn) {
                core::HideManagedNpc(a.uid); g_managedNpcSel.erase(a.uid);
                if (g_managedNpcPrimary == a.uid) g_managedNpcPrimary = g_managedNpcSel.empty() ? 0 : *g_managedNpcSel.begin();
            }
            else if (a.kind == Act::NpcDelete) {
                if (core::RestoreManagedNpc(a.uid)) { g_managedNpcSel.insert(a.uid); g_managedNpcPrimary = a.uid; g_managedNpcLast = a.uid; }
            }
            else if (a.kind == Act::NpcMove) core::MoveManagedNpc(a.uid, a.pos0);
            else if (a.kind == Act::NpcControl) core::SetManagedNpcControl(a.uid, a.flag0, a.behavior0);
            else if (a.kind == Act::NpcGroup) core::SetManagedNpcGroup(a.uid, a.group);
            else if (a.kind == Act::ObjectNote) core::SetObjectNote(a.uid, a.text0);
            else if (a.kind == Act::NpcNote) core::SetManagedNpcNote(a.uid, a.text0);
            else if (a.kind == Act::NpcLabel) core::SetManagedNpcLabel(a.uid, a.text0);
            else if (a.kind == Act::GroupName) core::SetGroupName(a.group, a.text0);
        }
        if (!moves.empty()) core::MoveMany(moves, true);
        g_redo.push_back(std::move(entry)); Note(T("undo"));
    }
    static void Redo() {
        if (g_redo.empty()) return;
        HistoryEntry entry = std::move(g_redo.back()); g_redo.pop_back();
        std::vector<Act>& acts = entry.acts;
        std::vector<core::MoveReq> moves;
        for (auto& a : acts) {
            if (a.kind == Act::Spawn) { const int oldUid = a.uid, nu = core::SpawnAt(a.prefab, a.pos1, a.rot1, a.sc1, a.group, a.proj); if (nu) { a.uid = nu; RemapUid(oldUid, nu); g_sel.insert(nu); g_primary = nu; } }
            else if (a.kind == Act::Delete) { core::HideUid(a.uid); RemoveSelectionUid(a.uid); }
            else if (a.kind == Act::SetGroup) core::SetGroup(a.uid, a.group1);
            else if (a.kind == Act::Move) moves.push_back({ a.uid, a.pos1, a.rot1, a.sc1 });
            else if (a.kind == Act::NpcSpawn) {
                if (core::RestoreManagedNpc(a.uid)) { g_managedNpcSel.insert(a.uid); g_managedNpcPrimary = a.uid; g_managedNpcLast = a.uid; }
            }
            else if (a.kind == Act::NpcDelete) {
                core::HideManagedNpc(a.uid); g_managedNpcSel.erase(a.uid);
                if (g_managedNpcPrimary == a.uid) g_managedNpcPrimary = g_managedNpcSel.empty() ? 0 : *g_managedNpcSel.begin();
            }
            else if (a.kind == Act::NpcMove) core::MoveManagedNpc(a.uid, a.pos1);
            else if (a.kind == Act::NpcControl) core::SetManagedNpcControl(a.uid, a.flag1, a.behavior1);
            else if (a.kind == Act::NpcGroup) core::SetManagedNpcGroup(a.uid, a.group1);
            else if (a.kind == Act::ObjectNote) core::SetObjectNote(a.uid, a.text1);
            else if (a.kind == Act::NpcNote) core::SetManagedNpcNote(a.uid, a.text1);
            else if (a.kind == Act::NpcLabel) core::SetManagedNpcLabel(a.uid, a.text1);
            else if (a.kind == Act::GroupName) core::SetGroupName(a.group, a.text1);
        }
        if (!moves.empty()) core::MoveMany(moves, true);
        g_undo.push_back(std::move(entry)); Note(T("redo"));
    }
    static const char* HistoryActionName(const HistoryEntry& entry) {
        if (entry.acts.empty()) return "";
        const Act::Kind k = entry.acts[0].kind;
        for (const auto& a : entry.acts) if (a.kind != k) return T("Edit");
        if (k == Act::Spawn) return T("SPAWN");
        if (k == Act::Delete) return T("Delete");
        if (k == Act::SetGroup) return T("Group");
        if (k == Act::NpcSpawn) return T("NPC spawn");
        if (k == Act::NpcMove) return T("NPC move");
        if (k == Act::NpcDelete) return T("NPC delete");
        if (k == Act::NpcControl) return T("NPC AI and behavior");
        if (k == Act::NpcGroup) return T("NPC group");
        if (k == Act::ObjectNote || k == Act::NpcNote) return T("Note");
        if (k == Act::NpcLabel) return T("Rename NPC");
        if (k == Act::GroupName) return T("Rename group");
        bool pos = false, rot = false, scale = false;
        for (const auto& a : entry.acts) {
            pos |= VecChanged(a.pos0, a.pos1);
            rot |= RotChanged(a.rot0, a.rot1);
            scale |= a.sc1 != a.sc0;
        }
        if (pos && !rot && !scale) return T("Move");
        if (scale && !pos && !rot) return T("scale");
        if (rot && !pos && !scale) return T("Rotate");
        return T("Grab and move");
    }
    static void DrawHistoryAct(const Act& a) {
        const std::string name = a.prefab.empty() ? std::string() : ShortName(a.prefab);
        if (name.empty()) ImGui::Text("#%d", a.uid); else ImGui::Text("#%d  %s", a.uid, name.c_str());
        ImGui::Indent();
        auto drawPos = [&](Vec3 p0, Vec3 p1, bool arrow) {
            if (arrow) {
                ImGui::Text("%s: %.6f, %.6f, %.6f  ->  %.6f, %.6f, %.6f", T("position"), p0.x, p0.y, p0.z, p1.x, p1.y, p1.z);
                ImGui::TextDisabled("d: %+.6f, %+.6f, %+.6f", p1.x - p0.x, p1.y - p0.y, p1.z - p0.z);
            } else ImGui::Text("%s: %.6f, %.6f, %.6f", T("position"), p1.x, p1.y, p1.z);
        };
        auto drawRot = [&](Rot r0, Rot r1, bool arrow) {
            if (arrow) {
                ImGui::Text("%s: %.5f, %.5f, %.5f  ->  %.5f, %.5f, %.5f", T("Rotate"), r0.yaw, r0.pitch, r0.roll, r1.yaw, r1.pitch, r1.roll);
                ImGui::TextDisabled("d: %+.5f, %+.5f, %+.5f", r1.yaw - r0.yaw, r1.pitch - r0.pitch, r1.roll - r0.roll);
            } else ImGui::Text("%s: %.5f, %.5f, %.5f", T("Rotate"), r1.yaw, r1.pitch, r1.roll);
        };
        if (a.kind == Act::Move) {
            if (VecChanged(a.pos0, a.pos1)) drawPos(a.pos0, a.pos1, true);
            if (RotChanged(a.rot0, a.rot1)) drawRot(a.rot0, a.rot1, true);
            if (a.sc1 != a.sc0) { ImGui::Text("%s: %.6f  ->  %.6f", T("scale"), a.sc0, a.sc1); ImGui::TextDisabled("d: %+.6f", a.sc1 - a.sc0); }
        } else if (a.kind == Act::SetGroup) {
            ImGui::Text("%s: %d  ->  %d", T("Group"), a.group, a.group1);
        } else if (a.kind == Act::NpcMove) {
            ImGui::TextDisabled("%s", T("managed NPC"));
            drawPos(a.pos0, a.pos1, true);
        } else if (a.kind == Act::NpcControl) {
            ImGui::TextDisabled("%s", T("managed NPC"));
            ImGui::Text("%s: %s, %s  ->  %s, %s", T("AI"),
                a.flag0 ? T("On") : T("Off"), a.behavior0 == 1 ? T("Hold") : T("Normal"),
                a.flag1 ? T("On") : T("Off"), a.behavior1 == 1 ? T("Hold") : T("Normal"));
        } else if (a.kind == Act::NpcGroup) {
            ImGui::TextDisabled("%s", T("managed NPC"));
            ImGui::Text("%s: %d  ->  %d", T("Group"), a.group, a.group1);
        } else if (a.kind == Act::ObjectNote || a.kind == Act::NpcNote || a.kind == Act::NpcLabel || a.kind == Act::GroupName) {
            ImGui::Text("%s: \"%s\"  ->  \"%s\"", a.kind == Act::NpcLabel ? T("NPC") : a.kind == Act::GroupName ? T("Group") : T("Note"), a.text0.c_str(), a.text1.c_str());
        } else if (a.kind == Act::NpcSpawn || a.kind == Act::NpcDelete) {
            ImGui::TextDisabled("%s", T("managed NPC"));
            const bool spawn = a.kind == Act::NpcSpawn;
            const Vec3 p = spawn ? a.pos1 : a.pos0; const bool ai = spawn ? a.flag1 : a.flag0; const int behavior = spawn ? a.behavior1 : a.behavior0;
            drawPos({}, p, false);
            ImGui::Text("%s: %s, %s", T("AI"), ai ? T("On") : T("Off"), behavior == 1 ? T("Hold") : T("Normal"));
            if (a.group) ImGui::Text("%s: %d", T("Group"), a.group);
        } else {
            const bool spawn = a.kind == Act::Spawn; const Vec3 p = spawn ? a.pos1 : a.pos0; const Rot r = spawn ? a.rot1 : a.rot0; const float sc = spawn ? a.sc1 : a.sc0;
            drawPos({}, p, false); drawRot({}, r, false); ImGui::Text("%s: %.6f", T("scale"), sc);
            if (a.group) ImGui::Text("%s: %d", T("Group"), a.group);
        }
        ImGui::Unindent();
    }
    static void DrawHistory() {
        ImGui::BeginDisabled(g_undo.empty()); if (ImGui::Button(T("Undo"))) Undo(); ImGui::EndDisabled(); ImGui::SameLine();
        ImGui::BeginDisabled(g_redo.empty()); if (ImGui::Button(T("Redo"))) Redo(); ImGui::EndDisabled(); ImGui::SameLine();
        ImGui::TextDisabled("%s %d of %d    %s %d", T("Undo"), (int)g_undo.size(), (int)kHistoryLimit, T("Redo"), (int)g_redo.size());
        ImGui::Separator();
        struct View { const HistoryEntry* entry; bool redo; };
        std::vector<View> view; view.reserve(g_undo.size() + g_redo.size());
        for (const auto& e : g_undo) view.push_back({ &e, false });
        for (const auto& e : g_redo) view.push_back({ &e, true });
        std::sort(view.begin(), view.end(), [](const View& a, const View& b) { return a.entry->serial > b.entry->serial; });
        ImGui::BeginChild("history_list", ImVec2(0, 0), ImGuiChildFlags_Borders);
        if (view.empty()) ImGui::TextDisabled("-");
        for (const View& v : view) {
            const HistoryEntry& e = *v.entry; ImGui::PushID((int)(e.serial & 0x7fffffff));
            char label[256]; snprintf(label, sizeof label, "#%llu   %s   %s   (%d)", e.serial, T(v.redo ? "Redo" : "Undo"), HistoryActionName(e), (int)e.acts.size());
            if (ImGui::TreeNodeEx("##history", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", label)) {
                for (const auto& a : e.acts) DrawHistoryAct(a);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    struct ClipItem { std::string prefab; Vec3 rel; Rot rot; float scale; };
    static std::vector<ClipItem> g_clip; static float g_clipRadius = 1; static Vec3 g_clipCenter{};
    static void CopySel() {
        auto list = core::Spawned(); g_clip.clear(); Vec3 c{ 0, 0, 0 }; int n = 0;
        for (int uid : g_sel) { const SpawnedObj* o = Find(list, uid); if (!o || o->hidden) continue; c.x += o->pos.x; c.y += o->pos.y; c.z += o->pos.z; n++; }
        if (!n) return; c.x /= n; c.y /= n; c.z /= n; g_clipCenter = c; g_clipRadius = 1;
        for (int uid : g_sel) { const SpawnedObj* o = Find(list, uid); if (!o || o->hidden) continue; g_clip.push_back({ o->prefab, { o->pos.x - c.x, o->pos.y - c.y, o->pos.z - c.z }, o->rot, o->scale });
            float dx = o->pos.x - c.x, dz = o->pos.z - c.z; g_clipRadius = std::max(g_clipRadius, sqrtf(dx * dx + dz * dz) + 1.0f); }
        Note(T("copied %d objects"), (int)g_clip.size());
    }
    // spawns a set of objects around a center, groups them (when more than one) and hands them to the placement mode
    static void SpawnSet(const std::vector<ClipItem>& items, Vec3 center, float radius, const char* what) {
        if (items.empty() || !core::GameThreadReady()) return;
        int group = items.size() > 1 ? core::NewGroupId() : 0; std::vector<int> uids;
        for (auto& it : items) { int uid = core::SpawnAt(it.prefab, { center.x + it.rel.x, center.y + it.rel.y, center.z + it.rel.z }, it.rot, it.scale, group); if (uid) uids.push_back(uid); }
        if (uids.empty()) return;
        std::string name = items.size() == 1 ? ShortName(items[0].prefab) : std::string(T(what)) + " (" + std::to_string(items.size()) + ")";
        StartGrab(uids, true, name);
        g_place.radius = std::max(g_place.radius, radius);
    }
    static void Paste(bool havePos) {
        (void)havePos; if (g_clip.empty()) return;
        // Prefer a copy next to its source. If that whole copied set would be outside the current camera, bring the copy into view
        // instead so duplicating a distant/off-screen object never creates another object the user cannot find.
        const float d = std::max(0.6f, std::min(2.0f, g_clipRadius * 0.35f));
        CamFrame cf = CurrentCam();
        Vec3 at = cf.ok ? Vec3{ g_clipCenter.x + cf.right.x * d, g_clipCenter.y + cf.right.y * d, g_clipCenter.z + cf.right.z * d }
                       : Vec3{ g_clipCenter.x + g_fz * d, g_clipCenter.y, g_clipCenter.z - g_fx * d };
        if (cf.ok) {
            ImVec2 s{}; const Vec3 rel = { at.x - cf.pos.x, at.y - cf.pos.y, at.z - cf.pos.z };
            const float depth = rel.x * cf.fwd.x + rel.y * cf.fwd.y + rel.z * cf.fwd.z;
            const float pxRadius = depth > 0.05f ? g_clipRadius * cf.f / depth * cf.h * 0.5f : 1e9f;
            const float margin = std::max(24.0f, std::min(std::min(cf.w, cf.h) * 0.42f, pxRadius + 12.0f));
            const bool visible = WorldToScreen(cf, at, &s) && s.x >= margin && s.x <= cf.w - margin && s.y >= margin && s.y <= cf.h - margin;
            if (!visible) {
                const float dist = std::max(4.0f, 1.0f + g_clipRadius * 3.2f);
                at = { cf.pos.x + cf.fwd.x * dist, cf.pos.y + cf.fwd.y * dist, cf.pos.z + cf.fwd.z * dist };
            }
        }
        SpawnSet(g_clip, at, g_clipRadius, "pasted");
    }
    static void GroupSel(bool group) {
        if (g_sel.empty()) return;
        auto list = core::Spawned(); int gid = group ? core::NewGroupId() : 0; int n = 0; std::vector<Act> acts;
        for (int uid : g_sel) {
            const SpawnedObj* o = Find(list, uid); if (!o || o->hidden || o->group == gid) continue;
            Act a{}; a.kind = Act::SetGroup; a.uid = uid; a.group = o->group; a.group1 = gid; a.proj = o->proj; acts.push_back(a);
            core::SetGroup(uid, gid); n++;
        }
        Push(std::move(acts));
        Note(T("%s %d objects"), T(group ? "grouped" : "ungrouped"), n);
    }
    static void RotateSel(float degrees) {
        if (g_sel.empty() || !std::isfinite(degrees)) return;
        auto list = core::Spawned(); Vec3 c{}; int n = 0;
        for (int uid : g_sel) { const SpawnedObj* o = Find(list, uid); if (!o || o->hidden) continue; c.x += o->pos.x; c.y += o->pos.y; c.z += o->pos.z; ++n; }
        if (!n) return; c.x /= n; c.y /= n; c.z /= n;
        const float a = degrees * 3.14159265f / 180.0f, cs = cosf(a), sn = sinf(a);
        std::vector<Act> acts; std::vector<core::MoveReq> moves;
        for (int uid : g_sel) {
            const SpawnedObj* o = Find(list, uid); if (!o || o->hidden) continue;
            const float dx = o->pos.x - c.x, dz = o->pos.z - c.z;
            const Vec3 pos{ c.x + cs * dx + sn * dz, o->pos.y, c.z - sn * dx + cs * dz };
            const Rot rot{ WrapYaw(o->rot.yaw + degrees), o->rot.pitch, o->rot.roll };
            Act act; act.kind = Act::Move; act.uid = uid; act.prefab = o->prefab; act.pos0 = o->pos; act.rot0 = o->rot; act.sc0 = o->scale; act.pos1 = pos; act.rot1 = rot; act.sc1 = o->scale;
            acts.push_back(act); moves.push_back({ uid, pos, rot, o->scale });
        }
        if (!moves.empty()) { if (!core::MoveMany(moves, true)) { Note(T("rotation could not be queued; game thread is not ready")); return; } Push(std::move(acts)); g_editUid = 0; }
    }
    static void MoveSel(Vec3 delta) {
        if (g_sel.empty()) return;
        auto list = core::Spawned(); std::vector<Act> acts; std::vector<core::MoveReq> moves;
        for (int uid : g_sel) {
            const SpawnedObj* o = Find(list, uid); if (!o || o->hidden) continue;
            const Vec3 pos{ o->pos.x + delta.x, o->pos.y + delta.y, o->pos.z + delta.z };
            Act a; a.kind = Act::Move; a.uid = uid; a.prefab = o->prefab; a.pos0 = o->pos; a.rot0 = o->rot; a.sc0 = o->scale; a.pos1 = pos; a.rot1 = o->rot; a.sc1 = o->scale;
            acts.push_back(a); moves.push_back({ uid, pos, o->rot, o->scale });
        }
        if (!moves.empty() && core::MoveMany(moves, true)) { Push(std::move(acts)); g_editUid = 0; }
    }
    static void ScaleSel(float factor) {
        if (g_sel.empty() || !std::isfinite(factor) || factor <= 0) return;
        auto list = core::Spawned(); Vec3 c{}; int n = 0;
        for (int uid : g_sel) { const SpawnedObj* o = Find(list, uid); if (!o || o->hidden) continue; c.x += o->pos.x; c.y += o->pos.y; c.z += o->pos.z; ++n; }
        if (!n) return; c.x /= n; c.y /= n; c.z /= n;
        std::vector<Act> acts; std::vector<core::MoveReq> moves;
        for (int uid : g_sel) {
            const SpawnedObj* o = Find(list, uid); if (!o || o->hidden) continue;
            const Vec3 pos{ c.x + (o->pos.x - c.x) * factor, c.y + (o->pos.y - c.y) * factor, c.z + (o->pos.z - c.z) * factor };
            const float scale = std::clamp(o->scale * factor, 0.05f, 20.0f);
            Act a; a.kind = Act::Move; a.uid = uid; a.prefab = o->prefab; a.pos0 = o->pos; a.rot0 = o->rot; a.sc0 = o->scale; a.pos1 = pos; a.rot1 = o->rot; a.sc1 = scale;
            acts.push_back(a); moves.push_back({ uid, pos, o->rot, scale });
        }
        if (!moves.empty() && core::MoveMany(moves, true)) { Push(std::move(acts)); g_editUid = 0; }
    }
    static void OpenSelectionProperties() {
        g_editUid = 0;
        if (g_compact) g_compactPage = TabScene;
        else { g_mainTab = TabScene; g_selectMainTab = true; }
    }
    static void AlignSel(int axis) {
        if (g_sel.size() < 2 || axis < 0 || axis > 2) return;
        auto list = core::Spawned(); const auto units = SelectionUnits(g_sel, list);
        if (units.size() < 2) return;
        const SelectionUnit* base = nullptr;
        for (const auto& unit : units) for (const SpawnedObj* o : unit.objects) if (o->uid == g_primary) { base = &unit; break; }
        if (!base) return;
        const float target = axis == 0 ? base->center.x : axis == 1 ? base->center.y : base->center.z;
        std::vector<Act> acts; std::vector<core::MoveReq> moves;
        for (const auto& unit : units) {
            const float current = axis == 0 ? unit.center.x : axis == 1 ? unit.center.y : unit.center.z;
            const float delta = target - current;
            if (delta == 0.0f) continue;
            for (const SpawnedObj* o : unit.objects) {
                Vec3 pos = o->pos; if (axis == 0) pos.x += delta; else if (axis == 1) pos.y += delta; else pos.z += delta;
                Act act; act.kind = Act::Move; act.uid = o->uid; act.prefab = o->prefab; act.pos0 = o->pos; act.rot0 = o->rot; act.sc0 = o->scale; act.pos1 = pos; act.rot1 = o->rot; act.sc1 = o->scale;
                acts.push_back(act); moves.push_back({ o->uid, pos, o->rot, o->scale });
            }
        }
        if (!moves.empty()) { if (!core::MoveMany(moves, true)) { Note(T("alignment could not be queued; game thread is not ready")); return; } Push(std::move(acts)); g_editUid = 0; }
    }
    static void QuickGimmickSpawn() {   // Log tab button: spawn the override prefab (or a standtorch) through the game's own spawn path, newest capture as the template
        if (!core::GimmickReplayPrefab()[0]) core::SetGimmickReplayPrefab("/object/cd_gimmick/00_common/lamp/gimmick_lamp_standtorch_03_on.prefab");
        core::GimmickCapInfo caps[4]; const int nc = core::GimmickCaptureList(caps, 4);
        if (!nc) { Note(T("no spawn template yet: walk a few meters first")); return; }
        core::ArmGimmickReplay(InFront(2.0f, 0.0f), caps[0].id);
        const char* fn = strrchr(core::GimmickReplayPrefab(), '/'); Note(T("spawning %s"), fn ? fn + 1 : core::GimmickReplayPrefab());
    }
    static void HandleHotkeys(bool havePos) {
        ImGuiIO& io = ImGui::GetIO();
        const bool sceneContext = g_compact ? g_compactPage == TabScene : g_mainTab == TabScene;
        auto selectAllForContext = [&]() {
            if (sceneContext) SelectAllSceneEntities(core::Spawned(), core::ManagedNpcs(), g_projTab);
        };
        auto deleteForContext = [&]() {
            if (sceneContext && SceneHasSelection()) DeleteSceneSelection();
        };
        auto groupForContext = [&]() {
            if (sceneContext && SceneHasSelection()) GroupSceneSelection(true);
        };
        if (g_cameraMode) {
            // Camera movement uses async key state because the game may not forward legacy keyboard messages.
            // Editing shortcuts must use the same reliable source while camera mode owns the workflow.
            auto pressed = [&](int vk) {
                const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
                const bool fire = now && !g_cameraShortcutDown[vk & 0xFF];
                g_cameraShortcutDown[vk & 0xFF] = now;
                return fire;
            };
            const bool pZ = pressed('Z'), pY = pressed('Y'), pC = pressed('C'), pV = pressed('V');
            const bool pD = pressed('D'), pG = pressed('G'), pA = pressed('A'), pDelete = pressed(VK_DELETE);
            const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            if (io.WantTextInput || g_playMode) return;
            if (ctrl && pZ) Undo();
            if (ctrl && pY) Redo();
            if (sceneContext && ctrl && pC && !g_sel.empty()) CopySel();
            if (sceneContext && ctrl && pV) Paste(havePos);
            if (sceneContext && ctrl && pD && !g_sel.empty()) { CopySel(); Paste(havePos); }
            if (ctrl && pG) groupForContext();
            if (ctrl && pA) selectAllForContext();
            if (!ctrl && pDelete) deleteForContext();
            return;
        }
        if (io.WantTextInput || g_playMode) return;
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) Undo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) Redo();
        if (sceneContext && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false) && !g_sel.empty()) CopySel();
        if (sceneContext && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) Paste(havePos);
        if (sceneContext && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && !g_sel.empty()) { CopySel(); Paste(havePos); }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G, false)) groupForContext();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) selectAllForContext();
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) deleteForContext();
    }

    // ---- line / circle tools ----
    static int g_arrCount = 5; static float g_arrSpacing = 2.0f, g_arrRadius = 4.0f; static int g_lineYawMode = 2, g_circleYawMode = 1;
    static void SpawnArray(bool circle, bool havePos) {
        if (g_selPrefab < 0 || !havePos) return;
        const auto& pi = core::PrefabIndex()[g_selPrefab];
        std::vector<ClipItem> items;
        const int n = std::max(1, std::min(200, g_arrCount));
        // yaw that turns the prefab's local X axis onto the direction (dx, dz): MakeTransform rotates X to (cos yaw, -sin yaw)
        auto yawTo = [](float dx, float dz) { return WrapYaw(atan2f(-dz, dx) * 180.0f / 3.14159265f); };
        if (!circle) {
            const float lx = g_fz, lz = -g_fx;   // the line runs across the line of sight (left to right)
            for (int i = 0; i < n; i++) { float d = (i - (n - 1) * 0.5f) * g_arrSpacing;
                float yaw = g_lineYawMode == 1 ? yawTo(lx, lz) : g_lineYawMode == 2 ? WrapYaw(yawTo(lx, lz) + 90) : g_spawnYaw;
                items.push_back({ pi.path, { lx * d, 0, lz * d }, Rot{ yaw }, g_spawnScale }); }
            SpawnSet(items, InFront(std::max(g_arrRadius, n * g_arrSpacing * 0.5f), g_off[1]), n * g_arrSpacing * 0.5f + 1, "line");
        } else {
            for (int i = 0; i < n; i++) { float a = i * 6.28318531f / n; float x = sinf(a) * g_arrRadius, z = cosf(a) * g_arrRadius;
                float yaw = g_circleYawMode == 1 ? yawTo(-x, -z) : g_circleYawMode == 2 ? yawTo(x, z) : g_spawnYaw;   // X axis towards / away from the center
                items.push_back({ pi.path, { x, 0, z }, Rot{ yaw }, g_spawnScale }); }
            SpawnSet(items, InFront(g_arrRadius + 1, g_off[1]), g_arrRadius + 1, "circle");
        }
    }

    static void PrefabContextMenu(int i, const core::PrefabInfo& pi, const char* id) {
        if (!ImGui::BeginPopupContextItem(id)) return;
        g_selPrefab = i;
        if (ImGui::MenuItem(T(core::IsFavorite(i) ? "remove favorite" : "add favorite"))) core::ToggleFavorite(i);
        if (thumbgen::Ready() && ImGui::MenuItem(T("render preview again"))) thumbgen::Refresh(pi.path);   // a broken or missing image from an earlier read error
        ImGui::Separator();
        for (int ci = 0; ci < (int)g_colls.size(); ci++) { bool in = InColl(g_colls[ci], pi.path); if (ImGui::MenuItem((std::string(T(in ? "remove from " : "add to ")) + g_colls[ci].name).c_str())) { auto& v = g_colls[ci].paths; if (in) v.erase(std::remove(v.begin(), v.end(), pi.path), v.end()); else v.push_back(pi.path); SaveColls(); g_lastKey.clear(); } }
        if (g_colls.empty()) ImGui::TextDisabled(T("no collections yet (left pane)"));
        ImGui::EndPopup();
    }
    static void ArmBrowserDrag(int prefab, bool blocked = false) {
        if (!blocked && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.0f)) g_browserDragPrefab = prefab;
    }
    // tile view: one card per match with the preview image, the name below, star = favorite; click selects, double-click spawns, right-click = menu
    static void StartPlaceNew(const PosInfo& p, bool havePos);
    static void DrawCards(const PosInfo& p, bool havePos, float listH, float ui, int forceCols = 0, float tilePx = 0) {
        const auto& idx = core::PrefabIndex();
        if (thumbgen::Ready()) {
            const int done = thumbgen::Done(), failed = thumbgen::Failed(), total = thumbgen::Total();
            if (done + failed < total && !thumbgen::Idle()) { ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 200, 90, 255)); ImGui::TextWrapped(T("Previews are still being rendered: %d of %d done (%d without visible geometry). Empty tiles fill in as they finish, the visible ones are rendered first."), done + failed, total, failed); ImGui::PopStyleColor(); }
            int pd = 0, pt = 0;   // re-render pass after a renderer fix: existing images are replaced, the counts above do not move
            if (thumbgen::PassProgress(&pd, &pt) && pt > 0) { ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 200, 255, 255)); ImGui::TextWrapped(T("Updating existing previews with the improved renderer: %d of %d. Tiles on screen are updated first."), pd, pt); ImGui::PopStyleColor(); }
        } else if (thumbgen::Error()[0]) { ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 120, 100, 255)); ImGui::TextWrapped(T("Previews are off: %s"), thumbgen::Error()); ImGui::PopStyleColor(); }
        else ImGui::TextDisabled(T("preview generator is starting..."));
        const float pad = 4.0f * ui, tile = tilePx > 0 ? tilePx : g_cardSize * ui, textH = ImGui::GetTextLineHeight() * 2 + 3;
        const float cw = tile + 2 * pad, ch = tile + 2 * pad + textH;
        ImGui::BeginChild("cards", ImVec2(0, listH), ImGuiChildFlags_Borders);
        const float sp = ImGui::GetStyle().ItemSpacing.x, spy = ImGui::GetStyle().ItemSpacing.y;
        const int cols = forceCols > 0 ? forceCols : std::max(1, (int)((ImGui::GetContentRegionAvail().x + sp) / (cw + sp)));
        const int n = (int)g_matches.size(), rows = (n + cols - 1) / cols;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiListClipper clip; clip.Begin(rows, ch + spy);
        while (clip.Step()) for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
            for (int c = 0; c < cols; c++) {
                const int k = r * cols + c; if (k >= n) break;
                const int i = g_matches[k]; const auto& pi = idx[i];
                if (c) ImGui::SameLine();
                ImGui::PushID(i);
                const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1 = { p0.x + cw, p0.y + ch };
                const ImVec2 t0 = { p0.x + pad, p0.y + pad }, t1 = { t0.x + tile, t0.y + tile };
                const ImVec2 star0 = t0, star1 = { t0.x + 20 * ui, t0.y + 20 * ui };
                ImGui::InvisibleButton("card", ImVec2(cw, ch));
                const bool hov = ImGui::IsItemHovered(), sel = g_selPrefab == i;
                const ImVec2 m = ImGui::GetIO().MousePos; const bool onStar = m.x >= star0.x && m.x < star1.x && m.y >= star0.y && m.y < star1.y;
                if (ImGui::IsItemClicked(0)) { if (onStar) core::ToggleFavorite(i); else g_selPrefab = i; }
                if (hov && !onStar && ImGui::IsMouseDoubleClicked(0) && havePos) { g_selPrefab = i; StartPlaceNew(p, havePos); }
                { const ImVec2 cp = ImGui::GetIO().MouseClickedPos[ImGuiMouseButton_Left]; const bool beganOnStar = cp.x >= star0.x && cp.x < star1.x && cp.y >= star0.y && cp.y < star1.y; ArmBrowserDrag(i, beganOnStar); }
                PrefabContextMenu(i, pi, "cardctx");
                dl->AddRectFilled(p0, p1, sel ? ImGui::GetColorU32(ImGuiCol_Header) : hov ? ImGui::GetColorU32(ImGuiCol_FrameBgHovered) : ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
                if (sel) dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_HeaderActive), 4.0f, 0, 2.0f);
                if (thumbgen::Ready()) thumbgen::Request(pi.path);   // no-op when done; during a re-render pass the visible tiles go first
                if (ImTextureID tex = overlay::Thumb(core::ThumbFile(pi.path))) dl->AddImage(tex, t0, t1);
                else {
                    dl->AddRectFilled(t0, t1, IM_COL32(0, 0, 0, 70), 3.0f);
                    const char* st = "no preview";
                    if (thumbgen::Ready() && !thumbgen::Processed(pi.path)) { thumbgen::Request(pi.path); st = thumbgen::Pending(pi.path) ? "rendering..." : "queued"; }
                    const ImVec2 ts = ImGui::CalcTextSize(st);
                    dl->AddText({ t0.x + (tile - ts.x) * 0.5f, t0.y + (tile - ts.y) * 0.5f }, ImGui::GetColorU32(ImGuiCol_TextDisabled), st);
                }
                const bool fav = core::IsFavorite(i);
                if (fav || hov) { dl->AddRectFilled(star0, star1, IM_COL32(0, 0, 0, 110), 3.0f); dl->AddText({ star0.x + 4 * ui, star0.y + 2 * ui }, fav ? IM_COL32(255, 199, 64, 255) : IM_COL32(200, 200, 200, 160), ICON_STAR); }
                dl->PushClipRect({ p0.x + pad, t1.y }, { p1.x - pad, p1.y }, true);
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), { p0.x + pad, t1.y + 2 }, ImGui::GetColorU32(ImGuiCol_Text), ShownName(pi).c_str(), nullptr, tile);
                dl->PopClipRect();
                if (hov && !ImGui::IsPopupOpen("cardctx")) ImGui::SetTooltip("%s\n%s\n%s\n%s%s", ShownName(pi).c_str(), pi.path.c_str(), core::Categories()[pi.cat].name.c_str(), pi.tags.c_str(), onStar ? (std::string("\n") + T("(click: favorite)")).c_str() : "");
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }
    // ---- NPCs and creatures: catalog + managed server actors ------------------------------------------------------------
    // NPCs created here are registered by World Builder. Their live actor is captured from the game's own SpawnCharacter
    // request so they can be selected, moved (remove + respawn), deleted, AI-controlled and persisted in .cdproj files.
    static char g_npcFilter[128] = ""; static int g_npcCat = 0, g_npcSel = -1, g_npcCount = 1; static float g_npcDist = 5.0f;
    static int g_npcFormation = 0; static float g_npcSpacing = 1.5f, g_npcRadius = 8.0f;
    static bool g_npcSpawnAi = true; static int g_npcSpawnBehavior = 0;
    static int g_npcNoteEditUid = 0, g_npcLabelEditUid = 0, g_groupNameEditId = 0, g_objectNoteEditUid = 0;
    static int g_metadataPopupRequest = 0;   // opened from the root ID scope so row/context-menu IDs do not trap the modal
    static char g_npcNoteEdit[512] = "", g_npcLabelEdit[160] = "", g_groupNameEdit[160] = "", g_objectNoteEdit[512] = "";
    // NPCs only live where the world is streamed in around the character, and every one is a full server actor: a few hundred
    // within a few hundred metres is what the game copes with (the server jobs are spread over ticks, see ProcessServerJobs).
    static const int kNpcMaxCount = 500; static const float kNpcMaxDist = 500.0f, kNpcMaxExtent = 200.0f;
    static std::vector<int> g_npcRows; static std::string g_npcKey;
    static const char* kNpcCats[] = { "all", "people", "animals and mounts", "monsters", "bosses", "other" };
    static const char* kNpcFormations[] = { "Line", "Matrix", "Circle" };
    static const char* kNpcBehaviors[] = { "Normal autonomous", "Hold position (AI paused)" };
    static void SpawnNpcFormation(uint32_t key, Vec3 center, int count, int formation, float spacing, float radius, float fx, float fz, bool ai, int behavior) {
        count = std::clamp(count, 1, kNpcMaxCount); formation = std::clamp(formation, 0, 2);
        const float fl = sqrtf(fx * fx + fz * fz); if (fl > 1e-4f) { fx /= fl; fz /= fl; } else { fx = 0; fz = 1; }
        const float rx = fz, rz = -fx;
        const int cols = std::max(1, (int)ceilf(sqrtf((float)count))), rows = std::max(1, (count + cols - 1) / cols);
        std::vector<Act> acts; acts.reserve(count);
        for (int k = 0; k < count; ++k) {
            float side = 0, depth = 0;
            if (formation == 0) side = (k - (count - 1) * 0.5f) * spacing;
            else if (formation == 1) {
                const int row = k / cols, col = k % cols, rowCount = std::min(cols, count - row * cols);
                side = (col - (rowCount - 1) * 0.5f) * spacing; depth = (row - (rows - 1) * 0.5f) * spacing;
            } else {
                const float a = 6.28318530718f * k / (float)count, r = count > 1 ? radius : 0.0f;
                side = cosf(a) * r; depth = sinf(a) * r;
            }
            const Vec3 at{ center.x + rx * side + fx * depth, center.y, center.z + rz * side + fz * depth };
            const int uid = core::SpawnManagedNpc(key, at, 1, 0, ai, behavior);
            if (uid) { Act a; a.kind = Act::NpcSpawn; a.uid = uid; a.prefab = std::string("NPC ") + std::to_string(key); a.pos1 = at; a.flag1 = ai; a.behavior1 = behavior; acts.push_back(std::move(a)); }
        }
        Push(std::move(acts));
    }
    static void SpawnNpcFormationGrounded(uint32_t key, Vec3 center, int count, int formation, float spacing, float radius, float fx, float fz, bool ai, int behavior) {
        if (core::GroundProbeReady()) {
            const float startY = center.y + 150.0f;
            const int ticket = core::GroundProbe({ center.x, startY, center.z }, 400.0f);
            if (ticket) {
                g_npcDropJobs.push_back({ key, ticket, center, count, formation, spacing, radius, fx, fz, ai, behavior });
                return;
            }
        }
        SpawnNpcFormation(key, center, count, formation, spacing, radius, fx, fz, ai, behavior);
    }
    static int NpcCategory(const std::string& n) {   // from the internal name's first token: NHM_ = human male, NGW_ = goblin female, ...
        const std::string t = n.substr(0, n.find('_'));
        if (t == "Animal" || t == "Riding" || t == "NatureCreature") return 2;
        if (t == "MON" || t == "Mon" || t == "Marni" || t == "Marionette") return 3;
        if (t == "Boss" || t == "MiddleBoss") return 4;
        if (t.size() >= 3 && t.size() <= 4 && t[0] == 'N' && (t.back() == 'M' || t.back() == 'W')) return 1;
        return 5;
    }
    static bool g_npcCards = false;
    static void SpawnNpcInFront(const thumbgen::CharInfo& c) {
        Vec3 at = { g_lastPlayer.x + g_fx * g_npcDist, g_lastPlayer.y, g_lastPlayer.z + g_fz * g_npcDist };
        SpawnNpcFormationGrounded(c.key, at, 1, 0, g_npcSpacing, g_npcRadius, g_fx, g_fz, g_npcSpawnAi, g_npcSpawnBehavior);
        Note(T("spawn %s"), c.name.empty() ? c.internal.c_str() : c.name.c_str());
    }
    static const ManagedNpc* FindManagedNpc(const std::vector<ManagedNpc>& list, int uid) {
        for (const auto& n : list) if (n.uid == uid) return &n; return nullptr;
    }
    static std::string ManagedNpcName(const ManagedNpc& n, const std::vector<thumbgen::CharInfo>* chars) {
        if (!n.label.empty()) return n.label;
        if (chars) for (const auto& c : *chars) if (c.key == n.key) return c.name.empty() ? c.internal : c.name;
        return std::string("NPC ") + std::to_string(n.key);
    }
    static std::string ManagedNpcHistoryName(const ManagedNpc& n) { return n.label.empty() ? std::string("NPC ") + std::to_string(n.key) : n.label; }
    static void SelectManagedNpc(int uid, bool add) {
        if (g_place.active) DropCarried();
        const auto list = core::ManagedNpcs(); const ManagedNpc* n = FindManagedNpc(list, uid);
        if (!add) ClearSceneSelection();
        if (g_selectGroups && n && n->group > 0 && !add) {
            for (const auto& x : list) if (!x.hidden && x.group == n->group) g_managedNpcSel.insert(x.uid);
            for (const auto& o : core::Spawned()) if (!o.hidden && o.group == n->group) g_sel.insert(o.uid);
        } else {
            if (add && g_managedNpcSel.count(uid)) g_managedNpcSel.erase(uid); else g_managedNpcSel.insert(uid);
        }
        g_managedNpcPrimary = uid; g_managedNpcLast = uid; g_primary = 0; g_sceneLastEntity = -uid; g_editUid = 0;
    }
    static void SelectAllManagedNpcs(const std::vector<ManagedNpc>& list, int projectFilter) {
        g_managedNpcSel.clear(); for (const auto& n : list) if (!n.hidden && (projectFilter < 0 || n.proj == projectFilter)) g_managedNpcSel.insert(n.uid);
        g_managedNpcPrimary = g_managedNpcSel.empty() ? 0 : *g_managedNpcSel.begin();
    }
    static void SetSelectedNpcAi(bool enabled) {
        const auto list = core::ManagedNpcs(); std::vector<Act> acts;
        for (int uid : g_managedNpcSel) if (const auto* n = FindManagedNpc(list, uid)) {
            const int afterBehavior = enabled && n->behavior == 1 ? 0 : n->behavior;
            Act a; a.kind = Act::NpcControl; a.uid = uid; a.prefab = ManagedNpcHistoryName(*n); a.flag0 = n->aiEnabled; a.flag1 = enabled; a.behavior0 = n->behavior; a.behavior1 = afterBehavior;
            core::SetManagedNpcControl(uid, enabled, afterBehavior); acts.push_back(std::move(a));
        }
        Push(std::move(acts));
    }
    static void SetSelectedNpcBehavior(int behavior) {
        behavior = behavior == 1 ? 1 : 0; const bool enabled = behavior == 0;
        const auto list = core::ManagedNpcs(); std::vector<Act> acts;
        for (int uid : g_managedNpcSel) if (const auto* n = FindManagedNpc(list, uid)) {
            Act a; a.kind = Act::NpcControl; a.uid = uid; a.prefab = ManagedNpcHistoryName(*n); a.flag0 = n->aiEnabled; a.flag1 = enabled; a.behavior0 = n->behavior; a.behavior1 = behavior;
            core::SetManagedNpcControl(uid, enabled, behavior); acts.push_back(std::move(a));
        }
        Push(std::move(acts));
    }
    static void MoveSelectedNpcs(Vec3 delta) {
        const auto list = core::ManagedNpcs(); std::vector<Act> acts;
        for (int uid : g_managedNpcSel) if (const auto* n = FindManagedNpc(list, uid)) {
            const Vec3 after{ n->pos.x + delta.x, n->pos.y + delta.y, n->pos.z + delta.z };
            Act a; a.kind = Act::NpcMove; a.uid = uid; a.prefab = ManagedNpcHistoryName(*n); a.pos0 = n->pos; a.pos1 = after; core::MoveManagedNpc(uid, after); acts.push_back(std::move(a));
        }
        Push(std::move(acts));
    }
    static void DeleteSelectedNpcs() {
        const auto list = core::ManagedNpcs(); std::vector<Act> acts;
        for (int uid : g_managedNpcSel) if (const auto* n = FindManagedNpc(list, uid)) if (!n->hidden) {
            Act a; a.kind = Act::NpcDelete; a.uid = uid; a.prefab = ManagedNpcHistoryName(*n); a.pos0 = n->pos; a.flag0 = n->aiEnabled; a.behavior0 = n->behavior; a.group = n->group; a.text0 = n->label;
            acts.push_back(a); core::HideManagedNpc(uid);
        }
        Push(std::move(acts)); g_managedNpcSel.clear(); g_managedNpcPrimary = 0;
    }
    static void GroupSelectedNpcs(bool makeGroup) {
        if (g_managedNpcSel.empty()) return; const int gid = makeGroup ? core::NewGroupId() : 0; const auto list = core::ManagedNpcs(); std::vector<Act> acts;
        for (int uid : g_managedNpcSel) if (const auto* n = FindManagedNpc(list, uid)) { Act a; a.kind = Act::NpcGroup; a.uid = uid; a.prefab = ManagedNpcHistoryName(*n); a.group = n->group; a.group1 = gid; acts.push_back(a); core::SetManagedNpcGroup(uid, gid); }
        Push(std::move(acts));
    }
    static void SelectAllSceneEntities(const std::vector<SpawnedObj>& objects, const std::vector<ManagedNpc>& npcs, int projectFilter) {
        ClearSceneSelection();
        for (const auto& o : objects) if (!o.hidden && (projectFilter < 0 || o.proj == projectFilter)) g_sel.insert(o.uid);
        for (const auto& n : npcs) if (!n.hidden && (projectFilter < 0 || n.proj == projectFilter)) g_managedNpcSel.insert(n.uid);
        if (!g_sel.empty()) { g_primary = *g_sel.begin(); g_sceneLastEntity = g_primary; }
        else if (!g_managedNpcSel.empty()) { g_managedNpcPrimary = *g_managedNpcSel.begin(); g_sceneLastEntity = -g_managedNpcPrimary; }
    }
    static void DeleteSceneSelection() {
        auto objects = core::Spawned(); auto npcs = core::ManagedNpcs(); std::vector<Act> acts;
        for (int uid : g_sel) if (const auto* o = Find(objects, uid)) if (!o->hidden) {
            Act a; a.kind = Act::Delete; a.uid = uid; a.prefab = o->prefab; a.pos0 = o->pos; a.rot0 = o->rot; a.sc0 = o->scale; a.group = o->group; a.proj = o->proj; a.text0 = o->note;
            acts.push_back(a); core::HideUid(uid);
        }
        for (int uid : g_managedNpcSel) if (const auto* n = FindManagedNpc(npcs, uid)) if (!n->hidden) {
            Act a; a.kind = Act::NpcDelete; a.uid = uid; a.prefab = ManagedNpcHistoryName(*n); a.pos0 = n->pos; a.flag0 = n->aiEnabled; a.behavior0 = n->behavior; a.group = n->group; a.proj = n->proj; a.text0 = n->label;
            acts.push_back(a); core::HideManagedNpc(uid);
        }
        if (!acts.empty()) Push(std::move(acts));
        ClearSceneSelection();
    }
    static void GroupSceneSelection(bool makeGroup) {
        if (!SceneHasSelection()) return;
        const int gid = makeGroup ? core::NewGroupId() : 0; auto objects = core::Spawned(); auto npcs = core::ManagedNpcs(); std::vector<Act> acts;
        for (int uid : g_sel) if (const auto* o = Find(objects, uid)) if (!o->hidden && o->group != gid) {
            Act a; a.kind = Act::SetGroup; a.uid = uid; a.prefab = o->prefab; a.group = o->group; a.group1 = gid; a.proj = o->proj; acts.push_back(a); core::SetGroup(uid, gid);
        }
        for (int uid : g_managedNpcSel) if (const auto* n = FindManagedNpc(npcs, uid)) if (!n->hidden && n->group != gid) {
            Act a; a.kind = Act::NpcGroup; a.uid = uid; a.prefab = ManagedNpcHistoryName(*n); a.group = n->group; a.group1 = gid; a.proj = n->proj; acts.push_back(a); core::SetManagedNpcGroup(uid, gid);
        }
        Push(std::move(acts));
    }
    static void MoveSceneSelection(Vec3 delta) {
        if (!SceneHasSelection()) return;
        auto objects = core::Spawned(); auto npcs = core::ManagedNpcs(); std::vector<Act> acts; std::vector<core::MoveReq> moves;
        for (int uid : g_sel) if (const auto* o = Find(objects, uid)) if (!o->hidden) {
            Vec3 p{ o->pos.x + delta.x, o->pos.y + delta.y, o->pos.z + delta.z };
            Act a; a.kind = Act::Move; a.uid = uid; a.prefab = o->prefab; a.pos0 = o->pos; a.rot0 = o->rot; a.sc0 = o->scale; a.pos1 = p; a.rot1 = o->rot; a.sc1 = o->scale;
            acts.push_back(a); moves.push_back({ uid, p, o->rot, o->scale });
        }
        for (int uid : g_managedNpcSel) if (const auto* n = FindManagedNpc(npcs, uid)) if (!n->hidden) {
            Vec3 p{ n->pos.x + delta.x, n->pos.y + delta.y, n->pos.z + delta.z };
            Act a; a.kind = Act::NpcMove; a.uid = uid; a.prefab = ManagedNpcHistoryName(*n); a.pos0 = n->pos; a.pos1 = p;
            acts.push_back(a); core::MoveManagedNpc(uid, p);
        }
        if (!moves.empty()) core::MoveMany(moves, true);
        Push(std::move(acts));
    }
    static void OpenNpcNoteEdit(const ManagedNpc& n) { g_npcNoteEditUid = n.uid; strncpy_s(g_npcNoteEdit, n.note.c_str(), _TRUNCATE); g_metadataPopupRequest = 1; }
    static void OpenNpcLabelEdit(const ManagedNpc& n) { g_npcLabelEditUid = n.uid; strncpy_s(g_npcLabelEdit, n.label.c_str(), _TRUNCATE); g_metadataPopupRequest = 2; }
    static void OpenGroupNameEdit(int gid) { g_groupNameEditId = gid; const std::string name = core::GroupName(gid); strncpy_s(g_groupNameEdit, name.c_str(), _TRUNCATE); g_metadataPopupRequest = 3; }
    static void OpenObjectNoteEdit(const SpawnedObj& o) { g_objectNoteEditUid = o.uid; strncpy_s(g_objectNoteEdit, o.note.c_str(), _TRUNCATE); g_metadataPopupRequest = 4; }
    static void DrawMetadataPopups() {
        if (g_metadataPopupRequest) {
            const char* id = g_metadataPopupRequest == 1 ? "Edit NPC note" : g_metadataPopupRequest == 2 ? "Rename NPC" : g_metadataPopupRequest == 3 ? "Rename group" : "Edit object note";
            ImGui::OpenPopup(id); g_metadataPopupRequest = 0;
        }
        if (ImGui::BeginPopupModal("Edit NPC note", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::SetNextItemWidth(460); ImGui::InputTextMultiline("##npcnote", g_npcNoteEdit, sizeof g_npcNoteEdit, ImVec2(460, 110));
            if (ImGui::Button(T("Save"))) {
                const auto list = core::ManagedNpcs(); if (const auto* n = FindManagedNpc(list, g_npcNoteEditUid)) {
                    Act a; a.kind = Act::NpcNote; a.uid = n->uid; a.text0 = n->note; a.text1 = g_npcNoteEdit; core::SetManagedNpcNote(a.uid, a.text1); Push({ a });
                } ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(); if (ImGui::Button(T("Cancel"))) ImGui::CloseCurrentPopup(); ImGui::EndPopup();
        }
        if (ImGui::BeginPopupModal("Rename NPC", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::SetNextItemWidth(360); ImGui::InputText("##npclabel", g_npcLabelEdit, sizeof g_npcLabelEdit);
            if (ImGui::Button(T("Save"))) {
                const auto list = core::ManagedNpcs(); if (const auto* n = FindManagedNpc(list, g_npcLabelEditUid)) {
                    Act a; a.kind = Act::NpcLabel; a.uid = n->uid; a.text0 = n->label; a.text1 = g_npcLabelEdit; core::SetManagedNpcLabel(a.uid, a.text1); Push({ a });
                } ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(); if (ImGui::Button(T("Cancel"))) ImGui::CloseCurrentPopup(); ImGui::EndPopup();
        }
        if (ImGui::BeginPopupModal("Rename group", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::SetNextItemWidth(360); ImGui::InputText("##groupname", g_groupNameEdit, sizeof g_groupNameEdit);
            if (ImGui::Button(T("Save"))) {
                Act a; a.kind = Act::GroupName; a.group = g_groupNameEditId; a.text0 = core::GroupName(a.group); a.text1 = g_groupNameEdit; core::SetGroupName(a.group, a.text1); Push({ a }); ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(); if (ImGui::Button(T("Cancel"))) ImGui::CloseCurrentPopup(); ImGui::EndPopup();
        }
        if (ImGui::BeginPopupModal("Edit object note", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::SetNextItemWidth(460); ImGui::InputTextMultiline("##objectnote", g_objectNoteEdit, sizeof g_objectNoteEdit, ImVec2(460, 110));
            if (ImGui::Button(T("Save"))) {
                const auto list = core::Spawned(); if (const auto* o = Find(list, g_objectNoteEditUid)) {
                    Act a; a.kind = Act::ObjectNote; a.uid = o->uid; a.text0 = o->note; a.text1 = g_objectNoteEdit; core::SetObjectNote(a.uid, a.text1); Push({ a });
                } ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(); if (ImGui::Button(T("Cancel"))) ImGui::CloseCurrentPopup(); ImGui::EndPopup();
        }
    }
    // tiles like the prefab browser's cards: the appearance preview, the in-game name below, a double-click spawns
    static void DrawNpcCards(const std::vector<thumbgen::CharInfo>& chars, float listH, float ui, bool canSpawn) {
        const float pad = 4.0f * ui, tile = g_cardSize * ui, textH = ImGui::GetTextLineHeight() * 2 + 3;
        const float cw = tile + 2 * pad, ch = tile + 2 * pad + textH;
        ImGui::BeginChild("npccards", ImVec2(0, listH), ImGuiChildFlags_Borders);
        const float sp = ImGui::GetStyle().ItemSpacing.x, spy = ImGui::GetStyle().ItemSpacing.y;
        const int cols = std::max(1, (int)((ImGui::GetContentRegionAvail().x + sp) / (cw + sp)));
        const int n = (int)g_npcRows.size(), rows = (n + cols - 1) / cols;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiListClipper clip; clip.Begin(rows, ch + spy);
        while (clip.Step()) for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
            for (int col = 0; col < cols; col++) {
                const int k = r * cols + col; if (k >= n) break;
                const int i = g_npcRows[k]; const auto& c = chars[i];
                if (col) ImGui::SameLine();
                ImGui::PushID(i);
                const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1 = { p0.x + cw, p0.y + ch };
                const ImVec2 t0 = { p0.x + pad, p0.y + pad }, t1 = { t0.x + tile, t0.y + tile };
                ImGui::InvisibleButton("npccard", ImVec2(cw, ch));
                const bool hov = ImGui::IsItemHovered(), sel = g_npcSel == i;
                if (ImGui::IsItemClicked(0)) g_npcSel = i;
                if (hov && ImGui::IsMouseDoubleClicked(0) && canSpawn) { g_npcSel = i; SpawnNpcInFront(c); }
                if (canSpawn && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.0f)) g_npcDragIndex = i;
                dl->AddRectFilled(p0, p1, sel ? ImGui::GetColorU32(ImGuiCol_Header) : hov ? ImGui::GetColorU32(ImGuiCol_FrameBgHovered) : ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
                if (sel) dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_HeaderActive), 4.0f, 0, 2.0f);
                ImTextureID tex = c.app.empty() ? ImTextureID{} : overlay::Thumb(core::ThumbFile(c.app));
                if (tex) dl->AddImage(tex, t0, t1);
                else {
                    dl->AddRectFilled(t0, t1, IM_COL32(0, 0, 0, 70), 3.0f);
                    const char* st = "no preview";
                    if (!c.app.empty() && thumbgen::Ready() && !thumbgen::Processed(c.app)) { thumbgen::Request(c.app); st = thumbgen::Pending(c.app) ? "rendering..." : "queued"; }
                    const char* shown = T(st); const ImVec2 ts = ImGui::CalcTextSize(shown);
                    dl->AddText({ t0.x + (tile - ts.x) * 0.5f, t0.y + (tile - ts.y) * 0.5f }, ImGui::GetColorU32(ImGuiCol_TextDisabled), shown);
                }
                dl->PushClipRect({ p0.x + pad, t1.y }, { p1.x - pad, p1.y }, true);
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), { p0.x + pad, t1.y + 2 }, ImGui::GetColorU32(ImGuiCol_Text), c.name.empty() ? c.internal.c_str() : c.name.c_str(), nullptr, tile);
                dl->PopClipRect();
                if (hov) ImGui::SetTooltip("%s\n%s   %s %u", c.name.empty() ? "-" : c.name.c_str(), c.internal.c_str(), T("ID"), c.key);
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }
    static void DrawManagedNpcs(const std::vector<thumbgen::CharInfo>* chars, bool compact, float ui, int projectFilter = -1) {
        auto list = core::ManagedNpcs();
        for (auto it = g_managedNpcSel.begin(); it != g_managedNpcSel.end();) {
            const auto* n = FindManagedNpc(list, *it); if (!n || n->hidden || (projectFilter >= 0 && n->proj != projectFilter)) it = g_managedNpcSel.erase(it); else ++it;
        }
        if (g_managedNpcPrimary && !g_managedNpcSel.count(g_managedNpcPrimary)) g_managedNpcPrimary = g_managedNpcSel.empty() ? 0 : *g_managedNpcSel.begin();
        int visible = 0; for (const auto& n : list) if (!n.hidden && (projectFilter < 0 || n.proj == projectFilter)) visible++;
        char hdr[96]; snprintf(hdr, sizeof hdr, "%s  (%d)", T("Spawned NPCs"), visible);
        if (!ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen)) return;

        int selCount = 0, pendingCount = 0, syncCount = 0; bool anyOn = false, anyOff = false, anyNormal = false, anyHold = false;
        for (int uid : g_managedNpcSel) if (const auto* n = FindManagedNpc(list, uid)) {
            if (projectFilter >= 0 && n->proj != projectFilter) continue;
            selCount++; pendingCount += (!n->actor || n->spawnPending) ? 1 : 0;
            syncCount += (n->actor && n->aiApplied != n->aiEnabled) ? 1 : 0;
            anyOn |= n->aiEnabled; anyOff |= !n->aiEnabled; anyNormal |= n->behavior == 0; anyHold |= n->behavior == 1;
        }
        if (selCount) {
            const char* aiState = anyOn && anyOff ? T("Mixed") : anyOn ? T("On") : T("Off");
            const char* behaviorState = anyNormal && anyHold ? T("Mixed") : anyHold ? T("Hold") : T("Normal");
            ImGui::TextDisabled("%s %d  |  %s: %s  |  %s: %s", T("selected"), selCount, T("AI"), aiState, T("Behavior"), behaviorState);
            if (pendingCount) { ImGui::SameLine(); ImGui::TextDisabled("  |  %d %s", pendingCount, T("pending")); }
            if (syncCount) { ImGui::SameLine(); ImGui::TextDisabled("  |  %d %s", syncCount, T("syncing")); }
        }
        if (ImGui::Button(T("Select all NPCs"))) SelectAllManagedNpcs(list, projectFilter);
        ImGui::SameLine(); if (ImGui::Button(T("Clear selection"))) { g_managedNpcSel.clear(); g_managedNpcPrimary = 0; }
        ImGui::SameLine(); ImGui::BeginDisabled(g_managedNpcSel.empty() || !core::NpcAiControlAvailable());
        if (ImGui::Button(T("AI on"))) SetSelectedNpcAi(true);
        ImGui::SameLine(); if (ImGui::Button(T("AI off"))) SetSelectedNpcAi(false);
        ImGui::SameLine(); ImGui::SetNextItemWidth(170 * ui);
        const char* behaviorPreview = anyNormal && anyHold ? T("Mixed") : anyHold ? T("Hold") : T("Normal");
        if (ImGui::BeginCombo("##selectednpcbehavior", behaviorPreview)) {
            if (ImGui::Selectable(T("Normal autonomous"), anyNormal && !anyHold)) SetSelectedNpcBehavior(0);
            if (ImGui::Selectable(T("Hold position (AI paused)"), anyHold && !anyNormal)) SetSelectedNpcBehavior(1);
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::BeginDisabled(g_managedNpcSel.empty());
        if (ImGui::Button(T("Delete selected"))) DeleteSelectedNpcs();
        ImGui::EndDisabled();
        if (!core::NpcAiControlAvailable()) { ImGui::SameLine(); ImGui::TextDisabled("%s", T("native AI control unavailable")); }

        std::vector<int> rows; rows.reserve(list.size());
        for (int i = 0; i < (int)list.size(); ++i) if (!list[i].hidden && (projectFilter < 0 || list[i].proj == projectFilter)) rows.push_back(i);
        std::sort(rows.begin(), rows.end(), [&](int a, int b) { return list[a].uid < list[b].uid; });
        const float h = (compact ? 155.0f : 205.0f) * ui;
        if (ImGui::BeginTable("managednpcs", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable, ImVec2(0, h))) {
            ImGui::TableSetupColumn(T("NPC"), ImGuiTableColumnFlags_WidthStretch, 2.5f);
            ImGui::TableSetupColumn(T("AI"), ImGuiTableColumnFlags_WidthFixed, 58 * ui);
            ImGui::TableSetupColumn(T("Behavior"), ImGuiTableColumnFlags_WidthFixed, 105 * ui);
            ImGui::TableSetupColumn(T("Group"), ImGuiTableColumnFlags_WidthFixed, 120 * ui);
            ImGui::TableSetupColumn(T("Position"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn(T("ID"), ImGuiTableColumnFlags_WidthFixed, 62 * ui);
            ImGui::TableHeadersRow();
            ImGuiListClipper clip; clip.Begin((int)rows.size());
            while (clip.Step()) for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                const ManagedNpc& n = list[rows[r]]; const bool selected = g_managedNpcSel.count(n.uid) != 0;
                const std::string name = ManagedNpcName(n, chars);
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::PushID(n.uid);
                if (ImGui::Selectable(name.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                    ImGuiIO& io = ImGui::GetIO();
                    if (ShiftHeld(io) && g_managedNpcLast) {
                        int a = -1, b = -1; for (int q = 0; q < (int)rows.size(); ++q) { if (list[rows[q]].uid == g_managedNpcLast) a = q; if (list[rows[q]].uid == n.uid) b = q; }
                        if (a >= 0 && b >= 0) { if (a > b) std::swap(a, b); if (!CtrlHeld(io)) g_managedNpcSel.clear(); for (int q = a; q <= b; ++q) g_managedNpcSel.insert(list[rows[q]].uid); g_managedNpcPrimary = n.uid; }
                    } else SelectManagedNpc(n.uid, CtrlHeld(io));
                }
                if (ImGui::BeginPopupContextItem("npcctx")) {
                    if (!g_managedNpcSel.count(n.uid)) SelectManagedNpc(n.uid, false);
                    if (!n.note.empty()) { ImGui::TextDisabled("%s", n.note.c_str()); ImGui::Separator(); }
                    ImGui::BeginDisabled(!core::NpcAiControlAvailable());
                    if (ImGui::MenuItem(T("Enable AI"))) SetSelectedNpcAi(true);
                    if (ImGui::MenuItem(T("Disable AI"))) SetSelectedNpcAi(false);
                    ImGui::EndDisabled();
                    if (ImGui::BeginMenu(T("Behavior"))) {
                        if (ImGui::MenuItem(T("Normal autonomous"), nullptr, n.behavior == 0)) SetSelectedNpcBehavior(0);
                        if (ImGui::MenuItem(T("Hold position (AI paused)"), nullptr, n.behavior == 1)) SetSelectedNpcBehavior(1);
                        ImGui::EndMenu();
                    }
                    if (ImGui::BeginMenu(T("Move"))) {
                        ImGui::SetNextItemWidth(110); ImGui::DragFloat(T("amount##npcmove"), &g_moveStep, 0.05f, 0.01f, 100.0f, "%.2f m"); g_moveStep = std::clamp(g_moveStep, 0.01f, 100.0f);
                        if (ImGui::MenuItem(T("+X"))) MoveSelectedNpcs({ g_moveStep, 0, 0 });
                        if (ImGui::MenuItem(T("-X"))) MoveSelectedNpcs({ -g_moveStep, 0, 0 });
                        if (ImGui::MenuItem(T("+Y"))) MoveSelectedNpcs({ 0, g_moveStep, 0 });
                        if (ImGui::MenuItem(T("-Y"))) MoveSelectedNpcs({ 0, -g_moveStep, 0 });
                        if (ImGui::MenuItem(T("+Z"))) MoveSelectedNpcs({ 0, 0, g_moveStep });
                        if (ImGui::MenuItem(T("-Z"))) MoveSelectedNpcs({ 0, 0, -g_moveStep });
                        ImGui::EndMenu();
                    }
                    if (ImGui::MenuItem(T(n.group > 0 ? "Ungroup" : "Group selection"))) GroupSelectedNpcs(n.group == 0);
                    if (n.group > 0 && ImGui::MenuItem(T("Rename group"))) OpenGroupNameEdit(n.group);
                    if (ImGui::MenuItem(T("Rename NPC"))) OpenNpcLabelEdit(n);
                    if (ImGui::MenuItem(T("Edit note"))) OpenNpcNoteEdit(n);
                    ImGui::Separator();
                    if (ImGui::MenuItem(T("Select all NPCs"))) SelectAllManagedNpcs(list, projectFilter);
                    if (ImGui::MenuItem(T("Delete"))) DeleteSelectedNpcs();
                    ImGui::EndPopup();
                }
                ImGui::TableSetColumnIndex(1);
                if (n.actor && n.aiApplied != n.aiEnabled) {
                    ImGui::TextDisabled("%s *", n.aiEnabled ? T("On") : T("Off"));
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("desired AI state has not reached the game yet; World Builder will retry when the server session is available"));
                } else ImGui::TextUnformatted(n.aiEnabled ? T("On") : T("Off"));
                ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("%s", n.behavior == 1 ? T("Hold") : T("Normal"));
                ImGui::TableSetColumnIndex(3);
                if (n.group > 0) { const std::string gn = core::GroupName(n.group); if (!gn.empty()) ImGui::TextUnformatted(gn.c_str()); else ImGui::Text(T("Group %d"), n.group); } else ImGui::TextDisabled("-");
                ImGui::TableSetColumnIndex(4);
                if (n.actor && !n.spawnPending) ImGui::TextDisabled("%.2f  %.2f  %.2f", n.pos.x, n.pos.y, n.pos.z);
                else ImGui::TextDisabled("%.2f  %.2f  %.2f  [%s]", n.pos.x, n.pos.y, n.pos.z, T("pending"));
                ImGui::TableSetColumnIndex(5); ImGui::TextDisabled("#%d", n.uid);
                if (ImGui::IsItemHovered() && !n.note.empty()) ImGui::SetTooltip("%s", n.note.c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (g_managedNpcPrimary) {
            if (const auto* n = FindManagedNpc(list, g_managedNpcPrimary)) {
                static int posUid = 0; static float ep[3] = {}; static Vec3 base{};
                if (posUid != n->uid || (!ImGui::IsAnyItemActive() && (ep[0] != n->pos.x || ep[1] != n->pos.y || ep[2] != n->pos.z))) {
                    posUid = n->uid; ep[0] = base.x = n->pos.x; ep[1] = base.y = n->pos.y; ep[2] = base.z = n->pos.z;
                }
                ImGui::TextDisabled("%s", T("Selected NPC properties"));
                ImGui::SameLine(); ImGui::Text("%s", ManagedNpcName(*n, chars).c_str());
                ImGui::SetNextItemWidth(compact ? -1.0f : 330 * ui);
                if (ImGui::DragFloat3(T("position##managednpc"), ep, 0.05f, -100000.0f, 100000.0f, "%.3f")) {}
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    const Vec3 delta{ ep[0] - base.x, ep[1] - base.y, ep[2] - base.z }; MoveSelectedNpcs(delta); base = { ep[0], ep[1], ep[2] };
                }
                ImGui::SameLine(); if (ImGui::SmallButton(T("Edit note"))) OpenNpcNoteEdit(*n);
                if (!n->note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", n->note.c_str()); }
            }
        }
        ImGui::Separator();
    }
    static void DrawNpcs(const PosInfo& p, bool havePos, bool compact = false) {
        const auto chars = thumbgen::Characters();
        const float ui = ImGui::GetFontSize() / 17.0f;
        const int st = core::NpcState();
        if (st == 0) { ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), T("NPC spawning is not available in this game build (see the log).")); }
        else if (st == 1) { ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), T("walk a few steps first: the game's spawn request needs your character's server actor")); }
        if (!chars) { ImGui::TextDisabled(T("reading the character list from the game files...")); return; }
        if (!compact) {   // view switch: list or tiles (tile size shared with the browser)
            const ImVec4 on = ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive), off = ImGui::GetStyleColorVec4(ImGuiCol_Button);
            ImGui::PushStyleColor(ImGuiCol_Button, g_npcCards ? off : on); if (ImGui::Button(T(ICON_LIST " list"))) g_npcCards = false; ImGui::PopStyleColor();
            ImGui::SameLine(0, 2); ImGui::PushStyleColor(ImGuiCol_Button, g_npcCards ? on : off); if (ImGui::Button(T(ICON_COPY " cards"))) g_npcCards = true; ImGui::PopStyleColor();
            if (g_npcCards) { ImGui::SameLine(); ImGui::SetNextItemWidth(100 * ui); ImGui::SliderFloat("##npccardsize", &g_cardSize, 64.0f, 200.0f, "%.0f px"); if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("tile size")); }
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(compact ? -1.0f : std::max(120.0f * ui, ImGui::GetContentRegionAvail().x - 260.0f * ui));
        InputTextI18n("##npcfilter", T("search  (name, internal name or key)"), g_npcFilter, sizeof g_npcFilter);
        if (!compact) ImGui::SameLine();
        ImGui::SetNextItemWidth(compact ? -1.0f : 180 * ui); ComboT("##npccat", &g_npcCat, kNpcCats, 6);
        const std::string key = std::string(g_npcFilter) + "|" + std::to_string(g_npcCat) + "|" + std::to_string((uintptr_t)chars.get());
        if (key != g_npcKey) {
            const int oldSel = g_npcSel;
            g_npcKey = key; g_npcRows.clear();
            std::vector<std::string> words; { std::string w; for (const char* c = g_npcFilter;; c++) { if (!*c || *c == ' ') { if (!w.empty()) words.push_back(w); w.clear(); if (!*c) break; } else w += (char)SearchFold((unsigned char)*c); } }
            for (int i = 0; i < (int)chars->size(); i++) {
                const auto& c = (*chars)[i];
                if (g_npcCat && NpcCategory(c.internal) != g_npcCat) continue;
                const std::string hay = c.name + " " + c.internal + " " + std::to_string(c.key); bool ok = true;
                for (const auto& w : words) if (!ContainsCI(hay, w)) { ok = false; break; }
                if (ok) g_npcRows.push_back(i);
            }
            std::stable_sort(g_npcRows.begin(), g_npcRows.end(), [&](int a, int b) { const auto& x = (*chars)[a]; const auto& y = (*chars)[b]; if (x.name.empty() != y.name.empty()) return !x.name.empty(); return (x.name.empty() ? x.internal : x.name) < (y.name.empty() ? y.internal : y.name); });
            g_npcSel = std::find(g_npcRows.begin(), g_npcRows.end(), oldSel) != g_npcRows.end() ? oldSel : -1;
        }
        ImGui::TextDisabled(T("%d characters"), (int)g_npcRows.size());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", T("double-click = one NPC; SPAWN or drag = current count and formation"));
        const float detailsH = (compact ? 265.0f : 205.0f) * ui;
        float listH = ImGui::GetContentRegionAvail().y - detailsH - ImGui::GetStyle().ItemSpacing.y; if (listH < 80 * ui) listH = 80 * ui;
        const bool useCards = compact || g_npcCards;
        if (useCards) DrawNpcCards(*chars, listH, ui, havePos && st == 2);
        else if (ImGui::BeginTable("npcs", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable, ImVec2(0, listH))) {
            ImGui::TableSetupColumn(T("name"), ImGuiTableColumnFlags_WidthStretch, 3);
            ImGui::TableSetupColumn(T("internal name"), ImGuiTableColumnFlags_WidthStretch, 4);
            ImGui::TableSetupColumn(T("ID"), ImGuiTableColumnFlags_WidthFixed, 70 * ui);
            ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
            ImGuiListClipper clip; clip.Begin((int)g_npcRows.size());
            while (clip.Step()) for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                const int i = g_npcRows[r]; const auto& c = (*chars)[i];
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::PushID(i);
                if (ImGui::Selectable(c.name.empty() ? "-" : c.name.c_str(), g_npcSel == i, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    g_npcSel = i;
                    if (ImGui::IsMouseDoubleClicked(0) && havePos && st == 2) SpawnNpcInFront(c);
                }
                if (havePos && st == 2 && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.0f)) g_npcDragIndex = i;
                ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("%s", c.internal.c_str());
                ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("%u", c.key);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::BeginChild("npcdetails", ImVec2(0, detailsH), ImGuiChildFlags_Borders);
        if (g_npcSel >= 0 && g_npcSel < (int)chars->size()) {
            const auto& c = (*chars)[g_npcSel];
            {   // the preview of its appearance (same renderer and cache as the character browser)
                const float th = (compact ? 72.0f : 96.0f) * ui;
                if (c.app.empty()) { ImGui::BeginChild("npcph", ImVec2(th, th), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar); ImGui::TextDisabled(T("no preview")); ImGui::EndChild(); }
                else if (ImTextureID tex = overlay::Thumb(core::ThumbFile(c.app))) ImGui::Image(tex, ImVec2(th, th));
                else {
                    if (thumbgen::Ready()) thumbgen::Request(c.app);
                    ImGui::BeginChild("npcph", ImVec2(th, th), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
                    ImGui::TextDisabled(T(!thumbgen::Ready() ? "no preview" : thumbgen::Pending(c.app) ? "rendering..." : thumbgen::Processed(c.app) ? "no preview" : "queued"));
                    ImGui::EndChild();
                }
                ImGui::SameLine();
            }
            ImGui::BeginGroup();
            ImGui::Text("%s", c.name.empty() ? c.internal.c_str() : c.name.c_str()); ImGui::SameLine(); ImGui::TextDisabled("  %s   ID %u", c.internal.c_str(), c.key);
            ImGui::SetNextItemWidth(compact ? 120 * ui : 150 * ui); ImGui::DragFloat(T("distance"), &g_npcDist, 1.0f, 1.0f, kNpcMaxDist, "%.0f m"); g_npcDist = std::clamp(g_npcDist, 1.0f, kNpcMaxDist); SameLineOrWrap(compact, 90 * ui);
            ImGui::SetNextItemWidth(110 * ui); ImGui::InputInt(T("count"), &g_npcCount); g_npcCount = std::clamp(g_npcCount, 1, kNpcMaxCount);
            SameLineOrWrap(compact, 250 * ui);
            ImGui::TextDisabled("%s", T("presets")); ImGui::SameLine(0, 3);
            static const int countPresets[] = { 1, 5, 10, 25, 50, 100, 250, 500 };
            for (int pi = 0; pi < (int)(sizeof(countPresets) / sizeof(countPresets[0])); ++pi) {
                if (pi) ImGui::SameLine(0, 2);
                char pl[16]; snprintf(pl, sizeof pl, "%d##np%d", countPresets[pi], countPresets[pi]);
                if (ImGui::SmallButton(pl)) g_npcCount = countPresets[pi];
            }
            SameLineOrWrap(compact, 130 * ui);
            ImGui::TextDisabled("%s", T("formation")); ImGui::SameLine(); ImGui::SetNextItemWidth(120 * ui); ComboT("##npcformation", &g_npcFormation, kNpcFormations, 3);
            SameLineOrWrap(compact, 130 * ui);
            ImGui::SetNextItemWidth(120 * ui);
            if (g_npcFormation == 2) {
                ImGui::DragFloat(T("radius"), &g_npcRadius, 0.25f, 0.5f, kNpcMaxExtent, "%.1f m");
                g_npcRadius = std::clamp(g_npcRadius, 0.5f, kNpcMaxExtent);
            } else {
                ImGui::DragFloat(T("spacing"), &g_npcSpacing, 0.1f, 0.25f, 50.0f, "%.1f m");
                g_npcSpacing = std::clamp(g_npcSpacing, 0.25f, 50.0f);
            }
            SameLineOrWrap(compact, 230 * ui);
            if (g_npcFormation == 0) {
                ImGui::TextDisabled(T("footprint: %.1f m"), (g_npcCount - 1) * g_npcSpacing);
            } else if (g_npcFormation == 1) {
                const int cols = std::max(1, (int)ceilf(sqrtf((float)g_npcCount))), rows = std::max(1, (g_npcCount + cols - 1) / cols);
                ImGui::TextDisabled(T("grid: %d x %d, %.1f x %.1f m"), cols, rows, (cols - 1) * g_npcSpacing, (rows - 1) * g_npcSpacing);
            } else {
                ImGui::TextDisabled(T("diameter: %.1f m"), g_npcRadius * 2.0f);
            }
            SameLineOrWrap(compact, 160 * ui);
            ImGui::BeginDisabled(!core::NpcAiControlAvailable());
            bool spawnAi = g_npcSpawnAi;
            if (ImGui::Checkbox(T("AI enabled on spawn"), &spawnAi)) { g_npcSpawnAi = spawnAi; g_npcSpawnBehavior = spawnAi ? 0 : 1; }
            ImGui::SameLine(); ImGui::SetNextItemWidth(compact ? 170 * ui : 210 * ui);
            int behavior = g_npcSpawnBehavior;
            if (ComboT("##npcspawnbehavior", &behavior, kNpcBehaviors, 2)) { g_npcSpawnBehavior = behavior; g_npcSpawnAi = behavior == 0; }
            ImGui::EndDisabled();
            if (!core::NpcAiControlAvailable()) {
                g_npcSpawnAi = true; g_npcSpawnBehavior = 0;
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", T("The native AI-control request was not resolved in this game build; NPCs will spawn with normal AI."));
            }
            if (g_npcCount > 100) ImGui::TextDisabled("%s", T("large batches are queued over multiple server ticks"));
            ImGui::BeginDisabled(!havePos || st != 2);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.36f, 0.22f, 1.0f)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.45f, 0.28f, 1.0f));
            char spawnLabel[96]; snprintf(spawnLabel, sizeof spawnLabel, "%s  x%d", T(ICON_LOCATION_CROSSHAIRS "   SPAWN   "), g_npcCount);
            if (ImGui::Button(spawnLabel, ImVec2(175 * ui, 0))) {
                const Vec3 center{ g_lastPlayer.x + g_fx * g_npcDist, g_lastPlayer.y, g_lastPlayer.z + g_fz * g_npcDist };
                SpawnNpcFormationGrounded(c.key, center, g_npcCount, g_npcFormation, g_npcSpacing, g_npcRadius, g_fx, g_fz, g_npcSpawnAi, g_npcSpawnBehavior);
                Note(T("spawn %s"), c.name.empty() ? c.internal.c_str() : c.name.c_str());
            }
            ImGui::PopStyleColor(2); ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(T("spawns the character <distance> in front of you; it appears after a few seconds. Hostile ones attack, wild animals may flee."));
            if (!c.app.empty()) ImGui::TextDisabled("%s", c.app.c_str());
            ImGui::EndGroup();
        } else ImGui::TextDisabled(T("select a character, then SPAWN. Double-click on a row or card spawns it right away."));
        ImGui::EndChild();
    }

    static void DrawEnvironment(bool compact = false) {
        const float ui = ImGui::GetFontSize() / 17.0f;
        ImGui::SeparatorText(T("Time"));
        if (!core::TimeControlAvailable()) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), T("Time controls are not available in this game build (see the log)."));
        } else {
            float current = 0.0f;
            if (core::TimeHour(&current)) {
                int total = (int)floorf(current * 60.0f + 0.5f) % (24 * 60);
                ImGui::TextDisabled(T("Current visual time: %02d:%02d"), total / 60, total % 60);
            } else {
                ImGui::TextDisabled(T("Current visual time: waiting for the world..."));
            }

            float target = core::TimeTargetHour();
            ImGui::SetNextItemWidth(compact ? -1.0f : 420.0f * ui);
            if (SliderFloatEdit(T("time of day"), &target, 0.0f, 23.9833f, "%.2f h"))
                core::SetTimeHour(target);

            if (ImGui::SmallButton(T("Dawn 06:00"))) core::SetTimeHour(6.0f);
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Noon 12:00"))) core::SetTimeHour(12.0f);
            if (!compact) ImGui::SameLine();
            if (ImGui::SmallButton(T("Sunset 18:00"))) core::SetTimeHour(18.0f);
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Midnight 00:00"))) core::SetTimeHour(0.0f);

            bool frozen = core::TimeFrozen();
            if (ImGui::Checkbox(T("freeze time (lighting only)"), &frozen)) core::SetTimeFrozen(frozen);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Stops the visual day and night lighting progression. Gameplay, NPCs, physics and combat keep running."));
            SameLineOrWrap(compact, 105.0f * ui);
            if (ImGui::SmallButton(T("use native time"))) core::ResetTimeControl();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Restore the game's normal visual time progression."));
        }

        ImGui::Spacing();
        ImGui::SeparatorText(T("Weather"));
        if (!core::WeatherControlAvailable()) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), T("Weather controls are not available in this game build (see the log)."));
            return;
        }

        bool clear = core::WeatherClearSky();
        if (ImGui::Checkbox(T("clear sky"), &clear)) core::SetWeatherClearSky(clear);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Suppresses rain, snow, clouds and the main weather fog while enabled."));

        float rain = 0.0f; bool rainOn = core::WeatherRainOverride(&rain);
        float snow = 0.0f; bool snowOn = core::WeatherSnowOverride(&snow);
        float cloud = 1.0f; bool cloudOn = core::WeatherCloudOverride(&cloud);
        float wind = 1.0f; bool windOn = core::WeatherWindOverride(&wind);

        // a control whose fields were not derived for this build stays disabled; the log names the missing piece
        const auto unavailableTip = [](bool available) {
            if (!available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", T("This control is not available in this game build (see the log)."));
        };
        ImGui::BeginDisabled(clear);
        const bool rainAvail = core::WeatherRainAvailable();
        ImGui::BeginDisabled(!rainAvail);
        if (ImGui::Checkbox(T("override rain"), &rainOn)) core::SetWeatherRainOverride(rainOn, rain);
        unavailableTip(rainAvail);
        if (rainOn) { ImGui::SetNextItemWidth(compact ? -1.0f : 420.0f * ui); if (SliderFloatEdit(T("rain intensity"), &rain, 0.0f, 1.0f, "%.2f")) core::SetWeatherRainOverride(true, rain); }
        ImGui::EndDisabled();
        const bool snowEffects = core::WeatherSnowEffectsAvailable();
        ImGui::BeginDisabled(!snowEffects);
        if (ImGui::Checkbox(T("override snow"), &snowOn)) core::SetWeatherSnowOverride(snowOn, snow);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !snowEffects)
            ImGui::SetTooltip("%s", T("Snow particle control is not available in this game build."));
        if (snowOn) { ImGui::SetNextItemWidth(compact ? -1.0f : 420.0f * ui); if (SliderFloatEdit(T("snow intensity"), &snow, 0.0f, 1.0f, "%.2f")) core::SetWeatherSnowOverride(true, snow); }
        ImGui::EndDisabled();
        const bool cloudAvail = core::WeatherCloudAvailable();
        ImGui::BeginDisabled(!cloudAvail);
        if (ImGui::Checkbox(T("override clouds"), &cloudOn)) core::SetWeatherCloudOverride(cloudOn, cloud);
        unavailableTip(cloudAvail);
        if (cloudOn) { ImGui::SetNextItemWidth(compact ? -1.0f : 420.0f * ui); if (SliderFloatEdit(T("cloud amount"), &cloud, 0.0f, 3.0f, "%.2f")) core::SetWeatherCloudOverride(true, cloud); }
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        const bool windAvail = core::WeatherWindAvailable();
        ImGui::BeginDisabled(!windAvail);
        if (ImGui::Checkbox(T("override wind"), &windOn)) core::SetWeatherWindOverride(windOn, wind);
        unavailableTip(windAvail);
        if (windOn) { ImGui::SetNextItemWidth(compact ? -1.0f : 420.0f * ui); if (SliderFloatEdit(T("wind multiplier"), &wind, 0.0f, 3.0f, "x%.2f")) core::SetWeatherWindOverride(true, wind); }
        ImGui::EndDisabled();

        if (ImGui::Button(T("use native weather"))) core::ResetWeatherControl();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Disable every World Builder weather override and return control to the game."));
        if (!compact) ImGui::SameLine();
        ImGui::TextDisabled(T("Weather values are applied to the game's composed environment each update."));
    }

    static void DrawBrowser(const PosInfo& p, bool havePos) {
        const auto& idx = core::PrefabIndex();
        ImGui::BeginChild("cats", ImVec2(g_catW, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
        g_catW = ImGui::GetWindowSize().x;
        ImGui::TextDisabled(T(ICON_FOLDER_TREE "  CATEGORIES")); if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("drag the right edge of this pane to resize it"));
        DrawCatNode(0);
        ImGui::Separator();
        LoadColls();
        ImGui::TextDisabled(T(ICON_STAR "  COLLECTIONS")); if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("your own named prefab lists; add the selected prefab from the details panel"));
        for (int i = 0; i < (int)g_colls.size(); i++) {
            ImGui::PushID(i);
            char lbl[120]; snprintf(lbl, sizeof lbl, "%s  (%d)", g_colls[i].name.c_str(), (int)g_colls[i].paths.size());
            if (ImGui::Selectable(lbl, g_selColl == i)) { g_selColl = g_selColl == i ? -1 : i; }
            if (ImGui::BeginPopupContextItem("collctx")) { if (ImGui::MenuItem(T("delete collection"))) { g_colls.erase(g_colls.begin() + i); SaveColls(); if (g_selColl == i) g_selColl = -1; ImGui::EndPopup(); ImGui::PopID(); break; } ImGui::EndPopup(); }
            ImGui::PopID();
        }
        ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - 60));
        bool enter = InputTextI18n("##newcoll", T("new collection"), g_newColl, sizeof g_newColl, ImGuiInputTextFlags_EnterReturnsTrue); ImGui::SameLine();
        if ((ImGui::SmallButton(T("add")) || enter) && g_newColl[0]) { g_colls.push_back({ g_newColl, {} }); SaveColls(); g_newColl[0] = 0; }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("right", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const float ui = ImGui::GetFontSize() / 17.0f;
        {   // view switch: list or tiles
            const ImVec4 on = ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive), off = ImGui::GetStyleColorVec4(ImGuiCol_Button);
            ImGui::PushStyleColor(ImGuiCol_Button, g_cardView ? off : on); if (ImGui::Button(T(ICON_LIST " list"))) g_cardView = false; ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("list view: name, folder, tags"));
            ImGui::SameLine(0, 2); ImGui::PushStyleColor(ImGuiCol_Button, g_cardView ? on : off); if (ImGui::Button(T(ICON_COPY " cards"))) g_cardView = true; ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("tile view with the preview images (same search and filters)"));
            if (g_cardView) { ImGui::SameLine(); ImGui::SetNextItemWidth(100 * ui); SliderFloatEdit("##cardsize", &g_cardSize, 64.0f, 200.0f, "%.0f px"); if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("tile size")); }
            ImGui::SameLine();
        }
        ImGui::SetNextItemWidth(std::max(120.0f * ui, ImGui::GetContentRegionAvail().x - 400.0f * ui));
        InputTextI18n("##filter", T("search  (words in any order, e.g. lamp torch)"), g_filter, sizeof g_filter);
        ImGui::SameLine(); ImGui::Checkbox(T(ICON_STAR " favorites"), &g_favOnly);
        ImGui::SameLine(); ImGui::Checkbox(T("meshes only"), &g_meshOnly); if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("hides prefabs without static meshes (skinned characters, logic-only prefabs): they spawn nothing visible"));
        ImGui::SameLine(); if (ImGui::SmallButton(T(ICON_XMARK " clear"))) { g_filter[0] = 0; g_tagFilter.clear(); g_favOnly = false; g_selCat = 0; }
        const auto& tags = core::TagCounts();
        int shown = 0;
        for (auto& t : tags) {
            if (shown++ >= 14) break;
            bool on = g_tagFilter.count(t.first) > 0;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
            char lbl[80]; snprintf(lbl, sizeof lbl, "%s##tag", t.first.c_str());
            if (ImGui::SmallButton(lbl)) { if (on) g_tagFilter.erase(t.first); else g_tagFilter.insert(t.first); }
            if (on) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("%d prefabs with %s"), t.second, t.first.c_str());
            ImGui::SameLine();
        }
        ImGui::NewLine();
        RefreshMatches();
        ImGui::TextDisabled(T("%d results%s"), (int)g_matches.size(), g_matches.size() >= 5000 ? T(" (first 5000)") : "");
        if (idx.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 120, 100, 255));
            ImGui::TextWrapped(T("Prefab list not found: the browser and the search stay empty."));
            ImGui::PopStyleColor();
        } else if (g_matches.empty()) {
            ImGui::TextDisabled(T("nothing matches: use fewer words, another category, or turn off favorites, meshes only, and tag filters. Clear resets all."));
        }
        const float detailsH = core::g_showSelectionDetails ? ((g_cardView ? 196.0f : 206.0f) + (g_showSpawnOpts ? 40.0f : 0.0f) + (g_showMass ? 82.0f : 0.0f)) * ui : 0.0f;
        float listH = ImGui::GetContentRegionAvail().y - detailsH - ImGui::GetStyle().ItemSpacing.y;
        if (listH < 80 * ui) listH = 80 * ui;
        if (g_cardView) DrawCards(p, havePos, listH, ui);
        else if (ImGui::BeginTable("list", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable, ImVec2(0, listH))) {
            ImGui::TableSetupColumn("*", ImGuiTableColumnFlags_WidthFixed, 22);
            ImGui::TableSetupColumn(T("name"), ImGuiTableColumnFlags_WidthStretch, 3);
            ImGui::TableSetupColumn(T("prefab"), ImGuiTableColumnFlags_WidthStretch, 3);   // file-derived name next to the in-game one
            ImGui::TableSetupColumn(T("folder"), ImGuiTableColumnFlags_WidthStretch, 2);
            ImGui::TableSetupColumn(T("tags"), ImGuiTableColumnFlags_WidthStretch, 2);
            ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
            ImGuiListClipper clip; clip.Begin((int)g_rows.size());
            while (clip.Step()) for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
                const Row& row = g_rows[r]; int i = row.prefab; const auto& pi = idx[i];
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                ImGui::PushID(r);
                if (row.head == 1) {   // group header: name = base, click expands
                    ImGui::TableSetColumnIndex(1);
                    const bool open = g_openVar.count(row.base) > 0;
                    std::string shown = row.base.substr(row.base.find('/') + 1); shown = shown.substr(0, shown.rfind('/'));
                    char lbl[200]; snprintf(lbl, sizeof lbl, "%s  %s...  (%d variants)", open ? "v" : ">", shown.c_str(), row.count);
                    if (ImGui::Selectable(lbl, false, ImGuiSelectableFlags_SpanAllColumns)) { if (open) g_openVar.erase(row.base); else g_openVar.insert(row.base); g_rowsDirty = true; }
                    ImGui::TableSetColumnIndex(3); ImGui::TextDisabled("%s", core::Categories()[pi.cat].name.c_str());
                    ImGui::PopID(); continue;
                }
                bool fav = core::IsFavorite(i);
                if (fav) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.25f, 1)); else ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.45f, 0.45f, 1));
                if (ImGui::SmallButton(ICON_STAR)) core::ToggleFavorite(i);
                ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(1);
                if (row.head == 2) ImGui::Indent(18.0f);
                if (ImGui::Selectable(ShownName(pi).c_str(), g_selPrefab == i, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    g_selPrefab = i;
                    if (ImGui::IsMouseDoubleClicked(0) && havePos) StartPlaceNew(p, havePos);
                }
                ArmBrowserDrag(i);
                PrefabContextMenu(i, pi, "rowctx");
                if (row.head == 2) ImGui::Unindent(18.0f);
                ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("%s", pi.name.c_str());
                ImGui::TableSetColumnIndex(3); ImGui::TextDisabled("%s", core::Categories()[pi.cat].name.c_str());
                ImGui::TableSetColumnIndex(4); ImGui::TextDisabled("%s", pi.tags.c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (core::g_showSelectionDetails) ImGui::BeginChild("details", ImVec2(0, detailsH), ImGuiChildFlags_Borders);   // optional selected-item information panel
        if (core::g_showSelectionDetails && g_selPrefab >= 0 && g_selPrefab < (int)idx.size()) {
            const auto& pi = idx[g_selPrefab];
            if (!g_cardView) {   // the card already shows the image
                const float th = 96.0f * ui;
                if (ImTextureID tex = overlay::Thumb(core::ThumbFile(pi.path))) { ImGui::Image(tex, ImVec2(th, th)); ImGui::SameLine(); }
                else if (thumbgen::Ready()) {
                    thumbgen::Request(pi.path);
                    ImGui::BeginChild("thumbph", ImVec2(th, th), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
                    ImGui::TextDisabled(T(thumbgen::Pending(pi.path) ? "rendering..." : thumbgen::Processed(pi.path) ? "no preview" : "queued"));
                    ImGui::EndChild(); ImGui::SameLine();
                }
            }
            ImGui::BeginGroup();
            ImGui::Text("%s", ShownName(pi).c_str()); ImGui::SameLine();
            if (ImGui::SmallButton(T(core::IsFavorite(g_selPrefab) ? ICON_STAR " unfavorite" : ICON_STAR " favorite"))) core::ToggleFavorite(g_selPrefab);
            ImGui::SameLine(); ImGui::SetNextItemWidth(160);
            if (ImGui::BeginCombo("##addcoll", T("add to collection"), ImGuiComboFlags_NoArrowButton)) {
                for (int ci = 0; ci < (int)g_colls.size(); ci++) { bool in = InColl(g_colls[ci], pi.path); if (ImGui::Selectable((std::string(T(in ? "remove from " : "add to ")) + g_colls[ci].name).c_str())) { auto& v = g_colls[ci].paths; if (in) v.erase(std::remove(v.begin(), v.end(), pi.path), v.end()); else v.push_back(pi.path); SaveColls(); g_lastKey.clear(); } }
                if (g_colls.empty()) ImGui::TextDisabled(T("create a collection in the left pane first"));
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("%s", pi.path.c_str());
            if (pi.sx > 0 || pi.sy > 0 || pi.sz > 0) ImGui::TextDisabled(T(ICON_RULER_COMBINED "  %.1f x %.1f x %.1f m    meshes %d    %s"), pi.sx, pi.sy, pi.sz, pi.meshes, pi.tags.c_str());
            else ImGui::TextDisabled(T("meshes %d    %s"), pi.meshes, pi.tags.c_str());
            ImGui::BeginDisabled(!havePos || !core::GameThreadReady());
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.36f, 0.22f, 1.0f)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.45f, 0.28f, 1.0f));
            if (ImGui::Button(T(ICON_LOCATION_CROSSHAIRS "   PLACE   "), ImVec2(150 * ui, 0))) StartPlaceNew(p, havePos);
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("puts the object in front of you and lets you move it with the mouse gizmo"));
            ImGui::SameLine(); if (ImGui::SmallButton(T("spawn only"))) SpawnSelected(p);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("drops the object in front of you without the placement mode (offset, yaw and scale from the spawn options)"));
            ImGui::EndDisabled();
            ImGui::EndGroup();
        } else if (core::g_showSelectionDetails) ImGui::TextDisabled(T("select a prefab, then PLACE. Double-click on a row or card places it right away."));
        if (core::g_showSelectionDetails && thumbgen::Ready()) { ImGui::SameLine(); ImGui::TextDisabled(T("   previews %d of %d"), thumbgen::Done(), thumbgen::Total());
            int pd = 0, pt = 0; if (thumbgen::PassProgress(&pd, &pt) && pt > 0) { ImGui::SameLine(); ImGui::TextDisabled(T("   updating %d of %d"), pd, pt); } }
        else if (core::g_showSelectionDetails && thumbgen::Error()[0]) { ImGui::SameLine(); ImGui::TextDisabled(T("   previews off: %s"), thumbgen::Error()); }
        g_showSpawnOpts = core::g_showSelectionDetails && ImGui::CollapsingHeader(TStable("Spawn options: offset, yaw, scale, direction"));
        if (core::g_showSelectionDetails && g_showSpawnOpts) {
            ImGui::SetNextItemWidth(260); DragFloat3Edit(T("offset forward, up, side"), g_off, 0.1f, -50, 50, "%.1f"); ImGui::SameLine();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("extra distance in front of you, height, and sideways offset; the object's footprint is already accounted for"));
            ImGui::SetNextItemWidth(150); SliderFloatEdit(T("yaw"), &g_spawnYaw, -180, 180, "%.0f"); ImGui::SameLine();
            ImGui::SetNextItemWidth(130); SliderFloatEdit(T("scale"), &g_spawnScale, 0.1f, 20.0f, "%.2f"); ImGui::SameLine();
            { Vec3 cf; bool haveCam = core::CameraPose(&cf, nullptr); ImGui::BeginDisabled(!haveCam); ImGui::Checkbox(T("front = camera view"), &g_useCamera); ImGui::EndDisabled();
              if (ImGui::IsItemHovered()) ImGui::SetTooltip(T(haveCam ? "where 'in front of you' points: the camera's view direction (on) or the direction you last walked (off)" : "camera not found yet; using the walking direction")); }
        }
        g_showMass = core::g_showSelectionDetails && ImGui::CollapsingHeader(TStable("Line and circle: many copies of the selected prefab at once"));
        if (core::g_showSelectionDetails && g_showMass) {
            ImGui::BeginDisabled(g_selPrefab < 0 || !havePos || !core::GameThreadReady());
            ImGui::TextUnformatted(T("count")); ImGui::SameLine(); ImGui::SetNextItemWidth(120 * ui); ImGui::InputInt("##count", &g_arrCount); if (g_arrCount < 1) g_arrCount = 1; if (g_arrCount > 200) g_arrCount = 200; ImGui::SameLine();
            ImGui::TextUnformatted(T("  spacing")); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * ui); DragFloatEdit("##spacing", &g_arrSpacing, 0.1f, 0.2f, 50, "%.1f m"); ImGui::SameLine();
            ImGui::TextUnformatted(T("  radius")); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * ui); DragFloatEdit("##radius", &g_arrRadius, 0.1f, 0.5f, 100, "%.1f m");
            static const char* kLineModes[] = { "yaw as set", "along the line", "across the line" };
            static const char* kCircleModes[] = { "yaw as set", "X to center", "X outward" };
            if (ImGui::Button(T(ICON_LIST " Line"))) SpawnArray(false, havePos);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("spawns <count> copies in a row across your line of sight, <spacing> apart, grouped, then hands them to the placement mode"));
            ImGui::SameLine(); ImGui::SetNextItemWidth(140 * ui); ComboT("##linemode", &g_lineYawMode, kLineModes, 3);
            ImGui::SameLine(); ImGui::TextDisabled(ICON_CIRCLE_INFO);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("How each copy is turned. The line runs from left to right in front of you.\n"
                "yaw as set:       every copy keeps the yaw from the spawn options, no matter where it stands.\n"
                "along the line:   the object's X axis points along the line (fences, walls, railings end to end).\n"
                "across the line:  the object's X axis is turned 90 degrees to the line, all copies face the same way\n"
                "                  (benches, market stalls, tents side by side)."));
            ImGui::SameLine(); ImGui::TextUnformatted("   "); ImGui::SameLine();
            if (ImGui::Button(T(ICON_CLOCK_ROTATE_LEFT " Circle"))) SpawnArray(true, havePos);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("spawns <count> copies on a circle of <radius> in front of you, grouped, then hands them to the placement mode"));
            ImGui::SameLine(); ImGui::SetNextItemWidth(140 * ui); ComboT("##circlemode", &g_circleYawMode, kCircleModes, 3);
            ImGui::SameLine(); ImGui::TextDisabled(ICON_CIRCLE_INFO);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("How each copy on the ring is turned.\n"
                "yaw as set:    every copy keeps the yaw from the spawn options (all parallel, like a grid).\n"
                "X to center:   the object's X axis points at the middle of the ring: chairs around a table, seats around a fire.\n"
                "X outward:     the object's X axis points away from the middle: torches, statues or spikes facing out."));
            ImGui::EndDisabled();
        }
        if (core::g_showSelectionDetails && !g_recent.empty()) {
            ImGui::TextDisabled(T(ICON_CLOCK_ROTATE_LEFT " recent:"));
            for (size_t k = 0; k < g_recent.size(); k++) {
                ImGui::SameLine(); ImGui::PushID((int)k);
                if (ImGui::SmallButton(idx[g_recent[k]].name.c_str())) g_selPrefab = g_recent[k];
                ImGui::PopID();
            }
        }
        if (core::g_showSelectionDetails) ImGui::EndChild();
        ImGui::EndChild();
    }

    static bool g_sceneCards = false; static std::set<int> g_closedGroups;
    static ImU32 GroupColor(int gid, int alpha) { const float h = fmodf(gid * 0.61803f, 1.0f); ImVec4 c = ImColor::HSV(h, 0.55f, 0.85f); return IM_COL32((int)(c.x * 255), (int)(c.y * 255), (int)(c.z * 255), alpha); }
    static void DrawSelectionTransformMenu() {
        if (ImGui::MenuItem(T("Properties"))) OpenSelectionProperties();
        if (ImGui::BeginMenu(T("Move"))) {
            ImGui::SetNextItemWidth(110); ImGui::DragFloat(T("amount##move"), &g_moveStep, 0.05f, 0.01f, 100.0f, "%.2f m");
            g_moveStep = std::clamp(g_moveStep, 0.01f, 100.0f);
            if (ImGui::MenuItem(T("+X"))) { if (g_place.active) DropCarried(); MoveSceneSelection({ g_moveStep, 0, 0 }); }
            if (ImGui::MenuItem(T("-X"))) { if (g_place.active) DropCarried(); MoveSceneSelection({ -g_moveStep, 0, 0 }); }
            if (ImGui::MenuItem(T("+Y"))) { if (g_place.active) DropCarried(); MoveSceneSelection({ 0, g_moveStep, 0 }); }
            if (ImGui::MenuItem(T("-Y"))) { if (g_place.active) DropCarried(); MoveSceneSelection({ 0, -g_moveStep, 0 }); }
            if (ImGui::MenuItem(T("+Z"))) { if (g_place.active) DropCarried(); MoveSceneSelection({ 0, 0, g_moveStep }); }
            if (ImGui::MenuItem(T("-Z"))) { if (g_place.active) DropCarried(); MoveSceneSelection({ 0, 0, -g_moveStep }); }
            ImGui::EndMenu();
        }
        ImGui::BeginDisabled(g_sel.empty());
        if (ImGui::BeginMenu(T("Scale"))) {
            ImGui::SetNextItemWidth(110); ImGui::DragFloat(T("enlarge##scaleup"), &g_scaleUpPct, 1.0f, 1.0f, 500.0f, "+%.0f%%");
            ImGui::SetNextItemWidth(110); ImGui::DragFloat(T("shrink##scaledown"), &g_scaleDownPct, 1.0f, 1.0f, 95.0f, "-%.0f%%");
            g_scaleUpPct = std::clamp(g_scaleUpPct, 1.0f, 500.0f); g_scaleDownPct = std::clamp(g_scaleDownPct, 1.0f, 95.0f);
            if (ImGui::MenuItem(T("Enlarge"))) { if (g_place.active) DropCarried(); ScaleSel(1.0f + g_scaleUpPct * 0.01f); }
            if (ImGui::MenuItem(T("Shrink"))) { if (g_place.active) DropCarried(); ScaleSel(1.0f - g_scaleDownPct * 0.01f); }
            ImGui::EndMenu();
        }
        ImGui::EndDisabled();
    }
    // members: the rows of a group header; its menu acts on the whole group even when "select groups" is off
    static void SceneObjectContext(const SpawnedObj& o, const std::vector<SpawnedObj>& list, bool havePos, const char* id, const std::vector<int>* members = nullptr) {
        if (!ImGui::BeginPopupContextItem(id)) return;
        if (members && !members->empty()) {
            bool all = true; for (int m : *members) if (!g_sel.count(list[(size_t)m].uid)) { all = false; break; }
            if (!all) { if (g_place.active) DropCarried(); g_sel.clear(); for (int m : *members) if (!list[(size_t)m].hidden) g_sel.insert(list[(size_t)m].uid); g_primary = o.uid; }
        }
        else if (!g_sel.count(o.uid)) SelectUid(o.uid, false, list);   // grouped members select their group by default
        if (!o.note.empty()) { ImGui::TextDisabled("%s", o.note.c_str()); ImGui::Separator(); }
        ImGui::BeginDisabled(!core::FreeCamAvailable());
        if (ImGui::MenuItem(T("Focus"))) FocusSelection();
        ImGui::EndDisabled();
        DrawSelectionTransformMenu();
        if (ImGui::MenuItem(T("Edit note"))) OpenObjectNoteEdit(o);
        ImGui::BeginDisabled(!g_managedNpcSel.empty());
        if (ImGui::MenuItem(T("Grab"))) StartGrab(SelUids(), false, g_sel.size() == 1 ? ShortName(o.prefab) : "selection");
        if (ImGui::MenuItem(T("To ground"))) SnapSelToGround();
        if (ImGui::MenuItem(T("Duplicate"))) { CopySel(); Paste(havePos); }
        ImGui::EndDisabled();
        if (ImGui::MenuItem(T(o.group > 0 ? "Ungroup" : "Group selection"))) GroupSceneSelection(o.group == 0);
        if (o.group > 0 && ImGui::MenuItem(T("Rename group"))) OpenGroupNameEdit(o.group);
        if (ImGui::BeginMenu(T("Rotate"))) {
            if (ImGui::MenuItem(T("Rotate left"))) RotateSel(-g_rotationStep);
            if (ImGui::MenuItem(T("Rotate right"))) RotateSel(g_rotationStep);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T("Align to primary"))) {
            if (ImGui::MenuItem(T("X"))) AlignSel(0);
            if (ImGui::MenuItem(T("Y"))) AlignSel(1);
            if (ImGui::MenuItem(T("Z"))) AlignSel(2);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(T("Delete"))) DeleteSceneSelection();
        ImGui::EndPopup();
    }
    struct SceneEntityRef {
        bool npc = false; int index = -1; int uid = 0; int group = 0; int proj = 0; DWORD tick = 0; bool hidden = false;
    };
    static int SceneEntityKey(const SceneEntityRef& e) { return e.npc ? -e.uid : e.uid; }
    static bool SceneEntitySelected(const SceneEntityRef& e) { return e.npc ? g_managedNpcSel.count(e.uid) != 0 : g_sel.count(e.uid) != 0; }
    static std::vector<SceneEntityRef> BuildSceneEntities(const std::vector<SpawnedObj>& objects, const std::vector<ManagedNpc>& npcs, int projectFilter) {
        std::vector<SceneEntityRef> out; out.reserve(objects.size() + npcs.size());
        for (int i = 0; i < (int)objects.size(); ++i) {
            const auto& o = objects[i]; if (projectFilter >= 0 && o.proj != projectFilter) continue; if (o.hidden && !g_showDeleted) continue;
            out.push_back({ false, i, o.uid, o.group, o.proj, o.tick, o.hidden });
        }
        for (int i = 0; i < (int)npcs.size(); ++i) {
            const auto& n = npcs[i]; if (projectFilter >= 0 && n.proj != projectFilter) continue; if (n.hidden && !g_showDeleted) continue;
            out.push_back({ true, i, n.uid, n.group, n.proj, n.tick, n.hidden });
        }
        std::stable_sort(out.begin(), out.end(), [](const SceneEntityRef& a, const SceneEntityRef& b) {
            if (a.tick != b.tick) return a.tick < b.tick;
            if (a.npc != b.npc) return a.npc < b.npc;
            return a.uid < b.uid;
        });
        return out;
    }
    static std::string SceneEntityName(const SceneEntityRef& e, const std::vector<SpawnedObj>& objects, const std::vector<ManagedNpc>& npcs, const std::vector<thumbgen::CharInfo>* chars) {
        if (e.npc) return ManagedNpcName(npcs[e.index], chars);
        return ShortName(objects[e.index].prefab);
    }
    static Vec3 SceneEntityPos(const SceneEntityRef& e, const std::vector<SpawnedObj>& objects, const std::vector<ManagedNpc>& npcs) {
        return e.npc ? npcs[e.index].pos : objects[e.index].pos;
    }
    static void SelectSceneEntity(const SceneEntityRef& e, bool add, bool range, const std::vector<SceneEntityRef>& order,
                                  const std::vector<SpawnedObj>& objects, const std::vector<ManagedNpc>& npcs) {
        if (range && g_sceneLastEntity) {
            int a = -1, b = -1; const int key = SceneEntityKey(e);
            for (int i = 0; i < (int)order.size(); ++i) { const int k = SceneEntityKey(order[i]); if (k == g_sceneLastEntity) a = i; if (k == key) b = i; }
            if (a >= 0 && b >= 0) {
                if (a > b) std::swap(a, b); if (!add) ClearSceneSelection();
                for (int i = a; i <= b; ++i) {
                    const auto& x = order[i]; if (x.hidden) continue;
                    if (x.npc) g_managedNpcSel.insert(x.uid); else g_sel.insert(x.uid);
                }
                if (e.npc) { g_managedNpcPrimary = e.uid; g_primary = 0; } else { g_primary = e.uid; g_managedNpcPrimary = 0; }
                g_sceneLastEntity = key; g_editUid = 0; return;
            }
        }
        if (e.npc) SelectManagedNpc(e.uid, add); else SelectUid(e.uid, add, objects);
    }
    static void SceneNpcContext(const ManagedNpc& n, bool havePos, const char* id) {
        if (!ImGui::BeginPopupContextItem(id)) return;
        if (!g_managedNpcSel.count(n.uid)) SelectManagedNpc(n.uid, false);
        if (!n.note.empty()) { ImGui::TextDisabled("%s", n.note.c_str()); ImGui::Separator(); }
        ImGui::BeginDisabled(!core::FreeCamAvailable()); if (ImGui::MenuItem(T("Focus"))) FocusSelection(); ImGui::EndDisabled();
        DrawSelectionTransformMenu();
        ImGui::Separator();
        ImGui::BeginDisabled(!core::NpcAiControlAvailable());
        if (ImGui::MenuItem(T("Enable AI"))) SetSelectedNpcAi(true);
        if (ImGui::MenuItem(T("Disable AI"))) SetSelectedNpcAi(false);
        if (ImGui::BeginMenu(T("Behavior"))) {
            if (ImGui::MenuItem(T("Normal autonomous"), nullptr, n.behavior == 0)) SetSelectedNpcBehavior(0);
            if (ImGui::MenuItem(T("Hold position (AI paused)"), nullptr, n.behavior == 1)) SetSelectedNpcBehavior(1);
            ImGui::EndMenu();
        }
        ImGui::EndDisabled();
        if (!core::NpcAiControlAvailable() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", T("native AI control unavailable"));
        if (ImGui::MenuItem(T("Rename NPC"))) OpenNpcLabelEdit(n);
        if (ImGui::MenuItem(T("Edit note"))) OpenNpcNoteEdit(n);
        if (ImGui::MenuItem(T(n.group > 0 ? "Ungroup" : "Group selection"))) GroupSceneSelection(n.group == 0);
        if (n.group > 0 && ImGui::MenuItem(T("Rename group"))) OpenGroupNameEdit(n.group);
        ImGui::Separator();
        if (ImGui::MenuItem(T("Delete"))) DeleteSceneSelection();
        ImGui::EndPopup();
    }
    static void SelectSceneGroup(int gid, const std::vector<SceneEntityRef>& entities, bool toggle) {
        bool all = true; for (const auto& e : entities) if (e.group == gid && !e.hidden && !SceneEntitySelected(e)) { all = false; break; }
        if (!toggle) ClearSceneSelection();
        for (const auto& e : entities) if (e.group == gid && !e.hidden) {
            if (toggle && all) { if (e.npc) g_managedNpcSel.erase(e.uid); else g_sel.erase(e.uid); }
            else { if (e.npc) g_managedNpcSel.insert(e.uid); else g_sel.insert(e.uid); }
        }
        g_sceneLastEntity = 0; g_editUid = 0;
    }
    // scene as tiles: loose objects first, then every group as a framed block with its own header (click = select all, arrow = collapse)
    // Unified Scene cards: prefab objects and managed NPCs are peers in the same ordered/grouped collection.
    static void DrawSceneCards(const std::vector<SpawnedObj>& objects, const std::vector<ManagedNpc>& npcs,
                               const std::vector<SceneEntityRef>& entities, const std::vector<thumbgen::CharInfo>* chars,
                               const PosInfo& p, bool havePos, float listH, float ui) {
        const auto& idx = core::PrefabIndex();
        std::vector<int> loose, gorder; std::map<int, std::vector<int>> groups;
        for (int i = 0; i < (int)entities.size(); ++i) { const auto& e = entities[i]; if (e.group > 0) { if (!groups.count(e.group)) gorder.push_back(e.group); groups[e.group].push_back(i); } else loose.push_back(i); }
        const float pad = 4.0f * ui, tile = g_cardSize * ui, textH = ImGui::GetTextLineHeight() * 2 + 3;
        const float cw = tile + 2 * pad, ch = tile + 2 * pad + textH;
        ImGui::BeginChild("scenecards", ImVec2(0, listH), ImGuiChildFlags_Borders);
        const float sp = ImGui::GetStyle().ItemSpacing.x; const int cols = std::max(1, (int)((ImGui::GetContentRegionAvail().x + sp) / (cw + sp)));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        auto card = [&](int ei) {
            const SceneEntityRef& e = entities[ei]; const std::string name = SceneEntityName(e, objects, npcs, chars); const Vec3 pos = SceneEntityPos(e, objects, npcs);
            ImGui::PushID(SceneEntityKey(e)); const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1 = { p0.x + cw, p0.y + ch }; const ImVec2 t0 = { p0.x + pad, p0.y + pad }, t1 = { t0.x + tile, t0.y + tile };
            ImGui::InvisibleButton("entitycard", ImVec2(cw, ch)); const bool hov = ImGui::IsItemHovered(), sel = SceneEntitySelected(e);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) { ImGuiIO& io = ImGui::GetIO(); SelectSceneEntity(e, CtrlHeld(io), ShiftHeld(io), entities, objects, npcs); }
            if (e.npc) SceneNpcContext(npcs[e.index], havePos, "entityctx"); else SceneObjectContext(objects[e.index], objects, havePos, "entityctx");
            dl->AddRectFilled(p0, p1, sel ? ImGui::GetColorU32(ImGuiCol_Header) : hov ? ImGui::GetColorU32(ImGuiCol_FrameBgHovered) : ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
            if (sel) dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_HeaderActive), 4.0f, 0, 2.0f);
            if (!e.npc) {
                const auto& o = objects[e.index];
                if (ImTextureID tex = overlay::Thumb(core::ThumbFile(o.prefab))) dl->AddImage(tex, t0, t1, ImVec2(0, 0), ImVec2(1, 1), o.hidden ? IM_COL32(255,255,255,90) : IM_COL32_WHITE);
                else { dl->AddRectFilled(t0, t1, IM_COL32(0,0,0,70), 3.0f); if (thumbgen::Ready() && !thumbgen::Processed(o.prefab)) thumbgen::Request(o.prefab); }
            } else {
                dl->AddRectFilled(t0, t1, IM_COL32(28,31,38,220), 3.0f);
                const char* tag = T("NPC"); const ImVec2 ts = ImGui::CalcTextSize(tag); dl->AddText({ t0.x + (tile-ts.x)*0.5f, t0.y + (tile-ts.y)*0.5f }, IM_COL32(210,220,235,230), tag);
            }
            float dist = 0; if (havePos) { float dx=pos.x-p.world.x,dy=pos.y-p.world.y,dz=pos.z-p.world.z; dist=sqrtf(dx*dx+dy*dy+dz*dz); }
            char info[80]; snprintf(info,sizeof info,"#%d  %s  %.0f m",e.uid,e.npc?T("NPC"):T("Object"),dist); dl->AddText({t0.x+3,t0.y+2},IM_COL32(255,255,255,210),info);
            if (e.group > 0) dl->AddRectFilled({t1.x-7,t0.y},{t1.x,t0.y+7},GroupColor(e.group,255));
            dl->PushClipRect({p0.x+pad,t1.y},{p1.x-pad,p1.y},true); dl->AddText(ImGui::GetFont(),ImGui::GetFontSize(),{p0.x+pad,t1.y+2},ImGui::GetColorU32(e.hidden?ImGuiCol_TextDisabled:ImGuiCol_Text),name.c_str(),nullptr,tile); dl->PopClipRect();
            if (hov) {
                if (e.npc) { const auto& n=npcs[e.index]; ImGui::SetTooltip(T("%s\nNPC #%d\n%.2f  %.2f  %.2f\nAI: %s   Behavior: %s%s%s"), name.c_str(),n.uid,n.pos.x,n.pos.y,n.pos.z,n.aiEnabled?T("On"):T("Off"),n.behavior==1?T("Hold"):T("Normal"),n.note.empty()?"":"\n",n.note.c_str()); }
                else { const auto& o=objects[e.index]; ImGui::SetTooltip(T("%s\nObject #%d\n%.2f  %.2f  %.2f   yaw %.0f   scale %.2f%s%s"),o.prefab.c_str(),o.uid,o.pos.x,o.pos.y,o.pos.z,o.rot.yaw,o.scale,o.note.empty()?"":"\n",o.note.c_str()); }
            }
            ImGui::PopID();
        };
        auto grid=[&](const std::vector<int>& order){ for(size_t k=0;k<order.size();++k){ if(k%cols) ImGui::SameLine(); card(order[k]); } };
        grid(loose);
        for(int gid:gorder){ const auto& mem=groups[gid]; const bool closed=g_closedGroups.count(gid)>0; bool allSel=true, hasNpc=false; for(int ei:mem){allSel&=SceneEntitySelected(entities[ei]);hasNpc|=entities[ei].npc;}
            ImGui::Spacing(); ImGui::PushID(gid); if(ImGui::SmallButton(closed?">":"v")){if(closed)g_closedGroups.erase(gid);else g_closedGroups.insert(gid);} ImGui::SameLine();
            const std::string gn=core::GroupName(gid); char gl[224]; if(!gn.empty()) snprintf(gl,sizeof gl,"%s  (%d)",gn.c_str(),(int)mem.size()); else snprintf(gl,sizeof gl,T("Group %d  (%d entities)"),gid,(int)mem.size());
            ImGui::PushStyleColor(ImGuiCol_Text,GroupColor(gid,255)); if(ImGui::Selectable(gl,allSel)){SelectSceneGroup(gid,entities,CtrlHeld(ImGui::GetIO()));} ImGui::PopStyleColor();
            if(ImGui::BeginPopupContextItem("groupctx")){ if(ImGui::MenuItem(T("Rename group"))) OpenGroupNameEdit(gid); ImGui::BeginDisabled(hasNpc); if(ImGui::MenuItem(T("Grab group"))){SelectSceneGroup(gid,entities,false);StartGrab(SelUids(),false,gn.empty()?"group":gn);} ImGui::EndDisabled(); if(ImGui::MenuItem(T("Ungroup"))){SelectSceneGroup(gid,entities,false);GroupSceneSelection(false);} if(ImGui::MenuItem(T("Delete"))){SelectSceneGroup(gid,entities,false);DeleteSceneSelection();} ImGui::EndPopup(); }
            if(!closed) grid(mem); ImGui::PopID(); ImGui::Spacing(); }
        ImGui::EndChild();
    }
    // Scene tabs: one per project whose objects are in the world, plus "new" for everything placed by hand since the last
    // save. Clicking through them shows only that project's objects, so a loaded project can be edited and written back
    // without touching the others. -1 = everything.
    static void DrawProjectTabs(const std::vector<SpawnedObj>& all, bool compact = false) {
        std::vector<int> ids; int newCount = 0, visible = 0;          // project ids present, in first-appearance order
        for (const auto& o : all) {
            if (o.hidden) continue;
            visible++;
            if (o.proj == 0) { newCount++; continue; }
            if (std::find(ids.begin(), ids.end(), o.proj) == ids.end()) ids.push_back(o.proj);
        }
        const auto npcs = core::ManagedNpcs();
        for (const auto& n : npcs) {
            if (n.hidden) continue;
            visible++;
            if (n.proj == 0) { newCount++; continue; }
            if (std::find(ids.begin(), ids.end(), n.proj) == ids.end()) ids.push_back(n.proj);
        }
        if (ids.empty()) { g_projTab = -1; return; }   // nothing from a project in the scene: the tabs would say nothing
        // the selected project can disappear (deleted, or the scene was emptied); without this the scene would stay blank
        if (g_projTab > 0 && std::find(ids.begin(), ids.end(), g_projTab) == ids.end()) g_projTab = -1;
        if (g_projTab == 0 && !newCount) g_projTab = -1;
        if (!ImGui::BeginTabBar("projtabs")) return;   // no AutoSelectNewTabs: loading a project must not drag the view away
        char lbl[96];
        snprintf(lbl, sizeof lbl, T("all (%d)###ptall"), visible);
        if (ImGui::BeginTabItem(lbl)) { g_projTab = -1; ImGui::EndTabItem(); }
        if (newCount) {
            snprintf(lbl, sizeof lbl, T(ICON_STAR " new (%d)###ptnew"), newCount);
            if (ImGui::BeginTabItem(lbl)) {
                g_projTab = 0; ImGui::EndTabItem();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("placed by hand and not part of a saved project yet - the Project tab saves exactly these under a new name"));
            }
        }
        for (int id : ids) {
            const std::string name = core::ProjectNameOf(id);
            const bool dirty = core::ProjectDirty(id);   // a star means: entities of it changed since the load / save
            snprintf(lbl, sizeof lbl, "%s%s (%d)###pt%d", name.c_str(), dirty ? " *" : "", core::ProjectObjectCount(id), id);
            if (dirty) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.82f, 0.35f, 1));
            const bool open = ImGui::BeginTabItem(lbl);
            if (dirty) ImGui::PopStyleColor();
            if (open) { g_projTab = id; ImGui::EndTabItem(); }
        }
        ImGui::EndTabBar();
        if (g_projTab > 0) {   // acting on the project that is currently shown
            const std::string name = core::ProjectNameOf(g_projTab);
            const int n = core::ProjectObjectCount(g_projTab);
            const bool dirty = core::ProjectDirty(g_projTab);
            if (dirty) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.33f, 0.12f, 1));
            if (ImGui::SmallButton(T(dirty ? ICON_FLOPPY_DISK " Save the changes" : ICON_FLOPPY_DISK " Overwrite this project"))) {
                if (core::SaveProject(name, core::SaveProjectOnly)) Note(T("overwrote %s (%d entities)"), name.c_str(), n);
                else Note(T("save failed"));
            }
            if (dirty) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("writes exactly the %d entities shown here back to %s.cdproj.\nEntities of other projects and new entities are not touched."), n, name.c_str());
            SameLineOrWrap(compact, 110);
            if (ImGui::SmallButton(T("Select all of them"))) {
                SelectAllSceneEntities(all, npcs, g_projTab);
            }
            SameLineOrWrap(compact, ImGui::CalcTextSize(name.c_str()).x); ImGui::TextDisabled("%s", name.c_str());
        }
    }
    static void DrawScene(const PosInfo& p, bool havePos, bool compact = false) {
        auto objects = core::Spawned(); auto npcs = core::ManagedNpcs(); const auto chars = thumbgen::Characters();
        const float ui = ImGui::GetFontSize() / 17.0f;
        DrawProjectTabs(objects, compact);
        const auto entities = BuildSceneEntities(objects, npcs, g_projTab);

        if (compact) g_sceneCards = true;
        if (!compact) {
            const ImVec4 on=ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive),off=ImGui::GetStyleColorVec4(ImGuiCol_Button);
            ImGui::PushStyleColor(ImGuiCol_Button,g_sceneCards?off:on); if(ImGui::Button(T(ICON_LIST " list")))g_sceneCards=false; ImGui::PopStyleColor();
            ImGui::SameLine(0,2); ImGui::PushStyleColor(ImGuiCol_Button,g_sceneCards?on:off); if(ImGui::Button(T(ICON_COPY " cards")))g_sceneCards=true; ImGui::PopStyleColor();
            if(g_sceneCards){ImGui::SameLine();ImGui::SetNextItemWidth(95*ui);SliderFloatEdit("##scardsize",&g_cardSize,64,200,"%.0f px");}
            ImGui::SameLine();
        }
        ImGui::Checkbox(T("click selects group"),&g_selectGroups); ImGui::SameLine(); ImGui::Checkbox(T("show deleted"),&g_showDeleted); ImGui::SameLine();
        ImGui::BeginDisabled(!SceneHasSelection()); if(ImGui::SmallButton(T("Clear selection")))ClearSceneSelection(); ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::BeginDisabled(g_undo.empty()); if(ImGui::SmallButton(T("Undo")))Undo(); ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::BeginDisabled(g_redo.empty()); if(ImGui::SmallButton(T("Redo")))Redo(); ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::TextDisabled(T("%d entities, %d selected"),(int)entities.size(),(int)SceneSelectionCount());

        const float availY=ImGui::GetContentRegionAvail().y; const bool anySel=SceneHasSelection(); const float footer=(compact?(anySel?210.0f:48.0f):190.0f)*ui;
        const float listH=compact?std::max(180.0f*ui,std::max(availY*0.48f,availY-footer)):std::max(120.0f,availY-footer);
        if(g_sceneCards) DrawSceneCards(objects,npcs,entities,chars?chars.get():nullptr,p,havePos,listH,ui);
        else if(ImGui::BeginTable("scene_entities",7,ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY|ImGuiTableFlags_BordersInnerH,ImVec2(-1,listH))){
            ImGui::TableSetupColumn("#",ImGuiTableColumnFlags_WidthFixed,48); ImGui::TableSetupColumn(T("entity")); ImGui::TableSetupColumn(T("type"),ImGuiTableColumnFlags_WidthFixed,70); ImGui::TableSetupColumn(T("grp"),ImGuiTableColumnFlags_WidthFixed,46); ImGui::TableSetupColumn(T("position"),ImGuiTableColumnFlags_WidthFixed,220); ImGui::TableSetupColumn(T("state"),ImGuiTableColumnFlags_WidthFixed,190); ImGui::TableSetupColumn(T("dist"),ImGuiTableColumnFlags_WidthFixed,64); ImGui::TableHeadersRow();
            std::map<int,std::vector<int>> groups; std::vector<int> order; for(int i=0;i<(int)entities.size();++i){const auto&e=entities[i];if(e.group>0){if(!groups.count(e.group))order.push_back(-e.group);groups[e.group].push_back(i);}else order.push_back(i+1);}
            std::vector<std::pair<int,bool>> rows; for(int x:order){if(x>0)rows.push_back({x-1,false});else{rows.push_back({x,false});if(!g_closedGroups.count(-x))for(int ei:groups[-x])rows.push_back({ei,true});}}
            for(const auto&rw:rows){
                if(rw.first<0){const int gid=-rw.first;const auto&mem=groups[gid];bool all=true;for(int ei:mem)all&=SceneEntitySelected(entities[ei]);ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::PushID(gid);if(ImGui::SmallButton(g_closedGroups.count(gid)?">":"v")){if(g_closedGroups.count(gid))g_closedGroups.erase(gid);else g_closedGroups.insert(gid);}ImGui::PopID();ImGui::TableSetColumnIndex(1);const std::string gn=core::GroupName(gid);char gl[240];if(!gn.empty())snprintf(gl,sizeof gl,"%s  (%d)##g%d",gn.c_str(),(int)mem.size(),gid);else snprintf(gl,sizeof gl,T("Group %d  (%d entities)##g%d"),gid,(int)mem.size(),gid);if(ImGui::Selectable(gl,all,ImGuiSelectableFlags_SpanAllColumns))SelectSceneGroup(gid,entities,CtrlHeld(ImGui::GetIO()));if(ImGui::BeginPopupContextItem("groupctx")){if(ImGui::MenuItem(T("Rename group")))OpenGroupNameEdit(gid);if(ImGui::MenuItem(T("Ungroup"))){SelectSceneGroup(gid,entities,false);GroupSceneSelection(false);}if(ImGui::MenuItem(T("Delete"))){SelectSceneGroup(gid,entities,false);DeleteSceneSelection();}ImGui::EndPopup();}ImGui::TableSetColumnIndex(3);ImGui::TextDisabled("%d",gid);continue;}
                const SceneEntityRef&e=entities[rw.first];const Vec3 pos=SceneEntityPos(e,objects,npcs);const std::string name=SceneEntityName(e,objects,npcs,chars?chars.get():nullptr);ImGui::PushID(SceneEntityKey(e));ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);char id[24];snprintf(id,sizeof id,"%c%d",e.npc?'N':'#',e.uid);if(rw.second)ImGui::Indent(12);if(ImGui::Selectable(id,SceneEntitySelected(e),ImGuiSelectableFlags_SpanAllColumns)){ImGuiIO&io=ImGui::GetIO();SelectSceneEntity(e,CtrlHeld(io),ShiftHeld(io),entities,objects,npcs);}if(e.npc)SceneNpcContext(npcs[e.index],havePos,"rowctx");else SceneObjectContext(objects[e.index],objects,havePos,"rowctx");if(rw.second)ImGui::Unindent(12);
                ImGui::TableSetColumnIndex(1);if(e.hidden)ImGui::TextDisabled(T("%s (deleted)"),name.c_str());else ImGui::TextUnformatted(name.c_str());
                const std::string note=e.npc?npcs[e.index].note:objects[e.index].note;if(!note.empty()&&ImGui::IsItemHovered())ImGui::SetTooltip("%s",note.c_str());
                ImGui::TableSetColumnIndex(2);ImGui::TextDisabled("%s",e.npc?T("NPC"):T("Object"));ImGui::TableSetColumnIndex(3);if(e.group)ImGui::TextDisabled("%d",e.group);
                ImGui::TableSetColumnIndex(4);ImGui::Text("%.2f  %.2f  %.2f",pos.x,pos.y,pos.z);ImGui::TableSetColumnIndex(5);
                if(e.npc){const auto&n=npcs[e.index];if(!n.actor||n.spawnPending)ImGui::TextDisabled("%s",T("pending"));else if(n.aiApplied!=n.aiEnabled)ImGui::TextDisabled("%s",T("syncing"));else ImGui::Text("%s   %s",n.aiEnabled?T("AI on"):T("AI off"),n.behavior==1?T("Hold"):T("Normal"));}
                else{const auto&o=objects[e.index];ImGui::TextDisabled(T("yaw %.0f   scale %.2f"),o.rot.yaw,o.scale);}
                ImGui::TableSetColumnIndex(6);if(havePos){float dx=pos.x-p.world.x,dy=pos.y-p.world.y,dz=pos.z-p.world.z;ImGui::Text("%.0f m",sqrtf(dx*dx+dy*dy+dz*dz));}ImGui::PopID();
            }
            ImGui::EndTable();
        }

        for(auto it=g_sel.begin();it!=g_sel.end();){if(!Find(objects,*it))it=g_sel.erase(it);else ++it;} for(auto it=g_managedNpcSel.begin();it!=g_managedNpcSel.end();){if(!FindManagedNpc(npcs,*it))it=g_managedNpcSel.erase(it);else ++it;}
        if(!SceneHasSelection()){ImGui::TextDisabled(T("Select an object or NPC to edit it. Ctrl-click adds, Shift-click selects a range."));return;}
        ImGui::Separator();
        const SpawnedObj* objPrim=(g_sel.size()==1&&g_managedNpcSel.empty())?Find(objects,*g_sel.begin()):nullptr;
        const ManagedNpc* npcPrim=(g_managedNpcSel.size()==1&&g_sel.empty())?FindManagedNpc(npcs,*g_managedNpcSel.begin()):nullptr;
        if(objPrim){
            const auto&o=*objPrim;if(g_editUid!=o.uid){g_edit[0]=o.pos.x;g_edit[1]=o.pos.y;g_edit[2]=o.pos.z;g_editRot=o.rot;g_editScale=o.scale;g_editPos0=o.pos;g_editRot0=o.rot;g_editScale0=o.scale;g_editUid=o.uid;}
            ImGui::TextDisabled("%s",o.prefab.c_str());bool changed=false;ImGui::SetNextItemWidth(300*ui);changed|=DragFloat3Edit(T("position"),g_edit,0.05f,-100000,100000,"%.3f");bool rel1=ImGui::IsItemDeactivatedAfterEdit()||NumericEditEnded(T("position"));ImGui::SameLine();ImGui::SetNextItemWidth(140*ui);changed|=SliderFloatEdit(T("yaw##e"),&g_editRot.yaw,-180,180,"%.1f");bool rel2=ImGui::IsItemDeactivatedAfterEdit()||NumericEditEnded(T("yaw##e"));ImGui::SameLine();ImGui::SetNextItemWidth(120*ui);changed|=SliderFloatEdit(T("scale##e"),&g_editScale,0.05f,20,"%.3f");bool rel3=ImGui::IsItemDeactivatedAfterEdit()||NumericEditEnded(T("scale##e"));
            const DWORD now=GetTickCount();static DWORD liveAt=0;if(changed&&g_live&&now-liveAt>50){core::MoveMany({{o.uid,{g_edit[0],g_edit[1],g_edit[2]},g_editRot,g_editScale}},false);liveAt=now;}
            if(rel1||rel2||rel3){Vec3 p1{g_edit[0],g_edit[1],g_edit[2]};if(p1.x!=g_editPos0.x||p1.y!=g_editPos0.y||p1.z!=g_editPos0.z||g_editRot.yaw!=g_editRot0.yaw||g_editRot.pitch!=g_editRot0.pitch||g_editRot.roll!=g_editRot0.roll||g_editScale!=g_editScale0){core::MoveMany({{o.uid,p1,g_editRot,g_editScale}},true);Act a;a.kind=Act::Move;a.uid=o.uid;a.prefab=o.prefab;a.pos0=g_editPos0;a.rot0=g_editRot0;a.sc0=g_editScale0;a.pos1=p1;a.rot1=g_editRot;a.sc1=g_editScale;Push({a});g_editPos0=p1;g_editRot0=g_editRot;g_editScale0=g_editScale;}}
        } else if(npcPrim){
            const auto&n=*npcPrim;static int posUid=0;static float ep[3]={};static Vec3 base{};if(posUid!=n.uid||(!ImGui::IsAnyItemActive()&&(ep[0]!=n.pos.x||ep[1]!=n.pos.y||ep[2]!=n.pos.z))){posUid=n.uid;ep[0]=base.x=n.pos.x;ep[1]=base.y=n.pos.y;ep[2]=base.z=n.pos.z;}
            ImGui::TextDisabled(T("NPC properties"));ImGui::SameLine();ImGui::Text("%s",ManagedNpcName(n,chars?chars.get():nullptr).c_str());ImGui::SetNextItemWidth(330*ui);ImGui::DragFloat3(T("position##scene_npc"),ep,0.05f,-100000,100000,"%.3f");if(ImGui::IsItemDeactivatedAfterEdit()){Vec3 d{ep[0]-base.x,ep[1]-base.y,ep[2]-base.z};MoveSelectedNpcs(d);base={ep[0],ep[1],ep[2]};}
            ImGui::SameLine();bool ai=n.aiEnabled;ImGui::BeginDisabled(!core::NpcAiControlAvailable());if(ImGui::Checkbox(T("AI enabled"),&ai))SetSelectedNpcAi(ai);ImGui::SameLine();int behavior=n.behavior;ImGui::SetNextItemWidth(180*ui);if(ComboT("##npc_behavior_scene",&behavior,kNpcBehaviors,2))SetSelectedNpcBehavior(behavior);ImGui::EndDisabled();ImGui::SameLine();if(ImGui::SmallButton(T("Rename")))OpenNpcLabelEdit(n);ImGui::SameLine();if(ImGui::SmallButton(T("Edit note")))OpenNpcNoteEdit(n);
        } else ImGui::TextDisabled(T("Mixed selection: %d objects and %d NPCs"),(int)g_sel.size(),(int)g_managedNpcSel.size());

        ImGui::BeginDisabled(!core::FreeCamAvailable());if(ImGui::Button(T("Focus")))FocusSelection();ImGui::EndDisabled();ImGui::SameLine();
        ImGui::BeginDisabled(!g_managedNpcSel.empty());if(ImGui::Button(T(ICON_HAND " Grab")))StartGrab(SelUids(),false,g_sel.size()==1&&objPrim?ShortName(objPrim->prefab):"selection");ImGui::SameLine();if(ImGui::Button(T("To ground")))SnapSelToGround();ImGui::SameLine();if(ImGui::Button(T(ICON_COPY " Duplicate"))){CopySel();Paste(havePos);}ImGui::EndDisabled();ImGui::SameLine();
        if(ImGui::Button(T("Group")))GroupSceneSelection(true);ImGui::SameLine();if(ImGui::Button(T("Ungroup")))GroupSceneSelection(false);
        if(!g_managedNpcSel.empty()){ImGui::SameLine();ImGui::BeginDisabled(!core::NpcAiControlAvailable());if(ImGui::Button(T("AI on")))SetSelectedNpcAi(true);ImGui::SameLine();if(ImGui::Button(T("AI off")))SetSelectedNpcAi(false);ImGui::SameLine();int b=-1;bool first=true,mixed=false;for(int uid:g_managedNpcSel)if(const auto*n=FindManagedNpc(npcs,uid)){if(first){b=n->behavior;first=false;}else if(b!=n->behavior)mixed=true;}ImGui::SetNextItemWidth(180*ui);const char* preview=mixed?T("Mixed"):b==1?T("Hold"):T("Normal");if(ImGui::BeginCombo("##batch_behavior",preview)){if(ImGui::Selectable(T("Normal autonomous"),b==0&&!mixed))SetSelectedNpcBehavior(0);if(ImGui::Selectable(T("Hold position (AI paused)"),b==1&&!mixed))SetSelectedNpcBehavior(1);ImGui::EndCombo();}ImGui::EndDisabled();}
        ImGui::SameLine();ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.55f,0.15f,0.12f,1));if(ImGui::Button(T(ICON_TRASH " Delete")))DeleteSceneSelection();ImGui::PopStyleColor();
        if(!compact){ImGui::Separator();ImGui::TextDisabled(T("Ctrl+Z and Ctrl+Y undo and redo   Ctrl+G group   Ctrl+A select all   Delete removes selection   Ctrl-click and Shift-click multi-select"));}
    }

    static void DrawProject() {
        static char s_name[64] = "mybuild";
        static std::vector<std::string> s_list, s_auto; static DWORD s_listAt = 0;
        if (GetTickCount() - s_listAt > 1200) { s_list = core::ListProjects(); s_auto = core::Autoload(); s_listAt = GetTickCount(); }

        const auto sceneObjects = core::Spawned();
        const int objectCount = (int)std::count_if(sceneObjects.begin(), sceneObjects.end(), [](const SpawnedObj& o) { return !o.hidden; });
        const auto managed = core::ManagedNpcs();
        const int npcCount = (int)std::count_if(managed.begin(), managed.end(), [](const ManagedNpc& n) { return !n.hidden; });
        const int totalCount = objectCount + npcCount;
        const int newCount = core::ProjectObjectCount(0);

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.09f, 0.11f, 0.55f));
        ImGui::BeginChild("project_status", ImVec2(0, 86), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
        bool autosave = core::g_projectAutoSave;
        if (ImGui::Checkbox(T("real-time project auto-save"), &autosave)) { core::g_projectAutoSave = autosave; core::SaveSettings(); }
        ImGui::SameLine();
        if (core::g_projectAutoSave) ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.55f, 1), "%s", T("ON - every committed project edit is saved immediately"));
        else ImGui::TextDisabled("%s", T("OFF - changes stay dirty until you save manually"));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Only entities already belonging to a saved project are auto-saved. New unassigned entities are never attached automatically."));
        ImGui::Text(T("Scene: %d entities"), totalCount); ImGui::SameLine();
        ImGui::TextDisabled(T("%d objects, %d NPCs, %d unassigned"), objectCount, npcCount, newCount);
        if (core::PendingSpawns()) { ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.3f, 1), T("%d pending spawns"), core::PendingSpawns()); }
        ImGui::EndChild(); ImGui::PopStyleColor();

        ImGui::SeparatorText(T("Create or update a project"));
        ImGui::SetNextItemWidth(260); ImGui::InputText(T("project name"), s_name, sizeof s_name);
        ImGui::SameLine();
        ImGui::BeginDisabled(!s_name[0] || totalCount == 0);
        if (ImGui::Button(T(ICON_FLOPPY_DISK " Save whole scene as project"))) {
            if (core::SaveProject(s_name, core::SaveWholeScene)) Note(T("saved %s (%d entities)"), s_name, totalCount); else Note(T("save failed"));
            s_listAt = 0;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!s_name[0] || newCount == 0);
        if (ImGui::Button(T("Save unassigned entities as project"))) {
            if (core::SaveProject(s_name, core::SaveNewOnly)) Note(T("saved %s (%d entities)"), s_name, newCount); else Note(T("save failed"));
            s_listAt = 0;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", T("Saves only entities that do not belong to any project yet. Loaded projects stay separate."));

        if (ImGui::Button(T(ICON_CUBE " New empty scene"))) ImGui::OpenPopup("newproj");
        ImGui::SameLine();
        if (ImGui::Button(T("Import project file"))) {
            char file[MAX_PATH] = { 0 }; OPENFILENAMEA ofn = {}; ofn.lStructSize = sizeof ofn; ofn.lpstrFilter = "World Builder project (*.cdproj)\0*.cdproj\0All files\0*.*\0"; ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameA(&ofn)) { std::string src = file; std::string base = src.substr(src.find_last_of("\\/") + 1); std::string dst = core::ModDir() + "\\projects\\" + base; CreateDirectoryA((core::ModDir() + "\\projects").c_str(), nullptr); if (CopyFileA(src.c_str(), dst.c_str(), FALSE)) Note(T("imported %s"), base.c_str()); else Note(T("import failed")); s_listAt = 0; }
        }
        ImGui::SameLine(); if (ImGui::Button(T("Open project folder"))) ShellExecuteA(nullptr, "open", (core::ModDir() + "\\projects").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (ImGui::BeginPopupModal(TStable("New empty scene###newproj"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped(T("Remove all %d live entities from the World Builder scene?"), totalCount);
            ImGui::TextDisabled(T("Project files on disk are not deleted."));
            if (ImGui::Button(T(ICON_TRASH " Clear scene"))) { core::DeleteAllSpawned(); ClearSceneSelection(); g_undo.clear(); g_redo.clear(); Note(T("scene cleared")); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine(); if (ImGui::Button(T("Cancel"))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        ImGui::SeparatorText(T("Saved projects"));
        if (g_compact) {
            for (const auto& pr : s_list) {
                const int pid = core::ProjectId(pr); const int inScene = core::ProjectObjectCount(pid); const bool dirty = inScene > 0 && core::ProjectDirty(pid);
                ImGui::PushID(pr.c_str()); ImGui::SeparatorText(pr.c_str());
                if (!inScene) ImGui::TextDisabled("%s", T("not loaded"));
                else if (dirty) ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.3f, 1), T("%d entities, modified"), inScene);
                else ImGui::TextDisabled(T("%d entities, saved"), inScene);
                ImGui::BeginDisabled(inScene > 0); if (ImGui::SmallButton(T("Load"))) { core::LoadProject(pr, false); Note(T("loaded %s"), pr.c_str()); } ImGui::EndDisabled(); ImGui::SameLine();
                ImGui::BeginDisabled(inScene == 0); if (ImGui::SmallButton(T("Reload"))) { core::UnloadProject(pid); core::LoadProject(pr, false); ClearSceneSelection(); g_undo.clear(); g_redo.clear(); Note(T("reloaded %s"), pr.c_str()); } ImGui::EndDisabled(); ImGui::SameLine();
                ImGui::BeginDisabled(inScene == 0 || !dirty); if (ImGui::SmallButton(T("Save"))) { if (core::SaveProject(pr, core::SaveProjectOnly)) Note(T("saved %s"), pr.c_str()); else Note(T("save failed")); } ImGui::EndDisabled(); ImGui::SameLine();
                ImGui::BeginDisabled(inScene == 0); if (ImGui::SmallButton(T("Unload"))) { core::UnloadProject(pid); ClearSceneSelection(); Note(T("unloaded %s"), pr.c_str()); } ImGui::EndDisabled();
                ImGui::BeginDisabled(inScene == 0 || newCount == 0); if (ImGui::SmallButton(T("Add unassigned"))) { if (core::SaveProject(pr, core::SaveProjectAndNew)) Note(T("saved %s (%d entities)"), pr.c_str(), inScene + newCount); else Note(T("save failed")); } ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(T("Adds the %d unassigned scene entities to this project and saves it."), newCount);
                bool isAuto = std::find(s_auto.begin(), s_auto.end(), pr) != s_auto.end(); ImGui::SameLine(); if (ImGui::Checkbox(T("Autoload"), &isAuto)) { core::SetAutoload(pr, isAuto); s_auto = core::Autoload(); }
                ImGui::PopID();
            }
        } else if (ImGui::BeginTable("projects_v2", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(T("Project"), ImGuiTableColumnFlags_WidthStretch, 2.2f);
            ImGui::TableSetupColumn(T("Status"), ImGuiTableColumnFlags_WidthStretch, 1.5f);
            ImGui::TableSetupColumn("##load", ImGuiTableColumnFlags_WidthFixed, 72);
            ImGui::TableSetupColumn("##reload", ImGuiTableColumnFlags_WidthFixed, 78);
            ImGui::TableSetupColumn("##save", ImGuiTableColumnFlags_WidthFixed, 72);
            ImGui::TableSetupColumn("##add", ImGuiTableColumnFlags_WidthFixed, 104);
            ImGui::TableSetupColumn("##unload", ImGuiTableColumnFlags_WidthFixed, 78);
            ImGui::TableSetupColumn(T("Autoload"), ImGuiTableColumnFlags_WidthFixed, 92);
            ImGui::TableHeadersRow();
            for (const auto& pr : s_list) {
                const int pid = core::ProjectId(pr); const int inScene = core::ProjectObjectCount(pid); const bool dirty = inScene > 0 && core::ProjectDirty(pid);
                ImGui::PushID(pr.c_str()); ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); if (dirty) ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.35f, 1), "%s *", pr.c_str()); else ImGui::TextUnformatted(pr.c_str());
                ImGui::TableSetColumnIndex(1); if (!inScene) ImGui::TextDisabled("%s", T("not loaded")); else if (dirty) ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.3f, 1), T("%d entities, modified"), inScene); else ImGui::TextDisabled(T("%d entities, saved"), inScene);
                ImGui::TableSetColumnIndex(2); ImGui::BeginDisabled(inScene > 0); if (ImGui::SmallButton(T("Load"))) { core::LoadProject(pr, false); Note(T("loaded %s"), pr.c_str()); } ImGui::EndDisabled();
                ImGui::TableSetColumnIndex(3); ImGui::BeginDisabled(inScene == 0); if (ImGui::SmallButton(T("Reload"))) { core::UnloadProject(pid); core::LoadProject(pr, false); ClearSceneSelection(); g_undo.clear(); g_redo.clear(); Note(T("reloaded %s"), pr.c_str()); } ImGui::EndDisabled();
                ImGui::TableSetColumnIndex(4); ImGui::BeginDisabled(inScene == 0 || !dirty); if (ImGui::SmallButton(T("Save"))) { if (core::SaveProject(pr, core::SaveProjectOnly)) Note(T("saved %s"), pr.c_str()); else Note(T("save failed")); } ImGui::EndDisabled();
                ImGui::TableSetColumnIndex(5); ImGui::BeginDisabled(inScene == 0 || newCount == 0); if (ImGui::SmallButton(T("Add unassigned"))) { if (core::SaveProject(pr, core::SaveProjectAndNew)) Note(T("saved %s (%d entities)"), pr.c_str(), inScene + newCount); else Note(T("save failed")); } ImGui::EndDisabled(); if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(T("Adds the %d unassigned scene entities to this project and saves it."), newCount);
                ImGui::TableSetColumnIndex(6); ImGui::BeginDisabled(inScene == 0); if (ImGui::SmallButton(T("Unload"))) { core::UnloadProject(pid); ClearSceneSelection(); Note(T("unloaded %s"), pr.c_str()); } ImGui::EndDisabled();
                ImGui::TableSetColumnIndex(7); bool isAuto = std::find(s_auto.begin(), s_auto.end(), pr) != s_auto.end(); if (ImGui::Checkbox("##autoload", &isAuto)) { core::SetAutoload(pr, isAuto); s_auto = core::Autoload(); }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (s_list.empty()) ImGui::TextDisabled(T("No saved projects yet."));
        ImGui::TextDisabled("%s", T("Loading a project creates both its objects and its managed NPCs. Unload removes both from the live scene but keeps the project file."));
    }

    // ---- travel: saved points (bin64\cdmodkit\teleports.txt: name|x|y|z) ----
    struct TravelPoint { std::string name; Vec3 pos; };
    static std::vector<TravelPoint> g_tp; static bool g_tpLoaded = false; static char g_tpName[48] = "";
    static std::string TpPath() { return core::ModDir() + "\\teleports.txt"; }
    static void LoadTp() { if (g_tpLoaded) return; g_tpLoaded = true; FILE* f = fopen(TpPath().c_str(), "r"); if (!f) return; char line[512];
        while (fgets(line, sizeof line, f)) { char name[128] = { 0 }; Vec3 v{}; char* bar = strchr(line, '|'); if (!bar) continue; *bar = 0; strncpy_s(name, line, _TRUNCATE); if (sscanf(bar + 1, "%f|%f|%f", &v.x, &v.y, &v.z) == 3) g_tp.push_back({ name, v }); } fclose(f); }
    static void SaveTp() { FILE* f = fopen(TpPath().c_str(), "w"); if (!f) return; for (auto& t : g_tp) fprintf(f, "%s|%.2f|%.2f|%.2f\n", t.name.c_str(), t.pos.x, t.pos.y, t.pos.z); fclose(f); }
    static void DrawTravel(const PosInfo& p, bool havePos) {
        LoadTp();
        ImGui::TextWrapped(T("Teleport writes the player's position directly. That works inside the loaded area (about 400 m); longer trips need the game's fast travel first, otherwise the world around you is not streamed in."));
        ImGui::SetNextItemWidth(220); InputTextI18n("##tpname", T("name for the current spot"), g_tpName, sizeof g_tpName); ImGui::SameLine();
        ImGui::BeginDisabled(!havePos || !g_tpName[0]);
        if (ImGui::Button(T(ICON_LOCATION_DOT " Save current position"))) { g_tp.push_back({ g_tpName, p.world }); SaveTp(); g_tpName[0] = 0; }
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::BeginTable("tp", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn(T("name"), ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn(T("position"), ImGuiTableColumnFlags_WidthFixed, 240); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70);
            for (int i = 0; i < (int)g_tp.size(); i++) {
                ImGui::PushID(i); ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(g_tp[i].name.c_str());
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.1f  %.1f  %.1f", g_tp[i].pos.x, g_tp[i].pos.y, g_tp[i].pos.z);
                ImGui::TableSetColumnIndex(2);
                float dist = havePos ? sqrtf((g_tp[i].pos.x - p.world.x) * (g_tp[i].pos.x - p.world.x) + (g_tp[i].pos.z - p.world.z) * (g_tp[i].pos.z - p.world.z)) : 0;
                ImGui::BeginDisabled(!havePos || dist > 400);
                if (ImGui::SmallButton(T("Go"))) { Vec3 t = g_tp[i].pos; t.y += 0.5f; if (core::SetPlayerPos(t)) Note(T("teleport to %s"), g_tp[i].name.c_str()); else Note(T("teleport failed")); }
                ImGui::EndDisabled();
                if (dist > 400 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(T("%.0f m away: the direct write only works inside the loaded area (about 400 m). Use the game's fast travel to get close first."), dist);
                ImGui::TableSetColumnIndex(3); if (ImGui::SmallButton(T("Delete"))) { g_tp.erase(g_tp.begin() + i); SaveTp(); ImGui::PopID(); break; }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (g_tp.empty()) ImGui::TextDisabled(T("(no saved points yet)"));
        ImGui::Separator();
        ImGui::TextDisabled(T("Console: tp x y z"));
    }

    // screen rectangle of a placed object's (yaw-rotated) bounding box; depth = distance along the view direction
    static bool ObjScreenRect(const CamFrame& cf, const SpawnedObj& o, ImVec2* mn, ImVec2* mx, float* depth) {
        float cx = 0, cy = 0.5f, cz = 0, sx = 1, sy = 1, sz = 1; int pi = IndexOfPrefab(o.prefab);
        if (pi >= 0) { const auto& info = core::PrefabIndex()[pi]; if (info.hasCenter) { cx = info.cx; cy = info.cy; cz = info.cz; } if (info.sx > 0) { sx = info.sx; sy = info.sy; sz = info.sz; } }
        *mn = { 1e9f, 1e9f }; *mx = { -1e9f, -1e9f }; float dsum = 0; int n = 0;
        for (int k = 0; k < 8; k++) {
            const Vec3 w = LocalToWorld(o, cx + ((k & 1) ? sx : -sx) * 0.5f, cy + ((k & 2) ? sy : -sy) * 0.5f, cz + ((k & 4) ? sz : -sz) * 0.5f);
            ImVec2 sp; if (!WorldToScreen(cf, w, &sp)) continue;
            mn->x = std::min(mn->x, sp.x); mn->y = std::min(mn->y, sp.y); mx->x = std::max(mx->x, sp.x); mx->y = std::max(mx->y, sp.y);
            const Vec3 d = { w.x - cf.pos.x, w.y - cf.pos.y, w.z - cf.pos.z }; dsum += d.x * cf.fwd.x + d.y * cf.fwd.y + d.z * cf.fwd.z; n++;
        }
        if (n < 4) return false; *depth = dsum / n; return true;
    }
    // edit mode: a click on a placed object selects it (Ctrl adds), a double-click grabs the selection
    static bool IsCarried(int uid) { if (!g_place.active) return false; for (const auto& m : g_place.m) if (m.uid == uid) return true; return false; }
    static bool NpcScreenRect(const CamFrame& cf, const ManagedNpc& n, ImVec2* mn, ImVec2* mx, float* depth) {
        Vec3 c{n.pos.x,n.pos.y+0.9f,n.pos.z}, feet=n.pos, head{n.pos.x,n.pos.y+1.8f,n.pos.z}; ImVec2 sc,sf,sh;
        if(!WorldToScreen(cf,c,&sc))return false; WorldToScreen(cf,feet,&sf); WorldToScreen(cf,head,&sh);
        const float h=std::max(28.0f,fabsf(sf.y-sh.y)); const float w=std::max(18.0f,h*0.34f); *mn={sc.x-w*0.5f,sc.y-h*0.5f}; *mx={sc.x+w*0.5f,sc.y+h*0.5f};
        Vec3 d{c.x-cf.pos.x,c.y-cf.pos.y,c.z-cf.pos.z}; *depth=d.x*cf.fwd.x+d.y*cf.fwd.y+d.z*cf.fwd.z; return *depth>0;
    }
    static void UpdateBoxSelection(const CamFrame& cf, const std::vector<SpawnedObj>& objects, const std::vector<ManagedNpc>& npcs, ImGuiIO& io) {
        if(!g_boxSelecting)return; if(!ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)&&!ImGui::IsAnyItemHovered())g_boxCurrent=io.MousePos;
        const float dx=g_boxCurrent.x-g_boxStart.x,dy=g_boxCurrent.y-g_boxStart.y;if(io.MouseDown[ImGuiMouseButton_Left]&&dx*dx+dy*dy>=25)g_boxMoved=true;
        if(g_boxMoved&&(io.MouseDown[ImGuiMouseButton_Left]||ImGui::IsMouseReleased(ImGuiMouseButton_Left))){
            const ImVec2 mn(std::min(g_boxStart.x,g_boxCurrent.x),std::min(g_boxStart.y,g_boxCurrent.y)),mx(std::max(g_boxStart.x,g_boxCurrent.x),std::max(g_boxStart.y,g_boxCurrent.y));std::set<int> nextObj,nextNpc;
            for(const auto&o:objects){if(o.hidden||IsCarried(o.uid))continue;ImVec2 a,b;float dep=0;if(ObjScreenRect(cf,o,&a,&b,&dep)&&dep>0&&a.x<=mx.x&&b.x>=mn.x&&a.y<=mx.y&&b.y>=mn.y)nextObj.insert(o.uid);}
            for(const auto&n:npcs){if(n.hidden)continue;ImVec2 a,b;float dep=0;if(NpcScreenRect(cf,n,&a,&b,&dep)&&a.x<=mx.x&&b.x>=mn.x&&a.y<=mx.y&&b.y>=mn.y)nextNpc.insert(n.uid);}
            if(g_selectGroups){std::set<int> gids;for(const auto&o:objects)if(nextObj.count(o.uid)&&o.group>0)gids.insert(o.group);for(const auto&n:npcs)if(nextNpc.count(n.uid)&&n.group>0)gids.insert(n.group);for(const auto&o:objects)if(!o.hidden&&gids.count(o.group))nextObj.insert(o.uid);for(const auto&n:npcs)if(!n.hidden&&gids.count(n.group))nextNpc.insert(n.uid);}
            if(g_boxAdd){nextObj.insert(g_boxBase.begin(),g_boxBase.end());nextNpc.insert(g_boxNpcBase.begin(),g_boxNpcBase.end());}g_sel.swap(nextObj);g_managedNpcSel.swap(nextNpc);
            g_primary=g_sel.empty()?0:*g_sel.begin();g_managedNpcPrimary=g_managedNpcSel.empty()?0:*g_managedNpcSel.begin();g_sceneLastEntity=g_primary?g_primary:(g_managedNpcPrimary?-g_managedNpcPrimary:0);g_editUid=0;
            ImDrawList*dl=ImGui::GetForegroundDrawList();dl->AddRectFilled(mn,mx,IM_COL32(65,165,230,36));dl->AddRect(mn,mx,IM_COL32(115,205,255,230),0,0,1.5f);
        }
        if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)){if(!g_boxMoved&&!g_boxAdd)ClearSceneSelection();g_boxSelecting=g_boxMoved=g_boxAdd=false;g_boxBase.clear();g_boxNpcBase.clear();}
    }
    static void ClickSelect(const PosInfo& p, bool havePos) {
        (void)p;(void)havePos;g_hoverUid=g_hoverNpcUid=0;ImGuiIO&io=ImGui::GetIO();if(g_browserDragPrefab>=0||g_npcDragIndex>=0)return;
        const bool overUi=ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)||ImGui::IsAnyItemHovered(),addSelect=CtrlHeld(io),addBox=CtrlHeld(io)||ShiftHeld(io),placing=g_place.active;if(g_playMode)return;
        auto objects=core::Spawned();auto npcs=core::ManagedNpcs();const int leftClicks=ImGui::GetMouseClickedCount(ImGuiMouseButton_Left);
        if(g_rightGesture){if(io.MouseDown[ImGuiMouseButton_Right]){float dx=io.MousePos.x-g_rightStart.x,dy=io.MousePos.y-g_rightStart.y;if(dx*dx+dy*dy>16)g_rightMoved=true;}if(ImGui::IsMouseReleased(ImGuiMouseButton_Right)){if(!g_rightMoved&&g_rightUid){if(g_rightUid>0){if(IsCarried(g_rightUid)){ClearSceneSelection();for(const auto&m:g_place.m)g_sel.insert(m.uid);g_primary=g_lastClicked=g_rightUid;g_sceneLastEntity=g_rightUid;}else if(!g_sel.count(g_rightUid))SelectUid(g_rightUid,false,objects);}else{const int uid=-g_rightUid;if(!g_managedNpcSel.count(uid))SelectManagedNpc(uid,false);}g_worldPopupRequested=true;g_worldPopupPos=io.MousePos;}g_rightGesture=g_rightMoved=false;g_rightUid=0;}}
        if(overUi&&!g_boxSelecting)return;if(ImGui::IsMouseClicked(ImGuiMouseButton_Right)){g_rightGesture=true;g_rightMoved=false;g_rightStart=io.MousePos;g_rightUid=0;if(placing&&!g_place.m.empty())g_rightUid=IsCarried(g_primary)?g_primary:g_place.m.front().uid;}
        CamFrame cf=CurrentCam();if(!cf.ok){if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)){g_boxSelecting=g_boxMoved=g_boxAdd=false;g_boxBase.clear();g_boxNpcBase.clear();}return;}
        if(g_boxSelecting){UpdateBoxSelection(cf,objects,npcs,io);if(g_boxSelecting||overUi)return;}if(placing&&(g_place.hover||g_place.drag))return;
        float bestDepth=1e30f;int bestKey=0;for(const auto&o:objects){if(o.hidden||IsCarried(o.uid))continue;ImVec2 mn,mx;float d;if(!ObjScreenRect(cf,o,&mn,&mx,&d)||d<=0)continue;if(io.MousePos.x<mn.x||io.MousePos.x>mx.x||io.MousePos.y<mn.y||io.MousePos.y>mx.y)continue;if(d<bestDepth){bestDepth=d;bestKey=o.uid;}}
        for(const auto&n:npcs){if(n.hidden)continue;ImVec2 mn,mx;float d;if(!NpcScreenRect(cf,n,&mn,&mx,&d))continue;if(io.MousePos.x<mn.x||io.MousePos.x>mx.x||io.MousePos.y<mn.y||io.MousePos.y>mx.y)continue;if(d<bestDepth){bestDepth=d;bestKey=-n.uid;}}
        if(bestKey>0)g_hoverUid=bestKey;else if(bestKey<0)g_hoverNpcUid=-bestKey;
        if(ImGui::IsMouseClicked(ImGuiMouseButton_Right)){if(bestKey)g_rightUid=bestKey;return;}
        if(!bestKey){if(placing){if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))DropCarried();return;}if(ImGui::IsMouseClicked(ImGuiMouseButton_Left)){g_boxSelecting=true;g_boxMoved=false;g_boxAdd=addBox;g_boxStart=g_boxCurrent=io.MousePos;g_boxBase=g_boxAdd?g_sel:std::set<int>{};g_boxNpcBase=g_boxAdd?g_managedNpcSel:std::set<int>{};}return;}
        if(leftClicks>=2){if(bestKey>0){SelectSingleUid(bestKey);StartGrab({bestKey},false,ShortName(Find(objects,bestKey)->prefab));}else{SelectManagedNpc(-bestKey,false);FocusSelection();}return;}
        if(ImGui::IsMouseClicked(ImGuiMouseButton_Left)){if(bestKey>0)SelectUid(bestKey,addSelect,objects);else SelectManagedNpc(-bestKey,addSelect);g_editUid=0;}
    }
    static bool BrowserDropPoint(const core::PrefabInfo& pi, ImVec2 mouse, Vec3* center, bool* onGroundPlane) {
        CamFrame cf = CurrentCam(); if (!cf.ok) return false;
        const Vec3 rd = MouseRay(cf, mouse);
        PosInfo pp{}; const bool havePlayer = core::PlayerPosInfo(&pp);
        bool plane = false; Vec3 hit{};
        if (havePlayer && fabsf(rd.y) > 1e-4f) {
            const float t = (pp.world.y - cf.pos.y) / rd.y;
            if (t > 0.25f && t < 500.0f) { hit = { cf.pos.x + rd.x * t, pp.world.y, cf.pos.z + rd.z * t }; plane = true; }
        }
        if (!plane) {
            const float extent = std::max(pi.sx, std::max(pi.sy, pi.sz)) * g_spawnScale;
            const float dist = std::max(5.0f, std::min(80.0f, 6.0f + extent * 0.75f));
            hit = { cf.pos.x + rd.x * dist, cf.pos.y + rd.y * dist, cf.pos.z + rd.z * dist };
        } else if (pi.hasCenter && pi.sy > 0.0f) {
            hit.y += pi.sy * g_spawnScale * 0.5f;   // cursor marks the surface; placement center is the box center
        }
        *center = hit; if (onGroundPlane) *onGroundPlane = plane; return true;
    }
    static void SpawnBrowserDrop(int prefab, Vec3 center, float yaw, float scale) {
        const auto& idx = core::PrefabIndex(); if (prefab < 0 || prefab >= (int)idx.size()) return;
        const auto& pi = idx[prefab]; g_selPrefab = prefab;
        if (g_previewShown) { core::PreviewClear(); g_previewShown = false; }
        Vec3 at = center;
        if (pi.hasCenter) {
            const float t = yaw * 3.14159265f / 180.0f, cs = cosf(t), sn = sinf(t);
            at.x -= scale * (cs * pi.cx + sn * pi.cz); at.z -= scale * (-sn * pi.cx + cs * pi.cz); at.y -= scale * pi.cy;
        }
        const int uid = core::SpawnAt(pi.path, at, Rot{ yaw }, scale);
        if (uid) StartGrab({ uid }, true, ShownName(pi));
    }
    static void PumpBrowserDropJobs() {
        const auto& idx = core::PrefabIndex();
        for (size_t i = 0; i < g_browserDropJobs.size(); ) {
            BrowserDropJob& j = g_browserDropJobs[i]; core::GroundHit gh;
            if (!core::GroundResult(j.ticket, &gh)) { ++i; continue; }
            Vec3 center = j.center;
            if (j.prefab >= 0 && j.prefab < (int)idx.size() && gh.hit) {
                const auto& pi = idx[j.prefab]; const float groundY = gh.centerY - core::g_probeRadius;
                // BrowserDropPoint stores a bbox center when bounds are known.  Put its lowest point exactly on the
                // physical surface; the prefab pivot is recovered by SpawnBrowserDrop afterwards.
                center.y = groundY + (pi.hasCenter ? std::max(0.0f, pi.sy) * j.scale * 0.5f : 0.0f);
            }
            SpawnBrowserDrop(j.prefab, center, j.yaw, j.scale);
            g_browserDropJobs.erase(g_browserDropJobs.begin() + i);
        }
    }
    static void ProcessBrowserDrag() {
        if (g_browserDragPrefab < 0) return;
        const auto& idx = core::PrefabIndex();
        if (g_browserDragPrefab >= (int)idx.size()) { g_browserDragPrefab = -1; return; }
        ImGuiIO& io = ImGui::GetIO(); const auto& pi = idx[g_browserDragPrefab];
        if (!io.MouseDown[ImGuiMouseButton_Left] && !ImGui::IsMouseReleased(ImGuiMouseButton_Left)) { g_browserDragPrefab = -1; return; }
        const bool overUi = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || ImGui::IsAnyItemHovered();
        Vec3 center{}; bool groundPlane = false; const bool projected = BrowserDropPoint(pi, io.MousePos, &center, &groundPlane);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const std::string shown = ShownName(pi);
        dl->AddText({ io.MousePos.x + 16.0f, io.MousePos.y + 14.0f }, IM_COL32(255, 220, 150, 255), shown.c_str());
        if (!overUi && projected) {
            ImVec2 s; CamFrame cf = CurrentCam();
            if (WorldToScreen(cf, center, &s)) { dl->AddCircle(s, 11.0f, IM_COL32(255, 190, 90, 255), 24, 2.0f); dl->AddCircleFilled(s, 3.0f, IM_COL32(255, 230, 180, 255)); }
        }
        if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left)) return;
        const int prefab = g_browserDragPrefab; g_browserDragPrefab = -1;
        if (overUi || !projected || !core::GameThreadReady() || IsAppearance(pi)) return;
        if (groundPlane && core::GroundProbeReady()) {
            const float startY = center.y + 150.0f;
            const int ticket = core::GroundProbe({ center.x, startY, center.z }, 400.0f);
            if (ticket) {
                g_browserDropJobs.push_back({ prefab, ticket, center, g_spawnYaw, g_spawnScale });
                return;
            }
        }
        // Physics may not be ready immediately after loading.  The fallback still places the measured bbox bottom on
        // the player-height plane; unlike the old path it does not spawn first and then cast through its own collision.
        SpawnBrowserDrop(prefab, center, g_spawnYaw, g_spawnScale);
    }
    static bool NpcDropPoint(ImVec2 mouse, Vec3* at, bool* onGroundPlane) {
        CamFrame cf = CurrentCam(); if (!cf.ok) return false;
        const Vec3 rd = MouseRay(cf, mouse);
        PosInfo pp{}; const bool havePlayer = core::PlayerPosInfo(&pp);
        bool plane = false; Vec3 hit{};
        if (havePlayer && fabsf(rd.y) > 1e-5f) {
            const float t = (pp.world.y - cf.pos.y) / rd.y;
            if (t > 0.25f && t < kNpcMaxDist) {
                hit = { cf.pos.x + rd.x * t, pp.world.y, cf.pos.z + rd.z * t };
                plane = true;
            }
        }
        if (!plane) {   // looking at or above the horizon: the chosen distance along the view, at the character's height
            const float dist = std::clamp(g_npcDist, 1.0f, kNpcMaxDist), h = sqrtf(rd.x * rd.x + rd.z * rd.z);
            if (havePlayer && h > 1e-3f) { hit = { cf.pos.x + rd.x / h * dist, pp.world.y, cf.pos.z + rd.z / h * dist }; plane = true; }
            else hit = { cf.pos.x + rd.x * dist, cf.pos.y + rd.y * dist, cf.pos.z + rd.z * dist };
        }
        *at = hit; if (onGroundPlane) *onGroundPlane = plane; return true;
    }
    static void PumpNpcDropJobs() {
        for (size_t i = 0; i < g_npcDropJobs.size();) {
            NpcDropJob& j = g_npcDropJobs[i]; core::GroundHit gh;
            if (!core::GroundResult(j.ticket, &gh)) { ++i; continue; }
            Vec3 at = j.at;
            if (gh.hit) at.y = gh.centerY - core::g_probeRadius;
            SpawnNpcFormation(j.key, at, j.count, j.formation, j.spacing, j.radius, j.fx, j.fz, j.ai, j.behavior);
            g_npcDropJobs.erase(g_npcDropJobs.begin() + i);
        }
    }
    static void ProcessNpcDrag() {
        if (g_npcDragIndex < 0) return;
        const auto chars = thumbgen::Characters();
        if (!chars || g_npcDragIndex >= (int)chars->size()) { g_npcDragIndex = -1; return; }
        ImGuiIO& io = ImGui::GetIO(); const auto& c = (*chars)[g_npcDragIndex];
        if (!io.MouseDown[ImGuiMouseButton_Left] && !ImGui::IsMouseReleased(ImGuiMouseButton_Left)) { g_npcDragIndex = -1; return; }
        const bool overUi = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || ImGui::IsAnyItemHovered();
        Vec3 at{}; const bool projected = NpcDropPoint(io.MousePos, &at, nullptr);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const char* shown = c.name.empty() ? c.internal.c_str() : c.name.c_str();
        dl->AddText({ io.MousePos.x + 16.0f, io.MousePos.y + 14.0f }, IM_COL32(255, 220, 150, 255), shown);
        if (!overUi && projected) {
            ImVec2 s; CamFrame cf = CurrentCam();
            if (WorldToScreen(cf, at, &s)) { dl->AddCircle(s, 11.0f, IM_COL32(255, 190, 90, 255), 24, 2.0f); dl->AddCircleFilled(s, 3.0f, IM_COL32(255, 230, 180, 255)); }
        }
        if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left)) return;
        const uint32_t key = c.key; g_npcDragIndex = -1;
        if (overUi || !projected || core::NpcState() != 2) return;
        CamFrame cf = CurrentCam(); float fx = cf.ok ? cf.fwd.x : g_fx, fz = cf.ok ? cf.fwd.z : g_fz;
        const float fl = sqrtf(fx * fx + fz * fz); if (fl > 1e-4f) { fx /= fl; fz /= fl; } else { fx = g_fx; fz = g_fz; }
        SpawnNpcFormationGrounded(key, at, g_npcCount, g_npcFormation, g_npcSpacing, g_npcRadius, fx, fz, g_npcSpawnAi, g_npcSpawnBehavior);
    }
    static void DrawWorldContextPopup(bool havePos) {
        ImGui::SetNextWindowPos(ImVec2(0,0),ImGuiCond_Always);ImGui::SetNextWindowSize(ImVec2(1,1),ImGuiCond_Always);
        const ImGuiWindowFlags hostFlags=ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoNav;
        ImGui::Begin("##worldctxhost",nullptr,hostFlags);if(g_worldPopupRequested){g_worldPopupRequested=false;ImGui::OpenPopup("worldctx");ImGui::SetNextWindowPos(g_worldPopupPos,ImGuiCond_Appearing);}g_worldPopupOpen=false;
        if(ImGui::BeginPopup("worldctx")){g_worldPopupOpen=true;const bool hasSel=SceneHasSelection();const auto objects=core::Spawned();const auto npcs=core::ManagedNpcs();bool hasGroup=false;for(int uid:g_sel){const auto*o=Find(objects,uid);if(o&&!o->hidden&&o->group>0){hasGroup=true;break;}}if(!hasGroup)for(int uid:g_managedNpcSel){const auto*n=FindManagedNpc(npcs,uid);if(n&&!n->hidden&&n->group>0){hasGroup=true;break;}}
            ImGui::BeginDisabled(!hasSel);ImGui::BeginDisabled(!core::FreeCamAvailable());if(ImGui::MenuItem(T("Focus")))FocusSelection();ImGui::EndDisabled();DrawSelectionTransformMenu();
            ImGui::BeginDisabled(!g_managedNpcSel.empty());if(ImGui::MenuItem(T("Grab")))StartGrab(SelUids(),false,g_sel.size()==1?"object":"selection");if(ImGui::MenuItem(T("To ground")))SnapSelToGround();if(ImGui::MenuItem(T("Duplicate"))){CopySel();Paste(havePos);}ImGui::EndDisabled();
            if(!g_managedNpcSel.empty()){ImGui::Separator();ImGui::BeginDisabled(!core::NpcAiControlAvailable());if(ImGui::MenuItem(T("Enable AI")))SetSelectedNpcAi(true);if(ImGui::MenuItem(T("Disable AI")))SetSelectedNpcAi(false);if(ImGui::BeginMenu(T("Behavior"))){if(ImGui::MenuItem(T("Normal autonomous")))SetSelectedNpcBehavior(0);if(ImGui::MenuItem(T("Hold position (AI paused)")))SetSelectedNpcBehavior(1);ImGui::EndMenu();}ImGui::EndDisabled();}
            ImGui::Separator();if(ImGui::MenuItem(T("Group selection")))GroupSceneSelection(true);if(ImGui::MenuItem(T("Ungroup"),nullptr,false,hasGroup))GroupSceneSelection(false);
            ImGui::BeginDisabled(!g_managedNpcSel.empty());if(ImGui::BeginMenu(T("Rotate"))){if(ImGui::MenuItem(T("Rotate left")))RotateSel(-g_rotationStep);if(ImGui::MenuItem(T("Rotate right")))RotateSel(g_rotationStep);ImGui::EndMenu();}if(ImGui::BeginMenu(T("Align to primary"))){if(ImGui::MenuItem(T("X")))AlignSel(0);if(ImGui::MenuItem(T("Y")))AlignSel(1);if(ImGui::MenuItem(T("Z")))AlignSel(2);ImGui::EndMenu();}ImGui::EndDisabled();
            if(ImGui::MenuItem(T("Delete")))DeleteSceneSelection();ImGui::EndDisabled();ImGui::EndPopup();}
        ImGui::End();
    }
    static void DrawSelectionOutlines() {
        if(g_sel.empty()&&g_managedNpcSel.empty()&&!g_hoverUid&&!g_hoverNpcUid)return;CamFrame cf=CurrentCam();if(!cf.ok)return;ImDrawList*dl=ImGui::GetForegroundDrawList();auto objects=core::Spawned();auto npcs=core::ManagedNpcs();
        for(const auto&o:objects){const bool sel=g_sel.count(o.uid)>0,hov=o.uid==g_hoverUid;if(o.hidden||(!sel&&!hov))continue;float cx=0,cy=0.5f,cz=0,sx=1,sy=1,sz=1;int pi=IndexOfPrefab(o.prefab);if(pi>=0){const auto&info=core::PrefabIndex()[pi];if(info.hasCenter){cx=info.cx;cy=info.cy;cz=info.cz;}if(info.sx>0){sx=info.sx;sy=info.sy;sz=info.sz;}}ImVec2 sp[8];bool ok[8];for(int k=0;k<8;k++)ok[k]=WorldToScreen(cf,LocalToWorld(o,cx+((k&1)?sx:-sx)*0.5f,cy+((k&2)?sy:-sy)*0.5f,cz+((k&4)?sz:-sz)*0.5f),&sp[k]);static const int edges[12][2]={{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};const ImU32 col=sel?IM_COL32(240,140,80,230):IM_COL32(255,255,255,120);for(auto&e:edges)if(ok[e[0]]&&ok[e[1]])dl->AddLine(sp[e[0]],sp[e[1]],col,sel?2.0f:1.0f);}
        for(const auto&n:npcs){const bool sel=g_managedNpcSel.count(n.uid)>0,hov=n.uid==g_hoverNpcUid;if(n.hidden||(!sel&&!hov))continue;ImVec2 mn,mx;float dep=0;if(!NpcScreenRect(cf,n,&mn,&mx,&dep))continue;const ImU32 col=sel?IM_COL32(240,140,80,230):IM_COL32(255,255,255,150);dl->AddRect(mn,mx,col,3.0f,0,sel?2.0f:1.0f);}
    }
    static void DrawDockPlacementControls(const PosInfo& p, bool havePos, float ui) {
        if (g_selPrefab < 0 || g_selPrefab >= (int)core::PrefabIndex().size()) { ImGui::TextDisabled(T("pick a card, then PLACE")); return; }
        const auto& pi = core::PrefabIndex()[g_selPrefab];
        ImGui::TextDisabled("%s", ShownName(pi).c_str());
        ImGui::BeginDisabled(!havePos || !core::GameThreadReady());
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.36f, 0.22f, 1.0f)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.45f, 0.28f, 1.0f));
        if (ImGui::Button(T(ICON_LOCATION_CROSSHAIRS " PLACE "), ImVec2(130 * ui, 0))) StartPlaceNew(p, havePos);
        ImGui::PopStyleColor(2); ImGui::SameLine();
        if (ImGui::Button(T("spawn only"), ImVec2(-1, 0))) SpawnSelected(p);
        ImGui::EndDisabled();
        if (ImGui::CollapsingHeader(TStable("Spawn options: offset, yaw, scale, direction"))) {
            ImGui::SetNextItemWidth(-1); DragFloat3Edit(T("offset forward, up, side"), g_off, 0.1f, -50, 50, "%.1f");
            ImGui::SetNextItemWidth(120 * ui); SliderFloatEdit(T("yaw##dock"), &g_spawnYaw, -180, 180, "%.0f"); ImGui::SameLine();
            ImGui::SetNextItemWidth(-1); SliderFloatEdit(T("scale##dock"), &g_spawnScale, 0.1f, 20.0f, "%.2f");
            { Vec3 cf; const bool haveCam = core::CameraPose(&cf, nullptr); ImGui::BeginDisabled(!haveCam); ImGui::Checkbox(T("front = camera view"), &g_useCamera); ImGui::EndDisabled(); }
        }
        if (ImGui::CollapsingHeader(TStable("Line and circle: many copies of the selected prefab at once"))) {
            ImGui::SetNextItemWidth(95 * ui); ImGui::InputInt(T("count##dock"), &g_arrCount); g_arrCount = std::clamp(g_arrCount, 1, 200); ImGui::SameLine();
            ImGui::SetNextItemWidth(-1); DragFloatEdit(T("spacing##dock"), &g_arrSpacing, 0.1f, 0.2f, 50, "%.1f m");
            ImGui::SetNextItemWidth(-1); DragFloatEdit(T("radius##dock"), &g_arrRadius, 0.1f, 0.5f, 100, "%.1f m");
            static const char* kLineModes[] = { "yaw as set", "along the line", "across the line" };
            static const char* kCircleModes[] = { "yaw as set", "X to center", "X outward" };
            ImGui::BeginDisabled(!havePos || !core::GameThreadReady());
            if (ImGui::Button(T(ICON_LIST " Line"), ImVec2(76 * ui, 0))) SpawnArray(false, havePos); ImGui::SameLine();
            ImGui::SetNextItemWidth(-1); ComboT("##docklinemode", &g_lineYawMode, kLineModes, 3);
            if (ImGui::Button(T(ICON_CLOCK_ROTATE_LEFT " Circle"), ImVec2(76 * ui, 0))) SpawnArray(true, havePos); ImGui::SameLine();
            ImGui::SetNextItemWidth(-1); ComboT("##dockcirclemode", &g_circleYawMode, kCircleModes, 3);
            ImGui::EndDisabled();
        }
    }
    // narrow dock: the browser keeps the full placement settings and drag/drop behavior of the full editor.
    static void DrawCompact(const PosInfo& p, bool havePos) {
        ImGuiIO& io = ImGui::GetIO(); const float ui = ImGui::GetFontSize() / 17.0f;
        ImGui::SetNextWindowSize(ImVec2(300.0f * ui, io.DisplaySize.y - 80.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 320.0f * ui, 40.0f), ImGuiCond_FirstUseEver);
        const bool playAlpha = g_playMode;
        if (playAlpha) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.20f);
        char title[160]; snprintf(title, sizeof title, T("World Builder [%s]###cdmodkit_dock"), T(g_cameraMode ? "CAMERA" : (g_playMode ? "PLAY" : "EDIT")));
        const bool began = ImGui::Begin(title, &g_open, playAlpha ? ImGuiWindowFlags_NoInputs : 0);
        if (!g_open) { ImGui::End(); if (playAlpha) ImGui::PopStyleVar(); FinishCloseEditor(); return; }
        if (!began) { ImGui::End(); if (playAlpha) ImGui::PopStyleVar(); return; }
        HandleHotkeys(havePos);
        if (ImGui::SmallButton(T(ICON_LIST " full editor"))) { g_compact = false; g_mainTab = g_compactPage; g_selectMainTab = true; }
        ImGui::SameLine();
        ImGui::BeginDisabled(!core::FreeCamAvailable());
        const bool flying = g_cameraMode;   // decided once: the click below toggles it
        if (flying) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.36f, 0.22f, 1.0f));
        if (ImGui::SmallButton(T(flying ? ICON_EYE " flying" : ICON_EYE " free camera"))) ToggleCameraMode();
        if (flying) ImGui::PopStyleColor();
        ImGui::EndDisabled();
        ImGui::SameLine(0, 3); DrawCameraViewTool();
        struct DockPage { int id; const char* label; };
        static const DockPage dockPages[] = {
            { TabBrowser, ICON_MAGNIFYING_GLASS " Browser" },
            { TabScene, ICON_CUBE " Scene" },
            { TabNpcs, ICON_LOCATION_DOT " NPCs" },
            { TabProject, ICON_FLOPPY_DISK " Project" },
            { TabEnvironment, ICON_CLOCK_ROTATE_LEFT " Time & Weather" },
        };
        for (int i = 0; i < (int)(sizeof(dockPages) / sizeof(dockPages[0])); ++i) {
            if (i && i != 2 && i != 4) ImGui::SameLine(0, 4);
            const bool act = g_compactPage == dockPages[i].id;   // decided once: the click below may change the page
            if (act) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
            if (ImGui::SmallButton(TStable(dockPages[i].label))) { g_compactPage = dockPages[i].id; g_mainTab = dockPages[i].id; }
            if (act) ImGui::PopStyleColor();
        }
        if (g_compactPage == TabScene) {
            if (g_previewShown) { core::PreviewClear(); g_previewShown = false; }
            const int gp = core::GimmickPending();
            if (gp > 0) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), core::GimmickTemplateReady() ? T("%d interactive object(s) spawning...") : T("%d interactive object(s) waiting for a spawn template: walk a few meters"), gp);
            DrawScene(p, havePos, true);
            ProcessBrowserDrag();
            ImGui::End();
            if (playAlpha) ImGui::PopStyleVar();
            return;
        }
        if (g_compactPage == TabProject) {
            if (g_previewShown) { core::PreviewClear(); g_previewShown = false; }
            DrawProject();
            ImGui::End();
            if (playAlpha) ImGui::PopStyleVar();
            return;
        }
        if (g_compactPage == TabNpcs) {
            if (g_previewShown) { core::PreviewClear(); g_previewShown = false; }
            DrawNpcs(p, havePos, true);
            ProcessNpcDrag();
            ImGui::End();
            if (playAlpha) ImGui::PopStyleVar();
            return;
        }
        if (g_compactPage == TabEnvironment) {
            if (g_previewShown) { core::PreviewClear(); g_previewShown = false; }
            DrawEnvironment(true);
            ImGui::End();
            if (playAlpha) ImGui::PopStyleVar();
            return;
        }
        ImGui::SetNextItemWidth(-1); InputTextI18n("##dockfilter", T("search  (words in any order)"), g_filter, sizeof g_filter);
        ImGui::Checkbox(ICON_STAR "##dfav", &g_favOnly); if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("favorites only"));
        ImGui::SameLine(); ImGui::Checkbox(T("meshes"), &g_meshOnly); if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("hide prefabs without a visible mesh"));
        ImGui::SameLine(); if (ImGui::SmallButton(T(ICON_XMARK " clear"))) { g_filter[0] = 0; g_tagFilter.clear(); g_favOnly = false; g_selCat = 0; g_selColl = -1; }
        if (g_selCat > 0) { ImGui::SameLine(); ImGui::TextDisabled(T("in %s"), core::Categories()[g_selCat].name.c_str()); }
        LoadColls();
        if (!g_colls.empty()) {   // collections as chips under the search
            for (int ci = 0; ci < (int)g_colls.size(); ci++) {
                if (ci) ImGui::SameLine();
                const bool on = g_selColl == ci;
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
                char lbl[80]; snprintf(lbl, sizeof lbl, ICON_STAR " %s (%d)##dcoll%d", g_colls[ci].name.c_str(), (int)g_colls[ci].paths.size(), ci);
                if (ImGui::SmallButton(lbl)) g_selColl = on ? -1 : ci;
                if (on) ImGui::PopStyleColor();
                if (ImGui::GetItemRectMax().x > ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 60 * ui && ci + 1 < (int)g_colls.size()) ImGui::NewLine();
            }
        }
        RefreshMatches();
        ImGui::TextDisabled(T("%d results"), (int)g_matches.size()); ImGui::SameLine();
        for (int c = 1; c <= 4; c++) {   // cards per row; the tiles stretch to fill the window
            ImGui::SameLine(0, c == 1 ? -1.0f : 2.0f);
            const bool on = g_dockCols == c; char lbl[8]; snprintf(lbl, sizeof lbl, "%d##dc%d", c, c);
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::SmallButton(lbl)) g_dockCols = c;
            if (on) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("%d cards per row"), c);
        }
        ImGui::SameLine(); ImGui::TextDisabled(T("double-click places"));
        const float footer = 330.0f * ui;
        const float listH = std::max(80.0f, ImGui::GetContentRegionAvail().y - footer);
        {   // tile size from the window width: inner width of the bordered, scrollable child divided by the columns
            const ImGuiStyle& st = ImGui::GetStyle();
            const float inner = ImGui::GetContentRegionAvail().x - 2 * st.WindowPadding.x - st.ScrollbarSize - 2.0f;
            const float tile = floorf((inner - (g_dockCols - 1) * st.ItemSpacing.x) / g_dockCols) - 2 * 4.0f * ui;
            DrawCards(p, havePos, listH, ui, g_dockCols, std::max(32.0f, tile));
        }
        DrawDockPlacementControls(p, havePos, ui);
        ProcessBrowserDrag();
        ImGui::End();
        if (playAlpha) ImGui::PopStyleVar();
    }
    static void CameraTick() {
        float dx = 0, dy = 0; input::TakeMouseDelta(&dx, &dy);   // always consume: entering camera mode must never replay old motion
        if (g_rightGesture && ImGui::GetIO().MouseDown[ImGuiMouseButton_Right] && dx * dx + dy * dy > 16.0f) g_rightMoved = true;
        if (!g_cameraMode) return;
        if (core::FreeCamActive()) g_cameraEverActive = true;
        else if (g_cameraEverActive || (g_cameraStartAt && GetTickCount() - g_cameraStartAt > 7000)) {
            core::Log("camera control: leaving camera mode after %s", g_cameraEverActive ? "camera control stopped" : "active camera capture timed out");
            StopCameraMode(); return;
        }

        ImGuiIO& io = ImGui::GetIO();
        // The game may use raw keyboard input, so do not depend on legacy WM_KEYDOWN reaching our WndProc.
        // The original working free-camera path used GetAsyncKeyState for this reason.
        const auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        const bool ctrl = down(VK_CONTROL);
        const bool ctrlCommand = ctrl && (down('Z') || down('Y') || down('C') || down('X') || down('V') || down('D') || down('G') || down('A'));
        const bool movementAllowed = !g_worldPopupOpen && !io.WantTextInput && !ImGui::IsAnyItemActive() && !ctrlCommand;
        const float forward = movementAllowed ? ((down('W') ? 1.0f : 0.0f) - (down('S') ? 1.0f : 0.0f)) : 0.0f;
        const float side = movementAllowed ? ((down('D') ? 1.0f : 0.0f) - (down('A') ? 1.0f : 0.0f)) : 0.0f;
        const float up = movementAllowed ? (((down('E') || down(VK_SPACE)) ? 1.0f : 0.0f) - (down('Q') ? 1.0f : 0.0f)) : 0.0f;
        const bool overUi = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || ImGui::IsAnyItemHovered();
        const float wheel = overUi ? 0.0f : io.MouseWheel;
        core::g_fcHoldMove = !movementAllowed;              // the free camera moves itself from the keys (cdmodkit.cpp FcStep)
        if (wheel != 0) core::FreeCamDolly(wheel * 3.0f);
        const bool looking = g_rightGesture && g_rightMoved && !overUi && !g_worldPopupOpen && !io.WantTextInput && !ImGui::IsAnyItemActive() && io.MouseDown[ImGuiMouseButton_Right];
        if (looking && (dx != 0 || dy != 0)) g_cameraViewMode = 0;
        static DWORD s_lastInputLog = 0; const DWORD now = GetTickCount();
        if ((forward != 0 || side != 0 || up != 0 || wheel != 0 || (looking && (dx != 0 || dy != 0))) && now - s_lastInputLog >= 1000) {
            s_lastInputLog = now;
            core::Log("camera input: forward %.0f side %.0f up %.0f wheel %.1f look %.0f %.0f fast %d",
                forward, side, up, wheel, looking ? dx : 0.0f, looking ? dy : 0.0f, down(VK_SHIFT) ? 1 : 0);
        }
    }


    // one line that always says who gets the mouse and the keyboard right now (top left, no inputs)
    static void DrawFocusHud() {
        if (!g_open && !g_place.active) return;
        ImGuiIO& io = ImGui::GetIO();
        const bool modMouse = g_open ? !g_playMode : MouseMode();
        ImGui::SetNextWindowPos(ImVec2(10.0f, 8.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(g_playMode ? 0.18f : 0.75f);
        if (ImGui::Begin("##focushud", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
            const ImVec4 mod(0.95f, 0.62f, 0.35f, 1.0f), game(0.55f, 0.85f, 0.55f, 1.0f);
            ImGui::TextColored(modMouse ? mod : game, T(modMouse ? "MOUSE: World Builder" : "MOUSE: game"));
            ImGui::SameLine(); ImGui::TextDisabled("  |  ");
            ImGui::SameLine(); ImGui::TextColored(g_cameraMode ? mod : game, T(g_cameraMode ? "KEYS: camera" : "KEYS: game"));
            if (g_place.active) { ImGui::SameLine(); ImGui::TextColored(mod, T(core::g_keyboardPlacement ? "  placement: keyboard + gizmo" : "  placement: mouse gizmo")); }
            if (g_open && !g_compact) { ImGui::SameLine(); ImGui::TextDisabled("     %s = %s", core::KeyName(core::g_keyMode), T(g_playMode ? "back to editing" : "play mode")); }
            if (g_open && !g_playMode && io.WantTextInput) { ImGui::SameLine(); ImGui::TextColored(mod, T("   typing: keys go to the text field")); }
        }
        ImGui::End();
        (void)io;
    }
    void Draw() {
        AutoSaveTick();
        {   // in-game names follow the UI language
            static std::string lastLang; std::string lang = i18n::ActiveLanguage();
            if (lang != lastLang) { lastLang = lang; thumbgen::WantNamesLanguage(lang); }
            g_gameNames = thumbgen::GameNames();
        }
        SampleCamera();
        PosInfo p{}; bool havePos = core::PlayerPosInfo(&p);
        DrawFocusHud();
        TrackFacing(p, havePos);
        PlaceTick();                           // runs with the menu closed as well
        PumpSnapJobs();
        PumpBrowserDropJobs();                 // a drop whose ground probe returns after the window was hidden still spawns
        PumpNpcDropJobs();
        if (g_place.active) DrawPlaceHud();
        if (g_place.active && (g_gizmo || MouseMode())) { const bool one = g_place.m.size() == 1; const CamFrame cf = CurrentCam();
            DrawGizmo(g_place.center, one ? WrapYaw(g_place.m[0].rot0.yaw + g_place.yaw) : g_place.yaw, one ? WrapYaw(g_place.m[0].rot0.pitch + g_place.pitch) : g_place.pitch, GizmoScreenSize(cf, g_place.center, g_place.radius), g_place.drag ? g_place.drag : g_place.hover); }
        if (g_place.active && !g_open) ImGui::GetIO().MouseDrawCursor = MouseMode();
        DrawCalibrationMarker(p, havePos);
        if (!g_open) { if (g_cameraMode) StopCameraMode(); return; }
        if (g_numericEditId && g_numericEditLastSeenFrame >= 0 && ImGui::GetFrameCount() - g_numericEditLastSeenFrame > 1) CancelNumericEdit();
        ImGuiIO& io = ImGui::GetIO();
        io.MouseDrawCursor = !g_playMode;
        ClickSelect(p, havePos); DrawWorldContextPopup(havePos); DrawSelectionOutlines();
        if (g_compact) { DrawCompact(p, havePos); DrawMetadataPopups(); CameraTick(); return; }
        {   // initial size follows the UI scale (style is scaled by screen height / 1080) and stays inside the screen
            const float ui = ImGui::GetFontSize() / 17.0f;
            ImVec2 want(1320.0f * ui, 800.0f * ui);
            want.x = std::min(want.x, io.DisplaySize.x - 60.0f); want.y = std::min(want.y, io.DisplaySize.y - 60.0f);
            ImGui::SetNextWindowSize(want, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2(30, 30), ImGuiCond_FirstUseEver);
        }
        const bool playAlpha = g_playMode;
        if (playAlpha) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.20f);
        char title[240]; snprintf(title, sizeof title, T("World Builder v%s [%s] %s = %s, %s = hide###cdmodkit"), kEditorVersion, T(g_cameraMode ? "CAMERA MODE" : (g_playMode ? "PLAY MODE" : "EDIT MODE")), core::KeyName(core::g_keyMode), T(g_playMode ? "back to editing" : "play mode"), core::KeyName(core::g_keyToggle));
        const bool began = ImGui::Begin(title, &g_open, playAlpha ? ImGuiWindowFlags_NoInputs : 0);
        if (!g_open) { ImGui::End(); if (playAlpha) ImGui::PopStyleVar(); FinishCloseEditor(); return; }
        if (!began) { ImGui::End(); if (playAlpha) ImGui::PopStyleVar(); CameraTick(); return; }
        HandleHotkeys(havePos);
        if (!core::BuildOk()) {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), T("Game functions not resolved: %s"), core::BuildMessage()[0] ? core::BuildMessage() : "verification pending");
            ImGui::TextWrapped(T("Spawning is disabled to avoid crashes. See bin64\\cdmodkit\\cdmodkit.log (RESOLVE FAILED)."));
            ImGui::TextWrapped(T("Game build %s - a patch moves the game's functions; report that build number so the signatures can be updated."), core::GameVersion()[0] ? core::GameVersion() : "unknown");
        }
        if (core::PrefabIndex().empty()) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.45f, 0.12f, 0.10f, 0.85f));
            ImGui::BeginChild("noindex", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 2.2f), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
            ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), T(ICON_XMARK "  Prefab list not found: the browser and the search stay empty."));
            ImGui::EndChild(); ImGui::PopStyleColor();
        }
        if (ImGui::SmallButton(T(ICON_COPY " dock"))) {
            g_compactPage = (g_mainTab == TabScene || g_mainTab == TabNpcs || g_mainTab == TabEnvironment) ? g_mainTab : TabBrowser;
            g_compact = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(
            g_mainTab == TabScene ? "narrow side window: Scene with the same editing controls" :
            g_mainTab == TabNpcs ? "narrow side window: NPC browser and spawning" :
            g_mainTab == TabEnvironment ? "narrow side window: time and weather controls" :
            "narrow side window: search, cards and PLACE"));
        ImGui::SameLine();
        {   // free-fly camera
            const bool fc = g_cameraMode;
            ImGui::BeginDisabled(!core::FreeCamAvailable());
            if (fc) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.36f, 0.22f, 1.0f));
            char fl[96]; snprintf(fl, sizeof fl, "%s (%s)###freecam", T(fc ? ICON_EYE " flying" : ICON_EYE " free camera"), core::KeyName(core::g_keyMode));
            if (ImGui::SmallButton(fl)) ToggleCameraMode();
            if (fc) ImGui::PopStyleColor();
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", T(core::FreeCamAvailable()
                ? "Free camera: W A S D move, E or Space up, Q down, Shift faster, mouse wheel forward. Ctrl remains available for multi-select and editor shortcuts. Drag with the right mouse button over the world to look around; a right click without moving opens the context menu. Your character stays where it is; new objects appear in front of the camera."
                : "The free camera is not available in this game build (see the log)."));
            ImGui::SameLine(); DrawCameraViewTool(); ImGui::SameLine();
        }
        if (havePos) ImGui::Text(T(ICON_LOCATION_DOT "  %.1f  %.1f  %.1f   tile %d,%d"), p.world.x, p.world.y, p.world.z, p.tileX, p.tileZ);
        else ImGui::TextDisabled(T("player position not available (load a save)"));
        ImGui::SameLine(ImGui::GetWindowWidth() - 330);
        {   // game thread state: the mod runs its work on the game's simulation tick; the counter tells whether that tick is alive
            static long s_lastTicks = 0; static DWORD s_lastChange = 0; const long ticks = core::PumpTicks(); const DWORD now = GetTickCount();
            if (ticks != s_lastTicks) { s_lastTicks = ticks; s_lastChange = now; }
            const char* state = !core::HooksReady() ? "hooks MISSING" : ticks == 0 ? "game thread: waiting" : now - s_lastChange < 500 ? "game thread: running" : "game thread: paused";
            ImGui::TextDisabled(T("objects %d  |  %s"), (int)core::Spawned().size(), T(state));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("Spawning and moving happen on the game's simulation tick (%ld ticks so far).\nPaused = loading screen, menu or pause; queued actions run once it continues."), ticks);
        }
        if (ImGui::BeginTabBar("tabs")) {
            bool inBrowser = false;
            const ImGuiTabItemFlags browserFlags = g_selectMainTab && g_mainTab == TabBrowser ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(TStable(ICON_MAGNIFYING_GLASS " Browser"), nullptr, browserFlags)) { inBrowser = true; g_mainTab = TabBrowser; DrawBrowser(p, havePos); ImGui::EndTabItem(); }
            const ImGuiTabItemFlags npcFlags = g_selectMainTab && g_mainTab == TabNpcs ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(TStable(ICON_LOCATION_DOT " NPCs"), nullptr, npcFlags)) { g_mainTab = TabNpcs; DrawNpcs(p, havePos); ImGui::EndTabItem(); }
            const ImGuiTabItemFlags sceneFlags = g_selectMainTab && g_mainTab == TabScene ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(TStable(ICON_CUBE " Scene"), nullptr, sceneFlags)) { g_mainTab = TabScene; { const int gp = core::GimmickPending(); if (gp > 0) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), core::GimmickTemplateReady() ? T("%d interactive object(s) spawning...") : T("%d interactive object(s) waiting for a spawn template: walk a few meters"), gp); } DrawScene(p, havePos); ImGui::EndTabItem(); }
            const ImGuiTabItemFlags environmentFlags = g_selectMainTab && g_mainTab == TabEnvironment ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(TStable(ICON_CLOCK_ROTATE_LEFT " Time & Weather"), nullptr, environmentFlags)) { g_mainTab = TabEnvironment; DrawEnvironment(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(TStable(ICON_CLOCK_ROTATE_LEFT " History"))) { g_mainTab = TabHistory; DrawHistory(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(TStable(ICON_FLOPPY_DISK " Project"))) { g_mainTab = TabProject; DrawProject(); ImGui::EndTabItem(); }
            static const bool s_showTravel = false;   // hidden until the game's own teleport path is found
            if (s_showTravel && ImGui::BeginTabItem(TStable(ICON_LOCATION_CROSSHAIRS " Travel"))) { g_mainTab = TabTravel; DrawTravel(p, havePos); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem(TStable(ICON_LIST " Settings"))) {
                g_mainTab = TabSettings;
                int languageCount = 0; const auto* languageOptions = i18n::Languages(&languageCount); int languageIndex = 0;
                for (int i = 0; i < languageCount; ++i) if (_stricmp(languageOptions[i].id, i18n::Preference()) == 0) { languageIndex = i; break; }
                // always also says "Language" in English: someone who picked a language they cannot read must find this combo again
                char langLabel[96]; snprintf(langLabel, sizeof langLabel, strcmp(T("Language"), "Language") ? "%s - Language###language" : "%s###language", T("Language"));
                if (ImGui::BeginCombo(langLabel, languageOptions[languageIndex].name)) {
                    for (int i = 0; i < languageCount; ++i) {
                        if (ImGui::Selectable(languageOptions[i].name, i == languageIndex) && i18n::SetPreference(languageOptions[i].id)) core::SaveSettings();
                    }
                    ImGui::EndCombo();
                }
                {   // preview quality: each step loads more textures per surface, so it is also the speed of the background pass
                    static const char* kQ[] = { "base colour (fastest)", "+ dye colours", "+ normal maps", "+ specular and glow (best)" };
                    int q = thumbgen::Quality(); ImGui::SetNextItemWidth(260);
                    if (ImGui::BeginCombo(T("preview quality"), T(kQ[q]))) { for (int i = 0; i < 4; i++) if (ImGui::Selectable(T(kQ[i]), i == q)) { thumbgen::SetQuality(i); core::SaveSettings(); } ImGui::EndCombo(); }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("previews on screen are always rendered first; a lower level only makes the background pass faster. Applies to new previews, right-click a tile to render it again"));
                }
                ImGui::TextDisabled(T("Hotkeys (saved to settings.txt in the cdmodkit folder, active immediately)"));
                auto keyCombo = [](const char* label, int* vk) {
                    int cur = -1; for (int i = 0; i < core::KeyCount(); i++) if (core::KeyVkAt(i) == *vk) cur = i;
                    ImGui::SetNextItemWidth(160);
                    if (ImGui::BeginCombo(T(label), cur >= 0 ? core::KeyNameAt(cur) : "?")) { for (int i = 0; i < core::KeyCount(); i++) if (ImGui::Selectable(core::KeyNameAt(i), i == cur)) { *vk = core::KeyVkAt(i); core::SaveSettings(); } ImGui::EndCombo(); }
                };
                keyCombo("show and hide the editor", &core::g_keyToggle);
                keyCombo("control and edit mode", &core::g_keyMode);
                ImGui::SetNextItemWidth(160); SliderFloatEdit(T("free camera speed"), &core::g_fcSpeed, 1.0f, 100.0f, "%.0f m per s"); if (ImGui::IsItemDeactivatedAfterEdit() || NumericEditEnded(T("free camera speed"))) core::SaveSettings();
                ImGui::SameLine(); ImGui::SetNextItemWidth(160); SliderFloatEdit(T("mouse sensitivity"), &core::g_fcSens, 0.02f, 0.5f, "%.2f"); if (ImGui::IsItemDeactivatedAfterEdit() || NumericEditEnded(T("mouse sensitivity"))) core::SaveSettings();
                if (ImGui::Checkbox(T("start free camera when opening the editor"), &core::g_autoFreeCamOnOpen)) core::SaveSettings();
                ImGui::Separator();
                if (ImGui::Checkbox(T("keyboard controls while placing objects (optional)"), &core::g_keyboardPlacement)) { core::ApplyPlaceKeys(); core::SaveSettings(); input::ClearKeys(); }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Off by default. When enabled, the placement keys below move, rotate and scale the carried object and are kept away from the game."));
                if (core::g_keyboardPlacement && ImGui::TreeNode(T("placement keyboard bindings"))) {
                    for (int i = 0; i < core::PK_COUNT; ++i) { ImGui::PushID(i); keyCombo(core::PlaceKeyLabel(i), &core::g_placeKeys[i]); ImGui::PopID(); }
                    if (ImGui::SmallButton(T("reset to numpad defaults"))) {
                        const int d[core::PK_COUNT] = { VK_NUMPAD8, VK_NUMPAD2, VK_NUMPAD4, VK_NUMPAD6, VK_NUMPAD9, VK_NUMPAD3, VK_NUMPAD7, VK_NUMPAD1, VK_ADD, VK_SUBTRACT, VK_NUMPAD0, VK_DECIMAL, VK_NUMPAD5, VK_MULTIPLY, VK_DIVIDE, VK_RETURN, VK_BACK, VK_SHIFT };
                        memcpy(core::g_placeKeys, d, sizeof d); core::ApplyPlaceKeys(); core::SaveSettings();
                    }
                    ImGui::TreePop();
                }
                ImGui::Separator();
                if (ImGui::Checkbox(T("project auto-save"), &core::g_projectAutoSave)) core::SaveSettings();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Save dirty loaded projects immediately after each committed edit. New unassigned objects are left alone."));
                if (ImGui::Checkbox(T("show selected item details panel"), &core::g_showSelectionDetails)) core::SaveSettings();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Turn this off to hide the information box that appears below the Browser after selecting an item."));
                ImGui::Separator();
                ImGui::TextDisabled(T("Gizmo projection (stage 1: display). Calibrate once: enable the marker, switch to camera mode (Home) and adjust until the yellow circles sit at your character's feet and head."));
                ImGui::Checkbox(T("show calibration marker"), &g_calib); ImGui::SameLine(); ImGui::Checkbox(T("axis gizmo while placing"), &g_gizmo);
                { float live = 0; bool haveLive = core::CameraFov(&live);
                  if (ImGui::Checkbox(T("read the field of view from the game"), &core::g_fovAuto)) core::SaveSettings();
                  // the renderer's projection is the first source (LiveCam); the camera object's own field is only the fallback
                  float rm00 = 0, rm11 = 0; Vec3 rp; const bool rc = core::RenderCamera(&rp, nullptr, nullptr, nullptr, &rm00, &rm11);
                  ImGui::SameLine();
                  if (rc) ImGui::TextDisabled(T("(game says %.1f deg)"), 2.0f * atanf(1.0f / rm11) * 57.2958f);
                  else if (haveLive) ImGui::TextDisabled(T("(game says %.1f deg)"), live);
                  else ImGui::TextDisabled(T("(not available, using the manual value)")); }
                ImGui::SetNextItemWidth(220); SliderFloatEdit(T("manual field of view"), &core::g_fovDeg, 20.0f, 120.0f, "%.1f deg"); if (ImGui::IsItemDeactivatedAfterEdit() || NumericEditEnded(T("manual field of view"))) core::SaveSettings();
                ImGui::SameLine(); if (ImGui::Checkbox(T("mirror horizontally"), &core::g_camMirror)) core::SaveSettings();
                { float m00 = 0, m11 = 0; Vec3 d0;
                  const bool haveRc = core::RenderCamera(&d0, nullptr, nullptr, nullptr, &m00, &m11);
                  if (haveRc) ImGui::TextDisabled(T("render camera: live field of view %.1f deg, aspect %.3f"), 2.0f * atanf(1.0f / m11) * 57.2958f, m11 / m00);
                  else ImGui::TextDisabled(T("render camera: not available, using the camera object fallback")); }
                if (ImGui::Button(T("fov trace (12 s, zoom in and out)"))) { core::FovTrace(12); Note(T("fovtrace started: close the menu and zoom the camera in and out for 12 s")); }
                ImGui::Separator();
                if (ImGui::Checkbox(T("console window (log output; applies on the next start)"), &core::g_showConsole)) core::SaveSettings();
                ImGui::Separator();
                // opt-in: the checkbox starts / stops the server at once and is remembered in settings.txt (http_api=)
                if (ImGui::Checkbox(T("HTTP API for programs on this PC"), &core::g_httpEnabled)) {
                    if (core::g_httpEnabled) httpapi::Start(core::g_httpPort); else httpapi::Stop();
                    core::SaveSettings();
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("listens on 127.0.0.1 only. Local tools and scripts can search prefabs and spawn, move and delete World Builder objects (see HTTP_API.md)"));
                ImGui::SameLine(); ImGui::SetNextItemWidth(70);
                ImGui::InputInt(T("port"), &core::g_httpPort, 0, 0);   // no step buttons: applied once on Enter / leaving the field, not per keystroke
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    if (core::g_httpPort < 1 || core::g_httpPort > 65535) core::g_httpPort = 8765;
                    core::SaveSettings();
                    if (core::g_httpEnabled) httpapi::Start(core::g_httpPort);   // restarts on the new port
                }
                if (const int port = httpapi::ActivePort()) {
                    char url[80]; snprintf(url, sizeof url, "http://127.0.0.1:%d/api/status", port);
                    ImGui::TextDisabled(T("running: %s"), url);
                    ImGui::SameLine(); if (ImGui::SmallButton(T("copy URL"))) ImGui::SetClipboardText(url);
                } else if (core::g_httpEnabled) {
                    const std::string err = httpapi::LastError();
                    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1), T("not running: %s"), err.empty() ? T("starting") : err.c_str());
                    ImGui::SameLine(); if (ImGui::SmallButton(T("retry"))) httpapi::Start(core::g_httpPort);
                } else ImGui::TextDisabled(T("off"));
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(TStable(ICON_LIST " Log"))) {
                g_mainTab = TabLog;
                if (ImGui::CollapsingHeader(TStable("Developer: how moves are applied"))) {
                    ImGui::Checkbox(T("live drag in the details pane"), &g_live); ImGui::SameLine();
                    ImGui::Checkbox(T("re-create on move"), &core::g_recreateOnMove); ImGui::SameLine();
                    ImGui::Checkbox(T("interactive objects through the game"), &core::g_gimmickSpawn); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("prefabs in the cd_gimmick folder are spawned through the game's own spawn path and react like real objects (torches, doors, chests); off means plain objects")); ImGui::SameLine();
                    static const char* kLiveModes[] = { "disable then set then enable", "transform only", "transform re-insert", "transform + enable" };
                    ImGui::SetNextItemWidth(170 * ImGui::GetIO().FontGlobalScale); ComboT("##livemode", &core::g_liveMode, kLiveModes, 4);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("how the object is updated while dragging (release always applies the final position)"));
                }
                {   // gimmick spawn research: the game's own server spawns, newest first; "here" issues that one again in front of you
                    core::GimmickCapInfo caps[16]; const int nc = core::GimmickCaptureList(caps, 16);
                    ImGui::TextDisabled(T("gimmick spawn research: captures %d%s"), nc, core::GimmickReplayArmed() ? "  (replay armed: walk a bit or drop an item)" : "");
                    {   // objects our replays created; "remove" repeats the game's own removal steps for that actor (experiment)
                        core::SpawnedInfo sp[16]; const int ns = core::SpawnedList(sp, 16);
                        if (ns) { ImGui::TextDisabled(T("spawned by replay: %d"), ns);
                            for (int i = 0; i < ns; i++) { ImGui::PushID((int)(sp[i].so & 0x7FFFFFFF)); const char* fn = strrchr(sp[i].prefab, '/'); ImGui::Text(T("%lu s  %s  actor %p"), sp[i].ageMs / 1000, fn ? fn + 1 : sp[i].prefab, (void*)sp[i].actor); ImGui::SameLine(); if (sp[i].actor && ImGui::SmallButton(T("remove"))) core::RequestRemoveSpawned(sp[i].actor); ImGui::PopID(); } }
                    }
                    {   // the prefab the replay creates its server object from (the prepare's 4th argument is that path)
                        static char prefabOverride[256] = {};
                        if (ImGui::SmallButton(T("spawn it in front of me"))) QuickGimmickSpawn(); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("spawns the override prefab (a stand torch when the field is empty) 2 m in front of you through the game's spawn path, from the newest capture")); ImGui::SameLine(); ImGui::SetNextItemWidth(-1);
                        if (InputTextI18n("##replayprefab", T("replay prefab path override (empty uses the captured path)"), prefabOverride, sizeof prefabOverride)) core::SetGimmickReplayPrefab(prefabOverride);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("\"here\" replays the capture, but the server object is built from this prefab instead of the captured one"));
                    }
                    if (nc && ImGui::BeginTable("gcaps", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
                        ImGui::TableSetupColumn(T("age")); ImGui::TableSetupColumn(T("name (from the scene object created after it)")); ImGui::TableSetupColumn(T("caller")); ImGui::TableSetupColumn(T("words")); ImGui::TableSetupColumn("");
                        ImGui::TableHeadersRow();
                        for (int i = 0; i < nc; i++) {
                            const auto& c = caps[i]; ImGui::TableNextRow(); ImGui::PushID(c.id);
                            ImGui::TableSetColumnIndex(0); ImGui::Text(T("%lu s"), c.ageMs / 1000);
                            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(c.name[0] ? c.name : "?"); if (c.path[0] && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", c.path);
                            ImGui::TableSetColumnIndex(2); { const char* cn = core::GimmickCallerName(c.caller); if (cn) ImGui::TextUnformatted(cn); else ImGui::Text(T("0x%llx"), (unsigned long long)c.caller); }
                            ImGui::TableSetColumnIndex(3); ImGui::Text("%u, %u", c.k1, c.k2);
                            ImGui::TableSetColumnIndex(4); if (ImGui::SmallButton(T("here"))) core::ArmGimmickReplay(InFront(2.0f, 0.0f), c.id);
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                }
                bool tr = core::Trace(); if (ImGui::Checkbox(T("trace game calls (writes to cdmodkit.log; turn on, move an object in the housing editor, turn off)"), &tr)) core::SetTrace(tr);
                if (ImGui::Button(T("camera trace (16 s)"))) { core::CamTrace(16); Note(T("camtrace started: close the menu and rotate the camera slowly for 16 s")); }
                ImGui::SameLine(); if (ImGui::Button(T("ray and shape cast trace"))) { core::RayTrace(12); Note(T("trace: close the menu (Home), walk a few steps, then aim with the bow and press F on something")); }
                ImGui::SameLine(); if (ImGui::Button(T("ground probe (experimental)"))) { core::ProbeGround(3.0f, 10.0f); Note(T("probe: replaying a captured shape cast 3 m above you, 10 m down (see the log)")); }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("logs which fields of the camera component change while you rotate the camera (used to find the view direction)"));
                ImGui::Separator();
                for (auto& l : g_log) ImGui::TextUnformatted(l.c_str()); ImGui::EndTabItem();
            }
            if (!inBrowser && g_previewShown) { core::PreviewClear(); g_previewShown = false; }
            ImGui::EndTabBar();
            if (g_selectMainTab) g_selectMainTab = false;
        }
        ProcessBrowserDrag();
        ProcessNpcDrag();
        DrawMetadataPopups();
        ImGui::End();
        if (playAlpha) ImGui::PopStyleVar();
        CameraTick();
    }
}
