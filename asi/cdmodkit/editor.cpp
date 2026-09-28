// In-game editor UI (Dear ImGui): category tree + filtered prefab list + details, scene objects (selection, groups, undo),
// placement mode (single objects and groups, snapping), copy/paste, line/circle tools, projects.
#define NOMINMAX
#include "core.h"
#include "report_projection.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <functional>
#include <fstream>
#include <iterator>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <set>
#include <map>
#include <unordered_map>
#include <tuple>
#include <cmath>
#include <commdlg.h>
#include <shellapi.h>
#include "icons.h"
#include "overlay.h"
#include "thumbgen.h"
#include "input.h"
#include "http_api.h"
#include "i18n.h"
#include "stb_image.h"

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
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        if (ImGui::GetItemRectMax().x + gap + buttonW <= right) ImGui::SameLine();
        ImGui::PushID(label);
        const bool clicked = ImGui::Button(g_numericEditId == id ? "v##number" : ">##number");
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
        (void)compact;   // kept in the signature because the same page functions are shared by the full editor and dock
        const float gap = spacing >= 0 ? spacing : ImGui::GetStyle().ItemSpacing.x;
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        if (ImGui::GetItemRectMax().x + gap + nextWidth <= right) ImGui::SameLine(0, spacing);
    }
    static void SameLineForControl(const char* label, bool checkbox = false) {
        const auto& s = ImGui::GetStyle();
        SameLineOrWrap(true, ImGui::CalcTextSize(T(label), nullptr, true).x + (checkbox ? ImGui::GetFrameHeight() + s.ItemInnerSpacing.x : s.FramePadding.x * 2));
    }
    static void SetLabeledItemWidth(const char* label, float preferred) {
        ImGui::SetNextItemWidth(std::max(ImGui::GetFrameHeight(), std::min(preferred, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(T(label), nullptr, true).x - ImGui::GetStyle().ItemInnerSpacing.x)));
    }
    // Long translated action labels wrap inside their real hit rectangle; the original label/ID
    // and native button behavior remain unchanged. Ordinary-width controls use ImGui directly.
    static bool FittedButton(const char* label, bool fill = false) {
        const auto& style = ImGui::GetStyle(); const float width = ImGui::GetContentRegionAvail().x;
        const char* end = ImGui::FindRenderedTextEnd(label);
        if (ImGui::CalcTextSize(label, end).x + style.FramePadding.x * 2 <= width)
            return ImGui::Button(label, fill ? ImVec2(-1, 0) : ImVec2(0, 0));
        const float wrap = std::max(1.0f, width - style.FramePadding.x * 2);
        const ImVec2 text = ImGui::CalcTextSize(label, end, false, wrap);
        const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
        const bool pressed = ImGui::Button(label, ImVec2(width, text.y + style.FramePadding.y * 2));
        ImGui::PopStyleColor();
        const ImVec2 at(ImGui::GetItemRectMin().x + style.FramePadding.x + (wrap - text.x) * style.ButtonTextAlign.x,
                        ImGui::GetItemRectMin().y + style.FramePadding.y);
        ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(), at, color, label, end, wrap);
        return pressed;
    }
    static bool FittedCheckbox(const char* label, bool* value) {
        const auto& style = ImGui::GetStyle(); const float width = ImGui::GetContentRegionAvail().x, side = ImGui::GetFrameHeight();
        const char* end = ImGui::FindRenderedTextEnd(label);
        if (side + style.ItemInnerSpacing.x + ImGui::CalcTextSize(label, end).x <= width) return ImGui::Checkbox(label, value);
        ImGuiWindow* window = ImGui::GetCurrentWindow(); if (window->SkipItems) return false;
        ImGuiContext& g = *ImGui::GetCurrentContext(); const ImGuiID id = window->GetID(label);
        const float wrap = std::max(1.0f, width - side - style.ItemInnerSpacing.x);
        const ImVec2 text = ImGui::CalcTextSize(label, end, false, wrap), pos = window->DC.CursorPos;
        const ImRect bounds(pos, ImVec2(pos.x + width, pos.y + std::max(side, text.y + style.FramePadding.y * 2)));
        ImGui::ItemSize(bounds, style.FramePadding.y);
        if (!ImGui::ItemAdd(bounds, id)) {
            IMGUI_TEST_ENGINE_ITEM_INFO(id, label, g.LastItemData.StatusFlags | ImGuiItemStatusFlags_Checkable | (*value ? ImGuiItemStatusFlags_Checked : 0));
            return false;
        }
        bool hovered, held; const bool pressed = ImGui::ButtonBehavior(bounds, id, &hovered, &held);
        if (pressed) { *value = !*value; ImGui::MarkItemEdited(id); }
        ImGui::RenderNavCursor(bounds, id);
        ImGui::RenderFrame(pos, ImVec2(pos.x + side, pos.y + side), ImGui::GetColorU32(held && hovered ? ImGuiCol_FrameBgActive : hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), true, style.FrameRounding);
        if (*value) { const float pad = std::max(1.0f, floorf(side / 6)); ImGui::RenderCheckMark(window->DrawList, ImVec2(pos.x + pad, pos.y + pad), ImGui::GetColorU32(ImGuiCol_CheckMark), side - pad * 2); }
        window->DrawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(pos.x + side + style.ItemInnerSpacing.x, pos.y + style.FramePadding.y), ImGui::GetColorU32(ImGuiCol_Text), label, end, wrap);
        IMGUI_TEST_ENGINE_ITEM_INFO(id, label, g.LastItemData.StatusFlags | ImGuiItemStatusFlags_Checkable | (*value ? ImGuiItemStatusFlags_Checked : 0));
        return pressed;
    }
    static constexpr const char* kEditorVersion = "0.97";

    static bool g_open = false, g_propertiesOpen = false, g_propertiesPopupRequested = false;
    // browser state
    static char  g_filter[128] = "";
    static int   g_selCat = 0;            // 0 = all
    static int   g_selPrefab = -1;
    static bool  g_favOnly = false; static bool g_meshOnly = true;
    enum MainTabId {
        TabBrowser = 0, TabScene = 1, TabProject = 2, TabTravel = 3, TabSettings = 4,
        TabLog = 5, TabHistory = 6, TabNpcs = 7, TabEnvironment = 8, TabTerrain = 9, TabBlueprint = 10
    };
    static bool  g_compact = false; static int g_dockCols = 2;   // narrow dock window; supported pages reuse the full editor functions
    static int g_mainTab = TabBrowser, g_compactPage = TabBrowser; static bool g_selectMainTab = false;
    static bool SharedPage(int page) {
        return page == TabBrowser || page == TabScene || page == TabNpcs || page == TabProject || page == TabBlueprint ||
               page == TabEnvironment || (page == TabTerrain && core::TerrainAvailable());
    }
    static bool  g_showSpawnOpts = false, g_showMass = false; static int g_hoverUid = 0, g_hoverNpcUid = 0;
    static bool  g_cardView = false; static float g_cardSize = 96.0f;   // browser: tile view instead of the list (same matches / filters)
    static int   g_browserDragPrefab = -1;   // browser row/card being dragged out into the game view
    struct BrowserDropJob { int prefab = -1, ticket = 0; DWORD queuedAt = 0; Vec3 center{}; float yaw = 0, scale = 1, probeTop = 0; int retry = 0; };
    static std::vector<BrowserDropJob> g_browserDropJobs;   // ground is probed before spawning, so a new object's own collision cannot be mistaken for the surface
    static int   g_npcDragIndex = -1;        // character row/card being dragged out into the game view
    struct NpcDropJob { uint32_t key = 0; int ticket = 0; DWORD queuedAt = 0; Vec3 at{}; int count = 1, formation = 1; float spacing = 1.5f, radius = 8.0f, fx = 0, fz = 1; bool ai = true; int behavior = 0;
        float probeTop = 0; int retry = 0; bool centerResolved = false; std::vector<Vec3> positions; std::vector<int> memberTickets; std::vector<uint8_t> memberRetries; };
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
    static char g_projName[64] = "", g_blueprintName[64] = "myblueprint";
    static std::string g_projectStatus;
    static bool g_projectRefresh = true, g_projectCommandPending = false;
    static bool SceneProjectMutation(const std::string& name, int mode);
    // scene state: object and managed-NPC selections share one Scene workflow.
    // g_rightUid and g_sceneLastEntity use positive object UIDs and negative NPC UIDs.
    static std::set<int> g_sel; static int g_primary = 0; static int g_lastClicked = 0;
    static bool g_terrainTileSelected = false;
    static int g_selectedTerrainX = 0, g_selectedTerrainZ = 0, g_selectedTerrainProject = 0;
    static int g_sceneLastEntity = 0;
    static bool g_boxSelecting = false, g_boxMoved = false, g_boxAdd = false; static ImVec2 g_boxStart{}, g_boxCurrent{}; static std::set<int> g_boxBase, g_boxNpcBase;
    static bool g_rightGesture = false, g_rightMoved = false, g_worldPopupRequested = false, g_worldPopupOpen = false; static ImVec2 g_rightStart{}, g_worldPopupPos{}; static int g_rightUid = 0;
    static bool  g_selectGroups = true, g_showDeleted = false;
    static float g_edit[3] = { 0, 0, 0 }, g_editScale = 1; static Rot g_editRot, g_editRot0; static Vec3 g_editPos0{}; static float g_editScale0 = 1;
    static int   g_editUid = 0; static bool g_live = true;
    static std::vector<std::string> g_log;
    static std::set<int> g_managedNpcSel; static int g_managedNpcPrimary = 0, g_managedNpcLast = 0;
    static int g_projTab = 0;    // project currently shown in Scene; 0 = no editing project
    static const ManagedNpc* FindManagedNpc(const std::vector<ManagedNpc>& list, int uid);
    static Vec3 ManagedNpcDisplayPos(const ManagedNpc& n);
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
        enum Kind { Spawn, Move, Delete, SetGroup, NpcSpawn, NpcMove, NpcDelete, NpcControl, NpcGroup, ObjectNote, NpcNote, NpcLabel, GroupName, TerrainBatch, TerrainClear, TerrainTileRemove } kind;
        int uid = 0; std::string prefab; Vec3 pos0{}, pos1{}; Rot rot0, rot1; float sc0 = 1, sc1 = 1;
        int group = 0, group1 = 0, tileX = 0, tileZ = 0; int proj = 0; bool flag0 = false, flag1 = false; int behavior0 = 0, behavior1 = 0; std::string text0, text1;
        std::vector<core::TerrainStroke> terrain;
        std::vector<size_t> terrainIndices;
        std::set<int> projects; // captured provenance survives forgetting records and empty named groups
    };   // object and managed-NPC committed edits share one chronological undo/redo stack
    struct HistoryEntry { unsigned long long serial = 0; std::vector<Act> acts; uint64_t branch = 0, placement = 0; core::PlaceRequestHandle spawnRequest; };
    static constexpr size_t kHistoryLimit = 1000;
    static unsigned long long g_historySerial = 0;
    static uint64_t g_historyBranch = 1, g_clipRevision = 0;
    static std::vector<HistoryEntry> g_undo, g_redo;
    static std::vector<core::GroundHandle> g_pendingGround;
    static std::vector<core::GroundView> g_groundLastResults;
    static report_projection::GroundArchive g_groundReports;
    static int g_numericSceneUid = 0, g_numericNpcUid = 0;
    static Vec3 g_numericNpcBase{};
    static bool NpcAct(Act::Kind kind) {
        return kind == Act::NpcSpawn || kind == Act::NpcMove || kind == Act::NpcDelete || kind == Act::NpcControl ||
            kind == Act::NpcGroup || kind == Act::NpcNote || kind == Act::NpcLabel;
    }
    static void CaptureProvenance(Act& a) {
        if (a.kind == Act::TerrainBatch || a.kind == Act::TerrainClear || a.kind == Act::TerrainTileRemove) {
            for (const auto& t : a.terrain) if (t.proj) a.projects.insert(t.proj);
        } else if (a.kind == Act::GroupName) {
            for (const auto& o : core::Spawned()) if (o.group == a.group && o.proj) a.projects.insert(o.proj);
            for (const auto& n : core::ManagedNpcs()) if (n.group == a.group && n.proj) a.projects.insert(n.proj);
        } else if (NpcAct(a.kind)) {
            for (const auto& n : core::ManagedNpcs()) if (n.uid == a.uid) { a.proj = n.proj; break; }
        } else {
            for (const auto& o : core::Spawned()) if (o.uid == a.uid) { a.proj = o.proj; break; }
        }
        if (a.proj) a.projects.insert(a.proj);
    }
    struct DeferredEditorIntent {
        std::function<void()> action;
        std::set<int> selection, npcSelection;
        int primary = 0, npcPrimary = 0, project = -1;
        uint64_t placement = 0, clipboard = 0;
        int prefab = -1;
        float spawnYaw = 0, spawnScale = 1;
        Vec3 offsets{};
        std::vector<SpawnedObj> targets;
        std::vector<ManagedNpc> npcTargets;
    };
    static DeferredEditorIntent g_deferredEditor;
    static bool g_resumingMutation = false;
    static uint64_t g_seenGroundNotice = 0;
    static bool ReconcileGroundBatch(bool cancelUnapplied);
    static void PumpGroundHistory();
    static bool PrepareHistoryMutation(std::function<void()> intent, const std::vector<int>& targets = {});
    static void CommitBrushHistory();
    static bool BrushMutationPending();
    static bool EditorMutationPending();
    static void PublishProjectContext();
    static void FinalizeNumericEdits();
    static const char* ProjectInPlaceSaveError(const std::string& name);
    static bool SaveProjectAction(const std::string& rawName, int scope, bool approved = false);
    static bool VecChanged(Vec3 a, Vec3 b) { return a.x != b.x || a.y != b.y || a.z != b.z; }
    static bool RotChanged(Rot a, Rot b) { return a.yaw != b.yaw || a.pitch != b.pitch || a.roll != b.roll; }
    static bool ActChanged(const Act& a) {
        if (a.kind == Act::Spawn || a.kind == Act::Delete || a.kind == Act::NpcSpawn || a.kind == Act::NpcDelete) return true;
        if (a.kind == Act::SetGroup || a.kind == Act::NpcGroup) return a.group != a.group1;
        if (a.kind == Act::NpcMove) return VecChanged(a.pos0, a.pos1);
        if (a.kind == Act::NpcControl) return a.flag0 != a.flag1 || a.behavior0 != a.behavior1;
        if (a.kind == Act::ObjectNote || a.kind == Act::NpcNote || a.kind == Act::NpcLabel || a.kind == Act::GroupName) return a.text0 != a.text1;
        if (a.kind == Act::TerrainBatch || a.kind == Act::TerrainClear || a.kind == Act::TerrainTileRemove) return !a.terrain.empty();
        // History is intentionally exact: even a sub-millimetre move or a tiny typed rotation/scale change counts.
        return VecChanged(a.pos0, a.pos1) || RotChanged(a.rot0, a.rot1) || a.sc0 != a.sc1;
    }
    static void ReconcileSpawnHistory() {
        // Admission reserves the upstream Spawn step immediately. A terminal native failure
        // retracts only that row's speculative act, even if Drop or Undo preceded settlement.
        for (auto* stack : { &g_undo, &g_redo }) for (auto it = stack->begin(); it != stack->end();) {
            if (!it->spawnRequest) { ++it; continue; }
            const auto result = core::PlaceRequestState(it->spawnRequest);
            std::set<int> failed;
            for (const auto& row : result.rows) if (row.state == core::PlaceFailed) failed.insert(row.uid);
            auto& acts = it->acts;
            if (!failed.empty()) acts.erase(std::remove_if(acts.begin(), acts.end(), [&](const Act& a) {
                return a.kind == Act::Spawn && failed.count(a.uid);
            }), acts.end());
            if (!result.pending) it->spawnRequest.reset();
            if (acts.empty()) it = stack->erase(it); else ++it;
        }
    }
    static void Push(std::vector<Act> acts, uint64_t placement = 0, const core::PlaceRequestHandle& spawnRequest = {}) {
        acts.erase(std::remove_if(acts.begin(), acts.end(), [](const Act& a) { return !ActChanged(a); }), acts.end());
        if (acts.empty()) return;
        for (auto& a : acts) CaptureProvenance(a);
        g_undo.push_back({ ++g_historySerial, std::move(acts), g_historyBranch, placement, spawnRequest });
        g_projectRefresh = true;
        if (g_undo.size() > kHistoryLimit) g_undo.erase(g_undo.begin());
        g_redo.clear();
    }

    // ---- placement mode: one or many objects carried as a rigid set around a center; the gizmo edits the set ----
    struct Member { int uid; bool npc = false; std::string prefab; Vec3 rel{}; Rot rot0; float scale0 = 1; Vec3 origPos{}; Rot origRot; float origScale = 1; };
    struct Place {
        bool active = false, isNew = false, reopen = false, hasNpc = false;
        std::vector<Member> m; std::string name;
        Vec3 center{}; float yaw = 0, scale = 1;     // yaw = rotation delta about the center, scale = multiplier
        float pitch = 0, roll = 0;                   // tilt deltas added to every member (no position change for sets)
        float radius = 1.0f;
        DWORD lastSend = 0; bool dirty = true; bool touched = false; Vec3 lastCenter{}; float lastYaw = 1e9f, lastScale = 1e9f, lastPitch = 1e9f, lastRoll = 1e9f;
        bool haveCenter = false; int prefabIdx = -1;   // single object: bbox center known (rotation about it) or still being measured
        float snapAx = 0, snapAz = 1;                  // fixed world-axis frame for optional keyboard placement
        bool mouse = false;                            // optional keyboard placement: mouse to gizmo while menu is closed
        bool keyTransformHeld = false, commitRequested = false;
        int hover = 0, drag = 0;   // gizmo: 1 X, 2 Y, 3 Z, 4 yaw ring, 5 center dot, 6 scale cube, 7 pitch ring, 8 roll ring
        float drag0 = 0; Vec3 dragCenter0{}; float dragYaw0 = 0, dragScale0 = 1, dragPitch0 = 0, dragRoll0 = 0;
        std::vector<core::MoveReq> historyBase;
        uint64_t generation = 0, transform = 0;
        core::PlaceRequestHandle req;
    };
    static Place g_place;
    struct PendingNpcGrab {
        bool active = false, isNew = false, hasPivot = false;
        DWORD queuedAt = 0;
        std::vector<int> uids;
        std::string name;
        Vec3 pivot{};
        core::PlaceRequestHandle request;
        std::set<int> objects, npcs;
    };
    static PendingNpcGrab g_pendingNpcGrab;
    struct ProjectPlacementReport { std::string name; core::PlaceRequestHandle request; bool approximate = false; core::FileSelectionHandle source; };
    static std::vector<ProjectPlacementReport> g_projectPlacements;
    static report_projection::PlacementArchive g_placementReports;
    static uint64_t g_nextPlacement = 0;
    static DWORD g_tickOverride = 0;
    static DWORD TickNow() { return g_tickOverride ? g_tickOverride : GetTickCount(); }
    static core::GroundPlacement GroundPlacementOf(const Place& p) {
        core::GroundPlacement c;
        if (p.active && !p.hasNpc) {
            c.generation = p.generation; c.transform = p.transform;
            for (const auto& m : p.m) c.members.push_back(m.uid);
        }
        return c;
    }
    static std::vector<core::GroundHandle> BeginGrounding(const std::vector<int>& uids, bool rigid, const core::GroundPlacement& carried = {});
    static void StopCameraMode(bool forceFreeCamOff = false) {
        if (!g_cameraMode && !(forceFreeCamOff && core::FreeCamActive())) return;
        g_cameraMode = false; g_cameraViewMode = 0; g_cameraStartAt = 0; g_cameraEverActive = false; memset(g_cameraShortcutDown, 0, sizeof g_cameraShortcutDown);
        g_rightGesture = g_rightMoved = g_worldPopupRequested = false; g_rightUid = 0;
        core::SetFreeCam(false); core::g_fcHoldMove = false; input::ClearKeys(); ImGui::GetIO().ClearInputKeys();
    }
    static void FinishCloseEditor() {
        CommitBrushHistory();
        StopCameraMode(true);
        g_propertiesOpen = g_propertiesPopupRequested = false;
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
        s.WindowPadding = ImVec2(12, 9); s.FramePadding = ImVec2(8, 7); s.ItemSpacing = ImVec2(8, 4); s.IndentSpacing = 18; s.ScrollbarSize = 14;
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
        if (!core::g_projectAutoSave || GetTickCount64() < retryAt || EditorMutationPending()) return;
        bool failed = false;
        std::set<int> ids;
        for (const auto& o : core::Spawned()) if (o.proj > 0) ids.insert(o.proj);
        for (const auto& n : core::ManagedNpcs()) if (n.proj > 0) ids.insert(n.proj);
        for (const auto& t : core::TerrainStrokes()) if (t.proj > 0) ids.insert(t.proj);
        for (int id : ids) {
            if (!core::ProjectDirty(id) || core::ProjectMutationPending(id)) continue;
            const std::string name = core::ProjectNameOf(id);
            if (!name.empty()) {
                if (!ProjectInPlaceSaveError(name) && SaveProjectAction(name, core::SaveProjectOnly, true)) core::Log("[editor] autosave: %s", name.c_str());
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
    static std::string LocalizedTags(const std::string& tags) {
        std::string shown;
        for (size_t p = 0; p < tags.size();) {
            const size_t q = tags.find(',', p);
            const std::string part = tags.substr(p, q == std::string::npos ? std::string::npos : q - p);
            const size_t colon = part.find(':');
            const std::string key = part.substr(0, colon);
            if (!shown.empty()) shown += ", ";
            shown += T(key == "Nude" ? "Character" : key.c_str());
            if (colon != std::string::npos) shown += part.substr(colon);
            if (q == std::string::npos) break;
            p = q + 1;
        }
        return shown;
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
        if (!uid) return;
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
        if (!PrepareHistoryMutation([p]() { SpawnSelected(p); })) return;
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
        float m00 = 0, m11 = 0; Vec3 rp;
        if (core::RenderCamera(&rp, &c.right, &c.up, &c.fwd, &m00, &m11) &&
            std::isfinite(rp.y) && std::isfinite(rp.z) && std::isfinite(m00) && std::isfinite(m11) && m00 > 0.01f && m11 > 0.01f) {
            c.pos = rp;
            if (core::g_camMirror) c.right = { -c.right.x, -c.right.y, -c.right.z };
            if (core::g_fovAuto) { c.f = m11; c.aspect = m11 / m00; c.ok = true; return c; }
        } else {
            if (!core::CameraBasis(&c.pos, &c.right, &c.up, &c.fwd)) return c;
            float sgn = g_camSign != 0 ? g_camSign : 1.0f;
            Vec3 freePos, freeForward;
            if (core::FreeCamPose(&freePos, &freeForward)) {
                const float dot = c.fwd.x * freeForward.x + c.fwd.z * freeForward.z;
                if (fabsf(dot) > 0.1f) sgn = dot > 0 ? 1.0f : -1.0f;
            }
            c.fwd = { c.fwd.x * sgn, c.fwd.y * sgn, c.fwd.z * sgn };
            if (core::g_camMirror) c.right = { -c.right.x, -c.right.y, -c.right.z };
        }
        float fov = core::g_fovDeg; if (core::g_fovAuto) { float live; if (core::CameraFov(&live)) fov = live; }
        c.f = 1.0f / tanf(fov * 3.14159265f / 360.0f); c.ok = true; return c;
    }
    static CamFrame g_currentCam;
    static void SampleCamera() { g_currentCam = LiveCam(); }
    static CamFrame CurrentCam() { return g_currentCam.ok ? g_currentCam : LiveCam(); }
    static bool WorldToScreen(const CamFrame& c, const Vec3& p, ImVec2* out) {
        const float dx = p.x - c.pos.x, dy = p.y - c.pos.y, dz = p.z - c.pos.z;
        const float x = dx * c.right.x + dy * c.right.y + dz * c.right.z, y = dx * c.up.x + dy * c.up.y + dz * c.up.z, z = dx * c.fwd.x + dy * c.fwd.y + dz * c.fwd.z;
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || z < 0.05f) return false;
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
    static bool ApplyCarriedPose(const Place& candidate, bool send);
    static bool g_placeHudDrawn = false;
    static void DrawPlacementSnapControls() {
        ImGui::Checkbox(T("snap"), &g_snap);
        SameLineOrWrap(true, ImGui::CalcTextSize(T("position")).x);
        ImGui::TextUnformatted(T("position"));
        SameLineOrWrap(true, 85.0f);
        ImGui::SetNextItemWidth(std::min(80.0f, ImGui::GetContentRegionAvail().x));
        ComboT("##snappos", &g_snapPosIdx, kSnapPosNames, 5);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("grid and angle steps for the placement mode"));
        SameLineOrWrap(true, ImGui::CalcTextSize(T("Rotate")).x);
        ImGui::TextUnformatted(T("Rotate"));
        SameLineOrWrap(true, 85.0f);
        ImGui::SetNextItemWidth(std::min(80.0f, ImGui::GetContentRegionAvail().x));
        ComboT("##snapyaw", &g_snapYawIdx, kSnapYawNames, 5);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("grid and angle steps for the placement mode"));
    }
    static void DrawPlaceHud(bool embedded = false, float top = -1.0f) {
        g_placeHudDrawn = true; Place P = g_place; ImGuiIO& io = ImGui::GetIO();
        const auto& style = ImGui::GetStyle();
        if (!embedded) {
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, top >= 0 ? top : 28.0f), ImGuiCond_Always, ImVec2(0.5f, 0));
            ImGui::SetNextWindowSize(ImVec2(std::min(io.DisplaySize.x - style.WindowPadding.x * 2, ImGui::GetFontSize() * 42), 0), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.75f);
        }
        if (embedded || ImGui::Begin("##placehud", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
            ImGui::PushID("placement-bar");
            ImGui::TextWrapped(ICON_CUBE "  %s  %s%s", T(P.isNew ? "Place" : "Grab"), P.name.c_str(), P.m.size() > 1 ? T("  (group)") : "");
            ImGui::TextWrapped(T("center %.1f  %.1f  %.1f    rotation %+.0f    tilt %+.0f, %+.0f    scale x%.2f    snap %s"), P.center.x, P.center.y, P.center.z, P.yaw, P.pitch, P.roll, P.scale,
                g_snap ? (std::string(T(kSnapPosNames[g_snapPosIdx])) + ", " + T(kSnapYawNames[g_snapYawIdx])).c_str() : T("off"));
            if (ImGui::Button(T("drop"))) DropCarried();
            SameLineForControl("Cancel"); if (ImGui::Button(T("Cancel"))) CancelCarried();
            SameLineForControl("To ground"); if (ImGui::Button(T("To ground"))) StartGroundSnap(P);
            if (!P.hasNpc) { SameLineForControl("level"); if (ImGui::Button(T("level"))) { if (P.m.size() == 1) { P.pitch = -P.m[0].rot0.pitch; P.roll = -P.m[0].rot0.roll; } else { P.pitch = P.roll = 0; } P.dirty = P.touched = true; CommitPlaceHistory(P); } }
            DrawPlacementSnapControls();
            // Explicit translation controls are useful when a world-space gizmo handle is hard to hit (especially for NPCs).
            // They operate on the same placement transaction, so one click is one undoable move and NPC AI stays paused until Drop.
            const float moveStep = g_snap ? kSnapPos[g_snapPosIdx] : 0.25f;
            auto nudge = [&](float dx, float dy, float dz) {
                P.center.x += dx; P.center.y += dy; P.center.z += dz;
                P.dirty = P.touched = true; CommitPlaceHistory(P);
            };
            ImGui::TextDisabled("%s  %.2f m", T("Move"), moveStep);
            SameLineForControl("X-"); if (ImGui::Button("X-")) nudge(-moveStep, 0, 0);
            SameLineForControl("X+"); if (ImGui::Button("X+")) nudge( moveStep, 0, 0);
            SameLineForControl("Y-"); if (ImGui::Button("Y-")) nudge(0, -moveStep, 0);
            SameLineForControl("Y+"); if (ImGui::Button("Y+")) nudge(0,  moveStep, 0);
            SameLineForControl("Z-"); if (ImGui::Button("Z-")) nudge(0, 0, -moveStep);
            SameLineForControl("Z+"); if (ImGui::Button("Z+")) nudge(0, 0,  moveStep);
            if (core::g_keyboardPlacement && !g_compact) {
                auto kn = [](int pk) { return core::KeyName(core::g_placeKeys[pk]); };
                ImGui::TextWrapped(T("Move: %s %s %s %s%s     Rotate: %s %s     Height: %s %s     Size: %s %s"), kn(core::PK_FWD), kn(core::PK_BACK), kn(core::PK_LEFT), kn(core::PK_RIGHT),
                    g_snap ? T(" (one grid step per press)") : (std::string(" (") + kn(core::PK_FAST) + " = " + T("fast") + ")").c_str(), kn(core::PK_ROT_L), kn(core::PK_ROT_R), kn(core::PK_UP), kn(core::PK_DOWN), kn(core::PK_SCALE_UP), kn(core::PK_SCALE_DOWN));
                ImGui::TextWrapped(T("%s = bring in front     %s = snap     %s = mouse gizmo and game     %s = level     %s = ground     %s = drop     %s = %s"),
                    kn(core::PK_FETCH), kn(core::PK_SNAP), kn(core::PK_MOUSE), kn(core::PK_LEVEL), kn(core::PK_GROUND), kn(core::PK_DROP), kn(core::PK_CANCEL), T(P.isNew ? "cancel" : "put back"));
            } else if (!g_compact && !g_cameraMode) ImGui::TextWrapped(T("To look around: %s opens the editor, %s there switches to the free camera"), core::KeyName(core::g_keyToggle), core::KeyName(core::g_keyMode));
            ImGui::PopID();
        }
        if (!embedded) ImGui::End();
        if (ApplyCarriedPose(P, true) && P.commitRequested) CommitPlaceHistory(g_place);
    }

    // ---- placement ----
    static bool IsCarried(int uid);
    static void StartGrab(const std::vector<int>& uids, bool isNew, const std::string& name, const Vec3* savedPivot = nullptr, const core::PlaceRequestHandle& request = {}) {
        const bool hasPivot = savedPivot != nullptr; const Vec3 pivot = savedPivot ? *savedPivot : Vec3{};
        if (!PrepareHistoryMutation([uids, isNew, name, hasPivot, pivot, request]() { StartGrab(uids, isNew, name, hasPivot ? &pivot : nullptr, request); }, uids)) return;
        if (uids.empty()) return;
        if (g_place.active) DropCarried();   // whatever is in the hands is left where it is (undo step included)
        const bool keepCamera = g_cameraMode;
        auto list = core::Spawned();
        auto npcs = core::ManagedNpcs();
        for (int key : uids) if (key < 0) {
            const ManagedNpc* npc = FindManagedNpc(npcs, -key);
            if (npc && npc->editMoving) {
                g_pendingNpcGrab = { true, isNew, hasPivot, TickNow(), uids, name, pivot, request, g_sel, g_managedNpcSel };
                return; // EndManagedNpcMove clears editMoving on the next server tick.
            }
        }
        g_pendingNpcGrab.active = false;
        Place P; P.active = true; P.isNew = isNew; P.name = name; P.req = request; P.generation = ++g_nextPlacement;
        Vec3 c{ 0, 0, 0 };
        bool needsGameThread = false, failedNpc = false;
        for (int key : uids) {
            if (key > 0) {
                const SpawnedObj* o = Find(list, key); if (!o || o->hidden) continue;
                needsGameThread = true; Member m; m.uid = key; m.prefab = o->prefab; m.rot0 = o->rot; m.scale0 = o->scale; m.origPos = o->pos; m.origRot = o->rot; m.origScale = o->scale; P.m.push_back(m);
            } else if (key < 0) {
                const int uid = -key; const ManagedNpc* n = nullptr; for (const auto& q : npcs) if (q.uid == uid) { n = &q; break; }
                if (!n || n->hidden) continue;
                Vec3 pos = n->pos; core::ManagedNpcLivePosition(*n, &pos);
                if (!core::BeginManagedNpcMove(uid)) { failedNpc = true; break; }
                Member m; m.uid = uid; m.npc = true; m.prefab = n->label.empty() ? (std::string("NPC ") + std::to_string(n->key)) : n->label; m.origPos = pos; P.m.push_back(m); P.hasNpc = true;
            }
        }
        if (failedNpc || (needsGameThread && !core::GameThreadReady())) {
            for (const auto& m : P.m) if (m.npc) core::EndManagedNpcMove(m.uid, m.origPos);
            Note("%s", T(failedNpc ? "NPC live move is not ready" : "transform could not be queued; the game thread is not ready"));
            return;
        }
        if (P.m.empty()) return;
        if (P.m.size() == 1 && !P.m[0].npc) { const SpawnedObj* o = Find(list, P.m[0].uid); c = BboxCenter(*o); P.radius = Footprint(*o); P.prefabIdx = IndexOfPrefab(o->prefab); P.haveCenter = P.prefabIdx >= 0 && core::PrefabIndex()[P.prefabIdx].hasCenter; }
        else if (P.m.size() == 1) { c = P.m[0].origPos; c.y += 0.9f; P.radius = 1.2f; }
        else { for (auto& m : P.m) { c.x += m.origPos.x; c.y += m.origPos.y; c.z += m.origPos.z; } const float inv = 1.0f / (float)P.m.size(); c.x *= inv; c.y *= inv; c.z *= inv;
               if (P.hasNpc) c.y += 0.9f;
               for (auto& m : P.m) { float dx = m.origPos.x - c.x, dz = m.origPos.z - c.z; P.radius = std::max(P.radius, sqrtf(dx * dx + dz * dz) + 1.0f); } }
        if (savedPivot) {
            c = *savedPivot; P.haveCenter = false; P.prefabIdx = -1;
            for (const auto& m : P.m) { const float dx = m.origPos.x - c.x, dz = m.origPos.z - c.z; P.radius = std::max(P.radius, sqrtf(dx * dx + dz * dz) + 1.0f); }
        }
        for (auto& m : P.m) m.rel = { m.origPos.x - c.x, m.origPos.y - c.y, m.origPos.z - c.z };
        P.center = c; P.lastCenter = c; P.lastYaw = P.lastPitch = P.lastRoll = 0; P.lastScale = 1; P.dirty = isNew;
        for (const auto& m : P.m) P.historyBase.push_back({ m.uid, m.origPos, m.origRot, m.origScale });
        if (isNew) {
            std::vector<Act> spawned; spawned.reserve(P.m.size());
            for (const auto& m : P.m) {
                if (m.npc) continue;
                Act a; a.kind = Act::Spawn; a.uid = m.uid; a.prefab = m.prefab; a.pos1 = m.origPos; a.rot1 = m.origRot; a.sc1 = m.origScale;
                const SpawnedObj* o = Find(list, m.uid); if (o) { a.group = o->group; a.proj = o->proj; }
                spawned.push_back(std::move(a));
            }
            Push(std::move(spawned), P.generation, request);
        }
        if (fabsf(g_fx) >= fabsf(g_fz)) { P.snapAx = g_fx > 0 ? 1.0f : -1.0f; P.snapAz = 0; } else { P.snapAx = 0; P.snapAz = g_fz > 0 ? 1.0f : -1.0f; }
        if (keepCamera) { P.reopen = false; }                             // camera mode stays live while the gizmo is shown
        else if (g_compact && g_open) { P.reopen = false; }               // the dock stays where it is
        else { P.reopen = false; }   // keep the browser and NPC library available while another object is carried
        if (!keepCamera) StopCameraMode();
        g_playMode = false; core::g_placing = true; input::ClearKeys();
        core::PublishGroundPlacement(GroundPlacementOf(P));
        g_place = P;
        if (core::g_keyboardPlacement) Note(T("%s: %s | %d objects | %s = done, %s %s"), T(isNew ? "placing" : "grabbed"), name.c_str(), (int)P.m.size(), core::KeyName(core::g_placeKeys[core::PK_DROP]), core::KeyName(core::g_placeKeys[core::PK_CANCEL]), T(isNew ? "cancels" : "puts back"));
        else Note("%s: %s", T(isNew ? "placing" : "grabbed"), name.c_str());
    }
    static void PumpPendingNpcGrab() {
        if (!g_pendingNpcGrab.active) return;
        if (g_pendingNpcGrab.objects != g_sel || g_pendingNpcGrab.npcs != g_managedNpcSel ||
            TickNow() - g_pendingNpcGrab.queuedAt > 3000) { g_pendingNpcGrab.active = false; return; }
        const auto npcs = core::ManagedNpcs();
        for (int key : g_pendingNpcGrab.uids) if (key < 0) {
            const ManagedNpc* npc = FindManagedNpc(npcs, -key);
            if (!npc || npc->hidden) { g_pendingNpcGrab.active = false; return; }
            if (npc->editMoving) return;
        }
        PendingNpcGrab retry = std::move(g_pendingNpcGrab);
        g_pendingNpcGrab = {};
        StartGrab(retry.uids, retry.isNew, retry.name, retry.hasPivot ? &retry.pivot : nullptr, retry.request);
    }
    static void StartPlaceNew(const PosInfo& p, bool havePos) {
        if (!PrepareHistoryMutation([p, havePos]() { StartPlaceNew(p, havePos); })) return;
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
        core::PublishGroundPlacement({});
        g_place.active = false; core::g_placing = false; input::ClearKeys();
        if (g_place.reopen) { g_open = true; g_playMode = true; }   // visible again, but the game keeps the input (Home to edit)
    }
    static void CancelCarried(bool notify) {
        if (!PrepareHistoryMutation([notify]() { CancelCarried(notify); })) return;
        Place& P = g_place; if (!P.active) return;
        if (P.req) core::PlaceRequestCancel(P.req);
        if (P.isNew) {
            for (const auto& m : P.m) if (!m.npc) { core::HideUid(m.uid); core::ForgetUid(m.uid); }
            for (auto* stack : { &g_undo, &g_redo }) stack->erase(std::remove_if(stack->begin(), stack->end(),
                [&](const HistoryEntry& e) { return e.placement == P.generation; }), stack->end());
        } else {
            // Gestures and accepted ground receipts are committed history, not a speculative suffix.
            // Put-back is an exact compensating move; unrelated entries and reserved serials survive.
            std::vector<Act> acts; std::vector<core::MoveReq> r;
            const auto objects = core::Spawned(); const auto npcs = core::ManagedNpcs();
            for (const auto& m : P.m) {
                Act a; a.kind = m.npc ? Act::NpcMove : Act::Move; a.uid = m.uid; a.prefab = m.prefab;
                if (m.npc) { const auto* n = FindManagedNpc(npcs, m.uid); if (!n) continue; a.pos0 = n->pos;
                    if (!core::EndManagedNpcMove(m.uid, m.origPos)) continue; }
                else { const auto* o = Find(objects, m.uid); if (!o) continue; a.pos0 = o->pos; a.rot0 = o->rot; a.sc0 = o->scale;
                    r.push_back({ m.uid, m.origPos, m.origRot, m.origScale }); }
                a.pos1 = m.origPos; a.rot1 = m.origRot; a.sc1 = m.origScale; acts.push_back(a);
            }
            if (!r.empty() && !core::MoveMany(r, true)) { Note("placement: put-back refused"); return; }
            Push(std::move(acts));
        }
        if (notify) Note(T("placement cancelled")); FinishPlace();
    }
    static bool PlaceGroupCopy(const proj_codec::Document& doc, Vec3 target, float yawDelta, float scale, const std::string& name,
                               const core::FileSelectionHandle& source = {}) {
        std::vector<proj_codec::EngineRow> checked;
        if (doc.kind != proj_codec::Kind::Group) { g_projectStatus = "wrong-kind"; return false; }
        if (!proj_codec::NarrowForEngine(doc, checked, g_projectStatus)) return false;
        if (!PrepareHistoryMutation([doc, target, yawDelta, scale, name, source]() { PlaceGroupCopy(doc, target, yawDelta, scale, name, source); })) return false;
        std::vector<int> uids; Vec3 pivot{};
        const auto report = source ? core::AdmitGroupFileCopy(source, target, yawDelta, scale, uids, pivot)
                                   : core::AdmitGroupCopy(doc, target, yawDelta, scale, uids, pivot);
        if (report.request) {
            g_projectPlacements.push_back({ name, report.request, doc.bounds.approximate, source });
            g_placementReports.Admit(report.request, name, doc.bounds.approximate);
        }
        g_projectStatus = report.valid ? "placement-admitted" : report.error;
        if (!report.valid) { Note("%s", report.error.c_str()); return false; }
        if (!uids.empty()) {
            const auto placed = core::Spawned();
            if (const auto* first = Find(placed, uids.front())) if (first->group > 0) core::SetGroupName(first->group, name);
        }
        StartGrab(uids, true, name, &pivot, report.request);
        return g_place.active && g_place.req == report.request;
    }
    static void CancelProjectPlacement(const core::PlaceRequestHandle& request) {
        if (!PrepareHistoryMutation([request]() { CancelProjectPlacement(request); })) return;
        if (g_place.active && g_place.req == request) { CancelCarried(); return; }
        core::PlaceRequestCancel(request);
        for (const auto& row : core::PlaceRequestState(request).rows) if (row.uid) {
            core::ForgetUid(row.uid); g_sel.erase(row.uid);
            if (g_primary == row.uid) g_primary = 0;
            if (g_lastClicked == row.uid) g_lastClicked = 0;
        }
    }
    // pivot of a member relative to the placement center for the given deltas. A single object with a measured box turns about
    // that box center for yaw, pitch and roll alike (the gizmo sits in the middle); sets keep their layout and turn about the +y axis.
    static Vec3 MemberRel(const Place& P, const Member& m, const Rot& fr, float yawDelta, float scaleMul) {
        if (P.hasNpc) return m.rel;   // a mixed/object+NPC grab is translation-only
        if (P.m.size() == 1 && P.haveCenter && P.prefabIdx >= 0) {
            const auto& info = core::PrefabIndex()[P.prefabIdx]; const float sc = m.scale0 * scaleMul;
            const Vec3 off = RotLocal(fr, info.cx * sc, info.cy * sc, info.cz * sc); return { -off.x, -off.y, -off.z };
        }
        const float t = yawDelta * 3.14159265f / 180.0f, cs = cosf(t), sn = sinf(t);
        return { (cs * m.rel.x + sn * m.rel.z) * scaleMul, m.rel.y * scaleMul, (-sn * m.rel.x + cs * m.rel.z) * scaleMul };
    }
    static void MembersTo(std::vector<core::MoveReq>& out, const Place& P, Vec3 center, float yawDelta, float scaleMul) {
        for (auto& m : P.m) {
            const Rot fr = P.hasNpc ? m.rot0 : Rot{ WrapYaw(m.rot0.yaw + yawDelta), WrapYaw(m.rot0.pitch + P.pitch), WrapYaw(m.rot0.roll + P.roll) };
            const Vec3 rel = MemberRel(P, m, fr, yawDelta, scaleMul);
            out.push_back({ m.uid, { center.x + rel.x, center.y + rel.y, center.z + rel.z }, fr, P.hasNpc ? m.scale0 : m.scale0 * scaleMul });
        }
    }
    static bool PrepareHistoryMutation(std::function<void()> intent, const std::vector<int>& targets) {
        ReconcileSpawnHistory();
        if (g_resumingMutation) return true;
        if (g_deferredEditor.action) { Note("grounding: mutation pending"); return false; }
        CommitBrushHistory();
        if (!ReconcileGroundBatch(true)) {
            auto& d = g_deferredEditor; d.action = std::move(intent); d.selection = g_sel; d.npcSelection = g_managedNpcSel;
            d.primary = g_primary; d.npcPrimary = g_managedNpcPrimary; d.project = g_projTab;
            d.placement = g_place.active ? g_place.generation : 0; d.clipboard = g_clipRevision; d.prefab = g_selPrefab;
            d.spawnYaw = g_spawnYaw; d.spawnScale = g_spawnScale; d.offsets = { g_off[0], g_off[1], g_off[2] };
            for (const auto& o : core::Spawned()) if (g_sel.count(o.uid) || std::find(targets.begin(), targets.end(), o.uid) != targets.end() ||
                (g_place.active && std::any_of(g_place.m.begin(), g_place.m.end(), [&](const Member& m) { return !m.npc && m.uid == o.uid; }))) d.targets.push_back(o);
            for (const auto& n : core::ManagedNpcs()) if (g_managedNpcSel.count(n.uid) || std::find(targets.begin(), targets.end(), -n.uid) != targets.end() ||
                (g_place.active && std::any_of(g_place.m.begin(), g_place.m.end(), [&](const Member& m) { return m.npc && m.uid == n.uid; }))) d.npcTargets.push_back(n);
            Note("grounding: waiting for actual movement completion"); return false;
        }
        ++g_historyBranch; return true;
    }
    static bool ReconcileGroundBatch(bool cancelUnapplied) {
        if (g_pendingGround.empty()) return true;
        auto views = core::GroundBarrier(g_pendingGround, cancelUnapplied);
        for (const auto& v : views) g_groundReports.Observe(v);
        for (const auto& v : views) if (!v.terminal()) return false;
        std::sort(views.begin(), views.end(), [](const auto& a, const auto& b) { return a.serial < b.serial; });
        bool any = false;
        for (const auto& v : views) {
            std::vector<Act> acts;
            for (const auto& m : v.members) if (m.accepted) {
                Act a; a.kind = Act::Move; a.uid = m.before.uid; a.prefab = m.before.prefab; a.proj = m.after.proj;
                if (m.before.proj) a.projects.insert(m.before.proj); if (m.after.proj) a.projects.insert(m.after.proj);
                a.pos0 = m.before.pos; a.rot0 = m.before.rot; a.sc0 = m.before.scale;
                a.pos1 = m.after.pos; a.rot1 = m.after.rot; a.sc1 = m.after.scale;
                if (ActChanged(a)) acts.push_back(a);
            }
            if (!acts.empty()) {
                const auto at = std::lower_bound(g_undo.begin(), g_undo.end(), v.serial, [](const HistoryEntry& e, uint64_t s) { return e.serial < s; });
                g_undo.insert(at, HistoryEntry{ v.serial, std::move(acts), v.branch, 0 }); any = true;
            }
            const auto carried = GroundPlacementOf(g_place);
            if (v.carried.generation && carried.generation == v.carried.generation && carried.transform == v.carried.transform && carried.members == v.carried.members) {
                bool moved = false; float dy = 0;
                for (const auto& m : v.members) if (m.accepted) { dy = m.after.pos.y - m.before.pos.y; moved = true; break; }
                if (moved) {
                    Place next = g_place; next.center.y += dy;
                    if (next.drag >= 1 && next.drag <= 3) next.dragCenter0.y += dy;
                    for (size_t i = 0; i < next.m.size(); ++i) {
                        auto& m = next.m[i]; const auto& r = v.members[i];
                        m.rel = { r.after.pos.x - next.center.x, r.after.pos.y - next.center.y, r.after.pos.z - next.center.z };
                        m.rot0 = r.after.rot; m.scale0 = r.after.scale;
                        next.historyBase[i] = { m.uid, r.after.pos, r.after.rot, r.after.scale };
                    }
                    next.yaw = next.pitch = next.roll = 0; next.scale = 1;
                    next.lastCenter = next.center; next.lastYaw = next.lastPitch = next.lastRoll = 0; next.lastScale = 1;
                    next.dirty = false; ++next.transform;
                    const auto h = *std::find_if(g_pendingGround.begin(), g_pendingGround.end(), [&](const auto& op) { return core::GroundStateOf(op).id == v.id; });
                    if (!core::GroundUpdateCarried(h, GroundPlacementOf(next), [next]() { g_place = next; })) {
                        Note("grounding: carried target changed after application"); FinishPlace();
                    }
                }
            }
        }
        if (any) { g_redo.clear(); g_editUid = 0; g_projectRefresh = true; }
        if (g_undo.size() > kHistoryLimit) g_undo.erase(g_undo.begin(), g_undo.end() - kHistoryLimit);
        core::GroundReconcile(g_pendingGround);
        g_groundLastResults = std::move(views); g_pendingGround.clear(); ++g_historyBranch;
        return true;
    }
    static void PumpGroundHistory() {
        g_seenGroundNotice = core::GroundNotice();
        if (!ReconcileGroundBatch(false) || !g_deferredEditor.action) return;
        DeferredEditorIntent d = std::move(g_deferredEditor); g_deferredEditor = {};
        bool valid = d.selection == g_sel && d.npcSelection == g_managedNpcSel && d.primary == g_primary && d.npcPrimary == g_managedNpcPrimary &&
            d.project == g_projTab && d.placement == (g_place.active ? g_place.generation : 0) && d.clipboard == g_clipRevision && d.prefab == g_selPrefab &&
            d.spawnYaw == g_spawnYaw && d.spawnScale == g_spawnScale && d.offsets.x == g_off[0] && d.offsets.y == g_off[1] && d.offsets.z == g_off[2];
        const auto all = core::Spawned(); const auto npcs = core::ManagedNpcs();
        for (auto expected : d.targets) {
            for (const auto& v : g_groundLastResults) for (const auto& m : v.members) if (m.before.uid == expected.uid) expected = m.after;
            const auto* n = Find(all, expected.uid);
            valid &= n && n->gen == expected.gen && n->poseGen == expected.poseGen && n->hidden == expected.hidden && n->proj == expected.proj && n->group == expected.group && n->note == expected.note;
        }
        for (const auto& e : d.npcTargets) {
            const auto* n = FindManagedNpc(npcs, e.uid);
            valid &= n && n->gen == e.gen && n->hidden == e.hidden && !VecChanged(n->pos, e.pos) && n->proj == e.proj && n->group == e.group &&
                n->aiEnabled == e.aiEnabled && n->behavior == e.behavior && n->label == e.label && n->note == e.note;
        }
        if (!valid) { Note("grounding: deferred mutation canceled (stale selection or target)"); return; }
        ++g_historyBranch;
        struct ResumeScope { ResumeScope() { g_resumingMutation = true; } ~ResumeScope() { g_resumingMutation = false; } } scope;
        d.action();
    }
    static bool ApplyCarriedPose(const Place& candidate, bool send) {
        if (!g_place.active || candidate.generation != g_place.generation || candidate.transform != g_place.transform) return false;
        const bool changed = VecChanged(candidate.center, g_place.center) || candidate.yaw != g_place.yaw || candidate.pitch != g_place.pitch ||
            candidate.roll != g_place.roll || candidate.scale != g_place.scale || candidate.haveCenter != g_place.haveCenter;
        if (changed) {
            // Exact UI motion accumulates against the last successful native send, not the previous frame.
            const bool significant = fabsf(candidate.center.x - g_place.lastCenter.x) > 0.005f ||
                fabsf(candidate.center.y - g_place.lastCenter.y) > 0.005f || fabsf(candidate.center.z - g_place.lastCenter.z) > 0.005f ||
                fabsf(candidate.yaw - g_place.lastYaw) > 0.01f || fabsf(candidate.pitch - g_place.lastPitch) > 0.01f ||
                fabsf(candidate.roll - g_place.lastRoll) > 0.01f || fabsf(candidate.scale - g_place.lastScale) > 0.001f;
            // A clean micro-sample during Applying is reconstructed by the held gesture next frame;
            // it must not consume the single deferred command slot or publish a stale carried identity.
            if (!g_place.dirty && !significant && !ReconcileGroundBatch(true)) return false;
            if (!PrepareHistoryMutation([candidate, send]() { if (ApplyCarriedPose(candidate, send) && candidate.commitRequested) CommitPlaceHistory(g_place); })) return false;
            if (!g_place.active || candidate.generation != g_place.generation || candidate.transform != g_place.transform) return false;
            Place next = candidate; next.commitRequested = false; next.touched = true; next.dirty = g_place.dirty || significant; ++next.transform;
            if (!core::PublishGroundPlacement(GroundPlacementOf(next))) return false;
            g_place = std::move(next);
        } else {
            g_place.hover = candidate.hover; g_place.drag = candidate.drag; g_place.drag0 = candidate.drag0;
            g_place.dragCenter0 = candidate.dragCenter0; g_place.dragYaw0 = candidate.dragYaw0; g_place.dragPitch0 = candidate.dragPitch0;
            g_place.dragRoll0 = candidate.dragRoll0; g_place.dragScale0 = candidate.dragScale0;
            g_place.keyTransformHeld = candidate.keyTransformHeld; g_place.mouse = candidate.mouse;
        }
        if (send && g_place.dirty && g_pendingGround.empty()) {
            std::vector<core::MoveReq> moves, objects; MembersTo(moves, g_place, g_place.center, g_place.yaw, g_place.scale); bool sent = true;
            for (size_t i = 0; i < moves.size(); ++i) {
                if (g_place.m[i].npc) sent = core::MoveManagedNpcLive(moves[i].uid, moves[i].pos) && sent; else objects.push_back(moves[i]);
            }
            if (!objects.empty()) sent = core::MoveMany(objects, false) && sent;
            if (sent) { g_place.dirty = false; g_place.lastSend = TickNow(); g_place.lastCenter = g_place.center;
                g_place.lastYaw = g_place.yaw; g_place.lastScale = g_place.scale; g_place.lastPitch = g_place.pitch; g_place.lastRoll = g_place.roll; }
        }
        return true;
    }
    static bool PlaceCommitLive(const Place& candidate, bool send) { return ApplyCarriedPose(candidate, send); }
    static void CommitPlaceHistory(Place& P) {
        if (&P != &g_place) { P.commitRequested = true; return; }
        if (!PrepareHistoryMutation([]() { CommitPlaceHistory(g_place); })) return;
        if (!P.active || P.historyBase.size() != P.m.size()) return;
        std::vector<core::MoveReq> now; MembersTo(now, P, P.center, P.yaw, P.scale);
        std::vector<Act> acts; acts.reserve(now.size());
        std::vector<core::MoveReq> objects;
        for (size_t i = 0; i < now.size(); ++i) {
            const auto& before = P.historyBase[i]; const auto& after = now[i];
            Act a; a.kind = P.m[i].npc ? Act::NpcMove : Act::Move; a.uid = P.m[i].uid; a.prefab = P.m[i].prefab;
            a.pos0 = before.pos; a.rot0 = before.rot; a.sc0 = before.scale;
            a.pos1 = after.pos; a.rot1 = after.rot; a.sc1 = after.scale;
            if (!ActChanged(a)) continue;
            if (!P.m[i].npc) objects.push_back(after);
            acts.push_back(std::move(a));
        }
        if (!objects.empty() && !core::MoveMany(objects, true)) { Note("placement: move refused"); return; }
        acts.erase(std::remove_if(acts.begin(), acts.end(), [](const Act& a) {
            return a.kind == Act::NpcMove && !core::CommitManagedNpcMove(a.uid, a.pos1);
        }), acts.end());
        Push(std::move(acts), P.generation);
        P.historyBase = std::move(now);
        P.lastCenter = P.center; P.lastYaw = P.yaw; P.lastScale = P.scale; P.lastPitch = P.pitch; P.lastRoll = P.roll; P.dirty = false;
    }
    // vertical extent of a member's (rotated, scaled) box relative to the placement center; objects without a box count as pivot .. pivot + 2 m
    static void MemberExtentY(const Place& P, const Member& m, float* lo, float* hi) {
        const Rot fr{ WrapYaw(m.rot0.yaw + P.yaw), WrapYaw(m.rot0.pitch + P.pitch), WrapYaw(m.rot0.roll + P.roll) };
        const Vec3 rel = MemberRel(P, m, fr, P.yaw, P.scale); const int pi = IndexOfPrefab(m.prefab);
        if (m.npc) { *lo = rel.y; *hi = rel.y + 1.8f; return; }
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
        const float cy = gh.centerY - gh.radius;
        core::Log("[ground] cast from y %.2f: %s fraction %.4f center %.2f (object %.2f..%.2f)", startY, gh.hit ? "hit" : "no hit", gh.fraction, gh.centerY, bottom, top);
        if (++iter > 40) return -1;
        if (!gh.hit) {
            if (iter == 1) { startY = top + 150.0f; return 0; }
            return -1;
        }
        const bool inside = gh.fraction <= 0.0005f;   // the sphere started inside a body (Havok reports a negative fraction)
        if (iter == 1 && inside) { startY = top + 150.0f; return 0; }   // buried: something solid sits above the top, so the surface is found from far above
        if (startY > bottom - 0.1f) {                                    // still beside the object: hits here are the object itself (a 36 m tower needs one jump, not 0.5 m steps)
            if (inside || (cy > bottom + 0.03f && cy < top + 0.03f)) { startY = bottom - 0.1f; return 0; }
            *groundY = cy; return 1;                                     // a surface above the object (far-above cast) or already below its bottom
        }
        if (inside) { startY -= 0.5f; return 0; }                        // collision reaches below the bounding box: step out of it
        *groundY = cy; return 1;
    }
    static void StartGroundSnap(Place& P) {
        if (P.hasNpc) { g_projectStatus = "Grounding supports objects only; NPC and mixed selections were not changed."; Note("%s", T(g_projectStatus.c_str())); return; }
        std::vector<int> uids; for (const auto& m : P.m) uids.push_back(m.uid);
        BeginGrounding(uids, true, GroundPlacementOf(P));
    }
    // drops the carried set at its current transform: fresh objects (collision + render state match the final transform), one undo step
    static void DropCarried() {
        if (!PrepareHistoryMutation([]() { DropCarried(); })) return;
        Place& P = g_place; if (!P.active) return;
        CommitPlaceHistory(P);
        if (P.dirty) return;
        if (P.hasNpc && P.historyBase.size() == P.m.size()) {
            for (size_t i = 0; i < P.m.size(); ++i) if (P.m[i].npc)
                core::EndManagedNpcMove(P.m[i].uid, P.historyBase[i].pos);
        }
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
        if (pressed(PK_LEVEL) && !P.hasNpc) {
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
            if (rot && !P.hasNpc) P.yaw += rot * ys;
            P.center = { SnapV(P.center.x, ps), SnapV(P.center.y, ps), SnapV(P.center.z, ps) }; if (!P.hasNpc) P.yaw = WrapYaw(SnapV(P.yaw, ys));
        } else {
            if (held(PK_FWD)) fwd += 1; if (held(PK_BACK)) fwd -= 1;
            if (held(PK_RIGHT)) side += 1; if (held(PK_LEFT)) side -= 1;
            if (held(PK_UP)) up += 1; if (held(PK_DOWN)) up -= 1;
            if (!P.hasNpc) { if (held(PK_ROT_L)) P.yaw -= 60.0f * dt; if (held(PK_ROT_R)) P.yaw += 60.0f * dt; P.yaw = WrapYaw(P.yaw); }
            if (fwd || side || up) { P.center.x += (ax * fwd + rx * side) * speed * dt; P.center.z += (az * fwd + rz * side) * speed * dt; P.center.y += up * speed * dt; }
        }
        if (!P.hasNpc && held(PK_SCALE_UP)) P.scale = std::min(20.0f, P.scale * (1.0f + dt));
        if (!P.hasNpc && held(PK_SCALE_DOWN)) P.scale = std::max(0.05f, P.scale / (1.0f + dt));
        if (fetch && g_havePlayer) { P.center = InFront(P.radius, P.center.y - g_lastPlayer.y); CommitPlaceHistory(P); }
        const bool transformHeld = held(PK_FWD) || held(PK_BACK) || held(PK_LEFT) || held(PK_RIGHT) || held(PK_UP) || held(PK_DOWN) ||
            (!P.hasNpc && (held(PK_ROT_L) || held(PK_ROT_R) || held(PK_SCALE_UP) || held(PK_SCALE_DOWN)));
        if (P.keyTransformHeld && !transformHeld) CommitPlaceHistory(P);
        P.keyTransformHeld = transformHeld;
        return false;
    }
    static void PlaceTick() {
        if (!g_place.active) return;
        Place P = g_place;
        if (P.hasNpc) { P.yaw = P.pitch = P.roll = 0.0f; P.scale = 1.0f; }   // NPC/mixed grabs are translation-only
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
                    if (P.hasNpc && P.hover != 1 && P.hover != 2 && P.hover != 3 && P.hover != 5) P.hover = 0;
                    if (P.hover && ImGui::IsMouseClicked(0) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
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
                    if (g_snap) { const float ps = kSnapPos[g_snapPosIdx], ys = kSnapYaw[g_snapYawIdx]; P.center = { SnapV(P.center.x, ps), SnapV(P.center.y, ps), SnapV(P.center.z, ps) }; if (!P.hasNpc) { P.yaw = WrapYaw(SnapV(P.yaw, ys)); P.pitch = WrapYaw(SnapV(P.pitch, ys)); P.roll = WrapYaw(SnapV(P.roll, ys)); } }
                }
            }
        } else { if (P.drag) CommitPlaceHistory(P); P.hover = 0; P.drag = 0; }
        if (PlaceCommitLive(P, TickNow() - P.lastSend >= 60) && P.commitRequested) CommitPlaceHistory(g_place);
    }
    // ---- selection helpers, undo, copy/paste ----
    static std::vector<int> SelUids() { return std::vector<int>(g_sel.begin(), g_sel.end()); }
    static std::vector<int> SceneGrabIds() {
        std::vector<int> ids; ids.reserve(g_sel.size() + g_managedNpcSel.size());
        for (int uid : g_sel) ids.push_back(uid);
        for (int uid : g_managedNpcSel) ids.push_back(-uid);
        return ids;
    }
    static bool CtrlHeld(const ImGuiIO& io) { return io.KeyCtrl || (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0; }
    static bool ShiftHeld(const ImGuiIO& io) { return io.KeyShift || (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0; }
    static void ClearSceneSelection() {
        g_sel.clear(); g_primary = g_lastClicked = 0;
        g_managedNpcSel.clear(); g_managedNpcPrimary = g_managedNpcLast = 0;
        g_terrainTileSelected = false;
        g_sceneLastEntity = 0; g_editUid = 0;
    }
    static void RefreshSelectionPrimary() {
        if (!g_sel.count(g_primary)) g_primary = g_sel.empty() ? 0 : *g_sel.begin();
        if (!g_managedNpcSel.count(g_managedNpcPrimary)) g_managedNpcPrimary = g_managedNpcSel.empty() ? 0 : *g_managedNpcSel.begin();
        if (!g_sel.count(g_lastClicked)) g_lastClicked = 0;
        if (!g_managedNpcSel.count(g_managedNpcLast)) g_managedNpcLast = 0;
        g_sceneLastEntity = g_primary ? g_primary : g_managedNpcPrimary ? -g_managedNpcPrimary : 0;
    }
    static void ClearDeletedSelectionUi(bool deletedObject) {
        g_propertiesOpen = g_propertiesPopupRequested = false;
        g_numericSceneUid = g_numericNpcUid = 0; CancelNumericEdit();
        g_hoverUid = g_hoverNpcUid = g_rightUid = 0;
        g_sceneLastEntity = g_lastClicked = g_editUid = 0;
        if (deletedObject) g_selPrefab = -1;
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
        for (int uid : g_managedNpcSel) { const ManagedNpc* m = FindManagedNpc(npcs, uid); if (!m || m->hidden) continue; const Vec3 p = ManagedNpcDisplayPos(*m); c.x += p.x; c.y += p.y + 0.9f; c.z += p.z; n++; }
        if (!n) return false; c.x /= n; c.y /= n; c.z /= n;
        float radius = 1.0f;
        for (int uid : g_sel) { const SpawnedObj* o = Find(list, uid); if (!o || o->hidden) continue; const Vec3 bc = BboxCenter(*o); const float dx = bc.x - c.x, dy = bc.y - c.y, dz = bc.z - c.z; radius = std::max(radius, sqrtf(dx * dx + dy * dy + dz * dz) + ObjectRadius(*o)); }
        for (int uid : g_managedNpcSel) { const ManagedNpc* m = FindManagedNpc(npcs, uid); if (!m || m->hidden) continue; const Vec3 p = ManagedNpcDisplayPos(*m); const float dx = p.x - c.x, dy = (p.y + 0.9f) - c.y, dz = p.z - c.z; radius = std::max(radius, sqrtf(dx * dx + dy * dy + dz * dz) + 1.2f); }
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
        g_terrainTileSelected = false;
        if (g_place.active) DropCarried();   // selecting something else ends the placement
        if (!add) ClearSceneSelection();
        auto addOne = [&](int u) { if (g_sel.count(u)) { if (add) g_sel.erase(u); } else g_sel.insert(u); };
        const SpawnedObj* o = Find(list, uid);
        if (g_selectGroups && o && o->group > 0 && !add) {
            for (auto& x : list) if (x.group == o->group && !x.hidden) g_sel.insert(x.uid);
            for (const auto& n : core::ManagedNpcs()) if (n.group == o->group && !n.hidden) g_managedNpcSel.insert(n.uid);
        }
        else addOne(uid);
        if (g_sel.count(uid)) g_primary = g_lastClicked = uid;
        RefreshSelectionPrimary();
        if (g_sel.count(uid)) g_sceneLastEntity = uid;
    }
    // snap to ground for placed objects: one probe per object, results applied as they arrive (undoable move)
    struct SnapJob { core::GroundHandle op; int ticket = 0, iter = 0; float bottom = 0, top = 0, startY = 0, x = 0, z = 0; };
    static std::vector<SnapJob> g_snapJobs;
    static void SceneNumericApply(int uid, Vec3 pos, Rot rot, float scale, bool final);
    static std::vector<core::GroundHandle> BeginGrounding(const std::vector<int>& uids, bool rigid, const core::GroundPlacement& carried) {
        if (!g_managedNpcSel.empty() || (g_place.active && g_place.hasNpc)) {
            g_projectStatus = "Grounding supports objects only; NPC and mixed selections were not changed.";
            Note("%s", T(g_projectStatus.c_str())); return {};
        }
        if (g_deferredEditor.action) { Note("grounding: mutation pending"); return {}; }
        CommitBrushHistory();
        for (const auto& op : g_pendingGround) {
            const auto v = core::GroundStateOf(op);
            const bool overlap = std::any_of(v.members.begin(), v.members.end(), [&](const auto& m) { return std::find(uids.begin(), uids.end(), m.before.uid) != uids.end(); });
            if (overlap && (v.state == core::GroundApplying || v.terminal())) {
                if (!PrepareHistoryMutation([uids, rigid, carried]() { BeginGrounding(uids, rigid, carried.generation ? GroundPlacementOf(g_place) : core::GroundPlacement{}); }, uids)) return {};
                break;
            }
        }
        ReconcileGroundBatch(false);
        if (g_numericSceneUid && std::find(uids.begin(), uids.end(), g_numericSceneUid) != uids.end()) {
            SceneNumericApply(g_numericSceneUid, { g_edit[0], g_edit[1], g_edit[2] }, g_editRot, g_editScale, true);
            if (g_deferredEditor.action) return {};
        }
        if (carried.generation) {
            if (g_place.generation != carried.generation || !g_place.active) return {};
            // Close the upstream gesture checkpoint before reserving a later grounding serial.
            CommitPlaceHistory(g_place);
            if (g_deferredEditor.action) return {};
        }
        const auto context = carried.generation ? GroundPlacementOf(g_place) : carried;
        std::vector<core::GroundHandle> added;
        const size_t count = rigid ? (uids.empty() ? 0 : 1) : uids.size();
        for (size_t i = 0; i < count; ++i) {
            const auto targets = rigid ? uids : std::vector<int>{ uids[i] };
            auto op = core::BeginGround(targets, ++g_historySerial, g_historyBranch, context);
            g_pendingGround.push_back(op); added.push_back(op);
            proj_codec::Bounds bounds;
            if (!core::GroundBounds(op, bounds)) { core::GroundCancel(op, "refused"); continue; }
            SnapJob j; j.op = op; j.bottom = (float)bounds.min.y; j.top = (float)bounds.max.y; j.startY = j.top + 5.0f;
            // Probe under the measured geometry, not its prefab pivot, which may sit well outside the visible mesh.
            j.x = (float)((bounds.min.x + bounds.max.x) * 0.5);
            j.z = (float)((bounds.min.z + bounds.max.z) * 0.5);
            j.ticket = core::GroundTicket(op, { j.x, j.startY, j.z }, 400.0f); g_snapJobs.push_back(j);
        }
        for (const auto& op : g_pendingGround) g_groundReports.Observe(core::GroundStateOf(op));
        return added;
    }
    static void SnapSelToGround() { BeginGrounding(SelUids(), false); }
    static void PumpSnapJobs() {
        core::GroundFrame(); PumpGroundHistory();
        for (size_t i = 0; i < g_snapJobs.size();) {
            auto& j = g_snapJobs[i];
            if (!core::GroundValidate(j.op)) { g_snapJobs.erase(g_snapJobs.begin() + i); continue; }
            if (!j.ticket) { j.ticket = core::GroundTicket(j.op, { j.x, j.startY, j.z }, 400.0f); ++i; continue; }
            core::GroundHit hit; if (!core::GroundPoll(j.op, &hit)) { ++i; continue; }
            j.ticket = 0; float groundY = 0;
            const int step = std::isfinite(hit.centerY) && std::isfinite(hit.fraction) ? GroundStep(hit, j.x, j.z, j.bottom, j.top, j.startY, j.iter, &groundY) : -1;
            if (!step && j.iter < 40) { j.ticket = core::GroundTicket(j.op, { j.x, j.startY, j.z }, 400.0f); ++i; continue; }
            if (step == 1) {
                std::vector<core::MoveReq> moves; const auto v = core::GroundStateOf(j.op);
                for (const auto& m : v.members) { Vec3 pos = m.before.pos; pos.y += groundY - j.bottom; moves.push_back({ m.before.uid, pos, m.before.rot, m.before.scale }); }
                core::GroundApply(j.op, moves);
            } else core::GroundCancel(j.op, "no-surface");
            g_snapJobs.erase(g_snapJobs.begin() + i);
        }
        for (const auto& op : g_pendingGround) g_groundReports.Observe(core::GroundStateOf(op));
    }
    static void ApplyNumeric(int uid, Vec3 pos, Rot rot, float scale, bool final) {
        if (!PrepareHistoryMutation([uid, pos, rot, scale, final]() { ApplyNumeric(uid, pos, rot, scale, final); }, { uid })) return;
        const auto list = core::Spawned(); const auto* o = Find(list, uid); if (!o || o->hidden) return;
        if (g_numericSceneUid != uid) { g_numericSceneUid = uid; g_editPos0 = o->pos; g_editRot0 = o->rot; g_editScale0 = o->scale; }
        if (!core::MoveMany({ { uid, pos, rot, scale } }, final)) return;
        if (final) {
            Act a; a.kind = Act::Move; a.uid = uid; a.prefab = o->prefab;
            a.pos0 = g_editPos0; a.rot0 = g_editRot0; a.sc0 = g_editScale0; a.pos1 = pos; a.rot1 = rot; a.sc1 = scale; Push({ a });
            g_editPos0 = pos; g_editRot0 = rot; g_editScale0 = scale; g_numericSceneUid = 0;
        }
    }
    static void SceneNumericApply(int uid, Vec3 pos, Rot rot, float scale, bool final) { ApplyNumeric(uid, pos, rot, scale, final); }
    static void NpcNumericApply(int uid, Vec3 pos, bool final) {
        if (!PrepareHistoryMutation([uid, pos, final]() { NpcNumericApply(uid, pos, final); }, { -uid })) return;
        const auto list = core::ManagedNpcs(); const auto* n = FindManagedNpc(list, uid); if (!n || n->hidden) return;
        if (g_numericNpcUid != uid) {
            if (!core::BeginManagedNpcMove(uid)) { Note("NPC live move is not ready"); return; }
            g_numericNpcUid = uid; g_numericNpcBase = ManagedNpcDisplayPos(*n);
        }
        if (final) {
            if (!core::EndManagedNpcMove(uid, pos)) return;
            Act a; a.kind = Act::NpcMove; a.uid = uid; a.pos0 = g_numericNpcBase; a.pos1 = pos; a.prefab = n->label; Push({ a }); g_numericNpcUid = 0;
        } else core::MoveManagedNpcLive(uid, pos);
    }
    static void FinalizeNumericEdits() {
        if (g_deferredEditor.action || !g_pendingGround.empty() || (g_open && (ImGui::IsAnyItemActive() || g_numericEditId))) return;
        if (g_numericSceneUid) {
            const auto all = core::Spawned(); const auto* o = Find(all, g_numericSceneUid);
            if (o && !o->hidden) SceneNumericApply(o->uid, o->pos, o->rot, o->scale, true);
            else g_numericSceneUid = 0;
        }
        if (g_numericNpcUid) {
            const auto all = core::ManagedNpcs(); const auto* n = FindManagedNpc(all, g_numericNpcUid);
            if (n && !n->hidden) NpcNumericApply(n->uid, n->liveMovePending ? n->liveMoveTarget : ManagedNpcDisplayPos(*n), true);
            else g_numericNpcUid = 0;
        }
    }
    static void ForgetSelection() {
        if (!PrepareHistoryMutation([]() { ForgetSelection(); })) return;
        for (int uid : g_sel) core::ForgetUid(uid);
        for (int uid : g_managedNpcSel) core::ForgetManagedNpc(uid);
        ClearSceneSelection();
    }
    static void DeleteSel() {
        if (!PrepareHistoryMutation([]() { DeleteSel(); })) return;
        auto list = core::Spawned(); std::vector<Act> acts;
        std::set<int> failed;
        for (int uid : g_sel) { const SpawnedObj* o = Find(list, uid); if (!o || o->hidden) continue; Act a; a.kind = Act::Delete; a.uid = uid; a.prefab = o->prefab; a.pos0 = o->pos; a.rot0 = o->rot; a.sc0 = o->scale; a.group = o->group; a.proj = o->proj; a.text0 = o->note; if (core::HideUid(uid)) acts.push_back(a); else failed.insert(uid); }
        if (!acts.empty()) { Note(T("deleted %d objects"), (int)acts.size()); Push(acts); }
        g_sel = std::move(failed); g_primary = g_sel.empty() ? 0 : *g_sel.begin();
        if (!SceneHasSelection()) ClearDeletedSelectionUi(!acts.empty());
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
    static void RemoveDuplicates() {
        if (!PrepareHistoryMutation([]() { RemoveDuplicates(); })) return;
        ClearSceneSelection();
        if (SelectDuplicates()) DeleteSel();
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
        if (!PrepareHistoryMutation([]() { Undo(); })) return;
        if (g_place.active) { DropCarried(); if (g_place.active) return; }
        if (g_undo.empty()) return;
        HistoryEntry entry = std::move(g_undo.back()); g_undo.pop_back();
        std::vector<Act>& acts = entry.acts;
        std::vector<core::MoveReq> moves;
        for (auto& a : acts) {
            if (a.kind == Act::Spawn) { core::HideUid(a.uid); RemoveSelectionUid(a.uid); }
            else if (a.kind == Act::Delete) { if (core::RestoreUid(a.uid)) { g_sel.insert(a.uid); g_primary = a.uid; } else RemoveSelectionUid(a.uid); }
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
            else if (a.kind == Act::TerrainBatch) {
                const auto live = core::TerrainStrokes();
                if (live.size() >= a.terrain.size()) a.terrain.assign(live.end() - a.terrain.size(), live.end());
                CaptureProvenance(a); // retain adoption before removing the last live reference
                for (size_t i = 0; i < a.terrain.size(); ++i) core::TerrainUndo();
            }
            else if (a.kind == Act::TerrainClear) { core::TerrainRestoreStrokes(a.terrain, a.terrainIndices); }
            else if (a.kind == Act::TerrainTileRemove) { CaptureProvenance(a); core::TerrainRestoreStrokes(a.terrain, a.terrainIndices); }
        }
        if (!moves.empty()) core::MoveMany(moves, true);
        g_redo.push_back(std::move(entry)); g_projectRefresh = true; Note(T("undo"));
    }
    static void Redo() {
        if (!PrepareHistoryMutation([]() { Redo(); })) return;
        if (g_place.active) { DropCarried(); if (g_place.active) return; }
        if (g_redo.empty()) return;
        HistoryEntry entry = std::move(g_redo.back()); g_redo.pop_back();
        std::vector<Act>& acts = entry.acts;
        std::vector<core::MoveReq> moves;
        for (auto& a : acts) {
            if (a.kind == Act::Spawn) { if (core::RestoreUid(a.uid)) { g_sel.insert(a.uid); g_primary = a.uid; } else RemoveSelectionUid(a.uid); }
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
            else if (a.kind == Act::TerrainBatch) { for (const auto& t : a.terrain) core::TerrainAddStroke(t); }
            else if (a.kind == Act::TerrainClear) { core::TerrainRemoveProject(a.proj, a.terrain, a.terrainIndices); CaptureProvenance(a); }
            else if (a.kind == Act::TerrainTileRemove) core::TerrainRemoveTile(a.tileX, a.tileZ, a.proj, a.terrain, a.terrainIndices);
        }
        if (!moves.empty()) core::MoveMany(moves, true);
        g_undo.push_back(std::move(entry)); g_projectRefresh = true; Note(T("redo"));
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
        if (k == Act::TerrainBatch) return T("Terrain");
        if (k == Act::TerrainClear) return T("Clear project terrain");
        if (k == Act::TerrainTileRemove) return T("Remove terrain tile");
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
        if (a.kind == Act::TerrainBatch || a.kind == Act::TerrainClear || a.kind == Act::TerrainTileRemove) {
            ImGui::Text("%s", T("Terrain"));
            if (a.kind == Act::TerrainTileRemove) ImGui::TextDisabled(T("Terrain tile %d, %d"), a.tileX, a.tileZ);
            ImGui::Indent(); ImGui::TextDisabled(T("%d strokes"), (int)a.terrain.size()); ImGui::Unindent();
            return;
        }
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
        if (view.empty()) ImGui::TextDisabled("%s", T("No history yet. Place or edit something to see it here."));
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
        ++g_clipRevision;
        auto list = core::Spawned(); g_clip.clear(); Vec3 c{ 0, 0, 0 }; int n = 0;
        for (int uid : g_sel) { const SpawnedObj* o = Find(list, uid); if (!o || o->hidden) continue; c.x += o->pos.x; c.y += o->pos.y; c.z += o->pos.z; n++; }
        if (!n) return; c.x /= n; c.y /= n; c.z /= n; g_clipCenter = c; g_clipRadius = 1;
        for (int uid : g_sel) { const SpawnedObj* o = Find(list, uid); if (!o || o->hidden) continue; g_clip.push_back({ o->prefab, { o->pos.x - c.x, o->pos.y - c.y, o->pos.z - c.z }, o->rot, o->scale });
            float dx = o->pos.x - c.x, dz = o->pos.z - c.z; g_clipRadius = std::max(g_clipRadius, sqrtf(dx * dx + dz * dz) + 1.0f); }
        Note(T("copied %d objects"), (int)g_clip.size());
    }
    // spawns a set of objects around a center, groups them (when more than one) and hands them to the placement mode
    static void SpawnSet(const std::vector<ClipItem>& items, Vec3 center, float radius, const char* what) {
        const std::string label = what;
        if (!PrepareHistoryMutation([items, center, radius, label]() { SpawnSet(items, center, radius, label.c_str()); })) return;
        if (items.empty() || !core::GameThreadReady()) return;
        int group = items.size() > 1 ? core::NewGroupId() : 0; std::vector<int> uids;
        for (auto& it : items) { int uid = core::SpawnAt(it.prefab, { center.x + it.rel.x, center.y + it.rel.y, center.z + it.rel.z }, it.rot, it.scale, group); if (uid) uids.push_back(uid); }
        if (uids.empty()) return;
        std::string name = items.size() == 1 ? ShortName(items[0].prefab) : std::string(T(what)) + " (" + std::to_string(items.size()) + ")";
        StartGrab(uids, true, name);
        g_place.radius = std::max(g_place.radius, radius);
    }
    static void Paste(bool havePos) {
        if (!PrepareHistoryMutation([havePos]() { Paste(havePos); })) return;
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
        if (!PrepareHistoryMutation([group]() { GroupSel(group); })) return;
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
        if (!PrepareHistoryMutation([degrees]() { RotateSel(degrees); })) return;
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
        if (!PrepareHistoryMutation([delta]() { MoveSel(delta); })) return;
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
        if (!PrepareHistoryMutation([factor]() { ScaleSel(factor); })) return;
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
        g_propertiesOpen = true;
        g_propertiesPopupRequested = true;
    }
    static void AlignSel(int axis) {
        if (!PrepareHistoryMutation([axis]() { AlignSel(axis); })) return;
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
        // The game routes ordinary keys to itself when no text field is active, so ImGui alone
        // cannot detect editor shortcuts. Poll the same key state used by the free camera.
        auto pressed = [&](int vk, ImGuiKey key) {
            const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0 || input::VkDown(vk);
            const bool fire = (now && !g_cameraShortcutDown[vk & 0xFF]) || ImGui::IsKeyPressed(key, false);
            g_cameraShortcutDown[vk & 0xFF] = now;
            return fire;
        };
        const bool pZ = pressed('Z', ImGuiKey_Z), pY = pressed('Y', ImGuiKey_Y);
        const bool pC = pressed('C', ImGuiKey_C), pV = pressed('V', ImGuiKey_V);
        const bool pD = pressed('D', ImGuiKey_D), pG = pressed('G', ImGuiKey_G);
        const bool pA = pressed('A', ImGuiKey_A), pDelete = pressed(VK_DELETE, ImGuiKey_Delete);
        const bool ctrl = CtrlHeld(io) || input::VkDown(VK_CONTROL);
        DWORD foregroundProcess = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
        if (foregroundProcess != GetCurrentProcessId()) return;
        if (io.WantTextInput || g_playMode) return;
        // World shortcuts belong to the 3D picking area. Let ImGui's own controls handle
        // keyboard commands while the pointer is over a window or an item is active.
        const bool worldSelectionContext =
            !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive() && !g_worldPopupOpen;
        if (ctrl && pZ) Undo();
        if (ctrl && pY) Redo();
        if (worldSelectionContext && ctrl && pA) {
            SelectAllSceneEntities(core::Spawned(), core::ManagedNpcs());
            core::Log("[editor/world-hotkey] Ctrl+A selected %zu objects and %zu NPCs", g_sel.size(), g_managedNpcSel.size());
        }
        if (worldSelectionContext && ctrl && pC && (!g_sel.empty() || g_managedNpcSel.empty())) CopySel();
        if (worldSelectionContext && ctrl && pV) Paste(havePos);
        if (worldSelectionContext && ctrl && pD && !g_sel.empty()) { CopySel(); Paste(havePos); }
        if (worldSelectionContext && ctrl && pG && SceneHasSelection()) GroupSceneSelection(true);
        if (worldSelectionContext && !ctrl && pDelete && SceneHasSelection()) DeleteSceneSelection();
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
                const ImVec2 star0 = t0, star1 = { t0.x + std::min(tile, 28.0f * ui), t0.y + std::min(tile, 28.0f * ui) };
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
                    st = T(st);
                    const ImVec2 ts = ImGui::CalcTextSize(st);
                    dl->AddText({ t0.x + (tile - ts.x) * 0.5f, t0.y + (tile - ts.y) * 0.5f }, ImGui::GetColorU32(ImGuiCol_TextDisabled), st);
                }
                const bool fav = core::IsFavorite(i);
                if (fav || hov) { const ImVec2 icon = ImGui::CalcTextSize(ICON_STAR); dl->AddRectFilled(star0, star1, IM_COL32(0, 0, 0, 110), 3.0f); dl->AddText({ star0.x + (star1.x - star0.x - icon.x) * 0.5f, star0.y + (star1.y - star0.y - icon.y) * 0.5f }, fav ? IM_COL32(255, 199, 64, 255) : IM_COL32(200, 200, 200, 160), ICON_STAR); }
                dl->PushClipRect({ p0.x + pad, t1.y }, { p1.x - pad, p1.y }, true);
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), { p0.x + pad, t1.y + 2 }, ImGui::GetColorU32(ImGuiCol_Text), ShownName(pi).c_str(), nullptr, tile);
                dl->PopClipRect();
                if (hov && !ImGui::IsPopupOpen("cardctx")) ImGui::SetTooltip("%s\n%s\n%s\n%s%s", ShownName(pi).c_str(), pi.path.c_str(), core::Categories()[pi.cat].name.c_str(), LocalizedTags(pi.tags).c_str(), onStar ? (std::string("\n") + T("(click: favorite)")).c_str() : "");
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }
    // ---- NPCs and creatures: catalog + managed server actors ------------------------------------------------------------
    // NPCs created here are registered by World Builder. Their live actor is captured from the game's own SpawnCharacter
    // request so they can be selected, moved in-place through TransformSync when safe (respawn fallback), deleted,
    // AI-controlled and persisted in .cdproj files.
    static char g_npcFilter[128] = ""; static int g_npcCat = 0, g_npcSel = -1, g_npcCount = 1; static float g_npcDist = 5.0f;
    static int g_npcFormation = 1; static float g_npcSpacing = 1.5f, g_npcRadius = 8.0f;
    static bool g_npcSpawnAi = true; static int g_npcSpawnBehavior = 0;
    static int g_npcNoteEditUid = 0, g_npcLabelEditUid = 0, g_groupNameEditId = 0, g_objectNoteEditUid = 0;
    static bool g_metadataEditing = false;
    static int g_metadataPopupRequest = 0;   // opened from the root ID scope so row/context-menu IDs do not trap the modal
    static char g_npcNoteEdit[512] = "", g_npcLabelEdit[160] = "", g_groupNameEdit[160] = "", g_objectNoteEdit[512] = "";
    // NPCs only live where the world is streamed in around the character, and every one is a full server actor: a few hundred
    // within a few hundred metres is what the game copes with (the server jobs are spread over ticks, see ProcessServerJobs).
    static const int kNpcMaxCount = 500; static const float kNpcMaxDist = 500.0f, kNpcMaxExtent = 200.0f;
    static std::vector<int> g_npcRows; static std::string g_npcKey;
    static const char* kNpcCats[] = { "all", "people", "animals and mounts", "monsters", "bosses", "other" };
    static const char* kNpcFormations[] = { "Line", "Matrix", "Circle" };
    static const char* kNpcBehaviors[] = { "Normal autonomous", "Hold position (AI paused)" };
    static std::vector<Vec3> NpcFormationPositions(Vec3 center, int count, int formation, float spacing, float radius, float fx, float fz) {
        count = std::clamp(count, 1, kNpcMaxCount); formation = std::clamp(formation, 0, 2);
        const float fl = sqrtf(fx * fx + fz * fz); if (fl > 1e-4f) { fx /= fl; fz /= fl; } else { fx = 0; fz = 1; }
        const float rx = fz, rz = -fx;
        const int cols = std::max(1, (int)ceilf(sqrtf((float)count))), rows = std::max(1, (count + cols - 1) / cols);
        std::vector<Vec3> positions; positions.reserve(count);
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
            positions.push_back({ center.x + rx * side + fx * depth, center.y, center.z + rz * side + fz * depth });
        }
        return positions;
    }
    static void SpawnNpcPositions(uint32_t key, const std::vector<Vec3>& positions, bool ai, int behavior) {
        if (!PrepareHistoryMutation([=]() { SpawnNpcPositions(key, positions, ai, behavior); })) return;
        std::vector<Act> acts; acts.reserve(positions.size()); std::vector<int> spawned; spawned.reserve(positions.size());
        for (const Vec3& at : positions) {
            const int uid = core::SpawnManagedNpc(key, at, 1, 0, ai, behavior);
            if (uid) { Act a; a.kind = Act::NpcSpawn; a.uid = uid; a.prefab = std::string("NPC ") + std::to_string(key); a.pos1 = at; a.flag1 = ai; a.behavior1 = behavior; acts.push_back(std::move(a)); spawned.push_back(uid); }
        }
        Push(std::move(acts));
        if (!spawned.empty()) {
            ClearSceneSelection();
            for (int uid : spawned) g_managedNpcSel.insert(uid);
            g_managedNpcPrimary = spawned.front(); g_managedNpcLast = spawned.back(); g_sceneLastEntity = -g_managedNpcPrimary;
        }
    }
    static void SpawnNpcFormation(uint32_t key, Vec3 center, int count, int formation, float spacing, float radius, float fx, float fz, bool ai, int behavior) {
        SpawnNpcPositions(key, NpcFormationPositions(center, count, formation, spacing, radius, fx, fz), ai, behavior);
    }
    static void SpawnNpcFormationGrounded(uint32_t key, Vec3 center, int count, int formation, float spacing, float radius, float fx, float fz, bool ai, int behavior) {
        if (!core::GroundProbeReady()) {
            g_npcDropJobs.push_back({ key, 0, GetTickCount(), center, count, formation, spacing, radius, fx, fz, ai, behavior });
            return;
        }
        const float startY = center.y + 5.0f;
        const int ticket = core::GroundProbe({ center.x, startY, center.z }, 30.0f);
        if (!ticket) {
            g_npcDropJobs.push_back({ key, 0, GetTickCount(), center, count, formation, spacing, radius, fx, fz, ai, behavior });
            return;
        }
        g_npcDropJobs.push_back({ key, ticket, GetTickCount(), center, count, formation, spacing, radius, fx, fz, ai, behavior });
        g_npcDropJobs.back().probeTop = startY;
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
    static const thumbgen::CharInfo* ManagedNpcChar(const ManagedNpc& n, const std::vector<thumbgen::CharInfo>* chars) {
        if (chars) for (const auto& c : *chars) if (c.key == n.key) return &c;
        return nullptr;
    }
    static Vec3 ManagedNpcDisplayPos(const ManagedNpc& n) {
        Vec3 p{}; return core::ManagedNpcLivePosition(n, &p) ? p : n.pos;
    }
    static std::string ManagedNpcName(const ManagedNpc& n, const std::vector<thumbgen::CharInfo>* chars) {
        if (!n.label.empty()) return n.label;
        if (const auto* c = ManagedNpcChar(n, chars)) return c->name.empty() ? c->internal : c->name;
        return std::string("NPC ") + std::to_string(n.key);
    }
    static void ManagedNpcRuntimeTooltip(const ManagedNpc& n, const std::string& name, Vec3 pos) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(name.c_str());
        ImGui::TextDisabled("%s #%d   %s %u", T("NPC"), n.uid, T("ID"), n.key);
        ImGui::Text("%s: %.2f  %.2f  %.2f", T("Position"), pos.x, pos.y, pos.z);
        ImGui::Separator();
        ImGui::Text("%s: %s", T("AI"), n.aiEnabled ? T("On") : T("Off"));
        ImGui::TextDisabled("%s: %s", T("Behavior"), n.behavior == 1 ? T("Hold") : T("Normal"));
        if (n.spawnPending || n.aiApplied != n.aiEnabled)
            ImGui::TextDisabled("%s: %s", T("Status"), T(n.spawnPending ? "pending" : "syncing"));
        if (!n.note.empty()) { ImGui::Separator(); ImGui::TextWrapped("%s", n.note.c_str()); }
        ImGui::EndTooltip();
    }
    static std::string ManagedNpcHistoryName(const ManagedNpc& n) { return n.label.empty() ? std::string("NPC ") + std::to_string(n.key) : n.label; }
    static void SelectManagedNpc(int uid, bool add) {
        g_terrainTileSelected = false;
        if (g_place.active) DropCarried();
        const auto list = core::ManagedNpcs(); const ManagedNpc* n = FindManagedNpc(list, uid);
        if (!add) ClearSceneSelection();
        if (g_selectGroups && n && n->group > 0 && !add) {
            for (const auto& x : list) if (!x.hidden && x.group == n->group) g_managedNpcSel.insert(x.uid);
            for (const auto& o : core::Spawned()) if (!o.hidden && o.group == n->group) g_sel.insert(o.uid);
        } else {
            if (add && g_managedNpcSel.count(uid)) g_managedNpcSel.erase(uid); else g_managedNpcSel.insert(uid);
        }
        if (g_managedNpcSel.count(uid)) g_managedNpcPrimary = g_managedNpcLast = uid;
        RefreshSelectionPrimary();
        if (g_managedNpcSel.count(uid)) g_sceneLastEntity = -uid;
        g_editUid = 0;
    }
    static void SelectAllManagedNpcs(const std::vector<ManagedNpc>& list, int projectFilter) {
        g_managedNpcSel.clear(); for (const auto& n : list) if (!n.hidden && (projectFilter < 0 || n.proj == projectFilter)) g_managedNpcSel.insert(n.uid);
        g_managedNpcPrimary = g_managedNpcSel.empty() ? 0 : *g_managedNpcSel.begin();
    }
    static void SetSelectedNpcAi(bool enabled) {
        if (!PrepareHistoryMutation([enabled]() { SetSelectedNpcAi(enabled); })) return;
        const auto list = core::ManagedNpcs(); std::vector<Act> acts;
        for (int uid : g_managedNpcSel) if (const auto* n = FindManagedNpc(list, uid)) {
            const int afterBehavior = enabled && n->behavior == 1 ? 0 : n->behavior;
            Act a; a.kind = Act::NpcControl; a.uid = uid; a.prefab = ManagedNpcHistoryName(*n); a.flag0 = n->aiEnabled; a.flag1 = enabled; a.behavior0 = n->behavior; a.behavior1 = afterBehavior;
            if (core::SetManagedNpcControl(uid, enabled, afterBehavior)) acts.push_back(std::move(a));
        }
        Push(std::move(acts));
    }
    static void SetSelectedNpcBehavior(int behavior) {
        if (!PrepareHistoryMutation([behavior]() { SetSelectedNpcBehavior(behavior); })) return;
        behavior = behavior == 1 ? 1 : 0; const bool enabled = behavior == 0;
        const auto list = core::ManagedNpcs(); std::vector<Act> acts;
        for (int uid : g_managedNpcSel) if (const auto* n = FindManagedNpc(list, uid)) {
            Act a; a.kind = Act::NpcControl; a.uid = uid; a.prefab = ManagedNpcHistoryName(*n); a.flag0 = n->aiEnabled; a.flag1 = enabled; a.behavior0 = n->behavior; a.behavior1 = behavior;
            if (core::SetManagedNpcControl(uid, enabled, behavior)) acts.push_back(std::move(a));
        }
        Push(std::move(acts));
    }
    static void MoveSelectedNpcs(Vec3 delta) {
        if (!PrepareHistoryMutation([delta]() { MoveSelectedNpcs(delta); })) return;
        const auto list = core::ManagedNpcs(); std::vector<Act> acts;
        for (int uid : g_managedNpcSel) if (const auto* n = FindManagedNpc(list, uid)) {
            const Vec3 before = ManagedNpcDisplayPos(*n);
            const Vec3 after{ before.x + delta.x, before.y + delta.y, before.z + delta.z };
            Act a; a.kind = Act::NpcMove; a.uid = uid; a.prefab = ManagedNpcHistoryName(*n); a.pos0 = before; a.pos1 = after;
            if (core::MoveManagedNpc(uid, after)) acts.push_back(std::move(a));
        }
        Push(std::move(acts));
    }
    static void DeleteSelectedNpcs() {
        if (!PrepareHistoryMutation([]() { DeleteSelectedNpcs(); })) return;
        if (g_place.active && std::any_of(g_place.m.begin(), g_place.m.end(), [](const Member& m) { return m.npc && g_managedNpcSel.count(m.uid); })) {
            DropCarried(); if (g_place.active) return;
        }
        const auto list = core::ManagedNpcs(); std::vector<Act> acts; std::set<int> failed;
        for (int uid : g_managedNpcSel) if (const auto* n = FindManagedNpc(list, uid)) if (!n->hidden) {
            Act a; a.kind = Act::NpcDelete; a.uid = uid; a.prefab = ManagedNpcHistoryName(*n); a.pos0 = n->pos; a.flag0 = n->aiEnabled; a.behavior0 = n->behavior; a.group = n->group; a.text0 = n->label;
            if (core::HideManagedNpc(uid)) acts.push_back(a); else failed.insert(uid);
        }
        Push(std::move(acts)); g_managedNpcSel = std::move(failed); g_managedNpcPrimary = g_managedNpcSel.empty() ? 0 : *g_managedNpcSel.begin();
        if (!SceneHasSelection()) ClearDeletedSelectionUi(false);
    }
    static void GroupSelectedNpcs(bool makeGroup) {
        if (!PrepareHistoryMutation([makeGroup]() { GroupSelectedNpcs(makeGroup); })) return;
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
        if (!PrepareHistoryMutation([]() { DeleteSceneSelection(); })) return;
        if (g_place.active && std::any_of(g_place.m.begin(), g_place.m.end(), [](const Member& m) { return m.npc ? g_managedNpcSel.count(m.uid) != 0 : g_sel.count(m.uid) != 0; })) {
            DropCarried(); if (g_place.active) return;
        }
        auto objects = core::Spawned(); auto npcs = core::ManagedNpcs(); std::vector<Act> acts; std::set<int> failedObjects, failedNpcs;
        for (int uid : g_sel) if (const auto* o = Find(objects, uid)) if (!o->hidden) {
            Act a; a.kind = Act::Delete; a.uid = uid; a.prefab = o->prefab; a.pos0 = o->pos; a.rot0 = o->rot; a.sc0 = o->scale; a.group = o->group; a.proj = o->proj; a.text0 = o->note;
            if (core::HideUid(uid)) acts.push_back(a); else failedObjects.insert(uid);
        }
        for (int uid : g_managedNpcSel) if (const auto* n = FindManagedNpc(npcs, uid)) if (!n->hidden) {
            Act a; a.kind = Act::NpcDelete; a.uid = uid; a.prefab = ManagedNpcHistoryName(*n); a.pos0 = n->pos; a.flag0 = n->aiEnabled; a.behavior0 = n->behavior; a.group = n->group; a.proj = n->proj; a.text0 = n->label;
            if (core::HideManagedNpc(uid)) acts.push_back(a); else failedNpcs.insert(uid);
        }
        const bool deletedObject = std::any_of(acts.begin(), acts.end(), [](const Act& a) { return a.kind == Act::Delete; });
        if (!acts.empty()) Push(std::move(acts));
        ClearSceneSelection();
        g_sel = std::move(failedObjects); g_managedNpcSel = std::move(failedNpcs);
        if (!g_sel.empty()) { g_primary = *g_sel.begin(); g_sceneLastEntity = g_primary; }
        else if (!g_managedNpcSel.empty()) { g_managedNpcPrimary = *g_managedNpcSel.begin(); g_sceneLastEntity = -g_managedNpcPrimary; }
        else ClearDeletedSelectionUi(deletedObject);
    }
    static void GroupSceneSelection(bool makeGroup) {
        if (!PrepareHistoryMutation([makeGroup]() { GroupSceneSelection(makeGroup); })) return;
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
        if (!PrepareHistoryMutation([delta]() { MoveSceneSelection(delta); })) return;
        if (!SceneHasSelection()) return;
        auto objects = core::Spawned(); auto npcs = core::ManagedNpcs(); std::vector<Act> acts; std::vector<core::MoveReq> moves;
        for (int uid : g_sel) if (const auto* o = Find(objects, uid)) if (!o->hidden) {
            Vec3 p{ o->pos.x + delta.x, o->pos.y + delta.y, o->pos.z + delta.z };
            Act a; a.kind = Act::Move; a.uid = uid; a.prefab = o->prefab; a.pos0 = o->pos; a.rot0 = o->rot; a.sc0 = o->scale; a.pos1 = p; a.rot1 = o->rot; a.sc1 = o->scale;
            acts.push_back(a); moves.push_back({ uid, p, o->rot, o->scale });
        }
        for (int uid : g_managedNpcSel) if (const auto* n = FindManagedNpc(npcs, uid)) if (!n->hidden) {
            const Vec3 before = ManagedNpcDisplayPos(*n);
            Vec3 p{ before.x + delta.x, before.y + delta.y, before.z + delta.z };
            Act a; a.kind = Act::NpcMove; a.uid = uid; a.prefab = ManagedNpcHistoryName(*n); a.pos0 = before; a.pos1 = p;
            acts.push_back(a); core::MoveManagedNpc(uid, p);
        }
        if (!moves.empty()) core::MoveMany(moves, true);
        Push(std::move(acts));
    }
    static void OpenNpcNoteEdit(const ManagedNpc& n) { g_npcNoteEditUid = n.uid; strncpy_s(g_npcNoteEdit, n.note.c_str(), _TRUNCATE); g_metadataPopupRequest = 1; }
    static void OpenNpcLabelEdit(const ManagedNpc& n) { g_npcLabelEditUid = n.uid; strncpy_s(g_npcLabelEdit, n.label.c_str(), _TRUNCATE); g_metadataPopupRequest = 2; }
    static void OpenGroupNameEdit(int gid) { g_groupNameEditId = gid; const std::string name = core::GroupName(gid); strncpy_s(g_groupNameEdit, name.c_str(), _TRUNCATE); g_metadataPopupRequest = 3; }
    static void OpenObjectNoteEdit(const SpawnedObj& o) { g_objectNoteEditUid = o.uid; strncpy_s(g_objectNoteEdit, o.note.c_str(), _TRUNCATE); g_metadataPopupRequest = 4; }
    static void SaveMetadata(Act::Kind kind, int id, const std::string& text) {
        std::vector<int> targets;
        if (kind == Act::GroupName) {
            for (const auto& o : core::Spawned()) if (o.group == id) targets.push_back(o.uid);
            for (const auto& n : core::ManagedNpcs()) if (n.group == id) targets.push_back(-n.uid);
        } else targets.push_back(NpcAct(kind) ? -id : id);
        if (!PrepareHistoryMutation([kind, id, text]() { SaveMetadata(kind, id, text); }, targets)) return;
        Act a; a.kind = kind; a.uid = kind == Act::GroupName ? 0 : id; a.text1 = text;
        if (kind == Act::GroupName) { a.group = id; a.text0 = core::GroupName(id); CaptureProvenance(a); core::SetGroupName(id, text); }
        else if (kind == Act::ObjectNote) {
            const auto all = core::Spawned(); const auto* o = Find(all, id); if (!o) return;
            a.text0 = o->note; a.proj = o->proj; core::SetObjectNote(id, text);
        } else {
            const auto all = core::ManagedNpcs(); const auto* n = FindManagedNpc(all, id); if (!n) return;
            a.proj = n->proj; a.text0 = kind == Act::NpcNote ? n->note : n->label;
            if (kind == Act::NpcNote) core::SetManagedNpcNote(id, text); else core::SetManagedNpcLabel(id, text);
        }
        Push({ a });
    }
    static void DrawMetadataPopups() {
        const auto title = [](int kind) -> const char* {
            switch (kind) {
            case 1: return TStable("Edit NPC note");
            case 2: return TStable("Rename NPC");
            case 3: return TStable("Rename group");
            default: return TStable("Edit object note");
            }
        };
        g_metadataEditing = g_metadataPopupRequest || ImGui::IsPopupOpen(title(1)) || ImGui::IsPopupOpen(title(2)) ||
            ImGui::IsPopupOpen(title(3)) || ImGui::IsPopupOpen(title(4));
        if (g_metadataPopupRequest) {
            ImGui::OpenPopup(title(g_metadataPopupRequest)); g_metadataPopupRequest = 0;
        }
        if (ImGui::BeginPopupModal(title(1), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            const float width = std::min(460.0f, std::max(120.0f, ImGui::GetIO().DisplaySize.x - 80.0f));
            if (ImGui::InputTextMultiline("##npcnote", g_npcNoteEdit, sizeof g_npcNoteEdit, ImVec2(width, 110))) i18n::AddGlyphText(g_npcNoteEdit);
            if (ImGui::Button(T("Save"))) {
                SaveMetadata(Act::NpcNote, g_npcNoteEditUid, g_npcNoteEdit); ImGui::CloseCurrentPopup();
            }
            SameLineForControl("Cancel"); if (ImGui::Button(T("Cancel"))) ImGui::CloseCurrentPopup(); ImGui::EndPopup();
        }
        if (ImGui::BeginPopupModal(title(2), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::SetNextItemWidth(std::min(360.0f, std::max(120.0f, ImGui::GetIO().DisplaySize.x - 80.0f))); InputTextI18n("##npclabel", "", g_npcLabelEdit, sizeof g_npcLabelEdit);
            if (ImGui::Button(T("Save"))) {
                SaveMetadata(Act::NpcLabel, g_npcLabelEditUid, g_npcLabelEdit); ImGui::CloseCurrentPopup();
            }
            SameLineForControl("Cancel"); if (ImGui::Button(T("Cancel"))) ImGui::CloseCurrentPopup(); ImGui::EndPopup();
        }
        if (ImGui::BeginPopupModal(title(3), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::SetNextItemWidth(std::min(360.0f, std::max(120.0f, ImGui::GetIO().DisplaySize.x - 80.0f))); InputTextI18n("##groupname", "", g_groupNameEdit, sizeof g_groupNameEdit);
            if (ImGui::Button(T("Save"))) {
                SaveMetadata(Act::GroupName, g_groupNameEditId, g_groupNameEdit); ImGui::CloseCurrentPopup();
            }
            SameLineForControl("Cancel"); if (ImGui::Button(T("Cancel"))) ImGui::CloseCurrentPopup(); ImGui::EndPopup();
        }
        if (ImGui::BeginPopupModal(title(4), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            const float width = std::min(460.0f, std::max(120.0f, ImGui::GetIO().DisplaySize.x - 80.0f));
            if (ImGui::InputTextMultiline("##objectnote", g_objectNoteEdit, sizeof g_objectNoteEdit, ImVec2(width, 110))) i18n::AddGlyphText(g_objectNoteEdit);
            if (ImGui::Button(T("Save"))) {
                SaveMetadata(Act::ObjectNote, g_objectNoteEditUid, g_objectNoteEdit); ImGui::CloseCurrentPopup();
            }
            SameLineForControl("Cancel"); if (ImGui::Button(T("Cancel"))) ImGui::CloseCurrentPopup(); ImGui::EndPopup();
        }
    }
    // tiles like the prefab browser's cards: the appearance preview, the in-game name below, a double-click spawns
    static void DrawNpcCards(const std::vector<thumbgen::CharInfo>& chars, float listH, float ui, bool canSpawn, bool canDrag) {
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
                if (canDrag && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.0f)) g_npcDragIndex = i;
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
        SameLineOrWrap(compact, ImGui::CalcTextSize(T("Clear selection")).x + ImGui::GetStyle().FramePadding.x * 2);
        if (ImGui::Button(T("Clear selection"))) { g_managedNpcSel.clear(); g_managedNpcPrimary = 0; }
        SameLineOrWrap(compact, ImGui::CalcTextSize(T("AI on")).x + ImGui::GetStyle().FramePadding.x * 2);
        ImGui::BeginDisabled(g_managedNpcSel.empty() || !core::NpcAiControlAvailable());
        if (ImGui::Button(T("AI on"))) SetSelectedNpcAi(true);
        SameLineOrWrap(compact, ImGui::CalcTextSize(T("AI off")).x + ImGui::GetStyle().FramePadding.x * 2);
        if (ImGui::Button(T("AI off"))) SetSelectedNpcAi(false);
        SameLineOrWrap(compact, 170 * ui);
        ImGui::SetNextItemWidth(170 * ui);
        const char* behaviorPreview = anyNormal && anyHold ? T("Mixed") : anyHold ? T("Hold") : T("Normal");
        if (ImGui::BeginCombo("##selectednpcbehavior", behaviorPreview)) {
            if (ImGui::Selectable(T("Normal autonomous"), anyNormal && !anyHold)) SetSelectedNpcBehavior(0);
            if (ImGui::Selectable(T("Hold position (AI paused)"), anyHold && !anyNormal)) SetSelectedNpcBehavior(1);
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        SameLineOrWrap(compact, ImGui::CalcTextSize(T("Delete selected")).x + ImGui::GetStyle().FramePadding.x * 2);
        ImGui::BeginDisabled(g_managedNpcSel.empty());
        if (ImGui::Button(T("Delete selected"))) DeleteSelectedNpcs();
        ImGui::EndDisabled();
        if (!core::NpcAiControlAvailable()) { SameLineOrWrap(compact, ImGui::CalcTextSize(T("native AI control unavailable")).x); ImGui::TextDisabled("%s", T("native AI control unavailable")); }

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
                const Vec3 livePos = ManagedNpcDisplayPos(*n);
                static int posUid = 0; static float ep[3] = {}; static Vec3 base{}; static bool liveMove = false; static DWORD liveAt = 0;
                if (posUid != n->uid || (!ImGui::IsAnyItemActive() && !liveMove && (ep[0] != livePos.x || ep[1] != livePos.y || ep[2] != livePos.z))) {
                    posUid = n->uid; ep[0] = base.x = livePos.x; ep[1] = base.y = livePos.y; ep[2] = base.z = livePos.z; liveMove = false;
                }
                ImGui::TextDisabled("%s", T("Selected NPC properties"));
                ImGui::SameLine(); ImGui::Text("%s", ManagedNpcName(*n, chars).c_str());
                ImGui::SetNextItemWidth(compact ? -1.0f : 330 * ui);
                const bool changed = ImGui::DragFloat3(T("position##managednpc"), ep, 0.05f, -100000.0f, 100000.0f, "%.3f");
                if (changed && g_managedNpcSel.size() == 1) {
                    const DWORD now = TickNow();
                    if (now - liveAt >= 45) { NpcNumericApply(n->uid, { ep[0], ep[1], ep[2] }, false); liveMove = g_numericNpcUid == n->uid; liveAt = now; }
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    const Vec3 finalPos{ ep[0], ep[1], ep[2] };
                    if (liveMove || g_managedNpcSel.size() == 1) {
                        NpcNumericApply(n->uid, finalPos, true);
                    } else {
                        const Vec3 delta{ finalPos.x - base.x, finalPos.y - base.y, finalPos.z - base.z }; MoveSelectedNpcs(delta);
                    }
                    base = finalPos; liveMove = false;
                }
                ImGui::SameLine(); if (ImGui::Button(T("Edit note"))) OpenNpcNoteEdit(*n);
                if (!n->note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", n->note.c_str()); }
            }
        }
        ImGui::Separator();
    }
    static void DrawNpcs(const PosInfo& p, bool havePos, bool compact = false) {
        const auto chars = thumbgen::Characters();
        const float ui = ImGui::GetFontSize() / 17.0f;
        const int st = core::NpcState();
        ImGui::PushTextWrapPos();
        if (st == 0) { ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), T("NPC spawning is not available in this game build (see the log).")); }
        else if (st == 1) { ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), T("walk a few steps first: the game's spawn request needs your character's server actor")); }
        ImGui::PopTextWrapPos();
        if (!chars) { ImGui::TextDisabled(T("reading the character list from the game files...")); return; }
        if (!compact) {   // view switch: list or tiles (tile size shared with the browser)
            const ImVec4 on = ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive), off = ImGui::GetStyleColorVec4(ImGuiCol_Button);
            ImGui::PushStyleColor(ImGuiCol_Button, g_npcCards ? off : on); if (ImGui::Button(T(ICON_LIST " list"))) g_npcCards = false; ImGui::PopStyleColor();
            SameLineOrWrap(compact, ImGui::CalcTextSize(T(ICON_COPY " cards")).x + ImGui::GetStyle().FramePadding.x * 2, 2);
            ImGui::PushStyleColor(ImGuiCol_Button, g_npcCards ? on : off); if (ImGui::Button(T(ICON_COPY " cards"))) g_npcCards = true; ImGui::PopStyleColor();
            if (g_npcCards) { SameLineOrWrap(compact, 100 * ui); ImGui::SetNextItemWidth(100 * ui); ImGui::SliderFloat("##npccardsize", &g_cardSize, 64.0f, 200.0f, "%.0f px"); if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("tile size")); }
            SameLineOrWrap(compact, 220 * ui);
        }
        ImGui::SetNextItemWidth(compact ? -1.0f : std::max(120.0f * ui, ImGui::GetContentRegionAvail().x - 260.0f * ui));
        InputTextI18n("##npcfilter", T("search  (name, internal name or key)"), g_npcFilter, sizeof g_npcFilter);
        if (!compact) SameLineOrWrap(compact, 180 * ui);
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
        SameLineOrWrap(compact, ImGui::CalcTextSize(T("double-click = one NPC; SPAWN or drag = current count and formation")).x);
        ImGui::PushTextWrapPos(); ImGui::TextDisabled("%s", T("double-click = one NPC; SPAWN or drag = current count and formation")); ImGui::PopTextWrapPos();
        const bool haveNpcSelection = g_npcSel >= 0 && g_npcSel < (int)chars->size();
        const float detailsH = haveNpcSelection ? (compact ? 265.0f : 205.0f) * ui : 0.0f;
        float listH = ImGui::GetContentRegionAvail().y - detailsH - (detailsH > 0 ? ImGui::GetStyle().ItemSpacing.y : 0.0f); if (listH < 80 * ui) listH = 80 * ui;
        const bool useCards = compact || g_npcCards;
        if (g_npcRows.empty()) ImGui::TextDisabled("%s", T("No characters match the current search and category."));
        else if (useCards) DrawNpcCards(*chars, listH, ui, havePos && st == 2, st == 2);
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
                if (st == 2 && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.0f)) g_npcDragIndex = i;
                ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("%s", c.internal.c_str());
                ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("%u", c.key);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (haveNpcSelection && ImGui::BeginChild("npcdetails", ImVec2(0, detailsH), ImGuiChildFlags_Borders)) {
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
                SameLineOrWrap(compact, 175 * ui + ImGui::CalcTextSize(T("distance")).x + ImGui::GetStyle().ItemInnerSpacing.x);
            }
            ImGui::BeginGroup();
            ImGui::TextWrapped("%s", c.name.empty() ? c.internal.c_str() : c.name.c_str());
            ImGui::PushTextWrapPos(); ImGui::TextDisabled("%s   ID %u", c.internal.c_str(), c.key); ImGui::PopTextWrapPos();
            SetLabeledItemWidth("distance", compact ? 120 * ui : 150 * ui); ImGui::DragFloat(T("distance"), &g_npcDist, 1.0f, 1.0f, kNpcMaxDist, "%.0f m"); g_npcDist = std::clamp(g_npcDist, 1.0f, kNpcMaxDist); SameLineOrWrap(compact, 110 * ui + ImGui::CalcTextSize(T("count")).x + ImGui::GetStyle().ItemInnerSpacing.x);
            SetLabeledItemWidth("count", 110 * ui); ImGui::InputInt(T("count"), &g_npcCount); g_npcCount = std::clamp(g_npcCount, 1, kNpcMaxCount);
            SameLineOrWrap(compact, 250 * ui);
            ImGui::TextDisabled("%s", T("presets")); ImGui::SameLine(0, 3);
            static const int countPresets[] = { 1, 5, 10, 25, 50, 100, 250, 500 };
            for (int pi = 0; pi < (int)(sizeof(countPresets) / sizeof(countPresets[0])); ++pi) {
                char pl[16]; snprintf(pl, sizeof pl, "%d##np%d", countPresets[pi], countPresets[pi]);
                if (pi) SameLineOrWrap(compact, ImGui::CalcTextSize(pl).x + ImGui::GetStyle().FramePadding.x * 2, 2);
                if (ImGui::Button(pl)) g_npcCount = countPresets[pi];
            }
            SameLineOrWrap(compact, 130 * ui);
            ImGui::TextDisabled("%s", T("formation")); SameLineOrWrap(compact, 120 * ui); ImGui::SetNextItemWidth(120 * ui); ComboT("##npcformation", &g_npcFormation, kNpcFormations, 3);
            SameLineOrWrap(compact, 130 * ui);
            SetLabeledItemWidth(g_npcFormation == 2 ? "radius" : "spacing", 120 * ui);
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
            SameLineForControl("AI enabled on spawn", true);
            ImGui::BeginDisabled(!core::NpcAiControlAvailable());
            bool spawnAi = g_npcSpawnAi;
            if (FittedCheckbox(T("AI enabled on spawn"), &spawnAi)) { g_npcSpawnAi = spawnAi; g_npcSpawnBehavior = spawnAi ? 0 : 1; }
            SameLineOrWrap(compact, compact ? 170 * ui : 210 * ui); ImGui::SetNextItemWidth(std::min(compact ? 170 * ui : 210 * ui, ImGui::GetContentRegionAvail().x));
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
            if (FittedButton(spawnLabel)) {
                const Vec3 center{ g_lastPlayer.x + g_fx * g_npcDist, g_lastPlayer.y, g_lastPlayer.z + g_fz * g_npcDist };
                SpawnNpcFormationGrounded(c.key, center, g_npcCount, g_npcFormation, g_npcSpacing, g_npcRadius, g_fx, g_fz, g_npcSpawnAi, g_npcSpawnBehavior);
                Note(T("spawn %s"), c.name.empty() ? c.internal.c_str() : c.name.c_str());
            }
            ImGui::PopStyleColor(2); ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(T("spawns the character <distance> in front of you; it appears after a few seconds. Hostile ones attack, wild animals may flee."));
            if (!c.app.empty()) ImGui::TextDisabled("%s", c.app.c_str());
            ImGui::EndGroup();
            ImGui::EndChild();
        }
    }

    static void DrawEnvironment(bool compact = false) {
        const float ui = ImGui::GetFontSize() / 17.0f;
        ImGui::SeparatorText(T("Time"));
        ImGui::PushTextWrapPos();
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
            SetLabeledItemWidth("time of day", compact ? ImGui::GetContentRegionAvail().x : 420.0f * ui);
            if (SliderFloatEdit(T("time of day"), &target, 0.0f, 23.9833f, "%.2f h"))
                core::SetTimeHour(target);

            if (ImGui::Button(T("Dawn 06:00"))) core::SetTimeHour(6.0f);
            SameLineForControl("Noon 12:00");
            if (ImGui::Button(T("Noon 12:00"))) core::SetTimeHour(12.0f);
            SameLineForControl("Sunset 18:00");
            if (ImGui::Button(T("Sunset 18:00"))) core::SetTimeHour(18.0f);
            SameLineForControl("Midnight 00:00");
            if (ImGui::Button(T("Midnight 00:00"))) core::SetTimeHour(0.0f);

            bool frozen = core::TimeFrozen();
            if (FittedCheckbox(T("freeze time (lighting only)"), &frozen)) core::SetTimeFrozen(frozen);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Stops the visual day and night lighting progression. Gameplay, NPCs, physics and combat keep running."));
            SameLineForControl("use native time");
            if (ImGui::Button(T("use native time"))) core::ResetTimeControl();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Restore the game's normal visual time progression."));
        }
        ImGui::PopTextWrapPos();

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
        if (rainOn) { SetLabeledItemWidth("rain intensity", compact ? ImGui::GetContentRegionAvail().x : 420.0f * ui); if (SliderFloatEdit(T("rain intensity"), &rain, 0.0f, 1.0f, "%.2f")) core::SetWeatherRainOverride(true, rain); }
        ImGui::EndDisabled();
        const bool snowEffects = core::WeatherSnowEffectsAvailable();
        ImGui::BeginDisabled(!snowEffects);
        if (ImGui::Checkbox(T("override snow"), &snowOn)) core::SetWeatherSnowOverride(snowOn, snow);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !snowEffects)
            ImGui::SetTooltip("%s", T("Snow particle control is not available in this game build."));
        if (snowOn) { SetLabeledItemWidth("snow intensity", compact ? ImGui::GetContentRegionAvail().x : 420.0f * ui); if (SliderFloatEdit(T("snow intensity"), &snow, 0.0f, 1.0f, "%.2f")) core::SetWeatherSnowOverride(true, snow); }
        ImGui::EndDisabled();
        const bool cloudAvail = core::WeatherCloudAvailable();
        ImGui::BeginDisabled(!cloudAvail);
        if (ImGui::Checkbox(T("override clouds"), &cloudOn)) core::SetWeatherCloudOverride(cloudOn, cloud);
        unavailableTip(cloudAvail);
        if (cloudOn) { SetLabeledItemWidth("cloud amount", compact ? ImGui::GetContentRegionAvail().x : 420.0f * ui); if (SliderFloatEdit(T("cloud amount"), &cloud, 0.0f, 3.0f, "%.2f")) core::SetWeatherCloudOverride(true, cloud); }
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        const bool windAvail = core::WeatherWindAvailable();
        ImGui::BeginDisabled(!windAvail);
        if (ImGui::Checkbox(T("override wind"), &windOn)) core::SetWeatherWindOverride(windOn, wind);
        unavailableTip(windAvail);
        if (windOn) { SetLabeledItemWidth("wind multiplier", compact ? ImGui::GetContentRegionAvail().x : 420.0f * ui); if (SliderFloatEdit(T("wind multiplier"), &wind, 0.0f, 3.0f, "x%.2f")) core::SetWeatherWindOverride(true, wind); }
        ImGui::EndDisabled();

        if (ImGui::Button(T("use native weather"))) core::ResetWeatherControl();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Disable every World Builder weather override and return control to the game."));
        SameLineOrWrap(compact, ImGui::CalcTextSize(T("Weather values are applied to the game's composed environment each update.")).x);
        ImGui::PushTextWrapPos(); ImGui::TextDisabled(T("Weather values are applied to the game's composed environment each update.")); ImGui::PopTextWrapPos();
    }

    static void DrawBrowser(const PosInfo& p, bool havePos) {
        const auto& idx = core::PrefabIndex();
        const float browserWidth = ImGui::GetContentRegionAvail().x;
        const auto drawCategories = [&]() {
            ImGui::TextDisabled(T(ICON_FOLDER_TREE "  CATEGORIES"));
            DrawCatNode(0);
            ImGui::Separator();
            LoadColls();
            ImGui::TextDisabled(T(ICON_STAR "  COLLECTIONS"));
            for (int i = 0; i < (int)g_colls.size(); i++) {
                ImGui::PushID(i);
                char lbl[120]; snprintf(lbl, sizeof lbl, "%s  (%d)", g_colls[i].name.c_str(), (int)g_colls[i].paths.size());
                if (ImGui::Selectable(lbl, g_selColl == i)) g_selColl = g_selColl == i ? -1 : i;
                if (ImGui::BeginPopupContextItem("collctx")) { if (ImGui::MenuItem(T("delete collection"))) { g_colls.erase(g_colls.begin() + i); SaveColls(); if (g_selColl == i) g_selColl = -1; ImGui::EndPopup(); ImGui::PopID(); break; } ImGui::EndPopup(); }
                ImGui::PopID();
            }
            ImGui::SetNextItemWidth(std::max(60.0f, ImGui::GetContentRegionAvail().x - 60));
            const bool enter = InputTextI18n("##newcoll", T("new collection"), g_newColl, sizeof g_newColl, ImGuiInputTextFlags_EnterReturnsTrue);
            SameLineForControl("add");
            if ((ImGui::Button(T("add")) || enter) && g_newColl[0]) { g_colls.push_back({ g_newColl, {} }); SaveColls(); g_newColl[0] = 0; }
        };
        if (browserWidth >= 400.0f) {
            const float categoryWidth = std::min(g_catW, browserWidth - 230.0f);
            ImGui::BeginChild("cats", ImVec2(categoryWidth, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
            g_catW = ImGui::GetWindowSize().x;
            drawCategories();
            ImGui::EndChild();
            ImGui::SameLine();
        } else {
            if (FittedButton(T(ICON_FOLDER_TREE "  CATEGORIES"))) ImGui::OpenPopup("browser-categories");
            if (ImGui::BeginPopup("browser-categories")) {
                ImGui::BeginChild("cats-popup", ImVec2(std::max(170.0f, browserWidth - 30.0f), 340.0f), ImGuiChildFlags_Borders);
                drawCategories();
                ImGui::EndChild();
                ImGui::EndPopup();
            }
        }
        ImGui::BeginChild("right", ImVec2(0, 0));
        const float ui = ImGui::GetFontSize() / 17.0f;
        {   // view switch: list or tiles
            const ImVec4 on = ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive), off = ImGui::GetStyleColorVec4(ImGuiCol_Button);
            ImGui::PushStyleColor(ImGuiCol_Button, g_cardView ? off : on); if (ImGui::Button(T(ICON_LIST " list"))) g_cardView = false; ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("list view: name, folder, tags"));
            ImGui::SameLine(0, 2); ImGui::PushStyleColor(ImGuiCol_Button, g_cardView ? on : off); if (ImGui::Button(T(ICON_COPY " cards"))) g_cardView = true; ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("tile view with the preview images (same search and filters)"));
            if (g_cardView) { ImGui::SameLine(); ImGui::SetNextItemWidth(100 * ui); SliderFloatEdit("##cardsize", &g_cardSize, 64.0f, 200.0f, "%.0f px"); if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("tile size")); }
        }
        SameLineOrWrap(true, 320.0f * ui);
        const float searchWidth = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(searchWidth < 520.0f * ui ? -1.0f : std::max(120.0f * ui, searchWidth - 400.0f * ui));
        InputTextI18n("##filter", T("search  (words in any order, e.g. lamp torch)"), g_filter, sizeof g_filter);
        SameLineForControl(ICON_STAR " favorites"); ImGui::Checkbox(T(ICON_STAR " favorites"), &g_favOnly);
        SameLineForControl("meshes only"); ImGui::Checkbox(T("meshes only"), &g_meshOnly); if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("hides prefabs without static meshes (skinned characters, logic-only prefabs): they spawn nothing visible"));
        SameLineForControl(ICON_XMARK " clear"); if (ImGui::Button(T(ICON_XMARK " clear"))) { g_filter[0] = 0; g_tagFilter.clear(); g_favOnly = false; g_selCat = 0; }
        const auto& tags = core::TagCounts();
        int shown = 0;
        for (auto& t : tags) {
            if (shown++ >= 14) break;
            bool on = g_tagFilter.count(t.first) > 0;
            const char* translated = T(t.first == "Nude" ? "Character" : t.first.c_str());
            if (shown > 1) SameLineOrWrap(true, ImGui::CalcTextSize(translated).x + ImGui::GetStyle().FramePadding.x * 2);
            ImGui::PushID(t.first.c_str());
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
            if (ImGui::Button(translated)) { if (on) g_tagFilter.erase(t.first); else g_tagFilter.insert(t.first); }
            if (on) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("%d prefabs with %s"), t.second, translated);
            ImGui::PopID();
        }
        RefreshMatches();
        ImGui::TextDisabled(T("%d results"), (int)g_matches.size());
        if (idx.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 120, 100, 255));
            ImGui::TextWrapped(T("Prefab list not found: the browser and the search stay empty."));
            ImGui::PopStyleColor();
        } else if (g_matches.empty()) {
            ImGui::TextDisabled(T("nothing matches: use fewer words, another category, or turn off favorites, meshes only, and tag filters. Clear resets all."));
        }
        const bool selectedDetails = core::g_showSelectionDetails && g_selPrefab >= 0 && g_selPrefab < (int)idx.size();
        const float detailsH = selectedDetails ? ((g_cardView ? 196.0f : 206.0f) + (g_showSpawnOpts ? 40.0f : 0.0f) + (g_showMass ? 82.0f : 0.0f)) * ui : 0.0f;
        const float browserAvailY = ImGui::GetContentRegionAvail().y;
        const float listH = std::max(64.0f * ui, browserAvailY - detailsH - (selectedDetails ? ImGui::GetStyle().ItemSpacing.y : 0.0f));
        if (!g_matches.empty() && g_cardView) DrawCards(p, havePos, listH, ui);
        else if (!g_matches.empty() && ImGui::BeginTable("list", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable, ImVec2(0, listH))) {
            ImGui::TableSetupColumn("*", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
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
                if (ImGui::Button(ICON_STAR)) core::ToggleFavorite(i);
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
                ImGui::TableSetColumnIndex(4); ImGui::TextDisabled("%s", LocalizedTags(pi.tags).c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (selectedDetails) ImGui::BeginChild("details", ImVec2(0, detailsH), ImGuiChildFlags_Borders);   // optional selected-item information panel
        if (selectedDetails) {
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
            ImGui::TextWrapped("%s", ShownName(pi).c_str());
            const char* favoriteLabel = T(core::IsFavorite(g_selPrefab) ? ICON_STAR " unfavorite" : ICON_STAR " favorite");
            SameLineOrWrap(true, ImGui::CalcTextSize(favoriteLabel).x + ImGui::GetStyle().FramePadding.x * 2);
            if (FittedButton(favoriteLabel)) core::ToggleFavorite(g_selPrefab);
            SameLineOrWrap(true, 160.0f);
            ImGui::SetNextItemWidth(std::min(160.0f, std::max(ImGui::GetFrameHeight(), ImGui::GetContentRegionAvail().x)));
            if (ImGui::BeginCombo("##addcoll", T("add to collection"), ImGuiComboFlags_NoArrowButton)) {
                for (int ci = 0; ci < (int)g_colls.size(); ci++) { bool in = InColl(g_colls[ci], pi.path); if (ImGui::Selectable((std::string(T(in ? "remove from " : "add to ")) + g_colls[ci].name).c_str())) { auto& v = g_colls[ci].paths; if (in) v.erase(std::remove(v.begin(), v.end(), pi.path), v.end()); else v.push_back(pi.path); SaveColls(); g_lastKey.clear(); } }
                if (g_colls.empty()) ImGui::TextDisabled(T("create a collection in the left pane first"));
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("%s", pi.path.c_str());
            const std::string shownTags = LocalizedTags(pi.tags);
            if (pi.sx > 0 || pi.sy > 0 || pi.sz > 0) ImGui::TextDisabled(T(ICON_RULER_COMBINED "  %.1f x %.1f x %.1f m    meshes %d    %s"), pi.sx, pi.sy, pi.sz, pi.meshes, shownTags.c_str());
            else ImGui::TextDisabled(T("meshes %d    %s"), pi.meshes, shownTags.c_str());
            ImGui::BeginDisabled(!havePos || !core::GameThreadReady());
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.36f, 0.22f, 1.0f)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.45f, 0.28f, 1.0f));
            if (FittedButton(T(ICON_LOCATION_CROSSHAIRS "   PLACE   "))) StartPlaceNew(p, havePos);
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("puts the object in front of you and lets you move it with the mouse gizmo"));
            SameLineForControl("spawn only"); if (ImGui::Button(T("spawn only"))) SpawnSelected(p);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("drops the object in front of you without the placement mode (offset, yaw and scale from the spawn options)"));
            ImGui::EndDisabled();
            ImGui::EndGroup();
        }
        if (selectedDetails && thumbgen::Ready()) { ImGui::SameLine(); ImGui::TextDisabled(T("   previews %d of %d"), thumbgen::Done(), thumbgen::Total());
            int pd = 0, pt = 0; if (thumbgen::PassProgress(&pd, &pt) && pt > 0) { ImGui::SameLine(); ImGui::TextDisabled(T("   updating %d of %d"), pd, pt); } }
        else if (selectedDetails && thumbgen::Error()[0]) { ImGui::SameLine(); ImGui::TextDisabled(T("   previews off: %s"), thumbgen::Error()); }
        g_showSpawnOpts = selectedDetails && ImGui::CollapsingHeader(TStable("Spawn options: offset, yaw, scale, direction"));
        if (selectedDetails && g_showSpawnOpts) {
            ImGui::SetNextItemWidth(260); DragFloat3Edit(T("offset forward, up, side"), g_off, 0.1f, -50, 50, "%.1f"); ImGui::SameLine();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("extra distance in front of you, height, and sideways offset; the object's footprint is already accounted for"));
            ImGui::SetNextItemWidth(150); SliderFloatEdit(T("yaw"), &g_spawnYaw, -180, 180, "%.0f"); ImGui::SameLine();
            ImGui::SetNextItemWidth(130); SliderFloatEdit(T("scale"), &g_spawnScale, 0.1f, 20.0f, "%.2f"); ImGui::SameLine();
            { Vec3 cf; bool haveCam = core::CameraPose(&cf, nullptr); ImGui::BeginDisabled(!haveCam); ImGui::Checkbox(T("front = camera view"), &g_useCamera); ImGui::EndDisabled();
              if (ImGui::IsItemHovered()) ImGui::SetTooltip(T(haveCam ? "where 'in front of you' points: the camera's view direction (on) or the direction you last walked (off)" : "camera not found yet; using the walking direction")); }
        }
        g_showMass = selectedDetails && ImGui::CollapsingHeader(TStable("Line and circle: many copies of the selected prefab at once"));
        if (selectedDetails && g_showMass) {
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
        if (selectedDetails && !g_recent.empty()) {
            ImGui::TextDisabled(T(ICON_CLOCK_ROTATE_LEFT " recent:"));
            for (size_t k = 0; k < g_recent.size(); k++) {
                ImGui::SameLine(); ImGui::PushID((int)k);
                if (ImGui::Button(idx[g_recent[k]].name.c_str())) g_selPrefab = g_recent[k];
                ImGui::PopID();
            }
        }
        if (selectedDetails) ImGui::EndChild();
        ImGui::EndChild();
    }

    static bool g_sceneCards = false; static std::set<int> g_closedGroups;
    static void DrawProjectGrounding();
    static void ClearTerrainAction();
    struct SceneTerrainTile { int tx = 0, tz = 0, project = 0; std::vector<core::TerrainStroke> strokes; };
    static std::vector<SceneTerrainTile> SceneTerrainTiles(int projectFilter) {
        std::map<std::tuple<int, int, int>, std::vector<core::TerrainStroke>> grouped;
        for (const auto& stroke : core::TerrainStrokes()) {
            if (projectFilter >= 0 && stroke.proj != projectFilter) continue;
            const int tx = stroke.tileScoped ? stroke.tileX : (int)std::floor(stroke.x / 1024.0f);
            const int tz = stroke.tileScoped ? stroke.tileZ : (int)std::floor(stroke.z / 1024.0f);
            grouped[{ tx, tz, stroke.proj }].push_back(stroke);
        }
        std::vector<SceneTerrainTile> tiles; tiles.reserve(grouped.size());
        for (auto& kv : grouped) tiles.push_back({ std::get<0>(kv.first), std::get<1>(kv.first), std::get<2>(kv.first), std::move(kv.second) });
        return tiles;
    }
    static void PaintTerrainTileThumbnail(const SceneTerrainTile& tile, ImVec2 min, ImVec2 max, ImDrawList* dl) {
        const ImVec2 size(max.x - min.x, max.y - min.y);
        dl->AddRectFilled(min, max, IM_COL32(27, 36, 43, 255), 4.0f);
        dl->PushClipRect(min, max, true);
        constexpr int dim = 32;
        struct Preview { int generation = -1; DWORD attempted = 0; bool ready = false; std::vector<float> delta; float peak = 0; };
        static std::map<std::tuple<int, int, int>, Preview> previews;
        if (previews.size() > 256) previews.clear();
        Preview& preview = previews[{ tile.tx, tile.tz, tile.project }];
        const int generation = core::TerrainPreviewGen();
        if (preview.generation != generation || (!preview.ready && GetTickCount() - preview.attempted >= 1000)) {
            preview.generation = generation;
            preview.attempted = GetTickCount();
            preview.ready = core::TerrainTilePreview(tile.tx, tile.tz, tile.project, dim, &preview.delta);
            preview.peak = 0;
            if (preview.ready) for (float value : preview.delta) preview.peak = std::max(preview.peak, std::fabs(value));
        }
        if (preview.ready) {
            for (int y = 0; y < dim; ++y) for (int x = 0; x < dim; ++x) {
                const float value = preview.delta[(size_t)y * dim + x];
                if (std::fabs(value) < 0.01f) continue;
                const float strength = preview.peak > 0 ? std::min(1.0f, std::fabs(value) / preview.peak) : 0;
                const int alpha = (int)(75 + 180 * strength);
                const ImU32 color = value > 0 ? IM_COL32(255, 177, 91, alpha) : IM_COL32(91, 175, 255, alpha);
                const ImVec2 a(min.x + size.x * x / dim, min.y + size.y * y / dim);
                const ImVec2 b(min.x + size.x * (x + 1) / dim, min.y + size.y * (y + 1) / dim);
                dl->AddRectFilled(a, b, color);
            }
        } else {
            // Unloaded tiles have no sampled heights yet. Show the saved stroke footprint until the tile streams in.
            const size_t begin = tile.strokes.size() > 256 ? tile.strokes.size() - 256 : 0;
            for (size_t i = begin; i < tile.strokes.size(); ++i) {
                const auto& stroke = tile.strokes[i];
                const float u = std::clamp((stroke.x - tile.tx * 1024.0f) / 1024.0f, 0.0f, 1.0f);
                const float v = std::clamp((stroke.z - tile.tz * 1024.0f) / 1024.0f, 0.0f, 1.0f);
                const float radius = std::max(2.5f, stroke.r * size.x / 1024.0f);
                const ImU32 color = stroke.mode == core::TerrainFlatten ? IM_COL32(115, 210, 145, 175) :
                    stroke.amount >= 0 ? IM_COL32(255, 177, 91, 175) : IM_COL32(91, 175, 255, 175);
                dl->AddCircleFilled(ImVec2(min.x + u * size.x, max.y - v * size.y), radius, color, 12);
            }
        }
        dl->AddRect(min, max, IM_COL32(105, 127, 143, 255), 4.0f, 0, 1.0f);
        dl->PopClipRect();
    }
    static void DrawTerrainTileThumbnail(const SceneTerrainTile& tile, ImVec2 size) {
        const ImVec2 min = ImGui::GetCursorScreenPos(), max(min.x + size.x, min.y + size.y);
        ImGui::Dummy(size);
        PaintTerrainTileThumbnail(tile, min, max, ImGui::GetWindowDrawList());
    }
    static void FocusTerrainTile(const SceneTerrainTile& tile) {
        if (tile.strokes.empty() || !core::FreeCamAvailable()) return;
        Vec3 center{};
        for (const auto& stroke : tile.strokes) {
            center.x += std::clamp(stroke.x, tile.tx * 1024.0f, (tile.tx + 1) * 1024.0f);
            center.y += stroke.y;
            center.z += std::clamp(stroke.z, tile.tz * 1024.0f, (tile.tz + 1) * 1024.0f);
        }
        const float count = (float)tile.strokes.size(); center.x /= count; center.y /= count; center.z /= count;
        float radius = 1.0f;
        for (const auto& stroke : tile.strokes) {
            const float dx = stroke.x - center.x, dy = stroke.y - center.y, dz = stroke.z - center.z;
            radius = std::max(radius, sqrtf(dx * dx + dy * dy + dz * dz) + stroke.r);
        }
        if (!g_cameraMode) ToggleCameraMode();
        if (!g_cameraMode) return;
        g_cameraViewMode = 0; core::FreeCamFocus(center, radius);
    }
    static void DeleteTerrainTileAction(int tx, int tz, int project) {
        if (!PrepareHistoryMutation([tx, tz, project]() { DeleteTerrainTileAction(tx, tz, project); })) return;
        Act act{}; act.kind = Act::TerrainTileRemove; act.tileX = tx; act.tileZ = tz; act.proj = project;
        if (!core::TerrainRemoveTile(tx, tz, project, act.terrain, act.terrainIndices)) return;
        if (g_terrainTileSelected && g_selectedTerrainX == tx && g_selectedTerrainZ == tz && g_selectedTerrainProject == project)
            g_terrainTileSelected = false;
        Push({ std::move(act) }); g_projectRefresh = true;
    }
    static bool TerrainTileSelected(const SceneTerrainTile& tile) {
        return g_terrainTileSelected && g_selectedTerrainX == tile.tx && g_selectedTerrainZ == tile.tz && g_selectedTerrainProject == tile.project;
    }
    static void SelectTerrainTile(const SceneTerrainTile& tile) {
        ClearSceneSelection();
        g_terrainTileSelected = true; g_selectedTerrainX = tile.tx; g_selectedTerrainZ = tile.tz; g_selectedTerrainProject = tile.project;
    }
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
        if (ImGui::MenuItem(T("Grab"))) StartGrab(SceneGrabIds(), false, SceneSelectionCount() == 1 ? ShortName(o.prefab) : "selection");
        ImGui::BeginDisabled(!g_managedNpcSel.empty());
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
        if (ImGui::MenuItem(T("Forget"))) ForgetSelection();
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
        const std::string name = e.npc ? ManagedNpcName(npcs[e.index], chars) : ShortName(objects[e.index].prefab);
        std::string note = e.npc ? npcs[e.index].note : objects[e.index].note;
        if (note.empty()) return name;
        for (char& c : note) if (c == '\r' || c == '\n') c = ' ';
        return note + " · " + name;
    }
    static Vec3 SceneEntityPos(const SceneEntityRef& e, const std::vector<SpawnedObj>& objects, const std::vector<ManagedNpc>& npcs) {
        return e.npc ? ManagedNpcDisplayPos(npcs[e.index]) : objects[e.index].pos;
    }
    static void SelectSceneEntity(const SceneEntityRef& e, bool add, bool range, const std::vector<SceneEntityRef>& order,
                                  const std::vector<SpawnedObj>& objects, const std::vector<ManagedNpc>& npcs) {
        g_terrainTileSelected = false;
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
        if (ImGui::MenuItem(T("Forget"))) ForgetSelection();
        if (ImGui::MenuItem(T("Delete"))) DeleteSceneSelection();
        ImGui::EndPopup();
    }
    static void SelectSceneGroup(int gid, const std::vector<SceneEntityRef>& entities, bool toggle) {
        g_terrainTileSelected = false;
        bool all = true; for (const auto& e : entities) if (e.group == gid && !e.hidden && !SceneEntitySelected(e)) { all = false; break; }
        if (!toggle) ClearSceneSelection();
        for (const auto& e : entities) if (e.group == gid && !e.hidden) {
            if (toggle && all) { if (e.npc) g_managedNpcSel.erase(e.uid); else g_sel.erase(e.uid); }
            else { if (e.npc) g_managedNpcSel.insert(e.uid); else g_sel.insert(e.uid); }
        }
        g_sceneLastEntity = 0; g_editUid = 0;
    }
    static int CardGridColumns(float availableWidth, float cardWidth, float spacing) {
        return std::max(1, (int)((availableWidth + spacing) / (cardWidth + spacing)));
    }
    // scene as tiles: loose objects first, then every group as a framed block with its own header (click = select all, arrow = collapse)
    // Unified Scene cards: prefab objects and managed NPCs are peers in the same ordered/grouped collection.
    static void DrawSceneCards(const std::vector<SpawnedObj>& objects, const std::vector<ManagedNpc>& npcs,
                               const std::vector<SceneEntityRef>& entities, const std::vector<SceneTerrainTile>& tiles,
                               const std::vector<thumbgen::CharInfo>* chars,
                               const PosInfo& p, bool havePos, float listH, float ui) {
        const auto& idx = core::PrefabIndex();
        std::vector<int> loose, gorder; std::map<int, std::vector<int>> groups;
        for (int i = 0; i < (int)entities.size(); ++i) { const auto& e = entities[i]; if (e.group > 0) { if (!groups.count(e.group)) gorder.push_back(e.group); groups[e.group].push_back(i); } else loose.push_back(i); }
        const float pad = 4.0f * ui, tile = g_cardSize * ui, textH = ImGui::GetTextLineHeight() * 2 + 3;
        const float cw = tile + 2 * pad, ch = tile + 2 * pad + textH;
        ImGui::BeginChild("scenecards", ImVec2(0, listH), ImGuiChildFlags_Borders);
        const float sp = ImGui::GetStyle().ItemSpacing.x; const int cols = CardGridColumns(ImGui::GetContentRegionAvail().x, cw, sp);
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
                const auto& n = npcs[e.index]; const auto* c = ManagedNpcChar(n, chars);
                ImTextureID tex{};
                if (c && !c->app.empty()) {
                    if (thumbgen::Ready() && !thumbgen::Processed(c->app)) thumbgen::Request(c->app);
                    tex = overlay::Thumb(core::ThumbFile(c->app));
                }
                if (tex) dl->AddImage(tex, t0, t1, ImVec2(0, 0), ImVec2(1, 1), n.hidden ? IM_COL32(255,255,255,90) : IM_COL32_WHITE);
                else {
                    dl->AddRectFilled(t0, t1, IM_COL32(28,31,38,220), 3.0f);
                    const char* tag = T("NPC"); const ImVec2 ts = ImGui::CalcTextSize(tag); dl->AddText({ t0.x + (tile-ts.x)*0.5f, t0.y + (tile-ts.y)*0.5f }, IM_COL32(210,220,235,230), tag);
                }
            }
            float dist = 0; if (havePos) { float dx=pos.x-p.world.x,dy=pos.y-p.world.y,dz=pos.z-p.world.z; dist=sqrtf(dx*dx+dy*dy+dz*dz); }
            char info[80]; snprintf(info,sizeof info,"#%d  %s  %.0f m",e.uid,e.npc?T("NPC"):T("Object"),dist); dl->AddText({t0.x+3,t0.y+2},IM_COL32(255,255,255,210),info);
            if (e.group > 0) dl->AddRectFilled({t1.x-7,t0.y},{t1.x,t0.y+7},GroupColor(e.group,255));
            dl->PushClipRect({p0.x+pad,t1.y},{p1.x-pad,p1.y},true); dl->AddText(ImGui::GetFont(),ImGui::GetFontSize(),{p0.x+pad,t1.y+2},ImGui::GetColorU32(e.hidden?ImGuiCol_TextDisabled:ImGuiCol_Text),name.c_str(),nullptr,tile); dl->PopClipRect();
            if (hov) {
                if (e.npc) { const auto& n=npcs[e.index]; ManagedNpcRuntimeTooltip(n, name, pos); }
                else { const auto& o=objects[e.index]; ImGui::SetTooltip(T("%s\nObject #%d\n%.2f  %.2f  %.2f   yaw %.0f   scale %.2f%s%s"),o.prefab.c_str(),o.uid,o.pos.x,o.pos.y,o.pos.z,o.rot.yaw,o.scale,o.note.empty()?"":"\n",o.note.c_str()); }
            }
            ImGui::PopID();
        };
        auto grid=[&](const std::vector<int>& order){ for(size_t k=0;k<order.size();++k){ if(k%cols) ImGui::SameLine(); card(order[k]); } };
        grid(loose);
        for (size_t i = 0; i < tiles.size(); ++i) {
            const auto& terrain = tiles[i];
            if ((loose.size() + i) % cols) ImGui::SameLine();
            ImGui::PushID(terrain.tx); ImGui::PushID(terrain.tz); ImGui::PushID(terrain.project);
            const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + cw, p0.y + ch);
            const ImVec2 t0(p0.x + pad, p0.y + pad), t1(t0.x + tile, t0.y + tile);
            ImGui::InvisibleButton("terraincard", ImVec2(cw, ch));
            const bool hovered = ImGui::IsItemHovered(), selected = TerrainTileSelected(terrain);
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) SelectTerrainTile(terrain);
            if (ImGui::BeginPopupContextItem("terrainctx")) {
                ImGui::BeginDisabled(!core::FreeCamAvailable());
                if (ImGui::MenuItem(T("Focus"))) FocusTerrainTile(terrain);
                ImGui::EndDisabled();
                if (ImGui::MenuItem(T("Delete"))) DeleteTerrainTileAction(terrain.tx, terrain.tz, terrain.project);
                ImGui::EndPopup();
            }
            dl->AddRectFilled(p0, p1, selected ? ImGui::GetColorU32(ImGuiCol_Header) : hovered ? ImGui::GetColorU32(ImGuiCol_FrameBgHovered) : ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
            if (selected) dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_HeaderActive), 4.0f, 0, 2.0f);
            PaintTerrainTileThumbnail(terrain, t0, t1, dl);
            char name[96]; snprintf(name, sizeof name, T("Terrain tile %d, %d"), terrain.tx, terrain.tz);
            dl->PushClipRect({p0.x + pad, t1.y}, {p1.x - pad, p1.y}, true);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), {p0.x + pad, t1.y + 2}, ImGui::GetColorU32(ImGuiCol_Text), name);
            dl->PopClipRect();
            if (hovered) {
                const std::string projectName = terrain.project ? core::ProjectNameOf(terrain.project) : T("Unassigned");
                ImGui::BeginTooltip();
                ImGui::Text(T("Terrain tile %d, %d"), terrain.tx, terrain.tz);
                ImGui::TextDisabled(T("%s | %d strokes"), projectName.c_str(), (int)terrain.strokes.size());
                ImGui::EndTooltip();
            }
            ImGui::PopID(); ImGui::PopID(); ImGui::PopID();
        }
        for(int gid:gorder){ const auto& mem=groups[gid]; const bool closed=g_closedGroups.count(gid)>0; bool allSel=true; for(int ei:mem)allSel&=SceneEntitySelected(entities[ei]);
            ImGui::Spacing(); ImGui::PushID(gid); if(ImGui::Button(closed?">":"v")){if(closed)g_closedGroups.erase(gid);else g_closedGroups.insert(gid);} ImGui::SameLine();
            const std::string gn=core::GroupName(gid); char gl[224]; if(!gn.empty()) snprintf(gl,sizeof gl,"%s  (%d)",gn.c_str(),(int)mem.size()); else snprintf(gl,sizeof gl,T("Group %d  (%d entities)"),gid,(int)mem.size());
            ImGui::PushStyleColor(ImGuiCol_Text,GroupColor(gid,255)); if(ImGui::Selectable(gl,allSel)){SelectSceneGroup(gid,entities,CtrlHeld(ImGui::GetIO()));} ImGui::PopStyleColor();
            if(ImGui::BeginPopupContextItem("groupctx")){ if(ImGui::MenuItem(T("Rename group"))) OpenGroupNameEdit(gid); if(ImGui::MenuItem(T("Grab group"))){SelectSceneGroup(gid,entities,false);StartGrab(SceneGrabIds(),false,gn.empty()?"group":gn);} if(ImGui::MenuItem(T("Ungroup"))){SelectSceneGroup(gid,entities,false);GroupSceneSelection(false);} if(ImGui::MenuItem(T("Delete"))){SelectSceneGroup(gid,entities,false);DeleteSceneSelection();} ImGui::EndPopup(); }
            if(!closed) grid(mem); ImGui::PopID(); ImGui::Spacing(); }
        ImGui::EndChild();
    }
    // The scene list follows the project selected for editing; other loaded projects remain in the game world.
    static void DrawEditingProjectHeader(bool compact = false) {
        const std::string name = core::EditingProject();
        g_projTab = name.empty() ? 0 : core::ProjectId(name);
        if (name.empty()) {
            ImGui::TextDisabled("%s", T("No project selected; placing creates a numbered Untitled project."));
            return;
        }
        ImGui::Text("%s: %s%s", T("Editing project"), name.c_str(), core::ProjectDirty(g_projTab) ? " *" : "");
        SameLineOrWrap(compact, ImGui::CalcTextSize(T("Save")).x + ImGui::GetStyle().FramePadding.x * 2);
        ImGui::BeginDisabled(!core::ProjectDirty(g_projTab));
        if (ImGui::Button(T("Save"))) SaveProjectAction(name, core::SaveProjectOnly, true);
        ImGui::EndDisabled();
        if (!core::IsProjectLoaded(name)) ImGui::TextWrapped("%s", T("The editing project is not loaded yet. Enter the game world, move your character, then load it."));
    }
    static void DrawScene(const PosInfo& p, bool havePos, bool compact = false) {
        auto objects = core::Spawned(); auto npcs = core::ManagedNpcs(); const auto chars = thumbgen::Characters();
        const float ui = ImGui::GetFontSize() / 17.0f;
        DrawEditingProjectHeader(compact);
        const auto entities = g_projTab > 0 ? BuildSceneEntities(objects, npcs, g_projTab) : std::vector<SceneEntityRef>{};
        const auto terrainTiles = g_projTab > 0 ? SceneTerrainTiles(g_projTab) : std::vector<SceneTerrainTile>{};
        auto selectedTerrain = std::find_if(terrainTiles.begin(), terrainTiles.end(), [](const SceneTerrainTile& tile) { return TerrainTileSelected(tile); });
        if (g_terrainTileSelected && selectedTerrain == terrainTiles.end()) g_terrainTileSelected = false;

        if (compact) g_sceneCards = true;
        const bool useCards = g_sceneCards || ImGui::GetContentRegionAvail().x < 780.0f * ui;
        if (!compact) {
            const ImVec4 on=ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive),off=ImGui::GetStyleColorVec4(ImGuiCol_Button);
            ImGui::PushStyleColor(ImGuiCol_Button,g_sceneCards?off:on); if(ImGui::Button(T(ICON_LIST " list")))g_sceneCards=false; ImGui::PopStyleColor();
            SameLineForControl(ICON_COPY " cards"); ImGui::PushStyleColor(ImGuiCol_Button,g_sceneCards?on:off); if(ImGui::Button(T(ICON_COPY " cards")))g_sceneCards=true; ImGui::PopStyleColor();
            if(g_sceneCards){SameLineOrWrap(compact,95*ui+ImGui::GetFrameHeightWithSpacing());ImGui::SetNextItemWidth(std::min(95*ui,ImGui::GetContentRegionAvail().x));SliderFloatEdit("##scardsize",&g_cardSize,64,200,"%.0f px");}
            SameLineForControl("click selects group", true);
        }
        ImGui::Checkbox(T("click selects group"),&g_selectGroups); SameLineForControl("show deleted", true); ImGui::Checkbox(T("show deleted"),&g_showDeleted);
        DrawPlacementSnapControls();
        SameLineOrWrap(compact, 75.0f);
        ImGui::SetNextItemWidth(std::min(70.0f, ImGui::GetContentRegionAvail().x));
        DragFloatEdit("##rotstep", &g_rotationStep, 1.0f, 1.0f, 90.0f, "%.0f deg");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("rotation step for Rotate - / Rotate +"));
        SameLineForControl("Clear selection"); ImGui::BeginDisabled(!SceneHasSelection() && !g_terrainTileSelected); if(ImGui::Button(T("Clear selection")))ClearSceneSelection(); ImGui::EndDisabled();
        SameLineForControl("Undo"); ImGui::BeginDisabled(g_undo.empty()); if(ImGui::Button(T("Undo")))Undo(); ImGui::EndDisabled();
        SameLineForControl("Redo"); ImGui::BeginDisabled(g_redo.empty()); if(ImGui::Button(T("Redo")))Redo(); ImGui::EndDisabled();
        SameLineForControl("Remove duplicates"); if(FittedButton(T("Remove duplicates")))RemoveDuplicates();
        ImGui::TextWrapped(T("%d entities, %d selected"),(int)(entities.size() + terrainTiles.size()),(int)SceneSelectionCount() + (g_terrainTileSelected ? 1 : 0));

        const float availY=ImGui::GetContentRegionAvail().y; const float footer=56.0f*ui;
        const float listH=std::max(64.0f*ui,availY-footer);
        if (entities.empty() && terrainTiles.empty()) ImGui::TextDisabled("%s", T("No scene entries in this view."));
        else if(useCards) DrawSceneCards(objects,npcs,entities,terrainTiles,chars?chars.get():nullptr,p,havePos,listH,ui);
        else if(ImGui::BeginTable("scene_entities",7,ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY|ImGuiTableFlags_BordersInnerH,ImVec2(-1,listH))){
            ImGui::TableSetupColumn("#",ImGuiTableColumnFlags_WidthFixed,48); ImGui::TableSetupColumn(T("entity")); ImGui::TableSetupColumn(T("type"),ImGuiTableColumnFlags_WidthFixed,70); ImGui::TableSetupColumn(T("grp"),ImGuiTableColumnFlags_WidthFixed,46); ImGui::TableSetupColumn(T("position"),ImGuiTableColumnFlags_WidthFixed,220); ImGui::TableSetupColumn(T("state"),ImGuiTableColumnFlags_WidthFixed,190); ImGui::TableSetupColumn(T("dist"),ImGuiTableColumnFlags_WidthFixed,64); ImGui::TableHeadersRow();
            std::map<int,std::vector<int>> groups; std::vector<int> order; for(int i=0;i<(int)entities.size();++i){const auto&e=entities[i];if(e.group>0){if(!groups.count(e.group))order.push_back(-e.group);groups[e.group].push_back(i);}else order.push_back(i+1);}
            std::vector<std::pair<int,bool>> rows; for(int x:order){if(x>0)rows.push_back({x-1,false});else{rows.push_back({x,false});if(!g_closedGroups.count(-x))for(int ei:groups[-x])rows.push_back({ei,true});}}
            for(const auto&rw:rows){
                if(rw.first<0){const int gid=-rw.first;const auto&mem=groups[gid];bool all=true;for(int ei:mem)all&=SceneEntitySelected(entities[ei]);ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::PushID(gid);if(ImGui::Button(g_closedGroups.count(gid)?">":"v")){if(g_closedGroups.count(gid))g_closedGroups.erase(gid);else g_closedGroups.insert(gid);}ImGui::PopID();ImGui::TableSetColumnIndex(1);const std::string gn=core::GroupName(gid);char gl[240];if(!gn.empty())snprintf(gl,sizeof gl,"%s  (%d)##g%d",gn.c_str(),(int)mem.size(),gid);else snprintf(gl,sizeof gl,T("Group %d  (%d entities)##g%d"),gid,(int)mem.size(),gid);if(ImGui::Selectable(gl,all,ImGuiSelectableFlags_SpanAllColumns))SelectSceneGroup(gid,entities,CtrlHeld(ImGui::GetIO()));if(ImGui::BeginPopupContextItem("groupctx")){if(ImGui::MenuItem(T("Rename group")))OpenGroupNameEdit(gid);if(ImGui::MenuItem(T("Ungroup"))){SelectSceneGroup(gid,entities,false);GroupSceneSelection(false);}if(ImGui::MenuItem(T("Delete"))){SelectSceneGroup(gid,entities,false);DeleteSceneSelection();}ImGui::EndPopup();}ImGui::TableSetColumnIndex(3);ImGui::TextDisabled("%d",gid);continue;}
                const SceneEntityRef&e=entities[rw.first];const Vec3 pos=SceneEntityPos(e,objects,npcs);const std::string name=SceneEntityName(e,objects,npcs,chars?chars.get():nullptr);ImGui::PushID(SceneEntityKey(e));ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);char id[24];snprintf(id,sizeof id,"%c%d",e.npc?'N':'#',e.uid);if(rw.second)ImGui::Indent(12);if(ImGui::Selectable(id,SceneEntitySelected(e),ImGuiSelectableFlags_SpanAllColumns)){ImGuiIO&io=ImGui::GetIO();SelectSceneEntity(e,CtrlHeld(io),ShiftHeld(io),entities,objects,npcs);}if(e.npc)SceneNpcContext(npcs[e.index],havePos,"rowctx");else SceneObjectContext(objects[e.index],objects,havePos,"rowctx");if(rw.second)ImGui::Unindent(12);
                ImGui::TableSetColumnIndex(1);if(e.hidden)ImGui::TextDisabled(T("%s (deleted)"),name.c_str());else ImGui::TextUnformatted(name.c_str());
                const std::string note=e.npc?npcs[e.index].note:objects[e.index].note;if(!note.empty()&&ImGui::IsItemHovered())ImGui::SetTooltip("%s",note.c_str());
                ImGui::TableSetColumnIndex(2);ImGui::TextDisabled("%s",e.npc?T("NPC"):T("Object"));ImGui::TableSetColumnIndex(3);if(e.group)ImGui::TextDisabled("%d",e.group);
                ImGui::TableSetColumnIndex(4);ImGui::Text("%.2f  %.2f  %.2f",pos.x,pos.y,pos.z);ImGui::TableSetColumnIndex(5);
                if(e.npc){const auto&n=npcs[e.index];if(!n.actor||n.spawnPending)ImGui::TextDisabled("%s",T("pending"));else if(n.aiApplied!=n.aiEnabled)ImGui::TextDisabled("%s",T("syncing"));else ImGui::Text("%s   %s",n.aiEnabled?T("AI on"):T("AI off"),n.behavior==1?T("Hold"):T("Normal"));if(ImGui::IsItemHovered())ManagedNpcRuntimeTooltip(n,name,pos);}
                else{const auto&o=objects[e.index];ImGui::TextDisabled(T("yaw %.0f   scale %.2f"),o.rot.yaw,o.scale);}
                ImGui::TableSetColumnIndex(6);if(havePos){float dx=pos.x-p.world.x,dy=pos.y-p.world.y,dz=pos.z-p.world.z;ImGui::Text("%.0f m",sqrtf(dx*dx+dy*dy+dz*dz));}ImGui::PopID();
            }
            for (const auto& tile : terrainTiles) {
                ImGui::PushID(tile.tx); ImGui::PushID(tile.tz); ImGui::PushID(tile.project);
                ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                if (ImGui::Selectable("T##terrain", TerrainTileSelected(tile), ImGuiSelectableFlags_SpanAllColumns, ImVec2(0, 48.0f * ui))) SelectTerrainTile(tile);
                if (ImGui::BeginPopupContextItem("terrain-rowctx")) {
                    ImGui::BeginDisabled(!core::FreeCamAvailable());
                    if (ImGui::MenuItem(T("Focus"))) FocusTerrainTile(tile);
                    ImGui::EndDisabled();
                    if (ImGui::MenuItem(T("Delete"))) DeleteTerrainTileAction(tile.tx, tile.tz, tile.project);
                    ImGui::EndPopup();
                }
                ImGui::TableSetColumnIndex(1);
                DrawTerrainTileThumbnail(tile, ImVec2(48.0f * ui, 36.0f * ui));
                ImGui::SameLine(); ImGui::Text(T("Terrain tile %d, %d"), tile.tx, tile.tz);
                ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("%s", T("Terrain"));
                ImGui::TableSetColumnIndex(4);
                const Vec3 center = tile.strokes.empty() ? Vec3{} : Vec3{tile.strokes.front().x, tile.strokes.front().y, tile.strokes.front().z};
                ImGui::Text("%.2f  %.2f  %.2f", center.x, center.y, center.z);
                ImGui::TableSetColumnIndex(5); ImGui::TextDisabled(T("%d strokes"), (int)tile.strokes.size());
                ImGui::TableSetColumnIndex(6);
                if (havePos) { const float dx=center.x-p.world.x, dy=center.y-p.world.y, dz=center.z-p.world.z; ImGui::Text("%.0f m",sqrtf(dx*dx+dy*dy+dz*dz)); }
                ImGui::PopID(); ImGui::PopID(); ImGui::PopID();
            }
            ImGui::EndTable();
        }

        selectedTerrain = std::find_if(terrainTiles.begin(), terrainTiles.end(), [](const SceneTerrainTile& tile) { return TerrainTileSelected(tile); });
        for(auto it=g_sel.begin();it!=g_sel.end();){const auto* o=Find(objects,*it);if(!o||o->hidden)it=g_sel.erase(it);else ++it;} for(auto it=g_managedNpcSel.begin();it!=g_managedNpcSel.end();){const auto* n=FindManagedNpc(npcs,*it);if(!n||n->hidden)it=g_managedNpcSel.erase(it);else ++it;}
        if (SceneHasSelection()) {
            SameLineForControl("Properties");
            if (ImGui::Button(T("Properties"))) OpenSelectionProperties();
        } else if (g_terrainTileSelected && selectedTerrain != terrainTiles.end()) {
            ImGui::Text(T("Terrain tile %d, %d"), selectedTerrain->tx, selectedTerrain->tz);
            SameLineForControl("Focus"); ImGui::BeginDisabled(!core::FreeCamAvailable());
            if (ImGui::Button(T("Focus"))) FocusTerrainTile(*selectedTerrain);
            ImGui::EndDisabled();
            SameLineForControl("Delete");
            if (ImGui::Button(T("Delete"))) DeleteTerrainTileAction(selectedTerrain->tx, selectedTerrain->tz, selectedTerrain->project);
        } else if (!entities.empty()) ImGui::TextDisabled("%s", T("Select an object or NPC to inspect it. Ctrl-click adds, Shift-click selects a range."));
        if (ImGui::CollapsingHeader(TStable("Grounding operations"))) DrawProjectGrounding();
    }
    static void DrawSelectionPropertiesContent(const PosInfo& p, bool havePos, bool compact) {
        auto objects = core::Spawned(); auto npcs = core::ManagedNpcs(); const auto chars = thumbgen::Characters();
        const float ui = ImGui::GetFontSize() / 17.0f;
        for (auto it = g_sel.begin(); it != g_sel.end();) { const auto* o = Find(objects, *it); if (!o || o->hidden) it = g_sel.erase(it); else ++it; }
        for (auto it = g_managedNpcSel.begin(); it != g_managedNpcSel.end();) { const auto* n = FindManagedNpc(npcs, *it); if (!n || n->hidden) it = g_managedNpcSel.erase(it); else ++it; }
        if (!SceneHasSelection()) { g_propertiesOpen = false; return; }
        ImGui::Separator();
        const SpawnedObj* objPrim=(g_sel.size()==1&&g_managedNpcSel.empty())?Find(objects,*g_sel.begin()):nullptr;
        const ManagedNpc* npcPrim=(g_managedNpcSel.size()==1&&g_sel.empty())?FindManagedNpc(npcs,*g_managedNpcSel.begin()):nullptr;
        if(objPrim){
            const auto&o=*objPrim;if(g_editUid!=o.uid){g_edit[0]=o.pos.x;g_edit[1]=o.pos.y;g_edit[2]=o.pos.z;g_editRot=o.rot;g_editScale=o.scale;g_editPos0=o.pos;g_editRot0=o.rot;g_editScale0=o.scale;g_editUid=o.uid;}
            ImGui::TextWrapped("%s",o.prefab.c_str());bool changed=false;
            SetLabeledItemWidth("position",300*ui);changed|=DragFloat3Edit(T("position"),g_edit,0.05f,-100000,100000,"%.3f");
            bool rel1=ImGui::IsItemDeactivatedAfterEdit()||NumericEditEnded(T("position"));
            SameLineOrWrap(compact,140*ui+ImGui::CalcTextSize(T("yaw##e"),nullptr,true).x+ImGui::GetFrameHeightWithSpacing());
            SetLabeledItemWidth("yaw##e",140*ui);changed|=SliderFloatEdit(T("yaw##e"),&g_editRot.yaw,-180,180,"%.1f");
            bool rel2=ImGui::IsItemDeactivatedAfterEdit()||NumericEditEnded(T("yaw##e"));
            SameLineOrWrap(compact,120*ui+ImGui::CalcTextSize(T("scale##e"),nullptr,true).x+ImGui::GetFrameHeightWithSpacing());
            SetLabeledItemWidth("scale##e",120*ui);changed|=SliderFloatEdit(T("scale##e"),&g_editScale,0.05f,20,"%.3f");
            bool rel3=ImGui::IsItemDeactivatedAfterEdit()||NumericEditEnded(T("scale##e"));
            const DWORD now=TickNow();static DWORD liveAt=0;
            if(changed&&g_live&&now-liveAt>50){SceneNumericApply(o.uid,{g_edit[0],g_edit[1],g_edit[2]},g_editRot,g_editScale,false);liveAt=now;}
            if(rel1||rel2||rel3) SceneNumericApply(o.uid,{g_edit[0],g_edit[1],g_edit[2]},g_editRot,g_editScale,true);
        } else if(npcPrim){
            const auto&n=*npcPrim;const Vec3 livePos=ManagedNpcDisplayPos(n);
            static int posUid=0;static float ep[3]={};static Vec3 base{};static bool liveMove=false;static DWORD liveAt=0;
            if(posUid!=n.uid||(!ImGui::IsAnyItemActive()&&!liveMove&&(ep[0]!=livePos.x||ep[1]!=livePos.y||ep[2]!=livePos.z))){
                posUid=n.uid;ep[0]=base.x=livePos.x;ep[1]=base.y=livePos.y;ep[2]=base.z=livePos.z;liveMove=false;
            }
            ImGui::TextWrapped("%s: %s",T("NPC properties"),ManagedNpcName(n,chars?chars.get():nullptr).c_str());
            SetLabeledItemWidth("position##scene_npc",330*ui);
            const bool npcPosChanged=ImGui::DragFloat3(T("position##scene_npc"),ep,0.05f,-100000,100000,"%.3f");
            if(npcPosChanged){
                const DWORD now=TickNow();
                if(now-liveAt>=45){NpcNumericApply(n.uid,{ep[0],ep[1],ep[2]},false);liveMove=g_numericNpcUid==n.uid;liveAt=now;}
            }
            if(ImGui::IsItemDeactivatedAfterEdit()){
                const Vec3 finalPos{ep[0],ep[1],ep[2]};
                NpcNumericApply(n.uid,finalPos,true);
                base=finalPos;liveMove=false;
            }
            SameLineForControl("AI enabled",true);bool ai=n.aiEnabled;ImGui::BeginDisabled(!core::NpcAiControlAvailable());if(ImGui::Checkbox(T("AI enabled"),&ai))SetSelectedNpcAi(ai);
            SameLineOrWrap(compact,180*ui);int behavior=n.behavior;ImGui::SetNextItemWidth(std::min(180*ui,ImGui::GetContentRegionAvail().x));if(ComboT("##npc_behavior_scene",&behavior,kNpcBehaviors,2))SetSelectedNpcBehavior(behavior);ImGui::EndDisabled();
            SameLineForControl("Rename");if(ImGui::Button(T("Rename")))OpenNpcLabelEdit(n);SameLineForControl("Edit note");if(ImGui::Button(T("Edit note")))OpenNpcNoteEdit(n);
        } else ImGui::TextWrapped(T("Mixed selection: %d objects and %d NPCs"),(int)g_sel.size(),(int)g_managedNpcSel.size());

        ImGui::BeginDisabled(!core::FreeCamAvailable());if(ImGui::Button(T("Focus")))FocusSelection();ImGui::EndDisabled();
        SameLineForControl(ICON_HAND " Grab");if(ImGui::Button(T(ICON_HAND " Grab"))){std::string grabName=g_sel.size()==1&&g_managedNpcSel.empty()&&objPrim?ShortName(objPrim->prefab):g_managedNpcSel.size()==1&&g_sel.empty()&&npcPrim?ManagedNpcHistoryName(*npcPrim):"selection";StartGrab(SceneGrabIds(),false,grabName);}
        SameLineForControl("To ground");ImGui::BeginDisabled(!g_managedNpcSel.empty());if(ImGui::Button(T("To ground")))SnapSelToGround();
        SameLineForControl(ICON_COPY " Duplicate");if(ImGui::Button(T(ICON_COPY " Duplicate"))){CopySel();Paste(havePos);}ImGui::EndDisabled();
        SameLineForControl("Group");if(ImGui::Button(T("Group")))GroupSceneSelection(true);SameLineForControl("Ungroup");if(ImGui::Button(T("Ungroup")))GroupSceneSelection(false);
        if(!g_managedNpcSel.empty()){
            SameLineForControl("AI on");ImGui::BeginDisabled(!core::NpcAiControlAvailable());if(ImGui::Button(T("AI on")))SetSelectedNpcAi(true);
            SameLineForControl("AI off");if(ImGui::Button(T("AI off")))SetSelectedNpcAi(false);
            SameLineOrWrap(compact,180*ui);int b=-1;bool first=true,mixed=false;for(int uid:g_managedNpcSel)if(const auto*n=FindManagedNpc(npcs,uid)){if(first){b=n->behavior;first=false;}else if(b!=n->behavior)mixed=true;}
            ImGui::SetNextItemWidth(std::min(180*ui,ImGui::GetContentRegionAvail().x));const char* preview=mixed?T("Mixed"):b==1?T("Hold"):T("Normal");if(ImGui::BeginCombo("##batch_behavior",preview)){if(ImGui::Selectable(T("Normal autonomous"),b==0&&!mixed))SetSelectedNpcBehavior(0);if(ImGui::Selectable(T("Hold position (AI paused)"),b==1&&!mixed))SetSelectedNpcBehavior(1);ImGui::EndCombo();}ImGui::EndDisabled();
        }
        SameLineForControl("Forget");if(FittedButton(T("Forget")))ForgetSelection();
        SameLineForControl(ICON_TRASH " Delete");ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.55f,0.15f,0.12f,1));if(ImGui::Button(T(ICON_TRASH " Delete")))DeleteSceneSelection();ImGui::PopStyleColor();
    }
    static void DrawPropertiesWindow(const PosInfo& p, bool havePos) {
        const char* title = TStable("Properties###selection-properties");
        if (g_propertiesPopupRequested) {
            ImGui::OpenPopup(title);
            g_propertiesPopupRequested = false;
        }
        if (!g_propertiesOpen && !ImGui::IsPopupOpen(title)) return;
        const float ui = ImGui::GetFontSize() / 17.0f;
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(std::min(520.0f * ui, display.x - 24.0f), std::min(440.0f * ui, display.y - 24.0f)), ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints(ImVec2(std::min(300.0f * ui, display.x - 24.0f), std::min(200.0f * ui, display.y - 24.0f)), ImVec2(display.x - 24.0f, display.y - 24.0f));
        if (ImGui::BeginPopupModal(title, &g_propertiesOpen)) {
            DrawSelectionPropertiesContent(p, havePos, false);
            if (!g_propertiesOpen) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    struct ProjectFile { std::string name; proj_codec::Kind kind = proj_codec::Kind::Project; bool archived = false; std::string path; };
    static ProjectFile g_blueprintDragFile;
    static bool g_blueprintDragging = false;
    struct PendingBlueprintPlace { bool active = false; ProjectFile file; Vec3 target{}; DWORD queuedAt = 0; };
    static PendingBlueprintPlace g_pendingBlueprintPlace;
    enum class ProjectAction { Read, Load, Replace, SaveBack, Autoload, Place, Overwrite, Reload, Save, Unload, AddUnassigned };
    static core::SavedLibrarySnapshot g_projectLibrary;
    static core::SavedFile g_librarySelected;
    static std::vector<size_t> g_libraryRows;
    static std::map<std::string, ULONGLONG> g_badSavedThumbnailStamps;
    static std::map<std::string, DWORD> g_pendingSavedThumbnailTicks;
    static std::set<std::string> g_currentSavedThumbnails;
    static char g_librarySearch[256] = {};
    static int g_libraryLocation = 0;
    static bool g_libraryShowingBlueprints = false;
    static bool g_libraryFilterDirty = true;
    static core::FileResult g_libraryResult;
    static std::vector<std::string> g_projectAutoload;
    static bool g_exportOpen = false, g_exportOverwrite = false;
    static core::GroupExportApproval g_exportApproval;
    static std::vector<SpawnedObj> g_exportObjects;
    static std::string g_exportStatus, g_projectOverwriteName;
    static int g_projectOverwriteScope = core::SaveWholeScene, g_groundMode = 0;
    struct ExportAttempt { core::GroupExportApproval approval; std::string result, name; };
    static std::vector<ExportAttempt> g_exportAttempts;
    static size_t g_exportAttempt = 0;
    struct FileFailure { std::string action, path, reason; };
    static std::vector<FileFailure> g_projectFileFailures;
    static std::set<std::string> g_projectDetails;
    static proj_codec::Document g_projectRead;
    static std::string g_projectReadPath;
    static bool g_projectReadValid = false;
    static void FinishProjectSceneMutation();
    static const char* ProjectUiStatus(const char* raw) {
        if (!raw) return "";
        struct CodeLabel { const char* code; const char* label; };
        static constexpr CodeLabel labels[] = {
            { "None", "No error" },
            { "InvalidName", "Invalid file name" },
            { "InvalidAction", "Action unavailable" },
            { "NotFound", "File not found" },
            { "StaleTarget", "File changed; refresh the list" },
            { "ConfirmationMismatch", "Confirmation does not match" },
            { "GuardMissing", "Operation guard expired" },
            { "SelectionChanged", "Selection changed; try again" },
            { "VisibleReference", "File has visible scene entries" },
            { "HiddenReference", "File has hidden scene entries" },
            { "UndoReference", "File is used by Undo history" },
            { "RedoReference", "File is used by Redo history" },
            { "DirtyProject", "Project has unsaved changes" },
            { "PendingOperation", "Operation still in progress" },
            { "InFlightExport", "Blueprint export in progress" },
            { "InFlightPlace", "Blueprint placement in progress" },
            { "AutoloadEnabled", "Disable auto-load first" },
            { "AutoloadReadFailed", "Could not read auto-load settings" },
            { "Collision", "File name already exists" },
            { "UnsafePath", "File path is not allowed" },
            { "ReadFailed", "Could not read file" },
            { "WriteFailed", "Could not write file" },
            { "MoveFailed", "Could not move file" },
            { "DeleteFailed", "Could not delete file" },
            { "VerifyFailed", "File verification failed" },
            { "InvalidReason", "Unknown file error" },
            { "wrong-kind", "Wrong file type" },
            { "cannot-read", "Could not read the file" },
            { "invalid-name", "Enter a valid name" },
            { "overwrite-approval-required", "Confirm before overwriting" },
            { "finish-edit-before-save", "Finish the current edit before saving" },
            { "invalid-action", "This action is unavailable" },
            { "placement-pending", "Finish placing before this action" },
            { "drop-before-per-object", "Drop the selection before grounding each object" },
            { "load-admitted", "Project loading started" },
            { "unloaded", "Project unloaded" },
            { "saved", "Project saved" },
            { "preflight-ready", "Blueprint ready to export" },
            { "record 0: game thread pump not active yet", "The game world is not ready for project objects yet. Move your character, then load the project again." },
        };
        for (const auto& entry : labels) if (strcmp(raw, entry.code) == 0) return T(entry.label);
        const char* translated = T(raw);
        if (translated != raw || i18n::ActiveLanguage() == "en") return translated;
        static std::string lastUntranslated;
        if (lastUntranslated != raw) {
            lastUntranslated = raw;
            core::Log("[i18n] project detail has no translation: %s", raw);
        }
        return T("Project operation failed; see log for details.");
    }
    static bool SceneProjectMutation(const std::string& name, int mode) {
        // Core owns validation and busy admission atomically. Do not run the editor mutation barrier
        // first: it can commit a brush, reconcile History or defer an intent even when this call fails.
        const bool ok = mode == 0 ? core::LoadProject(name, false) :
                        mode == 1 ? core::ReloadProject(name) :
                        mode == 2 ? core::LoadProject(name, true) :
                        mode == 3 ? core::ClearScene() : core::UnloadProject(core::ProjectId(name));
        if (!ok) { g_projectStatus = core::ProjectError(); Note("%s", ProjectUiStatus(g_projectStatus.c_str())); return false; }
        if (mode == 3 || (mode == 4 && core::FileNameEqual(core::EditingProject(), name))) core::SetEditingProject("");
        FinishProjectSceneMutation();
        g_projectStatus = mode < 3 ? "load-admitted" : mode == 3 ? "scene cleared" : "unloaded";
        Note("%s: %s", ProjectUiStatus(g_projectStatus.c_str()), name.c_str());
        if (mode < 3) {
            // Admission is not native attachment; report the actual object/NPC queue components.
            const auto report = core::ProjectLoadReportFor(name);
            Note("%s: objects %d requested / %d queued / %d excluded; NPCs %d requested / %d queued / %d excluded; terrain %d",
                name.c_str(), report.requested, report.queued, report.excluded,
                report.requestedNpcs, report.queuedNpcs, report.excludedNpcs, report.terrainStrokes);
        }
        return true;
    }
    static bool LoadProjectAction(const std::string& name, bool clearFirst) { return SceneProjectMutation(name, clearFirst ? 2 : 0); }
    static const char* ProjectInPlaceSaveError(const std::string& name) {
        if (name.find_first_not_of(" \t") == 0 && name.find_last_not_of(" \t") == name.size() - 1) return nullptr;
        return T("Save refused: this filename has surrounding spaces or tabs. Use Project Save with an unpadded name and approve any overwrite.");
    }
    static bool SameSavedFile(const core::SavedFile& a, const core::SavedFile& b) {
        return a.kind == b.kind && a.archived == b.archived && a.filename == b.filename && a.path == b.path;
    }
    static void RefreshProjectLibrary() {
        if (!g_projectRefresh) return;
        g_projectRefresh = false; g_badSavedThumbnailStamps.clear(); g_libraryResult = core::RefreshSavedLibrary(g_projectLibrary);
        if (!g_libraryResult.ok()) return;
        g_projectAutoload = core::Autoload(); g_libraryFilterDirty = true;
        if (std::none_of(g_projectLibrary.entries.begin(), g_projectLibrary.entries.end(), [](const auto& row) { return SameSavedFile(row.file, g_librarySelected); })) g_librarySelected = {};
    }
    static std::string ProjectFilePath(const ProjectFile& file) {
        if (!file.path.empty()) return file.path;
        const bool group = file.kind == proj_codec::Kind::Group;
        return core::ModDir() + (group ? "\\groups\\" : "\\projects\\") + (file.archived ? ".archive\\" : "") + file.name + (group ? ".cdgroup" : ".cdproj");
    }
    static bool ReadProjectFile(const ProjectFile& file, proj_codec::Document& doc, bool reportError = true) {
        const std::string path = ProjectFilePath(file); std::ifstream in(path, std::ios::binary);
        if (!in) { if (reportError) g_projectStatus = "cannot-read"; return false; }
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (in.bad()) { if (reportError) g_projectStatus = "cannot-read"; return false; }
        std::vector<proj_codec::EngineRow> rows;
        std::string error;
        const bool valid = proj_codec::Parse(bytes, path, file.kind, doc, error) && proj_codec::NarrowForEngine(doc, rows, error);
        if (!valid && reportError) g_projectStatus = error;
        return valid;
    }
    static std::string SavedThumbnailPath(const ProjectFile& file) {
        std::string path = ProjectFilePath(file);
        if (path.size() >= 8 && _stricmp(path.c_str() + path.size() - 8, ".cdgroup") == 0) path.resize(path.size() - 8);
        else if (path.size() >= 7 && _stricmp(path.c_str() + path.size() - 7, ".cdproj") == 0) path.resize(path.size() - 7);
        return path + ".png";
    }
    static bool GenerateSavedThumbnail(const ProjectFile& file, const proj_codec::Document& doc) {
        if (doc.records.empty() && doc.npcs.empty() && doc.terrain.empty()) return false;
        const std::string png = SavedThumbnailPath(file);
        thumbgen::RequestDocumentThumbnail(doc, png);
        g_pendingSavedThumbnailTicks[png] = GetTickCount();
        return true;
    }
    static void EnsureSavedThumbnail(const ProjectFile& file) {
        const std::string path = SavedThumbnailPath(file), source = ProjectFilePath(file);
        WIN32_FILE_ATTRIBUTE_DATA thumbInfo{}, sourceInfo{};
        if (!GetFileAttributesExA(source.c_str(), GetFileExInfoStandard, &sourceInfo) ||
            (sourceInfo.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return;
        const bool haveThumb = GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &thumbInfo) &&
            !(thumbInfo.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
        if (thumbgen::DocumentThumbnailPending(path)) return;
        if (haveThumb && !g_currentSavedThumbnails.count(path)) {
            int w = 0, h = 0, comp = 0; unsigned char* old = stbi_load(path.c_str(), &w, &h, &comp, 4);
            if (old && w == 320 && h == 240 && old[3] == 0) g_currentSavedThumbnails.insert(path);
            if (old) stbi_image_free(old);
        }
        const auto pending = g_pendingSavedThumbnailTicks.find(path);
        if (pending != g_pendingSavedThumbnailTicks.end() && GetTickCount() - pending->second < 2000) return;
        if (haveThumb && g_currentSavedThumbnails.count(path) &&
            CompareFileTime(&thumbInfo.ftLastWriteTime, &sourceInfo.ftLastWriteTime) >= 0) return;
        const ULONGLONG stamp = ((ULONGLONG)sourceInfo.ftLastWriteTime.dwHighDateTime << 32) | sourceInfo.ftLastWriteTime.dwLowDateTime;
        if (const auto it = g_badSavedThumbnailStamps.find(source); it != g_badSavedThumbnailStamps.end() && it->second == stamp) return;
        proj_codec::Document doc;
        if (!ReadProjectFile(file, doc, false) || !GenerateSavedThumbnail(file, doc)) g_badSavedThumbnailStamps[source] = stamp;
        else g_badSavedThumbnailStamps.erase(source);
    }
    static bool SaveProjectAction(const std::string& rawName, int scope, bool approved) {
        const bool inPlace = scope == core::SaveProjectOnly || scope == core::SaveProjectAndNew;
        if (inPlace) if (const char* error = ProjectInPlaceSaveError(rawName)) { g_projectStatus = error; return false; }
        const size_t first = rawName.find_first_not_of(" \t"), last = rawName.find_last_not_of(" \t");
        if (first == std::string::npos) { g_projectStatus = "invalid-name"; return false; }
        const std::string name = rawName.substr(first, last - first + 1);
        if (!approved && GetFileAttributesA(ProjectFilePath({ name }).c_str()) != INVALID_FILE_ATTRIBUTES) {
            g_projectOverwriteName = name; g_projectOverwriteScope = scope; g_projectStatus = "overwrite-approval-required"; return false;
        }
        if (g_projectCommandPending || g_place.active || g_numericSceneUid || g_numericNpcUid) { g_projectStatus = "finish-edit-before-save"; return false; }
        if (!PrepareHistoryMutation([name, scope, approved]() { SaveProjectAction(name, scope, approved); })) return false;
        const bool ok = core::SaveProject(name, scope); g_projectStatus = ok ? "saved" : core::ProjectError();
        if (ok) {
            g_projectRefresh = true;
            if (!inPlace) strncpy_s(g_projName, name.c_str(), _TRUNCATE);
            const ProjectFile file{ name, proj_codec::Kind::Project };
            proj_codec::Document doc;
            if (ReadProjectFile(file, doc, false)) GenerateSavedThumbnail(file, doc);
        }
        return ok;
    }
    static void PublishProjectContext() {
        core::ExportContext c; c.name = g_blueprintName; c.placing = g_place.active;
        // Empty publication invalidates object-only approvals when a mixed selection appears.
        if (g_managedNpcSel.empty() && !(g_place.active && g_place.hasNpc)) c.selection.assign(g_sel.begin(), g_sel.end());
        if (g_place.active) for (const auto& m : g_place.m) if (!m.npc) c.carried.push_back(m.uid);
        core::PublishExportContext(c);
    }
    static void ResetExportApproval() { g_exportApproval = {}; g_exportObjects.clear(); g_exportStatus.clear(); g_exportAttempt = 0; }
    static std::vector<SpawnedObj> ExportObjects() {
        auto all = core::Spawned(); all.erase(std::remove_if(all.begin(), all.end(), [](const auto& o) { return !g_sel.count(o.uid); }), all.end()); return all;
    }
    static void RefreshExportApproval() {
        if (!g_exportApproval.valid) return;
        const auto& c = g_exportApproval.context; const auto all = ExportObjects();
        bool same = g_managedNpcSel.empty() && !(g_place.active && g_place.hasNpc) && c.name == g_blueprintName && c.selection == SelUids() &&
            c.placing == g_place.active && g_exportApproval.overwriteApproved == g_exportOverwrite && all.size() == g_exportObjects.size();
        std::vector<int> carried; if (g_place.active) for (const auto& m : g_place.m) if (!m.npc) carried.push_back(m.uid);
        same &= c.carried == carried;
        if (same) for (size_t i = 0; i < all.size(); ++i) {
            const auto& a = all[i]; const auto& b = g_exportObjects[i];
            if (a.uid != b.uid || a.gen != b.gen || a.poseGen != b.poseGen || a.hidden != b.hidden || a.proj != b.proj || a.group != b.group || a.note != b.note) { same = false; break; }
        }
        if (!same) { g_exportApproval.valid = false; g_exportStatus = "preflight-changed"; }
    }
    static bool ProjectExportPreflight() {
        PublishProjectContext();
        if (!g_managedNpcSel.empty() || (g_place.active && g_place.hasNpc)) { ResetExportApproval(); g_exportStatus = "Export supports objects only; NPC and mixed selections were not changed."; return false; }
        const bool ok = core::PrepareGroupExport(SelUids(), g_blueprintName, g_exportOverwrite, g_exportApproval);
        g_exportObjects = ExportObjects(); g_exportStatus = ok ? "preflight-ready" : g_exportApproval.error;
        g_exportAttempts.push_back({ g_exportApproval, {}, g_blueprintName }); g_exportAttempt = g_exportAttempts.size(); return ok;
    }
    static bool ProjectExportWrite() {
        PublishProjectContext(); RefreshExportApproval(); bool ok = false;
        if (!g_exportApproval.valid) g_exportStatus = "preflight-changed";
        else ok = core::WriteGroupExport(g_exportApproval, g_exportStatus);
        if (ok) { g_exportStatus = "written"; GenerateSavedThumbnail({ g_blueprintName, proj_codec::Kind::Group, false, g_exportApproval.path }, g_exportApproval.document); g_projectRefresh = true; g_exportOpen = false; }
        if (g_exportAttempt) g_exportAttempts[g_exportAttempt - 1].result = g_exportStatus;
        g_exportApproval.valid = false; g_exportObjects.clear(); g_exportAttempt = 0; return ok;
    }
    static bool DispatchProjectAction(const ProjectFile& file, ProjectAction action, bool on = false, const Vec3* dropTarget = nullptr) {
        const bool group = file.kind == proj_codec::Kind::Group;
        if (file.archived && action != ProjectAction::Read) { g_projectStatus = "invalid-action"; return false; }
        const bool groupAction = action == ProjectAction::Place || action == ProjectAction::Overwrite;
        if (action != ProjectAction::Read && group != groupAction) { g_projectStatus = "wrong-kind"; return false; }
        if (action == ProjectAction::Autoload) {
            const auto result = core::SetAutoload(file.name, on); g_projectStatus = core::FileReasonCode(result.reason);
            if (result.ok()) g_projectAutoload = core::Autoload(); return result.ok();
        }
        if (action == ProjectAction::Save || action == ProjectAction::AddUnassigned || action == ProjectAction::SaveBack)
            return SaveProjectAction(file.name, action == ProjectAction::Save ? core::SaveProjectOnly : core::SaveProjectAndNew, true);
        if (action == ProjectAction::Unload) return SceneProjectMutation(file.name, 4);
        if (action == ProjectAction::Load) return LoadProjectAction(file.name, false);
        if (action == ProjectAction::Replace) return LoadProjectAction(file.name, true);
        if (action == ProjectAction::Reload) return SceneProjectMutation(file.name, 1);
        proj_codec::Document doc; if (action == ProjectAction::Read) g_projectReadValid = false;
        if (!ReadProjectFile(file, doc)) { g_projectFileFailures.push_back({ action == ProjectAction::Read ? "Read" : "Place", ProjectFilePath(file), g_projectStatus }); return false; }
        if (action == ProjectAction::Read) { g_projectRead = std::move(doc); g_projectReadPath = ProjectFilePath(file); g_projectReadValid = true; g_projectDetails.erase("read-preview"); return true; }
        if (action == ProjectAction::Overwrite) { strncpy_s(g_blueprintName, file.name.c_str(), _TRUNCATE); g_exportOpen = true; g_exportOverwrite = false; g_projectDetails.insert("create-blueprint"); ResetExportApproval(); PublishProjectContext(); return true; }
        if (g_place.active) {
            DropCarried();
            if (g_place.active) {
                g_pendingBlueprintPlace = { true, file, dropTarget ? *dropTarget : InFront(1, 0), GetTickCount() };
                g_projectStatus = "placement-pending";
                return true;
            }
        }
        const auto path = ProjectFilePath(file); core::SavedFile saved{ file.kind, false, path.substr(path.find_last_of("\\/") + 1), path };
        core::FileSelectionHandle selected; const auto result = core::SelectSavedFile(saved, selected);
        if (!result.ok()) { g_projectStatus = core::FileReasonCode(result.reason); return false; }
        return PlaceGroupCopy(doc, dropTarget ? *dropTarget : InFront(1, 0), 0, 1, file.name, selected);
    }
    static void PumpPendingBlueprintPlace() {
        if (!g_pendingBlueprintPlace.active || g_place.active) return;
        auto pending = g_pendingBlueprintPlace;
        g_pendingBlueprintPlace = {};
        if (GetTickCount() - pending.queuedAt < 30000)
            DispatchProjectAction(pending.file, ProjectAction::Place, false, &pending.target);
    }
    static void ProjectButtonWrap(const char* label) { SameLineOrWrap(true, ImGui::CalcTextSize(T(label)).x + ImGui::GetStyle().FramePadding.x * 2); }
    static bool ProjectDisclosure(const std::string& caption, const std::string& key) {
        ImGui::SetNextItemOpen(g_projectDetails.count(key) != 0, ImGuiCond_Always);
        std::string shown = caption; const float available = ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().FramePadding.x * 2;
        if (ImGui::CalcTextSize(shown.c_str()).x > available) {
            const float dots = ImGui::CalcTextSize("...").x;
            do { size_t end = shown.size() - 1; while (end && ((unsigned char)shown[end] & 0xc0) == 0x80) --end; shown.resize(end); }
            while (!shown.empty() && ImGui::CalcTextSize(shown.c_str()).x + dots > available);
            shown += "...";
        }
        const bool open = ImGui::CollapsingHeader((shown + "###" + key).c_str());
        if (shown != caption && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", caption.c_str());
        if (open) g_projectDetails.insert(key); else g_projectDetails.erase(key); return open;
    }
    struct LibraryFileAction {
        core::FileSelectionHandle selection; core::FileAction action = core::FileAction::Archive;
        core::FileResult result;
    };
    static LibraryFileAction g_libraryAction;
    static core::FileReason GuardLibraryFile(const core::SavedFile& file) {
        using R = core::FileReason;
        if (!SameSavedFile(file, g_librarySelected)) return R::SelectionChanged;
        const auto kind = file.kind == proj_codec::Kind::Group ? core::SavedLibraryKind::Groups : core::SavedLibraryKind::Projects;
        const auto rows = core::FilterSavedLibrary(g_projectLibrary, kind, (core::SavedLibraryLocation)g_libraryLocation, g_librarySearch);
        if (std::none_of(rows.begin(), rows.end(), [&](size_t i) { return SameSavedFile(g_projectLibrary.entries[i].file, file); })) return R::SelectionChanged;
        if (g_deferredEditor.action || g_projectCommandPending || g_metadataPopupRequest || g_metadataEditing) return R::PendingOperation;
        const bool group = file.kind == proj_codec::Kind::Group;
        const std::string stem = file.filename.substr(0, file.filename.size() - (group ? 8 : 7));
        if (group) {
            const std::string name = g_blueprintName; const auto first = name.find_first_not_of(" \t"), last = name.find_last_not_of(" \t");
            if (g_exportOpen && first != std::string::npos && core::FileNameEqual(name.substr(first, last - first + 1), stem)) return R::InFlightExport;
            for (const auto& report : g_projectPlacements) {
                const auto source = core::SelectedFile(report.source);
                const bool matches = report.source ? core::FileNameEqual(source.filename, file.filename) : core::FileNameEqual(report.name, stem);
                if (matches && (!core::PlaceRequestState(report.request).settled || (g_place.active && g_place.req == report.request))) return R::InFlightPlace;
            }
            return R::None;
        }
        if (core::FileNameEqual(g_projectOverwriteName, stem) || BrushMutationPending()) return R::PendingOperation;
        const auto objects = core::Spawned(); const auto npcs = core::ManagedNpcs();
        const auto refers = [&](int pid) { return pid > 0 && core::FileNameEqual(core::ProjectNameOf(pid), stem); };
        const auto uidRefers = [&](int uid, bool npc) {
            if (npc) { const auto* n = FindManagedNpc(npcs, uid); return n && refers(n->proj); }
            const auto* o = Find(objects, uid); return o && refers(o->proj);
        };
        for (int redo = 0; redo < 2; ++redo) for (const auto& entry : redo ? g_redo : g_undo) for (const auto& a : entry.acts) {
            bool used = refers(a.proj);
            if (a.kind != Act::GroupName && a.kind != Act::TerrainBatch && a.kind != Act::TerrainClear) used |= uidRefers(a.uid, NpcAct(a.kind));
            for (int pid : a.projects) used |= refers(pid);
            for (const auto& t : a.terrain) used |= refers(t.proj);
            if (used) return redo ? R::RedoReference : R::UndoReference;
        }
        if (uidRefers(g_numericSceneUid, false) || uidRefers(g_numericNpcUid, true)) return R::PendingOperation;
        for (const auto& op : g_pendingGround) for (const auto& m : core::GroundStateOf(op).members)
            if (refers(m.before.proj) || refers(m.after.proj) || uidRefers(m.before.uid, false)) return R::PendingOperation;
        for (const auto& report : g_projectPlacements) {
            const auto v = core::PlaceRequestState(report.request);
            if (!v.settled) for (const auto& r : v.rows) if (uidRefers(r.uid, false)) return R::PendingOperation;
        }
        if (g_place.active) for (const auto& m : g_place.m) if (uidRefers(m.uid, m.npc)) return R::PendingOperation;
        return R::None;
    }
    static bool ExecuteLibraryAction() {
        const core::SavedFile file = core::SelectedFile(g_libraryAction.selection);
        g_libraryAction.result = core::ExecuteFileAction(g_libraryAction.selection, g_libraryAction.action, file.filename, GuardLibraryFile);
        if (!g_libraryAction.result.ok()) return false;
        {
            const size_t extension = file.kind == proj_codec::Kind::Group ? 8 : 7;
            const ProjectFile saved{ file.filename.substr(0, file.filename.size() - extension), file.kind, file.archived, file.path };
            const std::string thumbnail = SavedThumbnailPath(saved);
            if (!DeleteFileA(thumbnail.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
                core::Log("[library] could not remove obsolete thumbnail %s (error %lu)", thumbnail.c_str(), GetLastError());
            overlay::InvalidateThumb(thumbnail);
            g_badSavedThumbnailStamps.erase(file.path);
            g_pendingSavedThumbnailTicks.erase(thumbnail);
            g_currentSavedThumbnails.erase(thumbnail);
            if (g_projectReadPath == file.path) g_projectReadValid = false;
        }
        g_projectRefresh = true; g_projectStatus.clear(); return true;
    }
    static void BeginLibraryAction(const core::SavedFile& file, core::FileAction action) {
        g_libraryAction = {}; g_libraryAction.action = action; g_projectStatus.clear();
        g_libraryAction.result = core::SelectSavedFile(file, g_libraryAction.selection);
        if (!g_libraryAction.result.ok()) return;
        ExecuteLibraryAction();
    }
    static void DrawLibraryFailure() {
        const auto& r = g_libraryAction.result; if (r.ok()) return;
        ImGui::TextWrapped("%s: %s (%lu)", T("File action failed"), ProjectUiStatus(core::FileReasonCode(r.reason)), r.systemError);
        if (r.reason == core::FileReason::AutoloadEnabled) ImGui::TextWrapped("%s", T("Turn autoload OFF explicitly before changing this file."));
        else ImGui::TextWrapped("%s", T("The file action failed. Check file access, then refresh and try again."));
    }
    static void DrawSavedLibrary(bool blueprints) {
        if (g_libraryShowingBlueprints != blueprints) {
            g_libraryShowingBlueprints = blueprints;
            g_libraryFilterDirty = true;
        }
        const bool groupKind = blueprints;
        const auto kind = groupKind ? core::SavedLibraryKind::Groups : core::SavedLibraryKind::Projects;
        ImGui::SeparatorText(T(blueprints ? "Blueprint library" : "Project library"));
        const float filterWidth = ImGui::GetContentRegionAvail().x;
        const float locationWidth = std::min(150.0f, filterWidth * 0.34f);
        ImGui::SetNextItemWidth(std::max(ImGui::GetFrameHeight(), filterWidth - locationWidth - ImGui::GetStyle().ItemSpacing.x));
        if (InputTextI18n("##library-search", T("Search saved files"), g_librarySearch, sizeof g_librarySearch)) g_libraryFilterDirty = true;
        const char* locations[] = { "Active", "Archived", "All locations" };
        SameLineOrWrap(true, locationWidth); ImGui::SetNextItemWidth(locationWidth); if (ComboT("##library-location", &g_libraryLocation, locations, 3)) g_libraryFilterDirty = true;
        if (g_libraryFilterDirty) { g_libraryRows = core::FilterSavedLibrary(g_projectLibrary, kind, (core::SavedLibraryLocation)g_libraryLocation, g_librarySearch); g_libraryFilterDirty = false; }
        const size_t activeCount = groupKind ? g_projectLibrary.active.groups : g_projectLibrary.active.projects;
        const size_t archiveCount = groupKind ? g_projectLibrary.archived.groups : g_projectLibrary.archived.projects;
        ImGui::TextDisabled(T("%zu active / %zu archived / %zu shown"), activeCount, archiveCount, g_libraryRows.size());
        if (!g_libraryResult.ok()) ImGui::TextWrapped("%s: %s (%lu)", T("Library refresh failed"), ProjectUiStatus(core::FileReasonCode(g_libraryResult.reason)), g_libraryResult.systemError);
        if (g_libraryRows.empty()) ImGui::TextDisabled("%s", T("No matching saved files"));
        else {
            const float ui = ImGui::GetFontSize() / 17.0f;
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const float cardWidth = g_cardSize * ui + 8.0f * ui;
            const float imageHeight = g_cardSize * ui * 0.75f;
            const float cardHeight = imageHeight + ImGui::GetTextLineHeightWithSpacing() + 10.0f;
            const float available = std::max(1.0f, ImGui::GetContentRegionAvail().x - 2.0f * ImGui::GetStyle().WindowPadding.x);
            const int estimatedColumns = CardGridColumns(available, cardWidth, spacing);
            const int rows = ((int)g_libraryRows.size() + estimatedColumns - 1) / estimatedColumns;
            if (ImGui::BeginChild("saved-gallery", ImVec2(0, std::min(350.0f, rows * (cardHeight + spacing) + 12.0f)), ImGuiChildFlags_Borders)) {
                const int columns = CardGridColumns(ImGui::GetContentRegionAvail().x, cardWidth, spacing);
                for (size_t n = 0; n < g_libraryRows.size(); ++n) {
                    const auto& row = g_projectLibrary.entries[g_libraryRows[n]];
                    const auto& saved = row.file;
                    const ProjectFile file{ saved.filename.substr(0, saved.filename.size() - (groupKind ? 8 : 7)), saved.kind, saved.archived, saved.path };
                    EnsureSavedThumbnail(file);
                    ImGui::PushID(saved.path.c_str());
                    const ImVec2 start = ImGui::GetCursorScreenPos();
                    const bool selected = SameSavedFile(saved, g_librarySelected);
                    if (ImGui::InvisibleButton("##saved-card", ImVec2(cardWidth, cardHeight))) g_librarySelected = saved;
                    const bool hovered = ImGui::IsItemHovered();
                    if (groupKind && !saved.archived && hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        DispatchProjectAction(file, ProjectAction::Place);
                    if (groupKind && !saved.archived && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.0f)) {
                        g_blueprintDragFile = file; g_blueprintDragging = true;
                    }
                    if (groupKind && ImGui::BeginPopupContextItem("##blueprint-actions")) {
                        g_librarySelected = saved;
                        if (!saved.archived) {
                            if (ImGui::MenuItem(T("Place"))) DispatchProjectAction(file, ProjectAction::Place);
                            ImGui::Separator();
                            if (ImGui::MenuItem(T("Archive"))) BeginLibraryAction(saved, core::FileAction::Archive);
                            if (ImGui::MenuItem(T("Delete"))) BeginLibraryAction(saved, core::FileAction::Delete);
                        } else {
                            if (ImGui::MenuItem(T("Restore"))) BeginLibraryAction(saved, core::FileAction::Restore);
                            if (ImGui::MenuItem(T("Delete"))) BeginLibraryAction(saved, core::FileAction::Purge);
                        }
                        ImGui::EndPopup();
                    }
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    const ImVec2 end{ start.x + cardWidth, start.y + cardHeight };
                    dl->AddRectFilled(start, end, ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0f);
                    const ImVec2 imageMin{ start.x + 4.0f, start.y + 4.0f };
                    const ImVec2 imageMax{ end.x - 4.0f, start.y + imageHeight + 4.0f };
                    if (ImTextureID image = overlay::Thumb(SavedThumbnailPath(file)))
                        dl->AddImage(image, imageMin, imageMax);
                    else {
                        dl->AddRectFilled(imageMin, imageMax, ImGui::GetColorU32(ImGuiCol_FrameBgHovered), 3.0f);
                        const char* icon = groupKind ? ICON_CUBE : ICON_FLOPPY_DISK;
                        const ImVec2 size = ImGui::CalcTextSize(icon);
                        dl->AddText(ImVec2((imageMin.x + imageMax.x - size.x) * 0.5f, (imageMin.y + imageMax.y - size.y) * 0.5f),
                            ImGui::GetColorU32(ImGuiCol_TextDisabled), icon);
                    }
                    dl->PushClipRect(ImVec2(start.x + 4, start.y + imageHeight + 4), ImVec2(end.x - 4, end.y - 2), true);
                    const std::string label = file.name + (row.ownership.dirty ? " *" : "") + (saved.archived ? " [A]" : "");
                    dl->AddText(ImVec2(start.x + 5, start.y + imageHeight + 6), ImGui::GetColorU32(ImGuiCol_Text), label.c_str());
                    dl->PopClipRect();
                    dl->AddRect(start, end, ImGui::GetColorU32(selected ? ImGuiCol_HeaderActive : ImGuiCol_Border), 4.0f, 0, selected ? 2.0f : 1.0f);
                    if (hovered) ImGui::SetTooltip("%s", saved.path.c_str());
                    ImGui::PopID();
                    if ((int)(n % columns) != columns - 1 && n + 1 < g_libraryRows.size()) ImGui::SameLine();
                }
            }
            ImGui::EndChild();
        }
        if (!groupKind) for (size_t index : g_libraryRows) {
            const auto row = g_projectLibrary.entries[index]; const auto& saved = row.file;
            if (!SameSavedFile(saved, g_librarySelected)) continue;
            const ProjectFile file{ saved.filename.substr(0, saved.filename.size() - 7), saved.kind, saved.archived, saved.path };
            const auto& o = row.ownership;
            ImGui::TextDisabled("%s: %zu / %s: %zu / %s: %zu", T("Object"), o.visible, T("NPC"), o.visibleNpcs, T("Terrain"), o.terrain);
            ImGui::TextDisabled(T("Hidden objects: %zu / hidden NPCs: %zu / pending: %zu"), o.hidden, o.hiddenNpcs, o.pendingObjects + o.pendingNpcs);
            ImGui::PushID("project"); ImGui::PushID(file.name.c_str());
            if (ImGui::Button(T("Read"))) DispatchProjectAction(file, ProjectAction::Read);
            if (!saved.archived) {
                const bool editing = core::FileNameEqual(core::EditingProject(), file.name);
                if (ImGui::RadioButton(T("Edit project"), editing)) {
                    if (core::SetEditingProject(file.name)) { ClearSceneSelection(); g_projectRefresh = true; }
                    else g_projectStatus = core::ProjectError();
                }
                ProjectButtonWrap("Load project"); bool loaded = core::IsProjectLoaded(file.name);
                ImGui::BeginDisabled(g_projectCommandPending || (editing && loaded));
                if (ImGui::Checkbox(T("Load project"), &loaded))
                    DispatchProjectAction(file, loaded ? ProjectAction::Load : ProjectAction::Unload);
                ImGui::EndDisabled();
                ProjectButtonWrap("Reload"); ImGui::BeginDisabled(!loaded || g_projectCommandPending); if (ImGui::Button(T("Reload"))) DispatchProjectAction(file, ProjectAction::Reload); ImGui::EndDisabled();
                ProjectButtonWrap("Save"); ImGui::BeginDisabled(!loaded || !o.dirty); if (ImGui::Button(T("Save"))) DispatchProjectAction(file, ProjectAction::Save); ImGui::EndDisabled();
                const auto& unassigned = g_projectLibrary.unassigned;
                ProjectButtonWrap("Add unassigned"); ImGui::BeginDisabled(!loaded || !(unassigned.visible || unassigned.visibleNpcs || unassigned.terrain)); if (ImGui::Button(T("Add unassigned"))) DispatchProjectAction(file, ProjectAction::AddUnassigned); ImGui::EndDisabled();
                ProjectButtonWrap("autoload"); bool on = std::any_of(g_projectAutoload.begin(), g_projectAutoload.end(), [&](const auto& name) { return core::FileNameEqual(name, file.name); });
                if (ImGui::Checkbox(T("autoload"), &on)) DispatchProjectAction(file, ProjectAction::Autoload, on);
                ProjectButtonWrap("Archive"); if (ImGui::Button(T("Archive"))) BeginLibraryAction(saved, core::FileAction::Archive);
                ProjectButtonWrap("Delete"); if (ImGui::Button(T("Delete"))) BeginLibraryAction(saved, core::FileAction::Delete);
            } else {
                ProjectButtonWrap("Restore"); if (ImGui::Button(T("Restore"))) BeginLibraryAction(saved, core::FileAction::Restore);
                ProjectButtonWrap("Delete"); if (ImGui::Button(T("Delete"))) BeginLibraryAction(saved, core::FileAction::Purge);
            }
            ImGui::PopID(); ImGui::PopID(); break;
        }
        DrawLibraryFailure();
    }
    static void DrawPlacementReport(const report_projection::Placement& report) {
        const auto& v = report.receipt; const std::string key = "placement-" + std::to_string(report.id); char summary[256];
        snprintf(summary, sizeof summary, "#%llu%s %s | %d %s", (unsigned long long)report.id, report.attention() ? " !" : "", T(v.cleanupPending ? "Cleanup" : report.carrying ? "Carried" : report.final() ? "Final" : "Pending"), v.pending ? v.pending : v.attached, T(v.pending ? "pending" : "attached"));
        const bool open = ProjectDisclosure(summary, key); ImGui::PushID(key.c_str());
        if (!v.requestCanceled && report.active() && ImGui::Button(T("Cancel placement"))) CancelProjectPlacement(report.request);
        if (open) {
            ImGui::TextWrapped("%s", report.name.c_str());
            ImGui::TextWrapped(T("requested %d / attached %d / excluded %d / failed %d / canceled %d / pending %d"), v.requested, v.attached, report.displayedExcluded(), v.failed, v.canceled, v.pending);
            ImGui::TextWrapped("%s", T("Attached during request; not current survivors."));
            if (report.approximate) ImGui::TextWrapped("%s", T("Approximate bounds: unknown geometry uses a 2 m cube."));
            static const char* states[] = { "Pending", "Attached", "Excluded", "Failed", "Canceled" };
            for (const auto& row : v.rows) { ImGui::TextWrapped("#%d / UID %d: %s", row.rowId, row.uid, T(states[row.state])); ImGui::TextWrapped("%s", row.prefab.c_str()); if (!row.reason.empty()) ImGui::TextWrapped("%s", row.reason.c_str());
                if (row.cleanupPending) ImGui::TextWrapped("%s", T("Cancellation cleanup pending")); if (row.removedAfterAttach) ImGui::TextWrapped("%s", T("Removed after attachment")); }
        }
        ImGui::PopID();
    }
    static void DrawGroundReport(const report_projection::Ground& report) {
        const auto& v = report.receipt; char summary[256];
        snprintf(summary, sizeof summary, "#%llu%s %s | %zu %s", (unsigned long long)v.id, report.attention() ? " !" : "", T(report.final() ? "Final" : "Pending"), report.pending ? report.pending : report.accepted, T(report.pending ? "pending" : "accepted"));
        if (!ProjectDisclosure(summary, "ground-" + std::to_string(v.id))) return;
        ImGui::TextWrapped(T("accepted %zu / not applied %zu / pending %zu"), report.accepted, report.notApplied, report.pending);
        if (!v.reason.empty()) ImGui::TextWrapped("%s", v.reason.c_str());
        for (const auto& row : v.members) { ImGui::TextWrapped("UID %d: %s", row.before.uid, T(!row.terminal ? "Pending" : row.accepted ? "Accepted" : "Not applied")); ImGui::TextWrapped("%s", row.before.prefab.c_str()); if (!row.reason.empty()) ImGui::TextWrapped("%s", row.reason.c_str()); }
    }
    static bool EarlierReports(const report_projection::Order& order, const char* key) {
        if (order.earlierCompleted.empty()) return false;
        char caption[256]; snprintf(caption, sizeof caption, "%zu / !%zu | %s", order.earlierCompleted.size(), order.earlierAttentionCount, T("Earlier reports")); return ProjectDisclosure(caption, key);
    }
    static void DrawProjectReports() {
        const auto placements = g_placementReports.Snapshot(g_place.active ? g_place.req : core::PlaceRequestHandle{}); const auto grounds = g_groundReports.Snapshot();
        if (!placements.reports.empty()) {
            ImGui::SeparatorText(T("Placement reports")); DrawPlacementReport(placements.reports.back());
            for (const auto& r : placements.reports) if (r.id != placements.order.newest && r.active()) DrawPlacementReport(r);
            if (EarlierReports(placements.order, "placement-earlier")) for (const auto& r : placements.reports) if (r.id != placements.order.newest && !r.active()) DrawPlacementReport(r);
        }
        if (!grounds.reports.empty()) {
            ImGui::SeparatorText(T("Grounding reports")); DrawGroundReport(grounds.reports.back());
            for (const auto& r : grounds.reports) if (r.receipt.id != grounds.order.newest && r.active()) DrawGroundReport(r);
            if (EarlierReports(grounds.order, "ground-earlier")) for (const auto& r : grounds.reports) if (r.receipt.id != grounds.order.newest && !r.active()) DrawGroundReport(r);
        }
    }
    static void DrawProjectGrounding() {
        ImGui::RadioButton(T("Rigid"), &g_groundMode, 0); ProjectButtonWrap("Per-object"); ImGui::RadioButton(T("Per-object"), &g_groundMode, 1);
        ProjectButtonWrap("Start grounding"); if (ImGui::Button(T("Start grounding"))) {
            if (g_place.active && g_groundMode == 1) g_projectStatus = "drop-before-per-object";
            else { const auto c = GroundPlacementOf(g_place); BeginGrounding(c.generation ? c.members : SelUids(), g_groundMode == 0, c); }
        }
        ProjectButtonWrap("Cancel grounding"); if (ImGui::Button(T("Cancel grounding"))) for (const auto& op : g_pendingGround) core::GroundCancel(op);
        if (g_deferredEditor.action) ImGui::TextWrapped("%s", T("Mutation pending; read-only controls remain available."));
    }
    static void DrawExportAttempts() {
        if (g_exportAttempts.empty() || !ProjectDisclosure(std::string(T("Export attempts")) + " (" + std::to_string(g_exportAttempts.size()) + ")", "export-attempts")) return;
        for (size_t i = 0; i < g_exportAttempts.size(); ++i) {
            const auto& attempt = g_exportAttempts[i]; const auto& a = attempt.approval;
            if (!ProjectDisclosure("#" + std::to_string(i + 1) + " " + T("Preflight"), "export-" + std::to_string(i + 1))) continue;
            ImGui::TextWrapped("%s", a.path.empty() ? attempt.name.c_str() : a.path.c_str()); ImGui::TextWrapped(T("included %d / excluded %d"), (int)a.included.size(), (int)a.excluded.size());
            if (!a.error.empty()) ImGui::TextWrapped("%s", a.error.c_str()); if (!attempt.result.empty()) ImGui::TextWrapped("%s", attempt.result.c_str());
            for (const auto& ex : a.excluded) { ImGui::TextWrapped("UID %d: %s", ex.uid, ex.prefab.c_str()); ImGui::TextWrapped("%s", ex.reason.c_str()); }
        }
    }
    static void DrawProjectExtras() {
        if (!g_projectOverwriteName.empty()) {
            ImGui::TextWrapped("%s: %s.cdproj", T("Overwrite project"), g_projectOverwriteName.c_str());
            if (ImGui::Button(T("Approve project overwrite"))) { const auto name = g_projectOverwriteName; const int scope = g_projectOverwriteScope; g_projectOverwriteName.clear(); SaveProjectAction(name, scope, true); }
            ProjectButtonWrap("Cancel overwrite"); if (ImGui::Button(T("Cancel overwrite"))) g_projectOverwriteName.clear();
        }
        if (ImGui::Button(T("Refresh"))) g_projectRefresh = true;
        if (!g_projectStatus.empty()) ImGui::TextWrapped("%s", ProjectUiStatus(g_projectStatus.c_str()));
        if (!g_projectFileFailures.empty() && ProjectDisclosure(std::string(T("File attempt details")) + " (! " + std::to_string(g_projectFileFailures.size()) + ")", "file-attempts"))
            for (const auto& f : g_projectFileFailures) { ImGui::TextWrapped("%s: %s", T(f.action.c_str()), f.path.c_str()); ImGui::TextWrapped("%s", ProjectUiStatus(f.reason.c_str())); }
        if (g_projectReadValid && ProjectDisclosure(std::string(T("Read preview")) + ": " + g_projectReadPath.substr(g_projectReadPath.find_last_of("\\/") + 1), "read-preview")) {
            const auto& d = g_projectRead; ImGui::TextWrapped("%s", g_projectReadPath.c_str());
            ImGui::TextWrapped("%s: %zu / %s: %zu", T("Object"), d.records.size(), T("NPC"), d.npcs.size()); ImGui::TextWrapped(T("Terrain strokes in file: %zu"), d.terrain.size());
            for (size_t i = 0; i < d.records.size(); ++i) { const auto& r = d.records[i]; ImGui::TextWrapped("#%zu: %s", i + 1, r.prefab.c_str()); ImGui::TextWrapped("%.6f %.6f %.6f | %.5f %.5f %.5f | x%.6f | %s %d", r.pos.x, r.pos.y, r.pos.z, r.yaw, r.pitch, r.roll, r.scale, T("Group"), r.group); if (!r.note.empty()) ImGui::TextWrapped("%s: %s", T("Note"), r.note.c_str()); }
            for (const auto& n : d.npcs) { ImGui::TextWrapped("%s %u: %s", T("NPC"), n.key, n.label.c_str()); ImGui::TextWrapped("%.6f %.6f %.6f | %s %d | %s %u | %s %d | %s %d | %s %d", n.pos.x, n.pos.y, n.pos.z, T("type"), n.type, T("extra"), n.extra, T("AI"), n.aiEnabled ? 1 : 0, T("Behavior"), n.behavior, T("Group"), n.group); if (!n.note.empty()) ImGui::TextWrapped("%s: %s", T("Note"), n.note.c_str()); }
            for (const auto& g : d.groupNames) ImGui::TextWrapped("%s %d: %s", T("Group"), g.first, g.second.c_str());
            for (const auto& t : d.terrain) ImGui::TextWrapped("%s %d: %.3f %.3f r%.3f | %.3f %.3f | %.3f %.3f y%.3f", T("Terrain"), t.mode, t.x, t.z, t.r, t.amount, t.strength, t.ax, t.az, t.y);
            if (d.hasBounds) ImGui::TextWrapped("%s: %.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f%s", T("Bounds"), d.bounds.anchor.x, d.bounds.anchor.y, d.bounds.anchor.z, d.bounds.min.x, d.bounds.min.y, d.bounds.min.z, d.bounds.max.x, d.bounds.max.y, d.bounds.max.z, d.bounds.approximate ? " ~" : "");
        }
    }
    static void DrawProjectsPage() {
        auto& name = g_projName;
        const std::string editing = core::EditingProject();
        ImGui::Text("%s: %s", T("Editing project"), editing.empty() ? T("None") : editing.c_str());
        bool autosave = core::g_projectAutoSave;
        if (ImGui::Checkbox("##project-autosave", &autosave)) { core::g_projectAutoSave = autosave; core::SaveSettings(); }
        SameLineForControl("Auto-save");
        ImGui::PushStyleColor(ImGuiCol_Text, autosave ? ImVec4(0.45f, 0.9f, 0.55f, 1) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::Text("%s: %s", T("Auto-save"), T(autosave ? "ON" : "OFF")); ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("New objects, NPCs and terrain are saved to their editing project."));
        if (ProjectDisclosure(T("Create project"), "create-project")) {
            SetLabeledItemWidth("project name", 260); InputTextI18n(TStable("project name"), "", name, sizeof name);
            ImGui::BeginDisabled(!name[0]);
            if (ImGui::Button(T("Create project"))) {
                const std::string project = name;
                const std::string path = core::ModDir() + "\\projects\\" + project + ".cdproj";
                if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) g_projectStatus = "Collision";
                else if (core::SaveProject(project, core::SaveProjectOnly) && core::SetEditingProject(project)) {
                    name[0] = 0; g_projectRefresh = true; ClearSceneSelection(); g_projectStatus = "saved";
                } else g_projectStatus = core::ProjectError();
            }
            ImGui::EndDisabled();
        }
        ProjectButtonWrap("Import project file");
        if (ImGui::Button(T("Import project file"))) {
            char file[MAX_PATH] = { 0 }; OPENFILENAMEA ofn = {}; ofn.lStructSize = sizeof ofn; ofn.lpstrFilter = "World Builder project (*.cdproj)\0*.cdproj\0All files\0*.*\0"; ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameA(&ofn)) { if (core::ImportProjectFile(file)) { Note(T("imported %s"), file); g_projectRefresh = true; } else { g_projectStatus = core::ProjectError(); Note(T("import failed")); } }
        }
        ProjectButtonWrap("Open project folder"); if (ImGui::Button(T("Open project folder"))) ShellExecuteA(nullptr, "open", (core::ModDir() + "\\projects").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        DrawProjectExtras();
        DrawSavedLibrary(false);
        DrawProjectReports();
    }
    static void DrawBlueprintsPage() {
        if (ProjectDisclosure(T("Create blueprint"), "create-blueprint")) {
            SetLabeledItemWidth("Blueprint name", 300);
            if (InputTextI18n(T("Blueprint name"), "", g_blueprintName, sizeof g_blueprintName)) ResetExportApproval();
            if (FittedCheckbox(T("Approve existing-file overwrite"), &g_exportOverwrite)) ResetExportApproval();
            ImGui::BeginDisabled(!g_blueprintName[0]);
            if (FittedButton(T("Write blueprint"))) { g_exportOpen = true; if (ProjectExportPreflight()) ProjectExportWrite(); }
            ImGui::EndDisabled();
            if (g_exportOpen) { ProjectButtonWrap("Cancel export"); if (ImGui::Button(T("Cancel export"))) { g_exportOpen = false; ResetExportApproval(); g_exportOverwrite = false; } }
        }
        ImGui::TextDisabled("%s", T("Blueprints are reusable object groups. They are stored separately from projects and do not load into the scene until placed."));
        ImGui::TextWrapped("%s", T("Double-click or drag a blueprint into the world to place it in the editing project."));
        DrawSavedLibrary(true);
        if (!g_exportStatus.empty()) ImGui::TextWrapped("%s: %s", T("Export status"), ProjectUiStatus(g_exportStatus.c_str()));
        ImGui::SeparatorText(T("Blueprint file"));
        if (ImGui::Button(T("Import blueprint"))) {
            char path[MAX_PATH] = {};
            OPENFILENAMEA ofn = {}; ofn.lStructSize = sizeof ofn;
            ofn.lpstrFilter = "World Builder blueprint (*.cdgroup)\0*.cdgroup\0All files\0*.*\0";
            ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH; ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameA(&ofn)) {
                if (core::ImportGroupFile(path)) {
                    const std::string importPath(path), base = importPath.substr(importPath.find_last_of("\\/") + 1);
                    const ProjectFile file{ base.substr(0, base.size() - 8), proj_codec::Kind::Group, false, core::ModDir() + "\\groups\\" + base };
                    proj_codec::Document doc; if (ReadProjectFile(file, doc, false)) GenerateSavedThumbnail(file, doc);
                    Note(T("imported %s"), importPath.c_str()); g_projectRefresh = true;
                } else { g_projectStatus = core::ProjectError(); Note(T("blueprint import failed")); }
            }
        }
        ProjectButtonWrap("Open blueprint folder");
        if (ImGui::Button(T("Open blueprint folder"))) ShellExecuteA(nullptr, "open", (core::ModDir() + "\\groups").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ProjectButtonWrap("Refresh"); if (ImGui::Button(T("Refresh"))) g_projectRefresh = true;
        if (!g_projectStatus.empty()) ImGui::TextWrapped("%s", ProjectUiStatus(g_projectStatus.c_str()));
    }
    static void DrawProject() {
        ImGui::PushOverrideID(ImHashStr("project-page"));
        RefreshProjectLibrary();
        DrawProjectsPage();
        ImGui::PopID();
    }
    static void DrawBlueprint() {
        ImGui::PushOverrideID(ImHashStr("blueprint-page"));
        RefreshProjectLibrary();
        DrawBlueprintsPage();
        ImGui::PopID();
    }

    // ---- travel: saved points (bin64\cdmodkit\teleports.txt: name|x|y|z) ----
    struct TravelPoint { std::string name; Vec3 pos; };
    static std::vector<TravelPoint> g_tp; static bool g_tpLoaded = false; static char g_tpName[48] = "";
    static std::string TpPath() { return core::ModDir() + "\\teleports.txt"; }
    static void LoadTp() { if (g_tpLoaded) return; g_tpLoaded = true; FILE* f = fopen(TpPath().c_str(), "r"); if (!f) return; char line[512];
        while (fgets(line, sizeof line, f)) { char name[128] = { 0 }; Vec3 v{}; char* bar = strchr(line, '|'); if (!bar) continue; *bar = 0; strncpy_s(name, line, _TRUNCATE); if (sscanf(bar + 1, "%f|%f|%f", &v.x, &v.y, &v.z) == 3) g_tp.push_back({ name, v }); } fclose(f); }
    static void SaveTp() { FILE* f = fopen(TpPath().c_str(), "w"); if (!f) return; for (auto& t : g_tp) fprintf(f, "%s|%.2f|%.2f|%.2f\n", t.name.c_str(), t.pos.x, t.pos.y, t.pos.z); fclose(f); }
    static Vec3 g_tpTarget{}; static bool g_tpTargetSet = false;
    static void TravelGo(Vec3 pos, const char* what) {
        pos.y += 0.5f;
        if (core::TravelTo(pos, 0.0f)) Note(T("travelling to %s"), what);
        else Note(T("preparing the fast travel (first use of a session, up to half a minute) - try again in a moment"));
    }
    static void DrawTravel(const PosInfo& p, bool havePos) {
        LoadTp(); core::TravelPrepare();   // the first use of a session looks for the game's travel system in the background
        ImGui::TextWrapped(T("Travel uses the game's own fast travel: a loading screen, then you stand at the destination. Any distance works, the world is streamed in there."));
        ImGui::TextDisabled(T("status: %s"), core::TravelStatus().c_str());
        ImGui::Separator();
        if (!g_tpTargetSet && havePos) { g_tpTarget = p.world; g_tpTargetSet = true; }
        ImGui::SetNextItemWidth(std::min(330.0f, ImGui::GetContentRegionAvail().x)); ImGui::InputFloat3(T("##tptarget"), &g_tpTarget.x, "%.1f"); SameLineForControl(ICON_LOCATION_CROSSHAIRS " Travel");
        ImGui::BeginDisabled(!core::TravelAvailable());
        if (ImGui::Button(T(ICON_LOCATION_CROSSHAIRS " Travel"))) TravelGo(g_tpTarget, T("the coordinates"));
        ImGui::EndDisabled(); SameLineForControl("current position");
        ImGui::BeginDisabled(!havePos); if (ImGui::Button(T("current position"))) g_tpTarget = p.world; ImGui::EndDisabled();
        ImGui::TextDisabled(T("x  y (height)  z - the height only needs to be roughly right, the game puts you on the ground"));
        ImGui::Separator();
        ImGui::SetNextItemWidth(std::min(220.0f, ImGui::GetContentRegionAvail().x)); InputTextI18n("##tpname", T("name for the current spot"), g_tpName, sizeof g_tpName); SameLineForControl(ICON_LOCATION_DOT " Save current position");
        ImGui::BeginDisabled(!havePos || !g_tpName[0]);
        if (ImGui::Button(T(ICON_LOCATION_DOT " Save current position"))) { g_tp.push_back({ g_tpName, p.world }); SaveTp(); g_tpName[0] = 0; }
        ImGui::EndDisabled();
        if (ImGui::BeginTable("tp", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollX)) {
            ImGui::TableSetupColumn(T("name"), ImGuiTableColumnFlags_WidthFixed, 150); ImGui::TableSetupColumn(T("position"), ImGuiTableColumnFlags_WidthFixed, 230);
            ImGui::TableSetupColumn(T("distance"), ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize(T("Go")).x + ImGui::GetStyle().FramePadding.x * 2);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize(T("Delete")).x + ImGui::GetStyle().FramePadding.x * 2);
            for (int i = 0; i < (int)g_tp.size(); i++) {
                ImGui::PushID(i); ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(g_tp[i].name.c_str());
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.1f  %.1f  %.1f", g_tp[i].pos.x, g_tp[i].pos.y, g_tp[i].pos.z);
                ImGui::TableSetColumnIndex(2);
                if (havePos) { const float dist = sqrtf((g_tp[i].pos.x - p.world.x) * (g_tp[i].pos.x - p.world.x) + (g_tp[i].pos.z - p.world.z) * (g_tp[i].pos.z - p.world.z));
                    if (dist >= 1000.0f) ImGui::Text(T("%.1f km"), dist / 1000.0f); else ImGui::Text(T("%.0f m"), dist); }
                ImGui::TableSetColumnIndex(3);
                ImGui::BeginDisabled(!core::TravelAvailable());
                if (ImGui::Button(T("Go"))) TravelGo(g_tp[i].pos, g_tp[i].name.c_str());
                ImGui::EndDisabled();
                ImGui::TableSetColumnIndex(4); if (ImGui::Button(T("Delete"))) { g_tp.erase(g_tp.begin() + i); SaveTp(); ImGui::PopID(); break; }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (g_tp.empty()) ImGui::TextDisabled(T("(no saved points yet)"));
        ImGui::Separator();
        ImGui::TextDisabled(T("Console: tp x y z"));
    }

    // ---- terrain brush (tab Terrain): strokes become core terrain edits; the ground changes once it streams again (Apply) ----
    enum BrushMode { BrushRaise = 0, BrushLower = 1, BrushFlatten = 2 };
    static bool g_brushOn = false; static int g_brushMode = BrushRaise;
    static float g_brushRadius = 8.0f, g_brushAmount = 0.5f, g_brushFlat = 0.5f;
    static Vec3 g_brushAt{}; static bool g_brushHave = false; static float g_brushY = 0; static bool g_brushYSet = false;
    static int g_brushTicket = 0; static DWORD g_brushProbeTick = 0, g_brushHitTick = 0; static bool g_brushLastHit = false; static float g_brushLastHitY = 0;
    static bool g_brushPainting = false; static size_t g_brushHistoryMark = 0; static Vec3 g_brushLast{}; static float g_brushAx = 0, g_brushAz = 0;
    static bool BrushMutationPending() { return g_brushPainting || g_brushTicket != 0; }
    static void FinishProjectSceneMutation() {
        // Called only after core's true synchronous outcome. Surviving NPCs in other projects
        // must leave the editor move lifecycle too; removed identities must never be recreated.
        for (const auto& n : core::ManagedNpcs()) if (!n.hidden && n.editMoving &&
            (n.uid == g_numericNpcUid || (g_place.active && std::any_of(g_place.m.begin(), g_place.m.end(),
                [&](const Member& m) { return m.npc && m.uid == n.uid; })))) {
            if (!core::EndManagedNpcMove(n.uid, n.liveMovePending ? n.liveMoveTarget : n.pos))
                Note("NPC %d: could not finish the surviving editor move", n.uid);
        }
        if (g_place.active) FinishPlace();
        g_deferredEditor = {};
        // Unload can succeed while an unrelated operation is Applying. Keep its owned receipt
        // for later reconciliation; only unapplied operations may be canceled here.
        ReconcileGroundBatch(true);
        g_snapJobs.clear();
        ClearSceneSelection(); g_undo.clear(); g_redo.clear(); ++g_historyBranch;
        g_numericSceneUid = g_numericNpcUid = 0; CancelNumericEdit();
        // Success resets upstream History. Do not slice the newly admitted terrain with the old
        // brush mark, or an earlier gesture would acquire the loaded document's strokes.
        g_brushPainting = g_brushHave = g_brushLastHit = false; g_brushTicket = 0;
        g_brushHistoryMark = 0; g_brushHitTick = 0;
        g_browserDragPrefab = g_npcDragIndex = -1; g_browserDropJobs.clear(); g_npcDropJobs.clear();
        g_boxSelecting = g_boxMoved = g_boxAdd = false; g_boxBase.clear(); g_boxNpcBase.clear();
        g_rightGesture = g_rightMoved = g_worldPopupRequested = false; g_rightUid = 0;
        g_projectRefresh = true; PublishProjectContext();
    }
    static bool TerrainTabShown() {
        return g_open && (g_compact ? g_compactPage == TabTerrain : g_mainTab == TabTerrain);
    }
    static bool g_shapePreview = false;   // with live editing the real ground changes at once; the preview is for builds without it
    struct ShapeGrid { int gen = -1; float x0 = 0, z0 = 0; int nx = 0, nz = 0; std::vector<float> orig, edit; };
    static ShapeGrid g_shape;
    // The edited surface as a wire grid on the 2 m texel grid (where it differs from the original): orange above, blue below the
    // current ground, stronger with the height change. Drawn over the picture, so dips hidden under the old ground show too.
    static void DrawShapePreview(ImDrawList* dl, const CamFrame& cf) {
        const auto strokes = core::TerrainStrokes(); if (strokes.empty()) return;
        float mnx = 1e9f, mnz = 1e9f, mxx = -1e9f, mxz = -1e9f; bool any = false;
        for (const auto& t : strokes) { const float dx = t.x - cf.pos.x, dz = t.z - cf.pos.z; if (dx * dx + dz * dz > 350.0f * 350.0f) continue;
            mnx = std::min(mnx, t.x - t.r); mnz = std::min(mnz, t.z - t.r); mxx = std::max(mxx, t.x + t.r); mxz = std::max(mxz, t.z + t.r); any = true; }
        if (!any) return;
        mnx = std::max(mnx, cf.pos.x - 200.0f); mnz = std::max(mnz, cf.pos.z - 200.0f); mxx = std::min(mxx, cf.pos.x + 200.0f); mxz = std::min(mxz, cf.pos.z + 200.0f);
        const float x0 = 2.0f * std::floor(mnx / 2.0f), z0 = 2.0f * std::floor(mnz / 2.0f);
        const int nx = std::min(200, (int)((mxx - x0) / 2.0f) + 2), nz = std::min(200, (int)((mxz - z0) / 2.0f) + 2); if (nx < 2 || nz < 2) return;
        const int gen = core::TerrainPreviewGen();
        if (gen != g_shape.gen || x0 != g_shape.x0 || z0 != g_shape.z0 || nx != g_shape.nx || nz != g_shape.nz) {
            g_shape.gen = gen; g_shape.x0 = x0; g_shape.z0 = z0; g_shape.nx = nx; g_shape.nz = nz;
            core::TerrainPreviewGrid(x0, z0, nx, nz, &g_shape.orig, &g_shape.edit);
        }
        if ((int)g_shape.edit.size() != nx * nz) return;
        // a shaded clay surface of the new ground: every 2 m cell that changes (plus the ring around it) as a lit quad, painted far
        // to near so nearer slopes cover farther ones; slightly orange where it rises, blue where it sinks
        auto H = [&](int i, int j) -> float { return g_shape.edit[(size_t)j * nx + i]; };
        auto D = [&](int i, int j) -> float { const size_t k = (size_t)j * nx + i; return g_shape.edit[k] - g_shape.orig[k]; };
        struct Quad { float depth; ImVec2 p[4]; ImU32 col; };
        static std::vector<Quad> quads; quads.clear();
        const float lx = 0.45f, ly = 0.80f, lz = 0.40f;   // light from above, slightly from the side (normalised below)
        const float ll = sqrtf(lx * lx + ly * ly + lz * lz);
        for (int j = 0; j + 1 < nz; j++) for (int i = 0; i + 1 < nx; i++) {
            const float h00 = H(i, j), h10 = H(i + 1, j), h01 = H(i, j + 1), h11 = H(i + 1, j + 1);
            if (std::isnan(h00) || std::isnan(h10) || std::isnan(h01) || std::isnan(h11)) continue;
            float dmax = 0, dsum = 0;   // changed here or next to a change (the ring keeps the edge of an edit visible)
            for (int jj = std::max(0, j - 1); jj <= std::min(nz - 1, j + 2); jj++) for (int ii = std::max(0, i - 1); ii <= std::min(nx - 1, i + 2); ii++) {
                const float d = D(ii, jj); if (std::isnan(d)) continue; dmax = std::max(dmax, std::fabs(d)); }
            if (dmax < 0.03f) continue;
            dsum = 0.25f * (D(i, j) + D(i + 1, j) + D(i, j + 1) + D(i + 1, j + 1));
            const float wx = x0 + 2.0f * i + 1.0f, wz = z0 + 2.0f * j + 1.0f;
            const Vec3 c[4] = { { wx, h00 + 0.05f, wz }, { wx + 2.0f, h10 + 0.05f, wz }, { wx + 2.0f, h11 + 0.05f, wz + 2.0f }, { wx, h01 + 0.05f, wz + 2.0f } };
            Quad q; bool ok = true; float dep = 0;
            for (int k = 0; k < 4 && ok; k++) {
                const float d = (c[k].x - cf.pos.x) * cf.fwd.x + (c[k].y - cf.pos.y) * cf.fwd.y + (c[k].z - cf.pos.z) * cf.fwd.z;
                ok = d > 1.0f && WorldToScreen(cf, c[k], &q.p[k]); dep += d;
            }
            if (!ok) continue;
            // normal of the cell from its height differences (x and z spacing 2 m), Lambert shading
            const float nxv = -((h10 + h11) - (h00 + h01)) * 0.25f, nzv = -((h01 + h11) - (h00 + h10)) * 0.25f, nyv = 1.0f;
            const float nl = sqrtf(nxv * nxv + nyv * nyv + nzv * nzv);
            const float lit = 0.30f + 0.70f * std::max(0.0f, (nxv * lx + nyv * ly + nzv * lz) / (nl * ll));
            const float tint = std::min(1.0f, std::fabs(dsum) / 3.0f) * (std::fabs(dsum) >= 0.03f ? 1.0f : 0.0f);
            float r = 205, g = 190, b = 165;   // clay
            if (dsum > 0) { r += (255 - r) * tint * 0.5f; g += (170 - g) * tint * 0.5f; b += (90 - b) * tint * 0.5f; }
            else          { r += (110 - r) * tint * 0.5f; g += (170 - g) * tint * 0.5f; b += (255 - b) * tint * 0.5f; }
            const float edge = dmax < 0.3f ? dmax / 0.3f : 1.0f;   // the untouched ring fades out
            q.col = IM_COL32((int)(r * lit), (int)(g * lit), (int)(b * lit), (int)(60 + 170 * edge));
            q.depth = dep; quads.push_back(q);
        }
        std::sort(quads.begin(), quads.end(), [](const Quad& a, const Quad& b) { return a.depth > b.depth; });
        for (const auto& q : quads) dl->AddQuadFilled(q.p[0], q.p[1], q.p[2], q.p[3], q.col);
    }
    static bool BrushActive() { return TerrainTabShown() && g_brushOn && core::TerrainAvailable() && !g_playMode; }
    static int TerrainBrushProject() {
        return core::EnsureEditingProject();
    }
    static void AddBrushStroke() {
        core::TerrainStroke t{};
        t.mode = g_brushMode == BrushFlatten ? core::TerrainFlatten : core::TerrainRaise;
        t.x = g_brushAt.x; t.z = g_brushAt.z; t.r = g_brushRadius; t.y = g_brushAt.y;
        t.amount = g_brushMode == BrushLower ? -g_brushAmount : g_brushAmount; t.strength = g_brushFlat;
        t.ax = g_brushAx; t.az = g_brushAz; t.proj = TerrainBrushProject();
        if (!t.proj) return;
        core::TerrainAddStroke(t); g_brushLast = g_brushAt;
    }
    static void CommitBrushHistory() {
        if (!g_brushPainting) return;
        const auto strokes = core::TerrainStrokes();
        if (g_brushHistoryMark < strokes.size()) {
            Act a{}; a.kind = Act::TerrainBatch;
            a.terrain.assign(strokes.begin() + g_brushHistoryMark, strokes.end());
            Push({ std::move(a) });
        }
        g_brushPainting = false;
    }
    static void DrawGroundRing(ImDrawList* dl, const CamFrame& cf, float x, float y, float z, float r, ImU32 col, int seg, float th) {
        ImVec2 prev{}; bool havePrev = false;   // points closer than 1 m to the camera plane would project far off screen: the ring breaks there
        for (int i = 0; i <= seg; i++) {
            const float a = 6.2831853f * i / seg; const Vec3 w{ x + r * cosf(a), y + 0.1f, z + r * sinf(a) };
            const float depth = (w.x - cf.pos.x) * cf.fwd.x + (w.y - cf.pos.y) * cf.fwd.y + (w.z - cf.pos.z) * cf.fwd.z;
            ImVec2 sp; const bool ok = depth > 1.0f && WorldToScreen(cf, w, &sp);
            if (ok && havePrev) dl->AddLine(prev, sp, col, th);
            prev = sp; havePrev = ok;
        }
    }
    static void BrushTick(const PosInfo& p, bool havePos) {
        if (!TerrainTabShown()) { CommitBrushHistory(); return; }
        const CamFrame cf = CurrentCam(); if (!cf.ok) { CommitBrushHistory(); return; }
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        if (g_shapePreview) DrawShapePreview(dl, cf);
        else {   // painted strokes, newest on top (only the ones near the camera)
            const auto strokes = core::TerrainStrokes(); int drawn = 0;
            for (int i = (int)strokes.size() - 1; i >= 0 && drawn < 600; i--) {
                const auto& t = strokes[i]; const float dx = t.x - cf.pos.x, dz = t.z - cf.pos.z; if (dx * dx + dz * dz > 400.0f * 400.0f) continue;
                const ImU32 col = t.mode == core::TerrainFlatten ? IM_COL32(120, 230, 140, 150) : t.amount >= 0 ? IM_COL32(255, 170, 80, 150) : IM_COL32(90, 170, 255, 150);
                DrawGroundRing(dl, cf, t.x, t.y, t.z, t.r, col, 20, 1.0f); drawn++;
            }
        }
        if (!BrushActive()) { CommitBrushHistory(); return; }
        ImGuiIO& io = ImGui::GetIO();
        (void)p; (void)havePos;
        // the brush sits where a cast from the camera along the mouse ray hits (the game's own sphere cast, one per frame at most);
        // the last hit stays shown while the next cast is on its way, a miss (sky, beyond the loaded collision) hides the brush
        if (g_brushTicket) {
            core::GroundHit gh; const auto status = core::GroundResultState(g_brushTicket, &gh);
            if (status == core::GroundProbeStatus::Invalidated || status == core::GroundProbeStatus::Unknown) {
                CommitBrushHistory(); // accepted strokes must survive invalidation of the next query
                g_brushTicket = 0; g_brushLastHit = g_brushHave = false; g_brushHitTick = 0; return;
            }
            if (status != core::GroundProbeStatus::Pending) {
                g_brushTicket = 0; g_brushLastHit = status == core::GroundProbeStatus::Hit && gh.fraction >= 0;
                if (g_brushLastHit) { g_brushAt = { gh.center.x, gh.center.y - gh.radius, gh.center.z }; g_brushY = g_brushAt.y; g_brushLastHitY = g_brushAt.y; g_brushHitTick = GetTickCount(); }
            }
        }
        const bool overUi = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) || ImGui::IsAnyItemHovered();
        if (io.MouseClicked[ImGuiMouseButton_Left]) {
            static int reports = 0;
            if (reports++ < 24) core::Log("[editor/brush] click: ui=%d camera=%d hit=%d ticket=%d probeReady=%d pending=%d",
                (int)overUi, (int)cf.ok, (int)g_brushLastHit, (int)(g_brushTicket != 0), (int)core::GroundProbeReady(), (int)(g_deferredEditor.action != nullptr));
        }
        const Vec3 rd = MouseRay(cf, io.MousePos);
        if (!overUi && !g_brushTicket && core::GroundProbeReady()) g_brushTicket = core::RayProbe(cf.pos, rd, 300.0f);
        g_brushHave = !overUi && g_brushLastHit && GetTickCount() - g_brushHitTick < 500;
        if (g_brushHave && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            // A held pointer sample is not a deferred command: start only against current ground authority.
            if (g_deferredEditor.action || !ReconcileGroundBatch(true)) return;
            g_brushHistoryMark = core::TerrainStrokes().size(); g_brushPainting = true; g_brushAx = g_brushAt.x; g_brushAz = g_brushAt.z; AddBrushStroke();
        }
        else if (g_brushPainting && g_brushHave && io.MouseDown[ImGuiMouseButton_Left]) {   // dragging paints a stroke every 40 % of the radius
            const float dx = g_brushAt.x - g_brushLast.x, dz = g_brushAt.z - g_brushLast.z, sp = std::max(0.5f, g_brushRadius * 0.4f);
            if (dx * dx + dz * dz >= sp * sp) AddBrushStroke();
        }
        if (!io.MouseDown[ImGuiMouseButton_Left]) CommitBrushHistory();
        if (g_brushHave) {
            const ImU32 col = g_brushMode == BrushFlatten ? IM_COL32(150, 255, 170, 255) : g_brushMode == BrushLower ? IM_COL32(120, 190, 255, 255) : IM_COL32(255, 190, 100, 255);
            DrawGroundRing(dl, cf, g_brushAt.x, g_brushAt.y, g_brushAt.z, g_brushRadius, col, 48, 2.0f);
            ImVec2 c; if (WorldToScreen(cf, g_brushAt, &c)) dl->AddCircleFilled(c, 3.0f, col);
        }
    }
    static void ClearTerrainAction() {
        if (!PrepareHistoryMutation([]() { ClearTerrainAction(); })) return;
        const std::string name = core::EditingProject();
        if (name.empty()) return;
        Act a{}; a.kind = Act::TerrainClear; a.proj = core::ProjectId(name);
        if (!core::TerrainRemoveProject(a.proj, a.terrain, a.terrainIndices)) return;
        CaptureProvenance(a); Push({ std::move(a) });
    }
    static void DrawTerrain(const PosInfo& p, bool havePos, bool compact = false) {
        if (!core::TerrainAvailable()) { ImGui::TextWrapped(T("Terrain editing is not available in this game build: %s"), core::TerrainStatus().c_str()); return; }
        core::TravelPrepare();
        const float ui = ImGui::GetFontSize() / 17.0f;
        FittedCheckbox(T("Brush active (left mouse paints in the world)"), &g_brushOn);
        SameLineForControl("Shape preview", true);
        FittedCheckbox(T("Shape preview"), &g_shapePreview);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("The ground as it will be after Apply, as a shaded surface: orange tint = higher, blue tint = lower than now."));
        if (compact) {
            static const char* modes[] = { "Raise", "Lower", "Flatten" };
            ImGui::SetNextItemWidth(-1);
            ComboT("##terrain_mode", &g_brushMode, modes, 3);
        } else {
            ImGui::RadioButton(T("Raise"), &g_brushMode, BrushRaise); SameLineForControl("Lower", true);
            ImGui::RadioButton(T("Lower"), &g_brushMode, BrushLower); SameLineForControl("Flatten", true);
            ImGui::RadioButton(T("Flatten"), &g_brushMode, BrushFlatten);
        }
        SetLabeledItemWidth("radius (m)", compact ? ImGui::GetContentRegionAvail().x : 260.0f * ui); ImGui::SliderFloat(T("radius (m)"), &g_brushRadius, 2.0f, 60.0f, "%.1f");
        SetLabeledItemWidth(g_brushMode == BrushFlatten ? "strength" : "height per stroke (m)", compact ? ImGui::GetContentRegionAvail().x : 260.0f * ui);
        if (g_brushMode == BrushFlatten) ImGui::SliderFloat(T("strength"), &g_brushFlat, 0.05f, 1.0f, "%.2f");
        else ImGui::SliderFloat(T("height per stroke (m)"), &g_brushAmount, 0.05f, 5.0f, "%.2f");
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", g_brushMode == BrushFlatten ? T("Flatten pulls the ground toward the height where the drag started.") : T("Dragging paints a stroke every 40 percent of the radius; strokes add up."));
        ImGui::PopStyleColor();
        const std::string editing = core::EditingProject();
        const int project = editing.empty() ? 0 : core::ProjectId(editing);
        const auto allStrokes = core::TerrainStrokes();
        const int strokeCount = (int)std::count_if(allStrokes.begin(), allStrokes.end(), [project](const auto& stroke) { return project > 0 && stroke.proj == project; });
        ImGui::Text(T("%d strokes"), strokeCount); SameLineForControl("Undo");
        ImGui::BeginDisabled(g_undo.empty());
        if (ImGui::Button(T("Undo"))) Undo();
        ImGui::EndDisabled(); SameLineForControl("Redo");
        ImGui::BeginDisabled(g_redo.empty());
        if (ImGui::Button(T("Redo"))) Redo();
        ImGui::EndDisabled(); SameLineForControl("Clear project terrain");
        ImGui::BeginDisabled(strokeCount == 0);
        if (FittedButton(T("Clear project terrain"))) ClearTerrainAction();
        ImGui::EndDisabled();
        ImGui::Separator();
        const std::string st = core::TerrainApplyState();
        ImGui::PushStyleColor(ImGuiCol_Text, !st.empty() || core::TerrainNeedsApply() ? ImVec4(1.0f, 0.8f, 0.4f, 1.0f) : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (!st.empty()) ImGui::TextWrapped(T("Applying: %s"), st.c_str());
        else if (core::TerrainNeedsApply()) ImGui::TextWrapped("%s", T("Not applied yet: the shaded surface shows the new shape."));
        else if (strokeCount == 0) ImGui::TextDisabled("%s", T("No terrain strokes yet. Enable the brush and paint on the ground."));
        else ImGui::TextWrapped("%s", T("The ground shows every stroke."));
        ImGui::PopStyleColor();
        ImGui::BeginDisabled(!st.empty() || !havePos || !core::TravelAvailable() || allStrokes.empty() && !core::TerrainNeedsApply());
        if (FittedButton(T(ICON_LOCATION_CROSSHAIRS " Apply (two loading screens)"), compact)) { if (core::TerrainApply(p.world)) Note(T("applying the terrain: fast travel away and back")); }
        ImGui::EndDisabled();
        if (!compact || ImGui::CollapsingHeader(TStable("Terrain help"))) {
            ImGui::TextWrapped(T("Strokes change the ground and its collision right away. Apply (a fast travel 5 km away and back, about half a minute) is only needed when a stroke could not be shown live. Strokes are saved with the project and are there right away when the project is autoloaded."));
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped(T("travel: %s"), core::TravelStatus().c_str()); ImGui::PopStyleColor();
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
    static bool IsCarried(int uid) { if (!g_place.active) return false; for (const auto& m : g_place.m) if (!m.npc && m.uid == uid) return true; return false; }
    static bool IsNpcCarried(int uid) { if (!g_place.active) return false; for (const auto& m : g_place.m) if (m.npc && m.uid == uid) return true; return false; }
    static bool NpcScreenRect(const CamFrame& cf, const ManagedNpc& n, ImVec2* mn, ImVec2* mx, float* depth) {
        const Vec3 p = ManagedNpcDisplayPos(n);
        Vec3 c{p.x,p.y+0.9f,p.z}, feet=p, head{p.x,p.y+1.8f,p.z}; ImVec2 sc{},sf{},sh{};
        if(!WorldToScreen(cf,c,&sc))return false;
        const bool feetVisible=WorldToScreen(cf,feet,&sf), headVisible=WorldToScreen(cf,head,&sh);
        const float h=feetVisible&&headVisible?std::max(28.0f,fabsf(sf.y-sh.y)):28.0f;
        const float w=std::max(18.0f,h*0.34f); *mn={sc.x-w*0.5f,sc.y-h*0.5f}; *mx={sc.x+w*0.5f,sc.y+h*0.5f};
        Vec3 d{c.x-cf.pos.x,c.y-cf.pos.y,c.z-cf.pos.z}; *depth=d.x*cf.fwd.x+d.y*cf.fwd.y+d.z*cf.fwd.z; return *depth>0;
    }
    static void UpdateBoxSelection(const CamFrame& cf, const std::vector<SpawnedObj>& objects, const std::vector<ManagedNpc>& npcs, ImGuiIO& io) {
        if(!g_boxSelecting)return; if(!ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)&&!ImGui::IsAnyItemHovered())g_boxCurrent=io.MousePos;
        const float dx=g_boxCurrent.x-g_boxStart.x,dy=g_boxCurrent.y-g_boxStart.y;if(io.MouseDown[ImGuiMouseButton_Left]&&dx*dx+dy*dy>=25)g_boxMoved=true;
        if(g_boxMoved&&(io.MouseDown[ImGuiMouseButton_Left]||ImGui::IsMouseReleased(ImGuiMouseButton_Left))){
            const ImVec2 mn(std::min(g_boxStart.x,g_boxCurrent.x),std::min(g_boxStart.y,g_boxCurrent.y)),mx(std::max(g_boxStart.x,g_boxCurrent.x),std::max(g_boxStart.y,g_boxCurrent.y));std::set<int> nextObj,nextNpc;
            for(const auto&o:objects){if(o.hidden||IsCarried(o.uid))continue;ImVec2 a,b;float dep=0;if(ObjScreenRect(cf,o,&a,&b,&dep)&&dep>0&&a.x<=mx.x&&b.x>=mn.x&&a.y<=mx.y&&b.y>=mn.y)nextObj.insert(o.uid);}
            for(const auto&n:npcs){if(n.hidden||IsNpcCarried(n.uid))continue;ImVec2 a,b;float dep=0;if(NpcScreenRect(cf,n,&a,&b,&dep)&&a.x<=mx.x&&b.x>=mn.x&&a.y<=mx.y&&b.y>=mn.y)nextNpc.insert(n.uid);}
            if(g_selectGroups){std::set<int> gids;for(const auto&o:objects)if(nextObj.count(o.uid)&&o.group>0)gids.insert(o.group);for(const auto&n:npcs)if(nextNpc.count(n.uid)&&n.group>0)gids.insert(n.group);for(const auto&o:objects)if(!o.hidden&&gids.count(o.group))nextObj.insert(o.uid);for(const auto&n:npcs)if(!n.hidden&&gids.count(n.group))nextNpc.insert(n.uid);}
            if(g_boxAdd){nextObj.insert(g_boxBase.begin(),g_boxBase.end());nextNpc.insert(g_boxNpcBase.begin(),g_boxNpcBase.end());}g_sel.swap(nextObj);g_managedNpcSel.swap(nextNpc);
            g_primary=g_sel.empty()?0:*g_sel.begin();g_managedNpcPrimary=g_managedNpcSel.empty()?0:*g_managedNpcSel.begin();g_sceneLastEntity=g_primary?g_primary:(g_managedNpcPrimary?-g_managedNpcPrimary:0);g_editUid=0;
            ImDrawList*dl=ImGui::GetForegroundDrawList();dl->AddRectFilled(mn,mx,IM_COL32(65,165,230,36));dl->AddRect(mn,mx,IM_COL32(115,205,255,230),0,0,1.5f);
        }
        if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)){if(!g_boxMoved&&!g_boxAdd)ClearSceneSelection();g_boxSelecting=g_boxMoved=g_boxAdd=false;g_boxBase.clear();g_boxNpcBase.clear();}
    }
    static void ClickSelect(const PosInfo& p, bool havePos) {
        (void)p;(void)havePos;g_hoverUid=g_hoverNpcUid=0;ImGuiIO&io=ImGui::GetIO();if(g_browserDragPrefab>=0||g_npcDragIndex>=0||g_blueprintDragging)return;if(BrushActive())return;
        const bool overUi = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) || ImGui::IsAnyItemHovered();
        const bool addSelect=CtrlHeld(io),addBox=CtrlHeld(io)||ShiftHeld(io),placing=g_place.active;if(g_playMode)return;
        if (io.MouseClicked[ImGuiMouseButton_Left]) {
            static int reports = 0;
            if (reports++ < 24) core::Log("[editor/pick] click: ui=%d camera=%d placing=%d objects=%zu npcs=%zu x=%.0f y=%.0f",
                (int)overUi, (int)CurrentCam().ok, (int)placing, core::Spawned().size(), core::ManagedNpcs().size(), io.MousePos.x, io.MousePos.y);
        }
        auto objects=core::Spawned();auto npcs=core::ManagedNpcs();
        if(g_rightGesture){if(io.MouseDown[ImGuiMouseButton_Right]){float dx=io.MousePos.x-g_rightStart.x,dy=io.MousePos.y-g_rightStart.y;if(dx*dx+dy*dy>16)g_rightMoved=true;}if(ImGui::IsMouseReleased(ImGuiMouseButton_Right)){if(!g_rightMoved&&g_rightUid){if(g_rightUid>0){if(IsCarried(g_rightUid)){ClearSceneSelection();for(const auto&m:g_place.m){if(m.npc)g_managedNpcSel.insert(m.uid);else g_sel.insert(m.uid);}g_primary=g_lastClicked=g_rightUid;g_sceneLastEntity=g_rightUid;}else if(!g_sel.count(g_rightUid))SelectUid(g_rightUid,false,objects);}else{const int uid=-g_rightUid;if(!g_managedNpcSel.count(uid))SelectManagedNpc(uid,false);}g_worldPopupRequested=true;g_worldPopupPos=io.MousePos;}g_rightGesture=g_rightMoved=false;g_rightUid=0;}}
        if(overUi&&!g_boxSelecting)return;if(ImGui::IsMouseClicked(ImGuiMouseButton_Right)){g_rightGesture=true;g_rightMoved=false;g_rightStart=io.MousePos;g_rightUid=0;if(placing&&!g_place.m.empty()){const auto&m=g_place.m.front();g_rightUid=m.npc?-m.uid:m.uid;}}
        CamFrame cf=CurrentCam();if(!cf.ok){if(ImGui::IsMouseReleased(ImGuiMouseButton_Left)){g_boxSelecting=g_boxMoved=g_boxAdd=false;g_boxBase.clear();g_boxNpcBase.clear();}return;}
        if(g_boxSelecting){UpdateBoxSelection(cf,objects,npcs,io);if(g_boxSelecting||overUi)return;}if(placing&&(g_place.hover||g_place.drag))return;
        float bestDepth=1e30f;int bestKey=0;for(const auto&o:objects){if(o.hidden||IsCarried(o.uid))continue;ImVec2 mn,mx;float d;if(!ObjScreenRect(cf,o,&mn,&mx,&d)||d<=0)continue;if(io.MousePos.x<mn.x||io.MousePos.x>mx.x||io.MousePos.y<mn.y||io.MousePos.y>mx.y)continue;if(d<bestDepth){bestDepth=d;bestKey=o.uid;}}
        for(const auto&n:npcs){if(n.hidden||IsNpcCarried(n.uid))continue;ImVec2 mn,mx;float d;if(!NpcScreenRect(cf,n,&mn,&mx,&d))continue;if(io.MousePos.x<mn.x||io.MousePos.x>mx.x||io.MousePos.y<mn.y||io.MousePos.y>mx.y)continue;if(d<bestDepth){bestDepth=d;bestKey=-n.uid;}}
        if(bestKey>0)g_hoverUid=bestKey;else if(bestKey<0)g_hoverNpcUid=-bestKey;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !overUi) {
            static int reports = 0;
            if (reports++ < 24) {
                int visibleObjects = 0, visibleNpcs = 0; float nearestPixels = 1e30f; int nearestUid = 0;
                for (const auto& o : objects) {
                    if (o.hidden || IsCarried(o.uid)) continue;
                    ImVec2 mn, mx; float depth = 0;
                    if (!ObjScreenRect(cf, o, &mn, &mx, &depth)) continue;
                    ++visibleObjects;
                    const float dx = std::max(mn.x - io.MousePos.x, std::max(0.0f, io.MousePos.x - mx.x));
                    const float dy = std::max(mn.y - io.MousePos.y, std::max(0.0f, io.MousePos.y - mx.y));
                    const float dist = sqrtf(dx * dx + dy * dy);
                    if (dist < nearestPixels) { nearestPixels = dist; nearestUid = o.uid; }
                }
                for (const auto& n : npcs) if (!n.hidden && !IsNpcCarried(n.uid)) {
                    ImVec2 mn, mx; float depth = 0;
                    if (NpcScreenRect(cf, n, &mn, &mx, &depth)) ++visibleNpcs;
                }
                core::Log("[editor/pick] candidate=%d objects=%zu/%d npcs=%zu/%d near=%d pixels=%.1f cam=(%.1f %.1f %.1f) fwd=(%.2f %.2f %.2f) mouse=(%.0f %.0f)",
                    bestKey, objects.size(), visibleObjects, npcs.size(), visibleNpcs, nearestUid, nearestPixels,
                    cf.pos.x, cf.pos.y, cf.pos.z, cf.fwd.x, cf.fwd.y, cf.fwd.z, io.MousePos.x, io.MousePos.y);
            }
        }
        if(ImGui::IsMouseClicked(ImGuiMouseButton_Right)){if(bestKey)g_rightUid=bestKey;return;}
        if(!bestKey){if(placing){if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))DropCarried();return;}if(ImGui::IsMouseClicked(ImGuiMouseButton_Left)){g_terrainTileSelected=false;g_boxSelecting=true;g_boxMoved=false;g_boxAdd=addBox;g_boxStart=g_boxCurrent=io.MousePos;g_boxBase=g_boxAdd?g_sel:std::set<int>{};g_boxNpcBase=g_boxAdd?g_managedNpcSel:std::set<int>{};}return;}
        if(ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)){if(bestKey>0){SelectSingleUid(bestKey);StartGrab({bestKey},false,ShortName(Find(objects,bestKey)->prefab));}else{const int uid=-bestKey;SelectManagedNpc(uid,false);const ManagedNpc*n=FindManagedNpc(npcs,uid);StartGrab({bestKey},false,n?ManagedNpcHistoryName(*n):std::string("NPC"));}return;}
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
        if (!PrepareHistoryMutation([=]() { SpawnBrowserDrop(prefab, center, yaw, scale); })) return;
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
            if (g_deferredEditor.action || !ReconcileGroundBatch(false)) return;
            if (g_place.active) { DropCarried(); if (g_place.active) { ++i; continue; } }
            if (GetTickCount() - j.queuedAt > 2500) {
                SpawnBrowserDrop(j.prefab, j.center, j.yaw, j.scale);
                g_browserDropJobs.erase(g_browserDropJobs.begin() + i); continue;
            }
            const auto status = core::GroundResultState(j.ticket, &gh);
            if (status == core::GroundProbeStatus::Pending) { ++i; continue; }
            if (status == core::GroundProbeStatus::Invalidated || status == core::GroundProbeStatus::Unknown) {
                SpawnBrowserDrop(j.prefab, j.center, j.yaw, j.scale);
                g_browserDropJobs.erase(g_browserDropJobs.begin() + i); continue;
            }
            if (status != core::GroundProbeStatus::Hit || !gh.hit || !std::isfinite(gh.centerY)) {
                if (j.retry++ == 0) {
                    PosInfo player{};
                    const float retryTop = core::PlayerPosInfo(&player) ? player.world.y + 25.0f : j.center.y + 25.0f;
                    j.ticket = core::GroundProbe({ j.center.x, retryTop, j.center.z }, 50.0f);
                    if (j.ticket) { ++i; continue; }
                }
                SpawnBrowserDrop(j.prefab, j.center, j.yaw, j.scale);
                g_browserDropJobs.erase(g_browserDropJobs.begin() + i); continue;
            }
            Vec3 center = j.center;
            if (j.prefab >= 0 && j.prefab < (int)idx.size()) {
                const auto& pi = idx[j.prefab]; const float groundY = gh.centerY - gh.radius;
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
        const bool overUi = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) || ImGui::IsAnyItemHovered();
        Vec3 center{}; const bool projected = BrowserDropPoint(pi, io.MousePos, &center, nullptr);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const std::string shown = ShownName(pi);
        dl->AddText({ io.MousePos.x + 16.0f, io.MousePos.y + 14.0f }, IM_COL32(255, 220, 150, 255), shown.c_str());
        if (!overUi && projected) {
            ImVec2 s; CamFrame cf = CurrentCam();
            if (WorldToScreen(cf, center, &s)) { dl->AddCircle(s, 11.0f, IM_COL32(255, 190, 90, 255), 24, 2.0f); dl->AddCircleFilled(s, 3.0f, IM_COL32(255, 230, 180, 255)); }
        }
        if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left)) return;
        const int prefab = g_browserDragPrefab; g_browserDragPrefab = -1;
        core::Log("[editor/browser-drop] release: prefab=%d ui=%d projected=%d gameReady=%d groundReady=%d at=(%.2f %.2f %.2f)",
            prefab, (int)overUi, (int)projected, (int)core::GameThreadReady(), (int)core::GroundProbeReady(), center.x, center.y, center.z);
        if (overUi || !projected || !core::GameThreadReady() || IsAppearance(pi)) return;
        if (!core::GroundProbeReady()) {
            g_browserDropJobs.push_back({ prefab, 0, GetTickCount(), center, g_spawnYaw, g_spawnScale });
            return;
        }
        const float halfHeight = pi.hasCenter ? std::max(0.0f, pi.sy) * g_spawnScale * 0.5f : 0.0f;
        const float startY = center.y - halfHeight + 5.0f;
        const int ticket = core::GroundProbe({ center.x, startY, center.z }, 30.0f);
        if (!ticket) {
            g_browserDropJobs.push_back({ prefab, 0, GetTickCount(), center, g_spawnYaw, g_spawnScale });
            return;
        }
        g_browserDropJobs.push_back({ prefab, ticket, GetTickCount(), center, g_spawnYaw, g_spawnScale });
        g_browserDropJobs.back().probeTop = startY;
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
    static void ProcessBlueprintDrag() {
        if (!g_blueprintDragging) return;
        ImGuiIO& io = ImGui::GetIO();
        if (!io.MouseDown[ImGuiMouseButton_Left] && !ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            g_blueprintDragging = false; return;
        }
        const bool overUi = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) || ImGui::IsAnyItemHovered();
        Vec3 at{}; const bool projected = NpcDropPoint(io.MousePos, &at, nullptr);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddText(ImVec2(io.MousePos.x + 16.0f, io.MousePos.y + 14.0f), IM_COL32(255, 220, 150, 255), g_blueprintDragFile.name.c_str());
        if (!overUi && projected) {
            ImVec2 screen{};
            if (WorldToScreen(CurrentCam(), at, &screen)) {
                dl->AddCircle(screen, 11.0f, IM_COL32(255, 190, 90, 255), 24, 2.0f);
                dl->AddCircleFilled(screen, 3.0f, IM_COL32(255, 230, 180, 255));
            }
        }
        if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left)) return;
        g_blueprintDragging = false;
        if (!overUi && projected) DispatchProjectAction(g_blueprintDragFile, ProjectAction::Place, false, &at);
    }
    static void PumpNpcDropJobs() {
        for (size_t i = 0; i < g_npcDropJobs.size();) {
            NpcDropJob& j = g_npcDropJobs[i]; core::GroundHit gh;
            if (g_deferredEditor.action || !ReconcileGroundBatch(false)) return;
            if (g_place.active) { DropCarried(); if (g_place.active) { ++i; continue; } }
            if (GetTickCount() - j.queuedAt > 2500) {
                SpawnNpcFormation(j.key, j.at, j.count, j.formation, j.spacing, j.radius, j.fx, j.fz, j.ai, j.behavior);
                g_npcDropJobs.erase(g_npcDropJobs.begin() + i); continue;
            }
            if (!j.centerResolved) {
                const auto status = core::GroundResultState(j.ticket, &gh);
                if (status == core::GroundProbeStatus::Pending) { ++i; continue; }
                if (status == core::GroundProbeStatus::Invalidated || status == core::GroundProbeStatus::Unknown) {
                    SpawnNpcFormation(j.key, j.at, j.count, j.formation, j.spacing, j.radius, j.fx, j.fz, j.ai, j.behavior);
                    g_npcDropJobs.erase(g_npcDropJobs.begin() + i); continue;
                }
                if (status != core::GroundProbeStatus::Hit || !gh.hit || !std::isfinite(gh.centerY)) {
                    if (j.retry++ == 0) {
                        PosInfo player{};
                        const float retryTop = core::PlayerPosInfo(&player) ? player.world.y + 25.0f : j.at.y + 25.0f;
                        j.ticket = core::GroundProbe({ j.at.x, retryTop, j.at.z }, 50.0f);
                        if (j.ticket) { ++i; continue; }
                    }
                    SpawnNpcFormation(j.key, j.at, j.count, j.formation, j.spacing, j.radius, j.fx, j.fz, j.ai, j.behavior);
                    g_npcDropJobs.erase(g_npcDropJobs.begin() + i); continue;
                }
                j.at.y = gh.centerY - gh.radius;
                j.positions = NpcFormationPositions(j.at, j.count, j.formation, j.spacing, j.radius, j.fx, j.fz);
                if (j.positions.size() == 1) {
                    SpawnNpcPositions(j.key, j.positions, j.ai, j.behavior);
                    g_npcDropJobs.erase(g_npcDropJobs.begin() + i); continue;
                }
                j.memberTickets.assign(j.positions.size(), 0);
                j.memberRetries.assign(j.positions.size(), 0);
                j.centerResolved = true;
            }
            int issued = 0; bool pending = false, invalidated = false;
            for (size_t k = 0; k < j.positions.size(); ++k) {
                int& ticket = j.memberTickets[k];
                if (ticket == 0 && issued < 16) {
                    ticket = core::GroundProbe({ j.positions[k].x, j.at.y + 5.0f, j.positions[k].z }, 30.0f);
                    ++issued;
                    if (!ticket) { invalidated = true; break; }
                }
                if (ticket == 0) { pending = true; continue; }
                if (ticket < 0) continue;
                core::GroundHit memberHit;
                const auto status = core::GroundResultState(ticket, &memberHit);
                if (status == core::GroundProbeStatus::Pending) { pending = true; continue; }
                if (status == core::GroundProbeStatus::Invalidated || status == core::GroundProbeStatus::Unknown) { invalidated = true; break; }
                if (status == core::GroundProbeStatus::Hit && memberHit.hit && std::isfinite(memberHit.centerY)) {
                    j.positions[k].y = memberHit.centerY - memberHit.radius;
                    ticket = -1;
                } else if (j.memberRetries[k]++ == 0) {
                    ticket = core::GroundProbe({ j.positions[k].x, j.at.y + 25.0f, j.positions[k].z }, 50.0f);
                    if (!ticket) { invalidated = true; break; }
                    pending = true;
                } else ticket = -2;
            }
            if (invalidated) { SpawnNpcPositions(j.key, j.positions, j.ai, j.behavior); g_npcDropJobs.erase(g_npcDropJobs.begin() + i); continue; }
            if (pending) { ++i; continue; }
            SpawnNpcPositions(j.key, j.positions, j.ai, j.behavior);
            g_npcDropJobs.erase(g_npcDropJobs.begin() + i);
        }
    }
    static void ProcessNpcDrag() {
        if (g_npcDragIndex < 0) return;
        const auto chars = thumbgen::Characters();
        if (!chars || g_npcDragIndex >= (int)chars->size()) { g_npcDragIndex = -1; return; }
        ImGuiIO& io = ImGui::GetIO(); const auto& c = (*chars)[g_npcDragIndex];
        if (!io.MouseDown[ImGuiMouseButton_Left] && !ImGui::IsMouseReleased(ImGuiMouseButton_Left)) { g_npcDragIndex = -1; return; }
        const bool overUi = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) || ImGui::IsAnyItemHovered();
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
        core::Log("[editor/npc-drop] release: key=%u ui=%d projected=%d npcState=%d groundReady=%d at=(%.2f %.2f %.2f)",
            key, (int)overUi, (int)projected, core::NpcState(), (int)core::GroundProbeReady(), at.x, at.y, at.z);
        if (overUi || !projected) return;
        if (core::NpcState() != 2) { Note(T("walk a few steps first: the game's spawn request needs your character's server actor")); return; }
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
            if(ImGui::MenuItem(T("Grab")))StartGrab(SceneGrabIds(),false,SceneSelectionCount()==1?"entity":"selection");ImGui::BeginDisabled(!g_managedNpcSel.empty());if(ImGui::MenuItem(T("To ground")))SnapSelToGround();if(ImGui::MenuItem(T("Duplicate"))){CopySel();Paste(havePos);}ImGui::EndDisabled();
            if(!g_managedNpcSel.empty()){ImGui::Separator();ImGui::BeginDisabled(!core::NpcAiControlAvailable());if(ImGui::MenuItem(T("Enable AI")))SetSelectedNpcAi(true);if(ImGui::MenuItem(T("Disable AI")))SetSelectedNpcAi(false);if(ImGui::BeginMenu(T("Behavior"))){if(ImGui::MenuItem(T("Normal autonomous")))SetSelectedNpcBehavior(0);if(ImGui::MenuItem(T("Hold position (AI paused)")))SetSelectedNpcBehavior(1);ImGui::EndMenu();}ImGui::EndDisabled();}
            ImGui::Separator();if(ImGui::MenuItem(T("Group selection")))GroupSceneSelection(true);if(ImGui::MenuItem(T("Ungroup"),nullptr,false,hasGroup))GroupSceneSelection(false);
            ImGui::BeginDisabled(!g_managedNpcSel.empty());if(ImGui::BeginMenu(T("Rotate"))){if(ImGui::MenuItem(T("Rotate left")))RotateSel(-g_rotationStep);if(ImGui::MenuItem(T("Rotate right")))RotateSel(g_rotationStep);ImGui::EndMenu();}if(ImGui::BeginMenu(T("Align to primary"))){if(ImGui::MenuItem(T("X")))AlignSel(0);if(ImGui::MenuItem(T("Y")))AlignSel(1);if(ImGui::MenuItem(T("Z")))AlignSel(2);ImGui::EndMenu();}ImGui::EndDisabled();
            if(ImGui::MenuItem(T("Forget")))ForgetSelection();if(ImGui::MenuItem(T("Delete")))DeleteSceneSelection();ImGui::EndDisabled();ImGui::EndPopup();}
        ImGui::End();
    }
    static void DrawSelectionOutlines() {
        if(g_sel.empty()&&g_managedNpcSel.empty()&&!g_hoverUid&&!g_hoverNpcUid)return;CamFrame cf=CurrentCam();if(!cf.ok)return;ImDrawList*dl=ImGui::GetForegroundDrawList();auto objects=core::Spawned();auto npcs=core::ManagedNpcs();
        for(const auto&o:objects){const bool sel=g_sel.count(o.uid)>0,hov=o.uid==g_hoverUid;if(o.hidden||(!sel&&!hov))continue;float cx=0,cy=0.5f,cz=0,sx=1,sy=1,sz=1;int pi=IndexOfPrefab(o.prefab);if(pi>=0){const auto&info=core::PrefabIndex()[pi];if(info.hasCenter){cx=info.cx;cy=info.cy;cz=info.cz;}if(info.sx>0){sx=info.sx;sy=info.sy;sz=info.sz;}}ImVec2 sp[8];bool ok[8];for(int k=0;k<8;k++)ok[k]=WorldToScreen(cf,LocalToWorld(o,cx+((k&1)?sx:-sx)*0.5f,cy+((k&2)?sy:-sy)*0.5f,cz+((k&4)?sz:-sz)*0.5f),&sp[k]);static const int edges[12][2]={{0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};const ImU32 col=sel?IM_COL32(255,150,55,255):IM_COL32(255,255,255,150);for(auto&e:edges)if(ok[e[0]]&&ok[e[1]])dl->AddLine(sp[e[0]],sp[e[1]],col,sel?2.5f:1.0f);}
        for(const auto&n:npcs){const bool sel=g_managedNpcSel.count(n.uid)>0,hov=n.uid==g_hoverNpcUid;if(n.hidden||(!sel&&!hov))continue;ImVec2 mn,mx;float dep=0;if(!NpcScreenRect(cf,n,&mn,&mx,&dep))continue;const ImU32 col=sel?IM_COL32(255,150,55,255):IM_COL32(255,255,255,180);dl->AddRect(mn,mx,col,3.0f,0,sel?3.0f:1.5f);}
    }
    static void DrawDockPlacementControls(const PosInfo& p, bool havePos, float ui) {
        if (g_selPrefab < 0 || g_selPrefab >= (int)core::PrefabIndex().size()) { ImGui::TextDisabled(T("pick a card, then PLACE")); return; }
        const auto& pi = core::PrefabIndex()[g_selPrefab];
        ImGui::TextDisabled("%s", ShownName(pi).c_str());
        ImGui::BeginDisabled(!havePos || !core::GameThreadReady());
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.36f, 0.22f, 1.0f)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.45f, 0.28f, 1.0f));
        if (FittedButton(T(ICON_LOCATION_CROSSHAIRS " PLACE "))) StartPlaceNew(p, havePos);
        ImGui::PopStyleColor(2); SameLineForControl("spawn only");
        if (FittedButton(T("spawn only"))) SpawnSelected(p);
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
        if (ImGui::Button(T(ICON_LIST " full editor"))) { g_compact = false; g_mainTab = g_compactPage; g_selectMainTab = true; }
        SameLineForControl(g_cameraMode ? ICON_EYE " flying" : ICON_EYE " free camera");
        ImGui::BeginDisabled(!core::FreeCamAvailable());
        const bool flying = g_cameraMode;   // decided once: the click below toggles it
        if (flying) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.36f, 0.22f, 1.0f));
        if (ImGui::Button(T(flying ? ICON_EYE " flying" : ICON_EYE " free camera"))) ToggleCameraMode();
        if (flying) ImGui::PopStyleColor();
        ImGui::EndDisabled();
        SameLineOrWrap(true, ImGui::GetFrameHeight(), 3); DrawCameraViewTool();
        struct DockPage { int id; const char* label; };
        static const DockPage dockPages[] = {
            { TabBrowser, ICON_MAGNIFYING_GLASS " Browser" },
            { TabScene, ICON_CUBE " Scene" },
            { TabNpcs, ICON_LOCATION_DOT " NPCs" },
            { TabProject, ICON_FLOPPY_DISK " Project" },
            { TabBlueprint, ICON_CUBE " Blueprints" },
            { TabEnvironment, ICON_CLOCK_ROTATE_LEFT " Time & Weather" },
            { TabTerrain, ICON_CUBE " Terrain" },
        };
        for (int i = 0; i < (int)(sizeof(dockPages) / sizeof(dockPages[0])); ++i) {
            if (dockPages[i].id == TabTerrain && !core::TerrainAvailable()) continue;
            if (i) SameLineForControl(dockPages[i].label);
            const bool act = g_compactPage == dockPages[i].id;   // decided once: the click below may change the page
            if (act) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
            if (FittedButton(TStable(dockPages[i].label))) { g_compactPage = dockPages[i].id; g_mainTab = dockPages[i].id; ImGui::SetScrollY(0); }
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
        if (g_compactPage == TabBlueprint) {
            if (g_previewShown) { core::PreviewClear(); g_previewShown = false; }
            DrawBlueprint();
            ProcessBlueprintDrag();
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
        if (g_compactPage == TabTerrain) {
            if (g_previewShown) { core::PreviewClear(); g_previewShown = false; }
            DrawTerrain(p, havePos, true);
            ImGui::End();
            if (playAlpha) ImGui::PopStyleVar();
            return;
        }
        ImGui::SetNextItemWidth(-1); InputTextI18n("##dockfilter", T("search  (words in any order)"), g_filter, sizeof g_filter);
        ImGui::Checkbox(ICON_STAR "##dfav", &g_favOnly); if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("favorites only"));
        SameLineForControl("meshes"); ImGui::Checkbox(T("meshes"), &g_meshOnly); if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("hide prefabs without a visible mesh"));
        SameLineForControl(ICON_XMARK " clear"); if (ImGui::Button(T(ICON_XMARK " clear"))) { g_filter[0] = 0; g_tagFilter.clear(); g_favOnly = false; g_selCat = 0; g_selColl = -1; }
        if (g_selCat > 0) { SameLineForControl("in %s"); ImGui::TextDisabled(T("in %s"), core::Categories()[g_selCat].name.c_str()); }
        LoadColls();
        if (!g_colls.empty()) {   // collections as chips under the search
            for (int ci = 0; ci < (int)g_colls.size(); ci++) {
                if (ci) ImGui::SameLine();
                const bool on = g_selColl == ci;
                if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
                char lbl[80]; snprintf(lbl, sizeof lbl, ICON_STAR " %s (%d)##dcoll%d", g_colls[ci].name.c_str(), (int)g_colls[ci].paths.size(), ci);
                if (ImGui::Button(lbl)) g_selColl = on ? -1 : ci;
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
            if (ImGui::Button(lbl)) g_dockCols = c;
            if (on) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("%d cards per row"), c);
        }
        ImGui::SameLine(); ImGui::TextDisabled(T("double-click places"));
        // The placement controls below the cards are collapsible. Do not reserve their
        // old worst-case height when both sections are closed: remember the height they
        // actually used last frame and give the rest back to the browser.
        static float s_dockPlacementH = 92.0f;
        const float availH = ImGui::GetContentRegionAvail().y;
        const float footer = std::clamp(s_dockPlacementH + ImGui::GetStyle().ItemSpacing.y,
                                        28.0f * ui, std::max(28.0f * ui, availH - 80.0f));
        const float listH = std::max(80.0f, availH - footer);
        {   // tile size from the window width: inner width of the bordered, scrollable child divided by the columns
            const ImGuiStyle& st = ImGui::GetStyle();
            const float inner = ImGui::GetContentRegionAvail().x - 2 * st.WindowPadding.x - st.ScrollbarSize - 2.0f;
            const float tile = floorf((inner - (g_dockCols - 1) * st.ItemSpacing.x) / g_dockCols) - 2 * 4.0f * ui;
            DrawCards(p, havePos, listH, ui, g_dockCols, std::max(32.0f, tile));
        }
        const float controlsY = ImGui::GetCursorPosY();
        DrawDockPlacementControls(p, havePos, ui);
        const float measuredControlsH = ImGui::GetCursorPosY() - controlsY;
        if (measuredControlsH > 1.0f) s_dockPlacementH = measuredControlsH;
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
        const bool overUi = ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) || ImGui::IsAnyItemHovered();
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


    static bool EditorMutationPending() {
        if (g_place.active || g_numericEditId || g_numericSceneUid || g_numericNpcUid || g_brushPainting || g_brushTicket ||
            g_projectCommandPending || g_exportOpen || !g_projectOverwriteName.empty() || g_deferredEditor.action || g_pendingNpcGrab.active || !g_pendingGround.empty() || !g_browserDropJobs.empty() || !g_npcDropJobs.empty() || ImGui::IsAnyItemActive()) return true;
        for (const auto& report : g_projectPlacements) if (!core::PlaceRequestState(report.request).settled) return true;
        return false;
    }
    void Draw() {
        ReconcileSpawnHistory();
        PumpPendingNpcGrab();
        g_placeHudDrawn = false;
        struct FrameCommit { ~FrameCommit() {
            if (!TerrainTabShown()) CommitBrushHistory(); PumpGroundHistory();
            if (g_place.active && !g_placeHudDrawn) DrawPlaceHud();
            FinalizeNumericEdits(); PublishProjectContext(); AutoSaveTick();
        } } frameCommit;
        {   // in-game names follow the UI language
            static std::string lastLang; std::string lang = i18n::ActiveLanguage();
            if (lang != lastLang) { lastLang = lang; thumbgen::WantNamesLanguage(lang); }
            g_gameNames = thumbgen::GameNames();
        }
        PosInfo p{}; bool havePos = core::PlayerPosInfo(&p);
        core::AutoloadFrame();
        TrackFacing(p, havePos);
        SampleCamera();
        PumpSnapJobs();
        PlaceTick();                           // runs with the menu closed as well
        PumpPendingBlueprintPlace();
        PumpBrowserDropJobs();                 // a drop whose ground probe returns after the window was hidden still spawns
        PumpNpcDropJobs();
        if (g_place.active && (g_place.hasNpc || g_gizmo || MouseMode())) { const bool one = g_place.m.size() == 1; const CamFrame cf = CurrentCam();
            DrawGizmo(g_place.center, one ? WrapYaw(g_place.m[0].rot0.yaw + g_place.yaw) : g_place.yaw, one ? WrapYaw(g_place.m[0].rot0.pitch + g_place.pitch) : g_place.pitch, GizmoScreenSize(cf, g_place.center, g_place.radius), g_place.drag ? g_place.drag : g_place.hover); }
        if (g_place.active && !g_open) ImGui::GetIO().MouseDrawCursor = MouseMode();
        DrawCalibrationMarker(p, havePos);
        {   // research overlay: world points from /api/research/points (e.g. edited terrain), drawn under the editor windows
            const auto pts = core::DebugPoints();
            if (!pts.empty()) { const CamFrame cf = CurrentCam(); ImDrawList* dl = ImGui::GetBackgroundDrawList(); ImVec2 s;
                for (const auto& d : pts) if (WorldToScreen(cf, d.p, &s)) dl->AddCircleFilled(s, 3.0f, d.col, 6); }
        }
        if (!g_open) { if (g_cameraMode) StopCameraMode(); return; }
        if (g_numericEditId && g_numericEditLastSeenFrame >= 0 && ImGui::GetFrameCount() - g_numericEditLastSeenFrame > 1) CancelNumericEdit();
        ImGuiIO& io = ImGui::GetIO();
        io.MouseDrawCursor = !g_playMode;
        BrushTick(p, havePos);
        ClickSelect(p, havePos); DrawWorldContextPopup(havePos); DrawSelectionOutlines();
        if (SharedPage(g_mainTab)) g_compactPage = g_mainTab;
        else if (!SharedPage(g_compactPage)) g_compactPage = TabBrowser;
        if (g_compact) { DrawCompact(p, havePos); DrawPropertiesWindow(p, havePos); DrawMetadataPopups(); CameraTick(); return; }
        {   // initial size follows the UI scale (style is scaled by screen height / 1080) and stays inside the screen
            const float ui = ImGui::GetFontSize() / 17.0f;
            ImVec2 want(1320.0f * ui, 800.0f * ui);
            want.x = std::min(want.x, io.DisplaySize.x - 60.0f); want.y = std::min(want.y, io.DisplaySize.y - 60.0f);
            ImGui::SetNextWindowSize(want, ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowPos(ImVec2(30, 30), ImGuiCond_FirstUseEver);
        }
        const bool playAlpha = g_playMode;
        if (playAlpha) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.20f);
        char title[240]; snprintf(title, sizeof title, T("World Builder v%s [%s] %s = %s, %s = hide###cdmodkit"), kEditorVersion, T(g_cameraMode ? "CAMERA MODE" : (g_playMode ? "PLAY MODE" : "EDIT MODE")), core::KeyName(core::g_keyMode), T(g_cameraMode ? "exit camera mode" : (g_playMode ? "back to editing" : "camera mode")), core::KeyName(core::g_keyToggle));
        const bool began = ImGui::Begin(title, &g_open, playAlpha ? ImGuiWindowFlags_NoInputs : 0);
        if (!g_open) { ImGui::End(); if (playAlpha) ImGui::PopStyleVar(); FinishCloseEditor(); return; }
        if (!began) { ImGui::End(); if (playAlpha) ImGui::PopStyleVar(); DrawPropertiesWindow(p, havePos); CameraTick(); return; }
        HandleHotkeys(havePos);
        if (!core::BuildOk()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.4f, 0.3f, 1)); ImGui::TextWrapped(T("Game functions not resolved: %s"), core::BuildMessage()[0] ? core::BuildMessage() : "verification pending"); ImGui::PopStyleColor();
            ImGui::TextWrapped(T("Spawning is disabled to avoid crashes. See bin64\\cdmodkit\\cdmodkit.log (RESOLVE FAILED)."));
            ImGui::TextWrapped(T("Game build %s - a patch moves the game's functions; report that build number so the signatures can be updated."), core::GameVersion()[0] ? core::GameVersion() : "unknown");
        }
        if (core::PrefabIndex().empty()) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.45f, 0.12f, 0.10f, 0.85f));
            ImGui::BeginChild("noindex", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 2.2f), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
            ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), T(ICON_XMARK "  Prefab list not found: the browser and the search stay empty."));
            ImGui::EndChild(); ImGui::PopStyleColor();
        }
        if (ImGui::Button(T(ICON_COPY " dock"))) g_compact = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T(
            g_mainTab == TabScene ? "narrow side window: show the Scene page and its controls" :
            g_mainTab == TabNpcs ? "narrow side window: NPC browser and spawning" :
            g_mainTab == TabProject ? "narrow side window: project management" :
            g_mainTab == TabBlueprint ? "Blueprint library" :
            g_mainTab == TabEnvironment ? "narrow side window: time and weather controls" :
            g_mainTab == TabTerrain ? "narrow side window: terrain brush and apply controls" :
            "narrow side window: search, cards in one column, PLACE"));
        ImGui::SameLine();
        {   // free-fly camera
            const bool fc = g_cameraMode;
            ImGui::BeginDisabled(!core::FreeCamAvailable());
            if (fc) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.36f, 0.22f, 1.0f));
            char fl[96]; snprintf(fl, sizeof fl, "%s (%s)###freecam", T(fc ? ICON_EYE " flying" : ICON_EYE " free camera"), core::KeyName(core::g_keyMode));
            if (ImGui::Button(fl)) ToggleCameraMode();
            if (fc) ImGui::PopStyleColor();
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", T(core::FreeCamAvailable()
                ? "Free camera: W/A/S/D move, E or Space up, Q down, Shift faster, mouse wheel forward. Ctrl remains available for multi-select and editor shortcuts. Drag with the right mouse button over the world to look around; a right click without moving opens the context menu. Your character stays where it is; new objects appear in front of the camera."
                : "The free camera is not available in this game build (see the log)."));
            SameLineOrWrap(false, ImGui::GetFrameHeight()); DrawCameraViewTool();
        }
        if (havePos) ImGui::TextWrapped(T(ICON_LOCATION_DOT "  %.1f  %.1f  %.1f   tile %d,%d"), p.world.x, p.world.y, p.world.z, p.tileX, p.tileZ);
        else ImGui::TextWrapped("%s", T("player position not available (load a save)"));
        SameLineOrWrap(false, 330);
        {   // game thread state: the mod runs its work on the game's simulation tick; the counter tells whether that tick is alive
            static long s_lastTicks = 0; static DWORD s_lastChange = 0; const long ticks = core::PumpTicks(); const DWORD now = GetTickCount();
            if (ticks != s_lastTicks) { s_lastTicks = ticks; s_lastChange = now; }
            const char* state = !core::HooksReady() ? "hooks MISSING" : ticks == 0 ? "game thread: waiting" : now - s_lastChange < 500 ? "game thread: running" : "game thread: paused";
            ImGui::TextDisabled(T("objects %d  |  %s"), (int)core::Spawned().size(), T(state));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(T("Spawning and moving happen on the game's simulation tick (%ld ticks so far).\nPaused = loading screen, menu or pause; queued actions run once it continues."), ticks);
        }
        {
            // ImGui's TabBar can shrink or scroll but cannot flow onto another row. The editor has enough pages that translated
            // labels routinely overflow, so render tab-shaped buttons with normal flow wrapping instead.
            if (g_mainTab == TabTravel && !core::TravelAvailable()) g_mainTab = TabBrowser;
            if (g_mainTab == TabTerrain && !core::TerrainAvailable()) g_mainTab = TabBrowser;
            struct MainNavPage { int id; const char* label; bool show; };
            const MainNavPage pages[] = {
                { TabBrowser, ICON_MAGNIFYING_GLASS " Browser", true },
                { TabNpcs, ICON_LOCATION_DOT " NPCs", true },
                { TabScene, ICON_CUBE " Scene", true },
                { TabEnvironment, ICON_CLOCK_ROTATE_LEFT " Time & Weather", true },
                { TabHistory, ICON_CLOCK_ROTATE_LEFT " History", true },
                { TabProject, ICON_FLOPPY_DISK " Project", true },
                { TabBlueprint, ICON_CUBE " Blueprints", true },
                { TabTravel, ICON_LOCATION_CROSSHAIRS " Travel", core::TravelAvailable() },
                { TabTerrain, ICON_CUBE " Terrain", core::TerrainAvailable() },
                { TabSettings, ICON_LIST " Settings", true },
                { TabLog, ICON_LIST " Log", true },
            };
            bool firstNav = true;
            for (const auto& page : pages) {
                if (!page.show) continue;
                const char* label = TStable(page.label);
                const float width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                if (!firstNav) {
                    const float gap = ImGui::GetStyle().ItemSpacing.x;
                    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
                    if (ImGui::GetItemRectMax().x + gap + width <= right) ImGui::SameLine();
                }
                ImGui::PushID(page.id);
                const bool active = g_mainTab == page.id;
                if (active) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
                }
                if (FittedButton(label)) g_mainTab = page.id;
                if (active) ImGui::PopStyleColor(2);
                ImGui::PopID();
                firstNav = false;
            }
            g_selectMainTab = false;
            ImGui::Separator();

            const bool inBrowser = g_mainTab == TabBrowser;
            if (g_mainTab == TabBrowser) DrawBrowser(p, havePos);
            else if (g_mainTab == TabNpcs) DrawNpcs(p, havePos);
            else if (g_mainTab == TabScene) {
                const int gp = core::GimmickPending();
                if (gp > 0) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), core::GimmickTemplateReady() ? T("%d interactive object(s) spawning...") : T("%d interactive object(s) waiting for a spawn template: walk a few meters"), gp);
                DrawScene(p, havePos);
            }
            else if (g_mainTab == TabEnvironment) DrawEnvironment();
            else if (g_mainTab == TabHistory) DrawHistory();
            else if (g_mainTab == TabProject) DrawProject();
            else if (g_mainTab == TabBlueprint) DrawBlueprint();
            else if (g_mainTab == TabTravel) DrawTravel(p, havePos);
            else if (g_mainTab == TabTerrain) DrawTerrain(p, havePos);
            else if (g_mainTab == TabSettings) {
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
                keyCombo("camera mode", &core::g_keyMode);
                ImGui::SetNextItemWidth(160); SliderFloatEdit(T("free camera speed"), &core::g_fcSpeed, 1.0f, 100.0f, "%.0f m per s"); if (ImGui::IsItemDeactivatedAfterEdit() || NumericEditEnded(T("free camera speed"))) core::SaveSettings();
                ImGui::SameLine(); ImGui::SetNextItemWidth(160); SliderFloatEdit(T("mouse sensitivity"), &core::g_fcSens, 0.02f, 0.5f, "%.2f"); if (ImGui::IsItemDeactivatedAfterEdit() || NumericEditEnded(T("mouse sensitivity"))) core::SaveSettings();
                if (ImGui::Checkbox(T("start free camera when opening the editor"), &core::g_autoFreeCamOnOpen)) core::SaveSettings();
                ImGui::Separator();
                if (ImGui::Checkbox(T("keyboard controls while placing objects (optional)"), &core::g_keyboardPlacement)) { core::ApplyPlaceKeys(); core::SaveSettings(); input::ClearKeys(); }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("Off by default. When enabled, the placement keys below move, rotate and scale the carried object and are kept away from the game."));
                if (core::g_keyboardPlacement && ImGui::TreeNode(T("placement keyboard bindings"))) {
                    for (int i = 0; i < core::PK_COUNT; ++i) { ImGui::PushID(i); keyCombo(core::PlaceKeyLabel(i), &core::g_placeKeys[i]); ImGui::PopID(); }
                    if (ImGui::Button(T("reset to numpad defaults"))) {
                        const int d[core::PK_COUNT] = { VK_NUMPAD8, VK_NUMPAD2, VK_NUMPAD4, VK_NUMPAD6, VK_NUMPAD9, VK_NUMPAD3, VK_NUMPAD7, VK_NUMPAD1, VK_ADD, VK_SUBTRACT, VK_NUMPAD0, VK_DECIMAL, VK_NUMPAD5, VK_MULTIPLY, VK_DIVIDE, VK_RETURN, VK_BACK, VK_SHIFT };
                        memcpy(core::g_placeKeys, d, sizeof d); core::ApplyPlaceKeys(); core::SaveSettings();
                    }
                    ImGui::TreePop();
                }
                ImGui::Separator();
                if (ImGui::Checkbox(T("project auto-save"), &core::g_projectAutoSave)) core::SaveSettings();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("New objects, NPCs and terrain are saved to their editing project."));
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
                    ImGui::SameLine(); if (ImGui::Button(T("copy URL"))) ImGui::SetClipboardText(url);
                } else if (core::g_httpEnabled) {
                    const std::string err = httpapi::LastError();
                    ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1), T("not running: %s"), err.empty() ? T("starting") : err.c_str());
                    ImGui::SameLine(); if (ImGui::Button(T("retry"))) httpapi::Start(core::g_httpPort);
                } else ImGui::TextDisabled(T("off"));
            }
            else if (g_mainTab == TabLog) {
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
                            for (int i = 0; i < ns; i++) { ImGui::PushID((int)(sp[i].so & 0x7FFFFFFF)); const char* fn = strrchr(sp[i].prefab, '/'); ImGui::Text(T("%lu s  %s  actor %p"), sp[i].ageMs / 1000, fn ? fn + 1 : sp[i].prefab, (void*)sp[i].actor); ImGui::SameLine(); if (sp[i].actor && ImGui::Button(T("remove"))) core::RequestRemoveSpawned(sp[i].actor); ImGui::PopID(); } }
                    }
                    {   // the prefab the replay creates its server object from (the prepare's 4th argument is that path)
                        static char prefabOverride[256] = {};
                        if (ImGui::Button(T("spawn it in front of me"))) QuickGimmickSpawn(); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T("spawns the override prefab (a stand torch when the field is empty) 2 m in front of you through the game's spawn path, from the newest capture")); ImGui::SameLine(); ImGui::SetNextItemWidth(-1);
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
                            ImGui::TableSetColumnIndex(4); if (ImGui::Button(T("here"))) core::ArmGimmickReplay(InFront(2.0f, 0.0f), c.id);
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
                if (g_log.empty()) ImGui::TextDisabled("%s", T("No diagnostic messages yet."));
                else for (auto& l : g_log) ImGui::TextUnformatted(l.c_str());
            }
            if (!inBrowser && g_previewShown) { core::PreviewClear(); g_previewShown = false; }
        }
        ProcessBrowserDrag();
        ProcessNpcDrag();
        ProcessBlueprintDrag();
        DrawMetadataPopups();
        ImGui::End();
        DrawPropertiesWindow(p, havePos);
        if (playAlpha) ImGui::PopStyleVar();
        CameraTick();
    }
#ifdef WB_UNIFIED_HOST_TEST
    namespace host_seam {
        struct ActView { int kind = -1, uid = 0, group = 0, group1 = 0, proj = 0; };
        int SpawnRecorded(const std::string& prefab, Vec3 pos, Rot rot, float scale, int group, int proj) {
            if (!PrepareHistoryMutation([=]() { SpawnRecorded(prefab, pos, rot, scale, group, proj); })) return 0;
            const int uid = core::SpawnAt(prefab, pos, rot, scale, group, proj);
            std::vector<Act> acts; RecordSpawn(acts, uid, prefab, pos, rot, scale, group); Push(std::move(acts)); return uid;
        }
        bool UndoOne() { if (g_undo.empty()) return false; Undo(); return true; }
        bool RedoOne() { if (g_redo.empty()) return false; Redo(); return true; }
        size_t UndoSize() { return g_undo.size(); }
        size_t RedoSize() { return g_redo.size(); }
        bool HistoryView(bool redo, size_t entry, size_t act, ActView* out) {
            const auto& stack = redo ? g_redo : g_undo;
            if (entry >= stack.size() || act >= stack[entry].acts.size() || !out) return false;
            const auto& a = stack[entry].acts[act]; *out = { (int)a.kind, a.uid, a.group, a.group1, a.proj }; return true;
        }
        void SelectUid(int uid) { ClearSceneSelection(); g_sel.insert(uid); g_primary = g_lastClicked = uid; }
        void SelectAdd(int uid) { g_sel.insert(uid); g_primary = g_lastClicked = uid; }
        void ClearSelection() { ClearSceneSelection(); }
        int SelectionSize() { return (int)SceneSelectionCount(); }
        bool IsSelected(int uid) { return g_sel.count(uid) != 0; }
        void DeleteSelection() { DeleteSceneSelection(); }
        void ForgetSelection() { ::editor::ForgetSelection(); }
        void ResetHistory() { if (!PrepareHistoryMutation([]() { ResetHistory(); })) return; g_undo.clear(); g_redo.clear(); ClearSceneSelection(); }
        bool LoadProjectAction(const std::string& name, bool clearFirst) { return ::editor::LoadProjectAction(name, clearFirst); }
        bool PlaceGroupCopy(const proj_codec::Document& doc, Vec3 target, float yawDelta, float scale, const std::string& name) { return ::editor::PlaceGroupCopy(doc, target, yawDelta, scale, name); }
        void ResetPlacement() { if (!PrepareHistoryMutation([]() { ResetPlacement(); })) return; if (g_place.active) CancelCarried(false); g_place = Place{}; core::g_placing = false; core::PublishGroundPlacement({}); }
        void SetTickNow(DWORD tick) { g_tickOverride = tick; }
    }
#endif
}
