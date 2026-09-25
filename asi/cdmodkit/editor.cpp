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
    static constexpr const char* kEditorVersion = "0.94";

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
    static bool  g_showSpawnOpts = false, g_showMass = false; static int g_hoverUid = 0;
    static bool  g_cardView = false; static float g_cardSize = 96.0f;   // browser: tile view instead of the list (same matches / filters)
    static int   g_browserDragPrefab = -1;   // browser row/card being dragged out into the game view
    struct BrowserDropJob { int prefab = -1, ticket = 0; Vec3 center{}; float yaw = 0, scale = 1; };
    static std::vector<BrowserDropJob> g_browserDropJobs;   // ground is probed before spawning, so a new object's own collision cannot be mistaken for the surface
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
    // scene state: selection by uid
    static std::set<int> g_sel; static int g_primary = 0; static int g_lastClicked = 0;
    static bool g_boxSelecting = false, g_boxMoved = false, g_boxAdd = false; static ImVec2 g_boxStart{}, g_boxCurrent{}; static std::set<int> g_boxBase;
    static bool g_rightGesture = false, g_rightMoved = false, g_worldPopupRequested = false, g_worldPopupOpen = false; static ImVec2 g_rightStart{}, g_worldPopupPos{}; static int g_rightUid = 0;
    static bool  g_selectGroups = true, g_showDeleted = false;
    static float g_edit[3] = { 0, 0, 0 }, g_editScale = 1; static Rot g_editRot, g_editRot0; static Vec3 g_editPos0{}; static float g_editScale0 = 1;
    static int   g_editUid = 0; static bool g_live = true;
    static std::vector<std::string> g_log;
    // snapping
    static bool  g_snap = false; static int g_snapPosIdx = 3, g_snapYawIdx = 2;
    static const float kSnapPos[] = { 0.1f, 0.25f, 0.5f, 1.0f, 2.0f }; static const char* kSnapPosNames[] = { "0.1 m", "0.25 m", "0.5 m", "1 m", "2 m" };
    static const float kSnapYaw[] = { 5.0f, 15.0f, 30.0f, 45.0f, 90.0f }; static const char* kSnapYawNames[] = { "5 deg", "15 deg", "30 deg", "45 deg", "90 deg" };
    static float g_rotationStep = 25.0f;

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
    struct Act { enum Kind { Spawn, Move, Delete, SetGroup } kind; int uid; std::string prefab; Vec3 pos0{}, pos1{}; Rot rot0, rot1; float sc0 = 1, sc1 = 1; int group = 0, group1 = 0; int proj = 0; };   // group/group1: old/new group for undoable grouping; proj keeps deleted objects in their project
    struct HistoryEntry { unsigned long long serial = 0; std::vector<Act> acts; };
    static constexpr size_t kHistoryLimit = 200;
    static unsigned long long g_historySerial = 0;
    static std::vector<HistoryEntry> g_undo, g_redo;
    static bool ActChanged(const Act& a) {
        if (a.kind == Act::Spawn || a.kind == Act::Delete) return true;
        if (a.kind == Act::SetGroup) return a.group != a.group1;
        return fabsf(a.pos1.x - a.pos0.x) > 0.0001f || fabsf(a.pos1.y - a.pos0.y) > 0.0001f || fabsf(a.pos1.z - a.pos0.z) > 0.0001f ||
               fabsf(a.rot1.yaw - a.rot0.yaw) > 0.0001f || fabsf(a.rot1.pitch - a.rot0.pitch) > 0.0001f || fabsf(a.rot1.roll - a.rot0.roll) > 0.0001f ||
               fabsf(a.sc1 - a.sc0) > 0.0001f;
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
        int hover = 0, drag = 0;   // gizmo: 1 X, 2 Y, 3 Z, 4 yaw ring, 5 center dot, 6 scale cube, 7 pitch ring, 8 roll ring
        float drag0 = 0; Vec3 dragCenter0{}; float dragYaw0 = 0, dragScale0 = 1, dragPitch0 = 0, dragRoll0 = 0;
        int groundTicket = 0, groundIter = 0; float groundBottom = 0, groundTop = 0, groundStartY = 0;   // snap to ground in flight (see GroundStep)
    };
    static Place g_place;
    static void StopCameraMode() {
        if (!g_cameraMode) return;
        g_cameraMode = false; g_cameraViewMode = 0; g_cameraStartAt = 0; g_cameraEverActive = false; memset(g_cameraShortcutDown, 0, sizeof g_cameraShortcutDown);
        g_rightGesture = g_rightMoved = g_worldPopupRequested = false; g_rightUid = 0;
        core::SetFreeCam(false); core::g_fcHoldMove = false; input::ClearKeys(); ImGui::GetIO().ClearInputKeys();
    }
    bool IsOpen() { return g_open; }
    bool PlayMode() { return g_playMode; }
    void TogglePlay() {
        if (!g_open) return;
        if (g_cameraMode) StopCameraMode();
        g_boxSelecting = g_boxMoved = g_boxAdd = false; g_boxBase.clear(); g_rightGesture = g_rightMoved = g_worldPopupRequested = false; g_rightUid = 0;
        g_playMode = !g_playMode; ImGui::GetIO().ClearInputKeys();
    }
    void ToggleCameraMode() {
        if (!g_open) return;
        if (g_cameraMode) { StopCameraMode(); return; }
        if (!core::FreeCamAvailable()) { core::Log("[editor] camera mode: the free camera is not available in this game build"); return; }   // the header button says so too
        g_boxSelecting = g_boxMoved = g_boxAdd = false; g_boxBase.clear(); g_rightGesture = g_rightMoved = g_worldPopupRequested = false; g_rightUid = 0;
        g_playMode = false; g_cameraMode = true; g_cameraStartAt = GetTickCount(); g_cameraEverActive = false; memset(g_cameraShortcutDown, 0, sizeof g_cameraShortcutDown);
        input::TakeMouseDelta(nullptr, nullptr);
        input::ClearKeys(); ImGui::GetIO().ClearInputKeys(); core::SetFreeCam(true);
    }
    bool Placing() { return g_place.active; }
    bool MouseMode() { return g_place.active && (g_open ? !g_playMode : true); }
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
        if (g_open && g_cameraMode) StopCameraMode();
        g_open = !g_open; g_playMode = false;
        if (!g_open) { g_boxSelecting = g_boxMoved = g_boxAdd = false; g_boxBase.clear(); g_rightGesture = g_rightMoved = g_worldPopupRequested = false; g_rightUid = 0; g_browserDragPrefab = -1; g_browserDropJobs.clear(); }
        ImGui::GetIO().ClearInputKeys();
        if (!g_open && g_previewShown) { core::PreviewClear(); g_previewShown = false; }
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
    static void DrawPlaceHud() {
        Place& P = g_place; ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 28.0f), ImGuiCond_Always, ImVec2(0.5f, 0));
        ImGui::SetNextWindowBgAlpha(0.75f);
        if (ImGui::Begin("##placehud", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
            ImGui::Text(ICON_CUBE "  %s  %s%s", T(P.isNew ? "Place" : "Grab"), P.name.c_str(), P.m.size() > 1 ? T("  (group)") : "");
            ImGui::TextDisabled(T("center %.1f  %.1f  %.1f    rotation %+.0f    tilt %+.0f / %+.0f    scale x%.2f    snap %s"), P.center.x, P.center.y, P.center.z, P.yaw, P.pitch, P.roll, P.scale,
                g_snap ? (std::string(T(kSnapPosNames[g_snapPosIdx])) + " / " + T(kSnapYawNames[g_snapYawIdx])).c_str() : T("off"));
            if (ImGui::Button(T("drop"))) DropCarried();
            ImGui::SameLine(); if (ImGui::Button(T("Cancel"))) CancelCarried();
            ImGui::SameLine(); if (ImGui::Button(T("To ground"))) StartGroundSnap(P);
            ImGui::SameLine(); if (ImGui::Button(T("level"))) { if (P.m.size() == 1) { P.pitch = -P.m[0].rot0.pitch; P.roll = -P.m[0].rot0.roll; } else { P.pitch = P.roll = 0; } P.dirty = P.touched = true; }
            ImGui::SameLine(); ImGui::Checkbox(T("snap"), &g_snap);
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
        Vec3 c{ 0, 0, 0 }; int n = 0;
        for (int uid : uids) { const SpawnedObj* o = Find(list, uid); if (!o || o->hidden) continue; Member m; m.uid = uid; m.prefab = o->prefab; m.rot0 = o->rot; m.scale0 = o->scale; m.origPos = o->pos; m.origRot = o->rot; m.origScale = o->scale; P.m.push_back(m); }
        if (P.m.empty()) return;
        if (P.m.size() == 1) { const SpawnedObj* o = Find(list, P.m[0].uid); c = BboxCenter(*o); P.radius = Footprint(*o); P.prefabIdx = IndexOfPrefab(o->prefab); P.haveCenter = P.prefabIdx >= 0 && core::PrefabIndex()[P.prefabIdx].hasCenter; }
        else { for (auto& m : P.m) { c.x += m.origPos.x; c.y += m.origPos.y; c.z += m.origPos.z; n++; } c.x /= n; c.y /= n; c.z /= n;
               for (auto& m : P.m) { float dx = m.origPos.x - c.x, dz = m.origPos.z - c.z; P.radius = std::max(P.radius, sqrtf(dx * dx + dz * dz) + 1.0f); } }
        for (auto& m : P.m) m.rel = { m.origPos.x - c.x, m.origPos.y - c.y, m.origPos.z - c.z };
        P.center = c; P.lastCenter = c; P.lastYaw = 0; P.lastScale = 1; P.dirty = isNew;
        if (keepCamera) { P.reopen = false; }                             // camera mode stays live while the gizmo is shown
        else if (g_compact && g_open) { P.reopen = false; }               // the dock stays where it is
        else { P.reopen = g_open; g_open = false; }
        if (!keepCamera) StopCameraMode();
        g_playMode = false; input::ClearKeys();
        g_place = P;
        Note("%s: %s", T(isNew ? "placing" : "grabbed"), name.c_str());
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
        g_place.active = false; input::ClearKeys();
        if (g_place.reopen) { g_open = true; g_playMode = true; }   // visible again, but the game keeps the input (Home to edit)
    }
    static void CancelCarried(bool notify) {
        Place& P = g_place; if (!P.active) return;
        if (P.isNew) { for (auto& m : P.m) { core::HideUid(m.uid); core::ForgetUid(m.uid); } }
        else { std::vector<core::MoveReq> r; for (auto& m : P.m) r.push_back({ m.uid, m.origPos, m.origRot, m.origScale }); core::MoveMany(r, true); }
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
        std::vector<core::MoveReq> r; MembersTo(r, P, P.center, P.yaw, P.scale); core::MoveMany(r, true);
        std::vector<Act> acts;
        for (size_t i = 0; i < P.m.size(); i++) {
            Act a; a.uid = P.m[i].uid; a.prefab = P.m[i].prefab; a.pos1 = r[i].pos; a.rot1 = r[i].rot; a.sc1 = r[i].scale;
            if (P.isNew) { a.kind = Act::Spawn; int ix = core::IndexOfUid(a.uid); a.group = ix >= 0 ? core::Spawned()[ix].group : 0; }
            else { a.kind = Act::Move; a.pos0 = P.m[i].origPos; a.rot0 = P.m[i].origRot; a.sc0 = P.m[i].origScale; }
            acts.push_back(a);
        }
        Push(acts);
        Note(T(P.isNew ? "placed %s" : "dropped %s"), P.name.c_str()); FinishPlace();
    }
    static void PlaceTick() {
        if (!g_place.active) return;
        Place& P = g_place;
        const bool gizmoMouse = g_open ? !g_playMode : true;
        if (P.m.size() == 1 && !P.haveCenter && P.prefabIdx >= 0 && core::PrefabIndex()[P.prefabIdx].hasCenter) {   // center measured meanwhile: keep the pivot, move the rotation center
            const auto& info = core::PrefabIndex()[P.prefabIdx]; Member& m = P.m[0];
            Vec3 pivot = { P.center.x + m.rel.x, P.center.y + m.rel.y, P.center.z + m.rel.z };   // current pivot (yaw/scale deltas are still 0 at this point in practice)
            const Vec3 off = RotLocal(m.rot0, info.cx * m.scale0, info.cy * m.scale0, info.cz * m.scale0);
            Vec3 bc = { pivot.x + off.x, pivot.y + off.y, pivot.z + off.z };
            m.rel = { pivot.x - bc.x, pivot.y - bc.y, pivot.z - bc.z }; P.center = bc; P.lastCenter = bc; P.haveCenter = true;
        }
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
                } else if (!ImGui::IsMouseDown(0)) { P.drag = 0; }
                else {
                    if (P.drag <= 3) { const Vec3& a = P.drag == 1 ? g.ax : P.drag == 2 ? g.ay : g.az; float t = LineRayParam(P.dragCenter0, a, cf.pos, rd) - P.drag0; if (fabsf(t) < 200) P.center = { P.dragCenter0.x + a.x * t, P.dragCenter0.y + a.y * t, P.dragCenter0.z + a.z * t }; }
                    else if (P.drag == 4 || P.drag == 7 || P.drag == 8) {
                        // the ring axes follow the current yaw/pitch, so recompute the frame the drag started in for a stable angle reference
                        GizmoGeo g0 = GizmoAt(cf, P.center, P.m.size() == 1 ? WrapYaw(P.m[0].rot0.yaw + P.dragYaw0) : P.dragYaw0, P.m.size() == 1 ? WrapYaw(P.m[0].rot0.pitch + P.dragPitch0) : P.dragPitch0, gsize);
                        const Ring& r0 = P.drag == 7 ? g0.ring[1] : P.drag == 8 ? g0.ring[2] : g0.ring[0];
                        float ang; if (RingAngle(cf, rd, P.center, r0, &ang)) { const float d = (ang - P.drag0) * 180.0f / 3.14159265f;
                            if (P.drag == 4) P.yaw = WrapYaw(P.dragYaw0 + d); else if (P.drag == 7) P.pitch = WrapYaw(P.dragPitch0 + d); else P.roll = WrapYaw(P.dragRoll0 + d); }
                    }
                    else if (P.drag == 6) { float d = sqrtf((io.MousePos.x - g.sc.x) * (io.MousePos.x - g.sc.x) + (io.MousePos.y - g.sc.y) * (io.MousePos.y - g.sc.y)); P.scale = std::max(0.05f, std::min(10.0f, P.dragScale0 * d / P.drag0)); }
                    else { Vec3 h; if (RayPlaneY(cf.pos, rd, P.center.y, &h)) P.center = { h.x - P.dragCenter0.x, P.center.y, h.z - P.dragCenter0.z }; }
                    if (g_snap) { const float ps = kSnapPos[g_snapPosIdx], ys = kSnapYaw[g_snapYawIdx]; P.center = { SnapV(P.center.x, ps), SnapV(P.center.y, ps), SnapV(P.center.z, ps) }; P.yaw = WrapYaw(SnapV(P.yaw, ys)); P.pitch = WrapYaw(SnapV(P.pitch, ys)); P.roll = WrapYaw(SnapV(P.roll, ys)); }
                }
            }
        } else { P.hover = 0; P.drag = 0; }
        if (P.groundTicket) {
            core::GroundHit gh;
            if (core::GroundResult(P.groundTicket, &gh)) {
                P.groundTicket = 0; float groundY = 0;
                const int r = GroundStep(gh, P.center.x, P.center.z, P.groundBottom, P.groundTop, P.groundStartY, P.groundIter, &groundY);
                if (r == 1) { P.center.y += groundY - P.groundBottom; Note(T("snapped to the ground (%+.2f m)"), groundY - P.groundBottom); }
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
    static void SelectSingleUid(int uid) { if (g_place.active) DropCarried(); g_sel.clear(); g_sel.insert(uid); g_primary = g_lastClicked = uid; g_editUid = 0; }
    static bool FocusSelection() {
        if (g_sel.empty() || !core::FreeCamAvailable()) return false;
        const auto list = core::Spawned(); Vec3 c{}; int n = 0;
        for (int uid : g_sel) { const SpawnedObj* o = Find(list, uid); if (!o || o->hidden) continue; const Vec3 bc = BboxCenter(*o); c.x += bc.x; c.y += bc.y; c.z += bc.z; n++; }
        if (!n) return false; c.x /= n; c.y /= n; c.z /= n;
        float radius = 1.0f;
        for (int uid : g_sel) { const SpawnedObj* o = Find(list, uid); if (!o || o->hidden) continue; const Vec3 bc = BboxCenter(*o); const float dx = bc.x - c.x, dy = bc.y - c.y, dz = bc.z - c.z; radius = std::max(radius, sqrtf(dx * dx + dy * dy + dz * dz) + ObjectRadius(*o)); }
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
        if (!add) g_sel.clear();
        auto addOne = [&](int u) { if (g_sel.count(u)) { if (add) g_sel.erase(u); } else g_sel.insert(u); };
        const SpawnedObj* o = Find(list, uid);
        if (g_selectGroups && o && o->group > 0 && !add) { for (auto& x : list) if (x.group == o->group && !x.hidden) g_sel.insert(x.uid); }
        else addOne(uid);
        g_primary = uid; g_lastClicked = uid;
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
        for (int uid : g_sel) { const SpawnedObj* o = Find(list, uid); if (!o || o->hidden) continue; Act a; a.kind = Act::Delete; a.uid = uid; a.prefab = o->prefab; a.pos0 = o->pos; a.rot0 = o->rot; a.sc0 = o->scale; a.group = o->group; a.proj = o->proj; acts.push_back(a); core::HideUid(uid); }
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
            else if (a.kind == Act::Delete) { const int oldUid = a.uid, nu = core::SpawnAt(a.prefab, a.pos0, a.rot0, a.sc0, a.group, a.proj); if (nu) { a.uid = nu; RemapUid(oldUid, nu); g_sel.insert(nu); g_primary = nu; } }
            else if (a.kind == Act::SetGroup) core::SetGroup(a.uid, a.group);
            else moves.push_back({ a.uid, a.pos0, a.rot0, a.sc0 });
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
            else moves.push_back({ a.uid, a.pos1, a.rot1, a.sc1 });
        }
        if (!moves.empty()) core::MoveMany(moves, true);
        g_undo.push_back(std::move(entry)); Note(T("redo"));
    }
    static const char* HistoryActionName(const HistoryEntry& entry) {
        if (entry.acts.empty()) return "";
        const Act::Kind k = entry.acts[0].kind;
        for (const auto& a : entry.acts) if (a.kind != k) return T("Grab / move");
        if (k == Act::Spawn) return T("SPAWN");
        if (k == Act::Delete) return T("Delete");
        if (k == Act::SetGroup) return T("Group");
        bool pos = false, rot = false, scale = false;
        for (const auto& a : entry.acts) {
            pos |= fabsf(a.pos1.x - a.pos0.x) > 0.0001f || fabsf(a.pos1.y - a.pos0.y) > 0.0001f || fabsf(a.pos1.z - a.pos0.z) > 0.0001f;
            rot |= fabsf(a.rot1.yaw - a.rot0.yaw) > 0.0001f || fabsf(a.rot1.pitch - a.rot0.pitch) > 0.0001f || fabsf(a.rot1.roll - a.rot0.roll) > 0.0001f;
            scale |= fabsf(a.sc1 - a.sc0) > 0.0001f;
        }
        if (scale && !pos && !rot) return T("scale");
        if (rot && !pos && !scale) return T("Rotate");
        return T("Grab / move");
    }
    static void DrawHistoryAct(const Act& a) {
        const std::string name = a.prefab.empty() ? std::string() : ShortName(a.prefab);
        if (name.empty()) ImGui::Text("#%d", a.uid); else ImGui::Text("#%d  %s", a.uid, name.c_str());
        ImGui::Indent();
        auto drawPos = [&](Vec3 p0, Vec3 p1, bool arrow) {
            if (arrow) ImGui::Text("%s: %.2f, %.2f, %.2f  ->  %.2f, %.2f, %.2f", T("position"), p0.x, p0.y, p0.z, p1.x, p1.y, p1.z);
            else ImGui::Text("%s: %.2f, %.2f, %.2f", T("position"), p1.x, p1.y, p1.z);
        };
        auto drawRot = [&](Rot r0, Rot r1, bool arrow) {
            if (arrow) ImGui::Text("%s: %.1f / %.1f / %.1f  ->  %.1f / %.1f / %.1f", T("Rotate"), r0.yaw, r0.pitch, r0.roll, r1.yaw, r1.pitch, r1.roll);
            else ImGui::Text("%s: %.1f / %.1f / %.1f", T("Rotate"), r1.yaw, r1.pitch, r1.roll);
        };
        if (a.kind == Act::Move) {
            if (fabsf(a.pos1.x - a.pos0.x) > 0.0001f || fabsf(a.pos1.y - a.pos0.y) > 0.0001f || fabsf(a.pos1.z - a.pos0.z) > 0.0001f) drawPos(a.pos0, a.pos1, true);
            if (fabsf(a.rot1.yaw - a.rot0.yaw) > 0.0001f || fabsf(a.rot1.pitch - a.rot0.pitch) > 0.0001f || fabsf(a.rot1.roll - a.rot0.roll) > 0.0001f) drawRot(a.rot0, a.rot1, true);
            if (fabsf(a.sc1 - a.sc0) > 0.0001f) ImGui::Text("%s: %.3f  ->  %.3f", T("scale"), a.sc0, a.sc1);
        } else if (a.kind == Act::SetGroup) {
            ImGui::Text("%s: %d  ->  %d", T("Group"), a.group, a.group1);
        } else {
            const bool spawn = a.kind == Act::Spawn; const Vec3 p = spawn ? a.pos1 : a.pos0; const Rot r = spawn ? a.rot1 : a.rot0; const float sc = spawn ? a.sc1 : a.sc0;
            drawPos({}, p, false); drawRot({}, r, false); ImGui::Text("%s: %.3f", T("scale"), sc);
            if (a.group) ImGui::Text("%s: %d", T("Group"), a.group);
        }
        ImGui::Unindent();
    }
    static void DrawHistory() {
        ImGui::BeginDisabled(g_undo.empty()); if (ImGui::Button(T("Undo"))) Undo(); ImGui::EndDisabled(); ImGui::SameLine();
        ImGui::BeginDisabled(g_redo.empty()); if (ImGui::Button(T("Redo"))) Redo(); ImGui::EndDisabled(); ImGui::SameLine();
        ImGui::TextDisabled("%s %d / %d    %s %d", T("Undo"), (int)g_undo.size(), (int)kHistoryLimit, T("Redo"), (int)g_redo.size());
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
            if (fabsf(delta) < 0.001f) continue;
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
            if (ctrl && pC) CopySel();
            if (ctrl && pV) Paste(havePos);
            if (ctrl && pD && !g_sel.empty()) { CopySel(); Paste(havePos); }
            if (ctrl && pG) GroupSel(true);
            if (ctrl && pA) { g_sel.clear(); for (auto& o : core::Spawned()) if (!o.hidden) g_sel.insert(o.uid); }
            if (!ctrl && pDelete && !g_sel.empty()) DeleteSel();
            return;
        }
        if (io.WantTextInput || g_playMode) return;
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) Undo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) Redo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) CopySel();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) Paste(havePos);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && !g_sel.empty()) { CopySel(); Paste(havePos); }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_G, false)) GroupSel(true);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) { g_sel.clear(); for (auto& o : core::Spawned()) if (!o.hidden) g_sel.insert(o.uid); }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !g_sel.empty()) DeleteSel();
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
    // ---- NPCs and creatures: every character of the game's characterinfo, spawned through the game's own spawn request ----
    // They become ordinary actors of the world (AI, combat, own despawn rules), so they are not scene objects: no moving, undo
    // or project saving. The list is read from the installed game at runtime (thumbgen::Characters), names in the UI language.
    static char g_npcFilter[128] = ""; static int g_npcCat = 0, g_npcSel = -1, g_npcCount = 1; static float g_npcDist = 5.0f;
    static std::vector<int> g_npcRows; static std::string g_npcKey;
    static const char* kNpcCats[] = { "all", "people", "animals and mounts", "monsters", "bosses", "other" };
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
        core::SpawnNpc(c.key, at); Note(T("spawn %s"), c.name.empty() ? c.internal.c_str() : c.name.c_str());
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
    static void DrawNpcs(const PosInfo& p, bool havePos, bool compact = false) {
        const auto chars = thumbgen::Characters();
        const float ui = ImGui::GetFontSize() / 17.0f;
        const int st = core::NpcState();
        if (st == 0) { ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), T("NPC spawning is not available in this game build (see the log).")); }
        else if (st == 1) { ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), T("walk a few steps first: the game's spawn request needs your character's server actor")); }
        else if (!compact) ImGui::TextDisabled(T("Spawned characters are part of the game world: they fight, walk and despawn by the game's rules, and are not in the scene list or in projects."));
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
            g_npcKey = key; g_npcRows.clear(); g_npcSel = -1;
            std::vector<std::string> words; { std::string w; for (const char* c = g_npcFilter;; c++) { if (!*c || *c == ' ') { if (!w.empty()) words.push_back(w); w.clear(); if (!*c) break; } else w += (char)SearchFold((unsigned char)*c); } }
            for (int i = 0; i < (int)chars->size(); i++) {
                const auto& c = (*chars)[i];
                if (g_npcCat && NpcCategory(c.internal) != g_npcCat) continue;
                const std::string hay = c.name + " " + c.internal + " " + std::to_string(c.key); bool ok = true;
                for (const auto& w : words) if (!ContainsCI(hay, w)) { ok = false; break; }
                if (ok) g_npcRows.push_back(i);
            }
            std::stable_sort(g_npcRows.begin(), g_npcRows.end(), [&](int a, int b) { const auto& x = (*chars)[a]; const auto& y = (*chars)[b]; if (x.name.empty() != y.name.empty()) return !x.name.empty(); return (x.name.empty() ? x.internal : x.name) < (y.name.empty() ? y.internal : y.name); });
        }
        ImGui::TextDisabled(T("%d characters"), (int)g_npcRows.size());
        const float detailsH = (compact ? 170.0f : 118.0f) * ui;
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
            ImGui::SetNextItemWidth(compact ? 100 * ui : 140 * ui); ImGui::SliderFloat(T("distance"), &g_npcDist, 1.0f, 30.0f, "%.0f m"); SameLineOrWrap(compact, 90 * ui);
            ImGui::SetNextItemWidth(110 * ui); ImGui::InputInt(T("count"), &g_npcCount); g_npcCount = std::clamp(g_npcCount, 1, 20);
            ImGui::BeginDisabled(!havePos || st != 2);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.36f, 0.22f, 1.0f)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.45f, 0.28f, 1.0f));
            if (ImGui::Button(T(ICON_LOCATION_CROSSHAIRS "   SPAWN   "), ImVec2(150 * ui, 0))) {
                for (int k = 0; k < g_npcCount; k++) {   // side by side across the view direction, 1.5 m apart, centred
                    const float side = (k - (g_npcCount - 1) * 0.5f) * 1.5f;
                    Vec3 at = { g_lastPlayer.x + g_fx * g_npcDist + g_fz * side, g_lastPlayer.y, g_lastPlayer.z + g_fz * g_npcDist - g_fx * side };
                    core::SpawnNpc(c.key, at);
                }
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
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Stops the visual day/night and lighting progression. Gameplay, NPCs, physics and combat keep running."));
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

        ImGui::BeginDisabled(clear);
        if (ImGui::Checkbox(T("override rain"), &rainOn)) core::SetWeatherRainOverride(rainOn, rain);
        if (rainOn) { ImGui::SetNextItemWidth(compact ? -1.0f : 420.0f * ui); if (SliderFloatEdit(T("rain intensity"), &rain, 0.0f, 1.0f, "%.2f")) core::SetWeatherRainOverride(true, rain); }
        const bool snowEffects = core::WeatherSnowEffectsAvailable();
        ImGui::BeginDisabled(!snowEffects);
        if (ImGui::Checkbox(T("override snow"), &snowOn)) core::SetWeatherSnowOverride(snowOn, snow);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !snowEffects)
            ImGui::SetTooltip("%s", T("Snow particle control is not available in this game build."));
        if (snowOn) { ImGui::SetNextItemWidth(compact ? -1.0f : 420.0f * ui); if (SliderFloatEdit(T("snow intensity"), &snow, 0.0f, 1.0f, "%.2f")) core::SetWeatherSnowOverride(true, snow); }
        ImGui::EndDisabled();
        if (ImGui::Checkbox(T("override clouds"), &cloudOn)) core::SetWeatherCloudOverride(cloudOn, cloud);
        if (cloudOn) { ImGui::SetNextItemWidth(compact ? -1.0f : 420.0f * ui); if (SliderFloatEdit(T("cloud amount"), &cloud, 0.0f, 3.0f, "%.2f")) core::SetWeatherCloudOverride(true, cloud); }
        ImGui::EndDisabled();

        if (ImGui::Checkbox(T("override wind"), &windOn)) core::SetWeatherWindOverride(windOn, wind);
        if (windOn) { ImGui::SetNextItemWidth(compact ? -1.0f : 420.0f * ui); if (SliderFloatEdit(T("wind multiplier"), &wind, 0.0f, 3.0f, "x%.2f")) core::SetWeatherWindOverride(true, wind); }

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
            ImGui::TextWrapped(T("No prefab list loaded: %s\\prefabs.tsv is missing. Mod managers such as DMM install only the .asi: copy the 'cdmodkit' folder from the zip into bin64 by hand (next to cdmodkit.asi) and restart the game."), core::ModDir().c_str());
            ImGui::PopStyleColor();
        } else if (g_matches.empty()) {
            ImGui::TextDisabled(T("nothing matches: fewer words, another category, or turn off 'favorites' / 'meshes only' / the tag filters ('clear' resets all)."));
        }
        const float detailsH = ((g_cardView ? 196.0f : 206.0f) + (g_showSpawnOpts ? 40.0f : 0.0f) + (g_showMass ? 82.0f : 0.0f)) * ui;
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
        ImGui::BeginChild("details", ImVec2(0, detailsH), ImGuiChildFlags_Borders);   // scrolls if a header needs more room than reserved
        if (g_selPrefab >= 0 && g_selPrefab < (int)idx.size()) {
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
        } else ImGui::TextDisabled(T("select a prefab, then PLACE. Double-click on a row or card places it right away."));
        if (thumbgen::Ready()) { ImGui::SameLine(); ImGui::TextDisabled(T("   previews %d / %d"), thumbgen::Done(), thumbgen::Total());
            int pd = 0, pt = 0; if (thumbgen::PassProgress(&pd, &pt) && pt > 0) { ImGui::SameLine(); ImGui::TextDisabled(T("   updating %d / %d"), pd, pt); } }
        else if (thumbgen::Error()[0]) { ImGui::SameLine(); ImGui::TextDisabled(T("   previews off: %s"), thumbgen::Error()); }
        g_showSpawnOpts = ImGui::CollapsingHeader(TStable("Spawn options: offset, yaw, scale, direction"));
        if (g_showSpawnOpts) {
            ImGui::SetNextItemWidth(260); DragFloat3Edit(T("offset fwd/up/side"), g_off, 0.1f, -50, 50, "%.1f"); ImGui::SameLine();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("extra distance in front of you, height, and sideways offset; the object's footprint is already accounted for"));
            ImGui::SetNextItemWidth(150); SliderFloatEdit(T("yaw"), &g_spawnYaw, -180, 180, "%.0f"); ImGui::SameLine();
            ImGui::SetNextItemWidth(130); SliderFloatEdit(T("scale"), &g_spawnScale, 0.1f, 5.0f, "%.2f"); ImGui::SameLine();
            { Vec3 cf; bool haveCam = core::CameraPose(&cf, nullptr); ImGui::BeginDisabled(!haveCam); ImGui::Checkbox(T("front = camera view"), &g_useCamera); ImGui::EndDisabled();
              if (ImGui::IsItemHovered()) ImGui::SetTooltip(T(haveCam ? "where 'in front of you' points: the camera's view direction (on) or the direction you last walked (off)" : "camera not found yet; using the walking direction")); }
        }
        g_showMass = ImGui::CollapsingHeader(TStable("Line / circle: many copies of the selected prefab at once"));
        if (g_showMass) {
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
        if (!g_recent.empty()) {
            ImGui::TextDisabled(T(ICON_CLOCK_ROTATE_LEFT " recent:"));
            for (size_t k = 0; k < g_recent.size(); k++) {
                ImGui::SameLine(); ImGui::PushID((int)k);
                if (ImGui::SmallButton(idx[g_recent[k]].name.c_str())) g_selPrefab = g_recent[k];
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::EndChild();
    }

    static bool g_sceneCards = false; static std::set<int> g_closedGroups;
    static ImU32 GroupColor(int gid, int alpha) { const float h = fmodf(gid * 0.61803f, 1.0f); ImVec4 c = ImColor::HSV(h, 0.55f, 0.85f); return IM_COL32((int)(c.x * 255), (int)(c.y * 255), (int)(c.z * 255), alpha); }
    // members: the rows of a group header; its menu acts on the whole group even when "select groups" is off
    static void SceneObjectContext(const SpawnedObj& o, const std::vector<SpawnedObj>& list, bool havePos, const char* id, const std::vector<int>* members = nullptr) {
        if (!ImGui::BeginPopupContextItem(id)) return;
        if (members && !members->empty()) {
            bool all = true; for (int m : *members) if (!g_sel.count(list[(size_t)m].uid)) { all = false; break; }
            if (!all) { if (g_place.active) DropCarried(); g_sel.clear(); for (int m : *members) if (!list[(size_t)m].hidden) g_sel.insert(list[(size_t)m].uid); g_primary = o.uid; }
        }
        else if (!g_sel.count(o.uid)) SelectUid(o.uid, false, list);   // grouped members select their group by default
        ImGui::BeginDisabled(!core::FreeCamAvailable());
        if (ImGui::MenuItem(T("Focus"))) FocusSelection();
        ImGui::EndDisabled();
        if (ImGui::MenuItem(T("Grab"))) StartGrab(SelUids(), false, g_sel.size() == 1 ? ShortName(o.prefab) : "selection");
        if (ImGui::MenuItem(T("To ground"))) SnapSelToGround();
        if (ImGui::MenuItem(T("Duplicate"))) { CopySel(); Paste(havePos); }
        if (ImGui::MenuItem(T(o.group > 0 ? "Ungroup" : "Group selection"))) GroupSel(o.group == 0);
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
        if (ImGui::MenuItem(T("Delete"))) DeleteSel();
        ImGui::EndPopup();
    }
    // scene as tiles: loose objects first, then every group as a framed block with its own header (click = select all, arrow = collapse)
    static void DrawSceneCards(const std::vector<SpawnedObj>& list, const PosInfo& p, bool havePos, float listH, float ui) {
        const auto& idx = core::PrefabIndex();
        std::vector<int> byUid; for (int i = 0; i < (int)list.size(); i++) if (!list[i].hidden || g_showDeleted) byUid.push_back(i);
        std::sort(byUid.begin(), byUid.end(), [&](int a, int b) { return list[a].uid < list[b].uid; });
        std::vector<int> loose; std::vector<int> gorder; std::map<int, std::vector<int>> groups;
        for (int i : byUid) { const auto& o = list[i]; if (o.group > 0) { if (!groups.count(o.group)) gorder.push_back(o.group); groups[o.group].push_back(i); } else loose.push_back(i); }
        const float pad = 4.0f * ui, tile = g_cardSize * ui, textH = ImGui::GetTextLineHeight() * 2 + 3;
        const float cw = tile + 2 * pad, ch = tile + 2 * pad + textH;
        ImGui::BeginChild("scenecards", ImVec2(0, listH), ImGuiChildFlags_Borders);
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        const int cols = std::max(1, (int)((ImGui::GetContentRegionAvail().x + sp) / (cw + sp)));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        auto card = [&](int li, int n, const std::vector<int>& order) {
            const SpawnedObj& o = list[li]; const int pi = IndexOfPrefab(o.prefab);
            ImGui::PushID(o.uid);
            const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1 = { p0.x + cw, p0.y + ch };
            const ImVec2 t0 = { p0.x + pad, p0.y + pad }, t1 = { t0.x + tile, t0.y + tile };
            ImGui::InvisibleButton("scard", ImVec2(cw, ch));
            const bool hov = ImGui::IsItemHovered(), sel = g_sel.count(o.uid) > 0;
            if (ImGui::IsItemClicked(0)) {
                const int clicks = ImGui::GetMouseClickedCount(ImGuiMouseButton_Left);
                ImGuiIO& io = ImGui::GetIO();
                if (clicks >= 2) SelectSingleUid(o.uid);   // second click drills into a group member
                else if (io.KeyShift && g_lastClicked) { int a = -1, b = -1; for (int q = 0; q < n; q++) { if (list[order[q]].uid == g_lastClicked) a = q; if (list[order[q]].uid == o.uid) b = q; }
                    if (a >= 0 && b >= 0) { if (a > b) std::swap(a, b); for (int q = a; q <= b; q++) if (!list[order[q]].hidden) g_sel.insert(list[order[q]].uid); g_primary = o.uid; } }
                else SelectUid(o.uid, io.KeyCtrl, list);
                g_editUid = 0;
            }
            SceneObjectContext(o, list, havePos, "scardctx");
            dl->AddRectFilled(p0, p1, sel ? ImGui::GetColorU32(ImGuiCol_Header) : hov ? ImGui::GetColorU32(ImGuiCol_FrameBgHovered) : ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
            if (sel) dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_HeaderActive), 4.0f, 0, 2.0f);
            if (ImTextureID tex = overlay::Thumb(core::ThumbFile(o.prefab))) dl->AddImage(tex, t0, t1, ImVec2(0, 0), ImVec2(1, 1), o.hidden ? IM_COL32(255, 255, 255, 90) : IM_COL32_WHITE);
            else { dl->AddRectFilled(t0, t1, IM_COL32(0, 0, 0, 70), 3.0f); if (thumbgen::Ready() && !thumbgen::Processed(o.prefab)) thumbgen::Request(o.prefab); }
            char info[64]; float dist = 0; if (havePos) { float dx = o.pos.x - p.world.x, dy = o.pos.y - p.world.y, dz = o.pos.z - p.world.z; dist = sqrtf(dx * dx + dy * dy + dz * dz); }
            snprintf(info, sizeof info, "#%d  %.0f m%s", o.uid, dist, o.hidden ? "  (deleted)" : "");
            dl->AddText({ t0.x + 3, t0.y + 2 }, IM_COL32(255, 255, 255, 200), info);
            if (o.group > 0) dl->AddRectFilled({ t1.x - 6, t0.y }, { t1.x, t0.y + 6 }, GroupColor(o.group, 255));   // group colour mark
            dl->PushClipRect({ p0.x + pad, t1.y }, { p1.x - pad, p1.y }, true);
            const std::string cardName = pi >= 0 ? ShownName(idx[pi]) : ShortName(o.prefab);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), { p0.x + pad, t1.y + 2 }, ImGui::GetColorU32(o.hidden ? ImGuiCol_TextDisabled : ImGuiCol_Text), cardName.c_str(), nullptr, tile);
            dl->PopClipRect();
            if (hov && !ImGui::IsPopupOpen("scardctx")) ImGui::SetTooltip(T("%s\n%.1f  %.1f  %.1f   yaw %.0f   scale %.2f%s"), o.prefab.c_str(), o.pos.x, o.pos.y, o.pos.z, o.rot.yaw, o.scale, o.group > 0 ? (std::string("\n") + T("(in a group)")).c_str() : "");
            ImGui::PopID();
        };
        auto grid = [&](const std::vector<int>& order) { for (size_t k = 0; k < order.size(); k++) { if (k % cols) ImGui::SameLine(); card(order[k], (int)order.size(), order); } };
        grid(loose);
        for (int gid : gorder) {
            const auto& mem = groups[gid]; const bool closed = g_closedGroups.count(gid) > 0;
            bool allSel = true; for (int m : mem) if (!g_sel.count(list[m].uid)) { allSel = false; break; }
            ImGui::Spacing();
            const ImVec2 f0 = ImGui::GetCursorScreenPos();
            ImGui::PushID(gid);
            if (ImGui::SmallButton(closed ? ">" : "v")) { if (closed) g_closedGroups.erase(gid); else g_closedGroups.insert(gid); }
            ImGui::SameLine();
            char glbl[64]; snprintf(glbl, sizeof glbl, T("Group %d  (%d objects)"), gid, (int)mem.size());
            ImGui::PushStyleColor(ImGuiCol_Text, GroupColor(gid, 255));
            if (ImGui::Selectable(glbl, allSel)) { ImGuiIO& io = ImGui::GetIO(); if (!io.KeyCtrl) g_sel.clear(); for (int m : mem) { if (allSel && io.KeyCtrl) g_sel.erase(list[m].uid); else g_sel.insert(list[m].uid); } g_primary = list[mem[0]].uid; g_editUid = 0; }
            ImGui::PopStyleColor();
            if (ImGui::BeginPopupContextItem("gctx")) { if (ImGui::MenuItem(T("Grab group"))) { g_sel.clear(); for (int m : mem) g_sel.insert(list[m].uid); StartGrab(SelUids(), false, "group"); } if (ImGui::MenuItem(T("Ungroup"))) { g_sel.clear(); for (int m : mem) g_sel.insert(list[m].uid); GroupSel(false); } ImGui::EndPopup(); }
            if (!closed) grid(mem);
            ImGui::PopID();
            const ImVec2 f1 = { f0.x + ImGui::GetContentRegionAvail().x + ImGui::GetCursorScreenPos().x - ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y };
            dl->AddRect({ f0.x - 3, f0.y - 3 }, { f0.x + cols * (cw + sp) - sp + 3, f1.y + 1 }, GroupColor(gid, 160), 5.0f, 0, 1.5f);   // frame around the block
            ImGui::Spacing();
        }
        ImGui::EndChild();
    }
    // Scene tabs: one per project whose objects are in the world, plus "new" for everything placed by hand since the last
    // save. Clicking through them shows only that project's objects, so a loaded project can be edited and written back
    // without touching the others. -1 = everything.
    static int g_projTab = -1;
    static void DrawProjectTabs(const std::vector<SpawnedObj>& all, bool compact = false) {
        std::vector<int> ids; int newCount = 0, visible = 0;          // project ids present, in first-appearance order
        for (const auto& o : all) {
            if (o.hidden) continue;
            visible++;
            if (o.proj == 0) { newCount++; continue; }
            if (std::find(ids.begin(), ids.end(), o.proj) == ids.end()) ids.push_back(o.proj);
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
            const bool dirty = core::ProjectDirty(id);   // a star means: objects of it were moved, deleted or regrouped since the load / save
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
                if (core::SaveProject(name, core::SaveProjectOnly)) Note(T("overwrote %s (%d objects)"), name.c_str(), n);
                else Note(T("save failed"));
            }
            if (dirty) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("writes exactly the %d objects shown here back to %s.cdproj.\nObjects of other projects and new objects are not touched."), n, name.c_str());
            SameLineOrWrap(compact, 110);
            if (ImGui::SmallButton(T("Select all of them"))) { g_sel.clear(); for (const auto& o : all) if (!o.hidden && o.proj == g_projTab) g_sel.insert(o.uid); }
            SameLineOrWrap(compact, ImGui::CalcTextSize(name.c_str()).x); ImGui::TextDisabled("%s", name.c_str());
        }
    }
    static void DrawScene(const PosInfo& p, bool havePos, bool compact = false) {
        auto list = core::Spawned();
        DrawProjectTabs(list, compact);
        if (g_projTab >= 0) {   // show one project only; everything below works on uids, so a filtered copy is enough
            std::vector<SpawnedObj> f;
            for (auto& o : list) if (o.proj == g_projTab) f.push_back(o);
            list.swap(f);
        }
        // toolbar
        if (compact) g_sceneCards = true;
        if (!compact) {   // list or tiles (the dock is intentionally cards-only, like the compact Browser)
            const ImVec4 on = ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive), off = ImGui::GetStyleColorVec4(ImGuiCol_Button);
            ImGui::PushStyleColor(ImGuiCol_Button, g_sceneCards ? off : on); if (ImGui::Button(T(ICON_LIST " list"))) g_sceneCards = false; ImGui::PopStyleColor();
            SameLineOrWrap(compact, 60, 2); ImGui::PushStyleColor(ImGuiCol_Button, g_sceneCards ? on : off); if (ImGui::Button(T(ICON_COPY " cards"))) g_sceneCards = true; ImGui::PopStyleColor();
            if (g_sceneCards) { SameLineOrWrap(compact, 90); ImGui::SetNextItemWidth(90 * ImGui::GetFontSize() / 17.0f); SliderFloatEdit("##scardsize", &g_cardSize, 64.0f, 200.0f, "%.0f px"); }
            SameLineOrWrap(compact, 135);
        }
        ImGui::Checkbox(T("click selects group"), &g_selectGroups); SameLineOrWrap(compact, 95);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("off: a click on a member selects only that member (Ctrl+click always toggles single members); the group row selects all"));
        ImGui::Checkbox(T("show deleted"), &g_showDeleted); SameLineOrWrap(compact, 60);
        ImGui::Checkbox(T("snap"), &g_snap); SameLineOrWrap(compact, 80);
        ImGui::SetNextItemWidth(80); ComboT("##snappos", &g_snapPosIdx, kSnapPosNames, 5); SameLineOrWrap(compact, 80);
        ImGui::SetNextItemWidth(80); ComboT("##snapyaw", &g_snapYawIdx, kSnapYawNames, 5); SameLineOrWrap(compact, 70);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("grid and angle steps for the placement mode"));
        ImGui::SetNextItemWidth(70); DragFloatEdit("##rotstep", &g_rotationStep, 1.0f, 1.0f, 90.0f, "%.0f deg"); SameLineOrWrap(compact, 45);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("rotation step for Rotate - / Rotate +"));
        ImGui::BeginDisabled(g_undo.empty()); if (ImGui::SmallButton(T("Undo"))) Undo(); ImGui::EndDisabled(); SameLineOrWrap(compact, 45);
        ImGui::BeginDisabled(g_redo.empty()); if (ImGui::SmallButton(T("Redo"))) Redo(); ImGui::EndDisabled();
        const float ui = ImGui::GetFontSize() / 17.0f;
        const float availY = ImGui::GetContentRegionAvail().y;
        const bool hasSelection = !g_sel.empty() && Find(list, g_primary);
        const float footer = compact ? (hasSelection ? (g_sel.size() == 1 ? 238.0f : 113.0f) : 56.0f) * ui : 178.0f * ui;
        const float listH = compact ? std::max(180.0f * ui, std::max(availY * 0.48f, availY - footer)) : std::max(100.0f, availY - footer);
        if (g_sceneCards) DrawSceneCards(list, p, havePos, listH, ui);
        else {
            ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH;
            if (compact) tableFlags |= ImGuiTableFlags_ScrollX;
            if (ImGui::BeginTable("objs", 6, tableFlags, ImVec2(-1, listH))) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40); ImGui::TableSetupColumn(T("object")); ImGui::TableSetupColumn(T("grp"), ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableSetupColumn(T("position"), ImGuiTableColumnFlags_WidthFixed, 230); ImGui::TableSetupColumn(T("yaw"), ImGuiTableColumnFlags_WidthFixed, 50); ImGui::TableSetupColumn(T("dist"), ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
            static std::set<int> s_closed;   // collapsed groups
            std::map<int, std::vector<int>> groups; std::vector<int> order;   // group id -> member list indices (first appearance order)
            std::vector<int> byUid(list.size()); for (size_t k = 0; k < byUid.size(); k++) byUid[k] = (int)k;
            std::sort(byUid.begin(), byUid.end(), [&](int a, int b) { return list[a].uid < list[b].uid; });   // creation order, whatever the registry did meanwhile
            for (int i : byUid) { const auto& o = list[i]; if (o.hidden && !g_showDeleted) continue; if (o.group > 0) { if (!groups.count(o.group)) order.push_back(-o.group); groups[o.group].push_back(i); } else order.push_back(i); }
            std::vector<std::pair<int, bool>> rows;   // (list index, child) with group headers as (-group, false)
            for (int e : order) { if (e >= 0) rows.push_back({ e, false }); else { rows.push_back({ e, false }); if (!s_closed.count(-e)) for (int m : groups[-e]) rows.push_back({ m, true }); } }
            for (auto& rw : rows) {
                if (rw.first < 0) {   // group header
                    const int gid = -rw.first; const auto& mem = groups[gid]; bool allSel = true; for (int m : mem) if (!g_sel.count(list[m].uid)) { allSel = false; break; }
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    char lbl[48]; snprintf(lbl, sizeof lbl, "%s##g%d", s_closed.count(gid) ? ">" : "v", gid);
                    if (ImGui::SmallButton(lbl)) { if (s_closed.count(gid)) s_closed.erase(gid); else s_closed.insert(gid); }
                    ImGui::TableSetColumnIndex(1);
                    char glbl[64]; snprintf(glbl, sizeof glbl, T("Group %d  (%d objects)##gs%d"), gid, (int)mem.size(), gid);
                    if (ImGui::Selectable(glbl, allSel, ImGuiSelectableFlags_SpanAllColumns)) { ImGuiIO& io = ImGui::GetIO(); if (!io.KeyCtrl) g_sel.clear(); for (int m : mem) { if (allSel && io.KeyCtrl) g_sel.erase(list[m].uid); else g_sel.insert(list[m].uid); } g_primary = list[mem[0]].uid; g_lastClicked = g_primary; g_editUid = 0; }
                    ImGui::PushID(gid);
                    SceneObjectContext(list[mem[0]], list, havePos, "groupctx", &mem);
                    ImGui::PopID();
                    ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("%d", gid);
                    continue;
                }
                const int i = rw.first; const auto& o = list[i];
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                char lbl[16]; snprintf(lbl, sizeof lbl, "%d", o.uid);
                if (rw.second) ImGui::Indent(14.0f);
                if (ImGui::Selectable(lbl, g_sel.count(o.uid) > 0, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                    const int clicks = ImGui::GetMouseClickedCount(ImGuiMouseButton_Left);
                    ImGuiIO& io = ImGui::GetIO();
                    if (clicks >= 2) SelectSingleUid(o.uid);
                    else if (io.KeyShift && g_lastClicked) {   // range in list order
                        int a = -1, b = -1; for (int k = 0; k < (int)list.size(); k++) { if (list[k].uid == g_lastClicked) a = k; if (list[k].uid == o.uid) b = k; }
                        if (a >= 0 && b >= 0) { if (a > b) std::swap(a, b); for (int k = a; k <= b; k++) if (!list[k].hidden) g_sel.insert(list[k].uid); g_primary = o.uid; }
                    } else SelectUid(o.uid, io.KeyCtrl, list);
                    g_editUid = 0;
                }
                // Each row needs its own popup ID.  Reusing the same explicit
                // "rowctx" ID makes every visible row append its menu entries
                // to the one popup that was opened from a grouped child.
                ImGui::PushID(o.uid);
                SceneObjectContext(o, list, havePos, "rowctx");
                ImGui::PopID();
                if (rw.second) ImGui::Unindent(14.0f);
                ImGui::TableSetColumnIndex(1);
                std::string name = ShortName(o.prefab);
                if (o.hidden) ImGui::TextDisabled(T("%s (deleted)"), name.c_str()); else ImGui::TextUnformatted(name.c_str());
                ImGui::TableSetColumnIndex(2); if (o.group) ImGui::TextDisabled("%d", o.group);
                ImGui::TableSetColumnIndex(3); ImGui::Text("%.1f  %.1f  %.1f", o.pos.x, o.pos.y, o.pos.z);
                ImGui::TableSetColumnIndex(4); if (fabsf(o.rot.pitch) > 0.05f || fabsf(o.rot.roll) > 0.05f) ImGui::Text("%.0f*", o.rot.yaw); else ImGui::Text("%.0f", o.rot.yaw);
                ImGui::TableSetColumnIndex(5);
                if (havePos) { float dx = o.pos.x - p.world.x, dy = o.pos.y - p.world.y, dz = o.pos.z - p.world.z; ImGui::Text("%.0f m", sqrtf(dx*dx + dy*dy + dz*dz)); }
            }
            ImGui::EndTable();
            }
        }
        // drop uids that no longer exist
        for (auto it = g_sel.begin(); it != g_sel.end(); ) { if (!Find(list, *it)) it = g_sel.erase(it); else ++it; }
        const SpawnedObj* prim = Find(list, g_primary);
        if (g_sel.empty() || !prim) {
            ImGui::TextDisabled(T("select an object to edit it (Ctrl+click adds, Shift+click selects a range)"));
            ImGui::Separator();
            if (compact) ImGui::TextWrapped(T("Ctrl+Z/Y undo/redo   Ctrl+C/V copy/paste   Ctrl+D duplicate   Ctrl+G group   Ctrl+A all   Del delete   Ctrl/Shift+click multi-select"));
            else ImGui::TextDisabled(T("Ctrl+Z/Y undo/redo   Ctrl+C/V copy/paste   Ctrl+D duplicate   Ctrl+G group   Ctrl+A all   Del delete   Ctrl/Shift+click multi-select"));
            return;
        }
        ImGui::Separator();
        if (g_sel.size() == 1) {
            const auto& o = *prim;
            if (g_editUid != o.uid && g_numericEditId != 0) {
                if (const SpawnedObj* old = Find(list, g_editUid)) {
                    const Vec3 p1 = { g_edit[0], g_edit[1], g_edit[2] };
                    if (p1.x != g_editPos0.x || p1.y != g_editPos0.y || p1.z != g_editPos0.z || g_editRot.yaw != g_editRot0.yaw || g_editRot.pitch != g_editRot0.pitch ||
                        g_editRot.roll != g_editRot0.roll || g_editScale != g_editScale0) {
                        core::MoveMany({ { old->uid, p1, g_editRot, g_editScale } }, true);
                        Act a; a.kind = Act::Move; a.uid = old->uid; a.prefab = old->prefab; a.pos0 = g_editPos0; a.rot0 = g_editRot0; a.sc0 = g_editScale0; a.pos1 = p1; a.rot1 = g_editRot; a.sc1 = g_editScale; Push({ a });
                    }
                }
                CancelNumericEdit();
            }
            if (g_editUid != o.uid) { g_edit[0] = o.pos.x; g_edit[1] = o.pos.y; g_edit[2] = o.pos.z; g_editRot = o.rot; g_editScale = o.scale; g_editPos0 = o.pos; g_editRot0 = o.rot; g_editScale0 = o.scale; g_editUid = o.uid; }
            ImGui::TextDisabled("%s", o.prefab.c_str());
            bool changed = false;
            ImGui::SetNextItemWidth(300.0f); changed |= DragFloat3Edit(T("position"), g_edit, 0.05f, -100000, 100000, "%.2f");
            bool rel1 = ImGui::IsItemDeactivatedAfterEdit() || NumericEditEnded(T("position")); SameLineOrWrap(compact, 150.0f);
            ImGui::SetNextItemWidth(150.0f); changed |= SliderFloatEdit(T("yaw##e"), &g_editRot.yaw, -180, 180, "%.0f");
            bool rel2 = ImGui::IsItemDeactivatedAfterEdit() || NumericEditEnded(T("yaw##e")); SameLineOrWrap(compact, 130.0f);
            ImGui::SetNextItemWidth(130.0f); changed |= SliderFloatEdit(T("scale##e"), &g_editScale, 0.05f, 10.0f, "%.2f");
            bool rel3 = ImGui::IsItemDeactivatedAfterEdit() || NumericEditEnded(T("scale##e"));
            SameLineOrWrap(compact, 150.0f);
            ImGui::SetNextItemWidth(150.0f); changed |= SliderFloatEdit(T("tilt X (pitch)##e"), &g_editRot.pitch, -180, 180, "%.0f");
            bool rel4 = ImGui::IsItemDeactivatedAfterEdit() || NumericEditEnded(T("tilt X (pitch)##e")); SameLineOrWrap(compact, 150.0f);
            ImGui::SetNextItemWidth(150.0f); changed |= SliderFloatEdit(T("tilt Z (roll)##e"), &g_editRot.roll, -180, 180, "%.0f");
            bool rel5 = ImGui::IsItemDeactivatedAfterEdit() || NumericEditEnded(T("tilt Z (roll)##e")); SameLineOrWrap(compact, 45);
            if (ImGui::SmallButton(T("level"))) { g_editRot.pitch = g_editRot.roll = 0; changed = true; rel4 = true; }
            bool released = rel1 || rel2 || rel3 || rel4 || rel5;
            bool applyBtn = ImGui::Button(T(ICON_CIRCLE_CHECK " Apply")); SameLineOrWrap(compact, 90);
            if (ImGui::Button(T(ICON_LOCATION_DOT " To player")) && havePos) { int pi = IndexOfPrefab(o.prefab); Vec3 at = pi >= 0 ? SpawnSpot(core::PrefabIndex()[pi], g_editRot.yaw, g_editScale) : InFront(1, 0); g_edit[0] = at.x; g_edit[1] = at.y; g_edit[2] = at.z; applyBtn = true; }
            static DWORD s_lastLive = 0; const DWORD now = GetTickCount();
            bool live = changed && g_live && now - s_lastLive >= 50;
            if (live) s_lastLive = now;
            if (live || released || applyBtn) {
                core::MoveMany({ { o.uid, { g_edit[0], g_edit[1], g_edit[2] }, g_editRot, g_editScale } }, released || applyBtn);
                if (released || applyBtn) { Act a; a.kind = Act::Move; a.uid = o.uid; a.prefab = o.prefab; a.pos0 = g_editPos0; a.rot0 = g_editRot0; a.sc0 = g_editScale0; a.pos1 = { g_edit[0], g_edit[1], g_edit[2] }; a.rot1 = g_editRot; a.sc1 = g_editScale; Push({ a });
                    g_editPos0 = a.pos1; g_editRot0 = a.rot1; g_editScale0 = a.sc1; }
            }
        } else {
            ImGui::Text(T("%d objects selected"), (int)g_sel.size()); ImGui::SameLine();
        }
        if (ImGui::Button(T(ICON_HAND " Grab"))) StartGrab(SelUids(), false, g_sel.size() == 1 ? ShortName(prim->prefab) : "selection");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("carry the selection and edit it with the mouse gizmo"));
        SameLineOrWrap(compact, 65); ImGui::BeginDisabled(!core::FreeCamAvailable()); if (ImGui::Button(T("Focus"))) FocusSelection(); ImGui::EndDisabled();
        SameLineOrWrap(compact, 75); if (ImGui::Button(T("To ground"))) SnapSelToGround(); if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("drops the selected objects onto the surface below them (a sphere cast of the game's physics)"));
        SameLineOrWrap(compact, 85); if (ImGui::Button(T(ICON_COPY " Duplicate"))) { CopySel(); Paste(havePos); }
        SameLineOrWrap(compact, 55); if (ImGui::Button(T("Group"))) GroupSel(true);
        SameLineOrWrap(compact, 65); if (ImGui::Button(T("Ungroup"))) GroupSel(false);
        SameLineOrWrap(compact, 70); if (ImGui::Button(T("Rotate -"))) RotateSel(-g_rotationStep);
        SameLineOrWrap(compact, 70); if (ImGui::Button(T("Rotate +"))) RotateSel(g_rotationStep);
        ImGui::BeginDisabled(g_sel.size() < 2);
        SameLineOrWrap(compact, 60); if (ImGui::Button(T("Align X"))) AlignSel(0);
        SameLineOrWrap(compact, 60); if (ImGui::Button(T("Align Y"))) AlignSel(1);
        SameLineOrWrap(compact, 60); if (ImGui::Button(T("Align Z"))) AlignSel(2);
        ImGui::EndDisabled();
        SameLineOrWrap(compact, 70);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.15f, 0.12f, 1)); if (ImGui::Button(T(ICON_TRASH " Delete"))) DeleteSel(); ImGui::PopStyleColor();
        SameLineOrWrap(compact, 55); if (ImGui::Button(T("Forget"))) { for (int uid : g_sel) core::ForgetUid(uid); g_sel.clear(); g_primary = 0; }
        SameLineOrWrap(compact, 120); if (ImGui::Button(T("Remove duplicates"))) { const int n = SelectDuplicates(); if (n) DeleteSel(); else Note(T("no duplicates found")); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("deletes every object that sits exactly on an identical one (same prefab, position, rotation, scale),\ne.g. after a project was loaded twice. The earlier copy stays. Ctrl+Z brings them back."));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("drops the entries from the list without touching the world"));
        ImGui::Separator();
        if (compact) ImGui::TextWrapped(T("Ctrl+Z/Y undo/redo   Ctrl+C/V copy/paste   Ctrl+D duplicate   Ctrl+G group   Ctrl+A all   Del delete   Ctrl/Shift+click multi-select"));
        else ImGui::TextDisabled(T("Ctrl+Z/Y undo/redo   Ctrl+C/V copy/paste   Ctrl+D duplicate   Ctrl+G group   Ctrl+A all   Del delete   Ctrl/Shift+click multi-select"));
    }

    static void DrawProject() {
        static char s_name[64] = "mybuild";
        static std::vector<std::string> s_list, s_auto; static DWORD s_listAt = 0;
        if (GetTickCount() - s_listAt > 2000) { s_list = core::ListProjects(); s_auto = core::Autoload(); s_listAt = GetTickCount(); }
        // what Save writes: the whole world, or only what was placed by hand since the last save (the "new" tab in the scene)
        static int s_scope = 0;
        const int newCount = core::ProjectObjectCount(0);
        ImGui::SetNextItemWidth(250); ImGui::InputText(T("name"), s_name, sizeof s_name); ImGui::SameLine();
        ImGui::BeginDisabled(!s_name[0] || (s_scope == 1 && newCount == 0));
        if (ImGui::Button(T(ICON_FLOPPY_DISK " Save"))) {
            const int scope = s_scope == 1 ? core::SaveNewOnly : core::SaveWholeScene;
            const int n = s_scope == 1 ? newCount : (int)core::Spawned().size();
            if (core::SaveProject(s_name, scope)) Note(T("saved %s (%d objects)"), s_name, n); else Note(T("save failed"));
            s_listAt = 0;
        }
        ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::SetNextItemWidth(220);
        char scopeNew[64]; snprintf(scopeNew, sizeof scopeNew, T("only the new objects (%d)"), newCount);
        const char* scopes[2] = { "everything in the scene", scopeNew };
        ComboT("##savescope", &s_scope, scopes, 2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("everything: the whole world including loaded projects - they all become part of this one project.\nonly the new objects: what you placed by hand since the last save, so a loaded project stays a project of its own.\nEither way the saved objects belong to this project afterwards, and the scene tab shows them under its name."));
        ImGui::SameLine();
        // Save always writes the whole scene, so a fresh project starts from an empty world
        if (ImGui::Button(T(ICON_CUBE " New"))) ImGui::OpenPopup("newproj");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("empty scene for a new project: removes everything World Builder has placed.\nSaved .cdproj files on disk are not touched."));
        if (ImGui::BeginPopupModal(TStable("New project###newproj"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped(T("Remove all %d placed objects and start an empty project?"), (int)core::Spawned().size());
            ImGui::TextDisabled(T("Saved projects stay on disk. Undo cannot bring the objects back."));
            ImGui::Separator();
            if (ImGui::Button(T(ICON_TRASH " Empty the scene"))) {
                core::DeleteAllSpawned(); g_sel.clear(); g_undo.clear(); g_redo.clear(); s_name[0] = 0;
                Note(T("new project: scene emptied - place your objects, then save under a new name"));
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(); if (ImGui::Button(T("Cancel"))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Import .cdproj"))) {   // copies a shared project file into the projects folder
            char file[MAX_PATH] = { 0 }; OPENFILENAMEA ofn = {}; ofn.lStructSize = sizeof ofn; ofn.lpstrFilter = "World Builder project (*.cdproj)\0*.cdproj\0All files\0*.*\0"; ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameA(&ofn)) { std::string src = file; std::string base = src.substr(src.find_last_of("\\/") + 1); std::string dst = core::ModDir() + "\\projects\\" + base; CreateDirectoryA((core::ModDir() + "\\projects").c_str(), nullptr);
                if (CopyFileA(src.c_str(), dst.c_str(), FALSE)) Note(T("imported %s"), base.c_str()); else Note(T("import failed")); s_listAt = 0; }
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Open folder"))) ShellExecuteA(nullptr, "open", (core::ModDir() + "\\projects").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("projects are plain text files with absolute world coordinates; share the .cdproj, others import it here"));
        ImGui::Separator();
        ImGui::TextDisabled(T("saved projects (bin64\\cdmodkit\\projects)"));
        if (ImGui::BeginTable("proj", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn(T("project"), ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 110); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 100); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 100);
            for (auto& pr : s_list) {
                ImGui::PushID(pr.c_str()); ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                {   // same marks as the scene tabs: how many of its objects are in the world, and whether they differ from the file
                    const int rowPid = core::ProjectId(pr); const int rowN = core::ProjectObjectCount(rowPid);
                    if (rowN && core::ProjectDirty(rowPid)) { ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.35f, 1), "%s *", pr.c_str()); if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("%d objects of this project are in the scene and were changed since the last save"), rowN); }
                    else if (rowN) { ImGui::Text("%s", pr.c_str()); ImGui::SameLine(); ImGui::TextDisabled(T("(%d in the scene)"), rowN); }
                    else ImGui::TextUnformatted(pr.c_str());
                }
                ImGui::TableSetColumnIndex(1); if (ImGui::SmallButton(T("Load"))) { core::LoadProject(pr, false); Note(T("loading %s (added to the scene)"), pr.c_str()); } if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("adds the project to what is already placed; Replace all removes the current objects first"));
                ImGui::TableSetColumnIndex(2); if (ImGui::SmallButton(T("Replace all"))) { core::LoadProject(pr, true); g_undo.clear(); g_redo.clear(); Note(T("replaced by %s"), pr.c_str()); }
                ImGui::TableSetColumnIndex(3);
                {   // a project that is loaded in the scene is written back with its own objects plus whatever is new;
                    // one that is not loaded would otherwise be overwritten with an unrelated scene, so that needs the Save button
                    const int pid = core::ProjectId(pr); const int inScene = core::ProjectObjectCount(pid);
                    if (ImGui::SmallButton(T(inScene ? "Save back" : "Overwrite"))) {
                        const int scope = inScene ? core::SaveProjectAndNew : core::SaveWholeScene;
                        if (core::SaveProject(pr, scope)) Note(T("overwrote %s"), pr.c_str()); else Note(T("save failed"));
                        strcpy_s(s_name, pr.c_str());
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(T(inScene ? "writes the %d objects of this project plus the new ones back into it; other projects stay untouched"
                                                                          : "this project is not in the scene: Overwrite would replace it with everything that is placed right now"), inScene);
                }
                ImGui::TableSetColumnIndex(4); bool isAuto = std::find(s_auto.begin(), s_auto.end(), pr) != s_auto.end();   // several projects may be ticked at once
                if (ImGui::Checkbox(T("autoload"), &isAuto)) { core::SetAutoload(pr, isAuto); s_auto = core::Autoload(); }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("load this project automatically at game start; you can tick as many as you like, they are all placed into the same scene"));
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (s_list.empty()) ImGui::TextDisabled(T("(none yet)"));
        ImGui::Separator();
        ImGui::Text(T("objects in scene: %d   spawns pending: %d"), (int)core::Spawned().size(), core::PendingSpawns());
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.15f, 0.12f, 1)); if (ImGui::Button(T(ICON_TRASH " Delete all objects"))) { core::DeleteAllSpawned(); g_sel.clear(); g_undo.clear(); g_redo.clear(); Note(T("deleted all")); } ImGui::PopStyleColor();
        if (s_auto.empty()) ImGui::TextDisabled(T("autoload: off. Tick as many projects as you like; they all spawn ~3 s after the player is in the world."));
        else {
            std::string names, missing;
            for (size_t i = 0; i < s_auto.size(); i++) {
                if (i) names += ", ";
                names += s_auto[i];
                if (std::find(s_list.begin(), s_list.end(), s_auto[i]) == s_list.end()) { if (!missing.empty()) missing += ", "; missing += s_auto[i]; }   // listed but no .cdproj (renamed / deleted)
            }
            ImGui::TextDisabled(T("autoload (%d): %s - spawned ~3 s after the player is in the world (file: bin64\\cdmodkit\\autoload.txt, one name per line)"), (int)s_auto.size(), names.c_str());
            if (!missing.empty()) ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1), T("autoload lists projects that do not exist any more: %s - remove those lines from autoload.txt"), missing.c_str());
        }
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
    static void UpdateBoxSelection(const CamFrame& cf, const std::vector<SpawnedObj>& list, ImGuiIO& io) {
        if (!g_boxSelecting) return;
        if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) && !ImGui::IsAnyItemHovered()) g_boxCurrent = io.MousePos;
        const float dx = g_boxCurrent.x - g_boxStart.x, dy = g_boxCurrent.y - g_boxStart.y;
        if (io.MouseDown[ImGuiMouseButton_Left] && dx * dx + dy * dy >= 25.0f) g_boxMoved = true;
        if (g_boxMoved && (io.MouseDown[ImGuiMouseButton_Left] || ImGui::IsMouseReleased(ImGuiMouseButton_Left))) {
            const ImVec2 mn(std::min(g_boxStart.x, g_boxCurrent.x), std::min(g_boxStart.y, g_boxCurrent.y));
            const ImVec2 mx(std::max(g_boxStart.x, g_boxCurrent.x), std::max(g_boxStart.y, g_boxCurrent.y));
            std::set<int> next;
            for (const auto& o : list) {
                if (o.hidden || IsCarried(o.uid)) continue;
                ImVec2 a, b; float depth = 0;
                if (!ObjScreenRect(cf, o, &a, &b, &depth) || depth <= 0) continue;
                if (a.x <= mx.x && b.x >= mn.x && a.y <= mx.y && b.y >= mn.y) next.insert(o.uid);
            }
            if (g_selectGroups) {
                std::set<int> groups;
                for (const auto& o : list) if (next.count(o.uid) && o.group > 0) groups.insert(o.group);
                for (const auto& o : list) if (!o.hidden && groups.count(o.group)) next.insert(o.uid);
            }
            if (g_boxAdd) next.insert(g_boxBase.begin(), g_boxBase.end());
            g_sel.swap(next);
            if (!g_sel.count(g_primary)) g_primary = g_sel.empty() ? 0 : *g_sel.begin();
            g_lastClicked = g_primary; g_editUid = 0;
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            dl->AddRectFilled(mn, mx, IM_COL32(65, 165, 230, 36));
            dl->AddRect(mn, mx, IM_COL32(115, 205, 255, 230), 0.0f, 0, 1.5f);
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (!g_boxMoved && !g_boxAdd) { g_sel.clear(); g_primary = g_lastClicked = g_editUid = 0; }
            g_boxSelecting = g_boxMoved = g_boxAdd = false; g_boxBase.clear();
        }
    }
    static void ClickSelect(const PosInfo& p, bool havePos) {
        (void)p; (void)havePos;
        g_hoverUid = 0;
        ImGuiIO& io = ImGui::GetIO();
        if (g_browserDragPrefab >= 0) return;   // a browser drag owns LMB until it is dropped or cancelled
        const bool overUi = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || ImGui::IsAnyItemHovered();
        const bool addSelect = g_cameraMode ? ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) : io.KeyCtrl;
        const bool addBox = g_cameraMode ? (((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_SHIFT)) & 0x8000) != 0) : (io.KeyCtrl || io.KeyShift);
        const bool placing = g_place.active;
        if (g_playMode) return;
        auto list = core::Spawned();
        const int leftClicks = ImGui::GetMouseClickedCount(ImGuiMouseButton_Left);
        if (g_rightGesture) {
            if (io.MouseDown[ImGuiMouseButton_Right]) {
                const float dx = io.MousePos.x - g_rightStart.x, dy = io.MousePos.y - g_rightStart.y;
                if (dx * dx + dy * dy > 16.0f) g_rightMoved = true;
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
                if (!g_rightMoved && g_rightUid) {
                    if (IsCarried(g_rightUid)) {   // opening the menu on the active gizmo must not drop the object
                        g_sel.clear(); for (const auto& m : g_place.m) g_sel.insert(m.uid);
                        g_primary = g_lastClicked = g_rightUid; g_editUid = 0;
                    } else if (!g_sel.count(g_rightUid)) SelectUid(g_rightUid, false, list);
                    g_worldPopupRequested = true; g_worldPopupPos = io.MousePos;
                }
                g_rightGesture = g_rightMoved = false; g_rightUid = 0;
            }
        }
        if (overUi && !g_boxSelecting) return;
        // Camera look must not depend on projection/selection being available. Start the RMB gesture
        // as soon as the click happens in world space; object picking below only fills in its context target.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            g_rightGesture = true; g_rightMoved = false; g_rightStart = io.MousePos; g_rightUid = 0;
            if (placing && !g_place.m.empty()) {
                g_rightUid = IsCarried(g_primary) ? g_primary : g_place.m.front().uid;   // gizmo/current carried set is the default context target
            }
        }
        CamFrame cf = CurrentCam();
        if (!cf.ok) {
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) { g_boxSelecting = g_boxMoved = g_boxAdd = false; g_boxBase.clear(); }
            return;
        }
        if (g_boxSelecting) {
            UpdateBoxSelection(cf, list, io);
            if (g_boxSelecting || overUi) return;
        }
        if (placing && (g_place.hover || g_place.drag)) return;   // upstream gizmo keeps first priority

        float bestDepth = 1e30f; int best = 0;
        for (const auto& o : list) {
            if (o.hidden || IsCarried(o.uid)) continue; ImVec2 mn, mx; float d;
            if (!ObjScreenRect(cf, o, &mn, &mx, &d) || d <= 0) continue;
            if (io.MousePos.x < mn.x || io.MousePos.x > mx.x || io.MousePos.y < mn.y || io.MousePos.y > mx.y) continue;
            if (d < bestDepth) { bestDepth = d; best = o.uid; }
        }
        g_hoverUid = best;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            if (best) g_rightUid = best;   // otherwise keep the carried gizmo as the context target
            return;
        }
        if (!best) {
            if (placing) { if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) DropCarried(); return; }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                g_boxSelecting = true; g_boxMoved = false; g_boxAdd = addBox;
                g_boxStart = g_boxCurrent = io.MousePos; g_boxBase = g_boxAdd ? g_sel : std::set<int>{};
            }
            return;
        }
        if (leftClicks >= 2) {
            g_sel.clear(); g_sel.insert(best); g_primary = g_lastClicked = best; g_editUid = 0;
            StartGrab({ best }, false, ShortName(Find(list, best)->prefab)); return;
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { SelectUid(best, addSelect, list); g_editUid = 0; }
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
    static void DrawWorldContextPopup(bool havePos) {
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(1, 1), ImGuiCond_Always);
        const ImGuiWindowFlags hostFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
        ImGui::Begin("##worldctxhost", nullptr, hostFlags);
        if (g_worldPopupRequested) {
            g_worldPopupRequested = false;
            ImGui::OpenPopup("worldctx");
            ImGui::SetNextWindowPos(g_worldPopupPos, ImGuiCond_Appearing);
        }
        g_worldPopupOpen = false;
        if (ImGui::BeginPopup("worldctx")) {
            g_worldPopupOpen = true;
            const bool hasSel = !g_sel.empty();
            bool hasGroup = false;
            const auto list = core::Spawned();
            for (int uid : g_sel) { const SpawnedObj* o = Find(list, uid); if (o && !o->hidden && o->group > 0) { hasGroup = true; break; } }
            auto settlePlacement = []() { if (g_place.active) DropCarried(); };
            ImGui::BeginDisabled(!hasSel);
            ImGui::BeginDisabled(!core::FreeCamAvailable()); if (ImGui::MenuItem(T("Focus"))) FocusSelection(); ImGui::EndDisabled();
            if (ImGui::MenuItem(T("Grab"))) StartGrab(SelUids(), false, g_sel.size() == 1 ? "object" : "selection");
            if (ImGui::MenuItem(T("To ground"))) { settlePlacement(); SnapSelToGround(); }
            if (ImGui::MenuItem(T("Duplicate"))) { settlePlacement(); CopySel(); Paste(havePos); }
            if (ImGui::MenuItem(T("Group selection"))) { settlePlacement(); GroupSel(true); }
            if (ImGui::MenuItem(T("Ungroup"), nullptr, false, hasGroup)) { settlePlacement(); GroupSel(false); }
            if (ImGui::BeginMenu(T("Rotate"))) {
                if (ImGui::MenuItem(T("Rotate left"))) { settlePlacement(); RotateSel(-g_rotationStep); }
                if (ImGui::MenuItem(T("Rotate right"))) { settlePlacement(); RotateSel(g_rotationStep); }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu(T("Align to primary"))) {
                if (ImGui::MenuItem(T("X"))) { settlePlacement(); AlignSel(0); }
                if (ImGui::MenuItem(T("Y"))) { settlePlacement(); AlignSel(1); }
                if (ImGui::MenuItem(T("Z"))) { settlePlacement(); AlignSel(2); }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem(T("Delete"))) { settlePlacement(); DeleteSel(); }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
        ImGui::End();
    }
    static void DrawSelectionOutlines() {
        if (g_sel.empty() && !g_hoverUid) return;
        CamFrame cf = CurrentCam(); if (!cf.ok) return;
        ImDrawList* dl = ImGui::GetForegroundDrawList(); auto list = core::Spawned();
        for (const auto& o : list) {
            const bool sel = g_sel.count(o.uid) > 0, hov = o.uid == g_hoverUid; if (o.hidden || (!sel && !hov)) continue;
            float cx = 0, cy = 0.5f, cz = 0, sx = 1, sy = 1, sz = 1; int pi = IndexOfPrefab(o.prefab);
            if (pi >= 0) { const auto& info = core::PrefabIndex()[pi]; if (info.hasCenter) { cx = info.cx; cy = info.cy; cz = info.cz; } if (info.sx > 0) { sx = info.sx; sy = info.sy; sz = info.sz; } }
            ImVec2 sp[8]; bool ok[8];
            for (int k = 0; k < 8; k++) ok[k] = WorldToScreen(cf, LocalToWorld(o, cx + ((k & 1) ? sx : -sx) * 0.5f, cy + ((k & 2) ? sy : -sy) * 0.5f, cz + ((k & 4) ? sz : -sz) * 0.5f), &sp[k]);
            static const int edges[12][2] = { {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7} };
            const ImU32 col = sel ? IM_COL32(240, 140, 80, 230) : IM_COL32(255, 255, 255, 120);
            for (auto& e : edges) if (ok[e[0]] && ok[e[1]]) dl->AddLine(sp[e[0]], sp[e[1]], col, sel ? 2.0f : 1.0f);
        }
    }
    // narrow dock: search, one column of cards, PLACE. Same matches and filters as the full editor.
    static void DrawCompact(const PosInfo& p, bool havePos) {
        ImGuiIO& io = ImGui::GetIO(); const float ui = ImGui::GetFontSize() / 17.0f;
        ImGui::SetNextWindowSize(ImVec2(300.0f * ui, io.DisplaySize.y - 80.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 320.0f * ui, 40.0f), ImGuiCond_FirstUseEver);
        if (g_playMode) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.45f);
        char title[160]; snprintf(title, sizeof title, T("World Builder [%s]###cdmodkit_dock"), T(g_cameraMode ? "CAMERA" : (g_playMode ? "PLAY" : "EDIT")));
        const bool began = ImGui::Begin(title, &g_open, g_playMode ? ImGuiWindowFlags_NoInputs : 0);
        if (!began) { ImGui::End(); if (g_playMode) ImGui::PopStyleVar(); return; }
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
        ImGui::TextDisabled(T("%s = %s   %s = hide"), core::KeyName(core::g_keyMode), T(g_cameraMode ? "exit camera" : (g_playMode ? "edit" : "camera")), core::KeyName(core::g_keyToggle));
        struct DockPage { int id; const char* label; };
        static const DockPage dockPages[] = {
            { TabBrowser, ICON_MAGNIFYING_GLASS " Browser" },
            { TabScene, ICON_CUBE " Scene" },
            { TabNpcs, ICON_LOCATION_DOT " NPCs" },
            { TabEnvironment, ICON_CLOCK_ROTATE_LEFT " Time & Weather" },
        };
        for (int i = 0; i < (int)(sizeof(dockPages) / sizeof(dockPages[0])); ++i) {
            if (i && i != 2) ImGui::SameLine(0, 4);   // two compact rows: Browser/Scene, NPCs/Time & Weather
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
            if (g_playMode) ImGui::PopStyleVar();
            return;
        }
        if (g_compactPage == TabNpcs) {
            if (g_previewShown) { core::PreviewClear(); g_previewShown = false; }
            DrawNpcs(p, havePos, true);
            ImGui::End();
            if (g_playMode) ImGui::PopStyleVar();
            return;
        }
        if (g_compactPage == TabEnvironment) {
            if (g_previewShown) { core::PreviewClear(); g_previewShown = false; }
            DrawEnvironment(true);
            ImGui::End();
            if (g_playMode) ImGui::PopStyleVar();
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
        const float footer = (ImGui::GetFrameHeightWithSpacing() * 2 + 8) ;
        const float listH = std::max(80.0f, ImGui::GetContentRegionAvail().y - footer);
        {   // tile size from the window width: inner width of the bordered, scrollable child divided by the columns
            const ImGuiStyle& st = ImGui::GetStyle();
            const float inner = ImGui::GetContentRegionAvail().x - 2 * st.WindowPadding.x - st.ScrollbarSize - 2.0f;
            const float tile = floorf((inner - (g_dockCols - 1) * st.ItemSpacing.x) / g_dockCols) - 2 * 4.0f * ui;
            DrawCards(p, havePos, listH, ui, g_dockCols, std::max(32.0f, tile));
        }
        if (g_selPrefab >= 0 && g_selPrefab < (int)core::PrefabIndex().size()) {
            const auto& pi = core::PrefabIndex()[g_selPrefab];
            ImGui::TextDisabled("%s", ShownName(pi).c_str());
            ImGui::BeginDisabled(!havePos || !core::GameThreadReady());
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.36f, 0.22f, 1.0f)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.45f, 0.28f, 1.0f));
            if (ImGui::Button(T(ICON_LOCATION_CROSSHAIRS "   PLACE   "), ImVec2(-1, 0))) StartPlaceNew(p, havePos);
            ImGui::PopStyleColor(2); ImGui::EndDisabled();
        } else { ImGui::TextDisabled(T("pick a card, then PLACE")); ImGui::Dummy(ImVec2(0, ImGui::GetFrameHeight())); }
        ProcessBrowserDrag();
        ImGui::End();
        if (g_playMode) ImGui::PopStyleVar();
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
        const float up = movementAllowed ? ((down(VK_SPACE) ? 1.0f : 0.0f) - (ctrl && !ctrlCommand ? 1.0f : 0.0f)) : 0.0f;
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
        const bool modMouse = g_open ? !g_playMode : g_place.active;
        ImGui::SetNextWindowPos(ImVec2(10.0f, 8.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.75f);
        if (ImGui::Begin("##focushud", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
            const ImVec4 mod(0.95f, 0.62f, 0.35f, 1.0f), game(0.55f, 0.85f, 0.55f, 1.0f);
            ImGui::TextColored(modMouse ? mod : game, T(modMouse ? "MOUSE: World Builder" : "MOUSE: game"));
            ImGui::SameLine(); ImGui::TextDisabled("  |  ");
            ImGui::SameLine(); ImGui::TextColored(g_cameraMode ? mod : game, T(g_cameraMode ? "KEYS: camera" : "KEYS: game"));
            if (g_place.active) { ImGui::SameLine(); ImGui::TextColored(mod, T("  placement: mouse gizmo")); }
            if (g_open) { ImGui::SameLine(); ImGui::TextDisabled("     %s = %s", core::KeyName(core::g_keyMode), T(g_cameraMode ? "exit camera mode" : "camera mode")); }
            if (g_open && !g_playMode && io.WantTextInput) { ImGui::SameLine(); ImGui::TextColored(mod, T("   typing: keys go to the text field")); }
        }
        ImGui::End();
        (void)io;
    }
    void Draw() {
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
        if (g_place.active) DrawPlaceHud();
        if (g_place.active && (g_gizmo || MouseMode())) { const bool one = g_place.m.size() == 1; const CamFrame cf = CurrentCam();
            DrawGizmo(g_place.center, one ? WrapYaw(g_place.m[0].rot0.yaw + g_place.yaw) : g_place.yaw, one ? WrapYaw(g_place.m[0].rot0.pitch + g_place.pitch) : g_place.pitch, GizmoScreenSize(cf, g_place.center, g_place.radius), g_place.drag ? g_place.drag : g_place.hover); }
        if (g_place.active && !g_open) ImGui::GetIO().MouseDrawCursor = true;
        DrawCalibrationMarker(p, havePos);
        if (!g_open) { if (g_cameraMode) StopCameraMode(); return; }
        if (g_numericEditId && g_numericEditLastSeenFrame >= 0 && ImGui::GetFrameCount() - g_numericEditLastSeenFrame > 1) CancelNumericEdit();
        ImGuiIO& io = ImGui::GetIO();
        io.MouseDrawCursor = !g_playMode;
        ClickSelect(p, havePos); DrawWorldContextPopup(havePos); DrawSelectionOutlines();
        if (g_compact) { DrawCompact(p, havePos); CameraTick(); return; }
        {   // initial size follows the UI scale (style is scaled by screen height / 1080) and stays inside the screen
            const float ui = ImGui::GetFontSize() / 17.0f;
            ImVec2 want(1320.0f * ui, 800.0f * ui);
            want.x = std::min(want.x, io.DisplaySize.x - 60.0f); want.y = std::min(want.y, io.DisplaySize.y - 60.0f);
            ImGui::SetNextWindowSize(want, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2(30, 30), ImGuiCond_FirstUseEver);
        }
        if (g_playMode) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.45f);
        char title[240]; snprintf(title, sizeof title, T("World Builder v%s [%s] %s = %s, %s = hide###cdmodkit"), kEditorVersion, T(g_cameraMode ? "CAMERA MODE" : (g_playMode ? "PLAY MODE" : "EDIT MODE")), core::KeyName(core::g_keyMode), T(g_cameraMode ? "exit camera mode" : (g_playMode ? "back to editing" : "camera mode")), core::KeyName(core::g_keyToggle));
        const bool began = ImGui::Begin(title, &g_open, g_playMode ? ImGuiWindowFlags_NoInputs : 0);
        if (!began) { ImGui::End(); if (g_playMode) ImGui::PopStyleVar(); CameraTick(); return; }
        HandleHotkeys(havePos);
        if (!core::BuildOk()) {
            ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), T("Game functions not resolved: %s"), core::BuildMessage()[0] ? core::BuildMessage() : "verification pending");
            ImGui::TextWrapped(T("Spawning is disabled to avoid crashes. See bin64\\cdmodkit\\cdmodkit.log (RESOLVE FAILED)."));
            ImGui::TextWrapped(T("Game build %s - a patch moves the game's functions; report that build number so the signatures can be updated."), core::GameVersion()[0] ? core::GameVersion() : "unknown");
        }
        if (core::PrefabIndex().empty()) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.45f, 0.12f, 0.10f, 0.85f));
            ImGui::BeginChild("noindex", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 4.2f), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
            ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), T(ICON_XMARK "  Prefab list not found: the browser and the search stay empty."));
            ImGui::TextWrapped(T("Expected file: %s\\prefabs.tsv. It is part of the download (bin64\\cdmodkit\\prefabs.tsv). Mod managers such as DMM install only cdmodkit.asi and skip this folder."), core::ModDir().c_str());
            ImGui::TextWrapped(T("Fix: open the World Builder zip, copy the folder 'cdmodkit' from its bin64 folder into <game>\\bin64\\ next to cdmodkit.asi, then restart the game."));
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
                ? "Free camera: W/A/S/D move, E or Space up, Q or Ctrl down, Shift faster, mouse wheel forward. Drag with the right mouse button over the world to look around; a right click without moving opens the context menu. Your character stays where it is; new objects appear in front of the camera."
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
                char langLabel[96]; snprintf(langLabel, sizeof langLabel, strcmp(T("Language"), "Language") ? "%s / Language###language" : "%s###language", T("Language"));
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
                keyCombo("show / hide the editor", &core::g_keyToggle);
                keyCombo("camera mode", &core::g_keyMode);
                ImGui::SetNextItemWidth(160); SliderFloatEdit(T("free camera speed"), &core::g_fcSpeed, 1.0f, 100.0f, "%.0f m/s"); if (ImGui::IsItemDeactivatedAfterEdit() || NumericEditEnded(T("free camera speed"))) core::SaveSettings();
                ImGui::SameLine(); ImGui::SetNextItemWidth(160); SliderFloatEdit(T("mouse sensitivity"), &core::g_fcSens, 0.02f, 0.5f, "%.2f"); if (ImGui::IsItemDeactivatedAfterEdit() || NumericEditEnded(T("mouse sensitivity"))) core::SaveSettings();
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
                    ImGui::Checkbox(T("gimmicks through the game"), &core::g_gimmickSpawn); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("prefabs under /object/cd_gimmick/ are spawned through the game's own spawn path and react like real objects (torches, doors, chests); off = plain objects")); ImGui::SameLine();
                    static const char* kLiveModes[] = { "disable/set/enable", "transform only", "transform re-insert", "transform + enable" };
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
                        if (InputTextI18n("##replayprefab", T("replay prefab path override, e.g. /object/cd_gimmick/00_common/lamp/gimmick_lamp_stand_candle_0003_index01.prefab (empty = as captured)"), prefabOverride, sizeof prefabOverride)) core::SetGimmickReplayPrefab(prefabOverride);
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
                            ImGui::TableSetColumnIndex(3); ImGui::Text("%u / %u", c.k1, c.k2);
                            ImGui::TableSetColumnIndex(4); if (ImGui::SmallButton(T("here"))) core::ArmGimmickReplay(InFront(2.0f, 0.0f), c.id);
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                }
                bool tr = core::Trace(); if (ImGui::Checkbox(T("trace game calls (writes to cdmodkit.log; turn on, move an object in the housing editor, turn off)"), &tr)) core::SetTrace(tr);
                if (ImGui::Button(T("camera trace (16 s)"))) { core::CamTrace(16); Note(T("camtrace started: close the menu and rotate the camera slowly for 16 s")); }
                ImGui::SameLine(); if (ImGui::Button(T("ray / shape cast trace"))) { core::RayTrace(12); Note(T("trace: close the menu (Home), walk a few steps, then aim with the bow and press F on something")); }
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
        ImGui::End();
        if (g_playMode) ImGui::PopStyleVar();
        CameraTick();
    }
}
