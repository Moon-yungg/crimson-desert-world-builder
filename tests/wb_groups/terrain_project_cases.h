// Actual editor save dispatch + actual core/terrain registry + real synthetic project files.
#pragma once
static std::string TerrainStateValue() {
    std::string value;
    for (const auto& s : core::TerrainStrokes())
        value += std::to_string(s.mode) + ":" + std::to_string(s.x) + ":" + std::to_string(s.z) + ":" +
            std::to_string(s.r) + ":" + std::to_string(s.amount) + ":" + std::to_string(s.strength) + ":" +
            std::to_string(s.ax) + ":" + std::to_string(s.az) + ":" + std::to_string(s.y) + ":" + std::to_string(s.proj) + ";";
    return value;
}
static void CaseTerrainSaveScopes() {
    BeginCase("main-terrain-project-save-scopes");
    Require(WriteTextFile(ProjPath("terrain-a"), "# cdmodkit project v3: prefab|x|y|z|yawDeg|scale|group|pitchDeg|rollDeg (absolute world coordinates)\n#terrain|0|10|20|2|1|1|10|20|0\n"), "terrain-legacy-file");
    const bool loaded=core::LoadProject("terrain-a", false);
    Require(loaded, "terrain-only-project-load", core::ProjectError());
    const int a = core::ProjectId("terrain-a"), b = core::ProjectId("terrain-b");
    core::TerrainStroke other{}; other.x = 30; other.r = 2; other.strength = 1;
    core::TerrainReplaceProject(b, {other});
    const int otherObject = SpawnCore("/object/other.prefab", {30, 4, 5}, {}, 1, 0, b); PumpAll();
    Require(core::SaveProject("terrain-b", core::SaveProjectOnly), "unrelated-project-file-saved");
    const auto otherBytes = FileText(ProjPath("terrain-b"));
    core::TerrainStroke fresh = other; fresh.x = 50; core::TerrainReplaceProject(0, {fresh});
    Check("terrain-only-composite-counts-include-strokes", core::ProjectObjectCount(a) == 1 && core::ProjectObjectCount(0) == 1);
    // Supply a deliberately old object-only display snapshot. Execution must read live terrain ownership.
    editor::g_projectLibrary = {};
    Require(editor::DispatchProjectAction({"terrain-a", FK::Project}, editor::ProjectAction::SaveBack), "terrain-only-real-editor-saveback");
    proj_codec::Document saved; std::string error;
    Require(proj_codec::Parse(FileText(ProjPath("terrain-a")), ProjPath("terrain-a"), FK::Project, saved, error), "parse-terrain-saveback", error);
    Check("saveback-keeps-only-target-and-new-terrain", saved.records.empty() && saved.terrain.size() == 2 && saved.terrain[0].x == 10 && saved.terrain[1].x == 50);
    Snapshot(); const auto* object = Rec(otherObject);
    Check("saveback-does-not-adopt-unrelated-object", object && object->proj == b);
    Check("saveback-leaves-unrelated-file-exact", FileText(ProjPath("terrain-b")) == otherBytes);
    auto strokes = core::TerrainStrokes();
    Check("saveback-adopts-new-strokes-only", std::count_if(strokes.begin(), strokes.end(), [a](const auto& s) { return s.proj == a; }) == 2 &&
          std::count_if(strokes.begin(), strokes.end(), [b](const auto& s) { return s.proj == b; }) == 1);
    fresh.x = 70; core::TerrainReplaceProject(0, {fresh});
    Require(editor::SaveProjectAction("terrain-new", core::SaveNewOnly), "terrain-new-only-real-save-action");
    Require(proj_codec::Parse(FileText(ProjPath("terrain-new")), ProjPath("terrain-new"), FK::Project, saved, error), "parse-terrain-new-only", error);
    Check("new-only-writes-unassigned-terrain", saved.records.empty() && saved.terrain.size() == 1 && saved.terrain[0].x == 70);
    Check("new-only-preserves-other-project-file", FileText(ProjPath("terrain-b")) == otherBytes);
    const std::string before = TerrainStateValue();
    host::SetSaveFault(host::SaveFault::ReplaceAbort);
    Check("terrain-save-failure-reported", !core::SaveProject("terrain-a", core::SaveProjectOnly));
    Check("terrain-save-failure-no-adoption", TerrainStateValue() == before);
    host::SetSaveFault(host::SaveFault::None);
    core::DeleteAllSpawned(); PumpAll();
    Require(core::LoadProject("terrain-a", true), "terrain-roundtrip-load-after-clear");
    Check("terrain-roundtrip-restores-strokes-without-objects", core::TerrainStrokes().size() == 2 && core::ProjectObjectCount(a) == 2 && core::Spawned().empty() && core::ManagedNpcs().empty());
}
static void CaseTerrainFileGuard() {
    BeginCase("main-terrain-only-file-guard");
    const auto file = Entry(FK::Project, false, "terrain-reference.cdproj");
    MakeFile(file, "#terrain|0|10|20|2|1|1|10|20|0\n");
    core::SavedLibrarySnapshot old;
    Require(core::RefreshSavedLibrary(old).ok(), "terrain-file-old-empty-snapshot");
    const auto selection = Select(file);
    const int pid = core::ProjectId("terrain-reference");
    core::TerrainStroke stroke{}; stroke.r = 2; stroke.x = 10; stroke.strength = 1;
    core::TerrainReplaceProject(pid, {stroke});
    const auto terrainBefore = TerrainStateValue();
    Check("terrain-reference-counted-without-object-record", core::ProjectObjectCount(pid) == 1 && core::Spawned().empty() && core::ManagedNpcs().empty());
    Refusal("terrain-live-archive-refusal", selection, FA::Archive, FR::VisibleReference);
    Refusal("terrain-live-delete-refusal", selection, FA::Delete, FR::VisibleReference, file.filename);
    Check("terrain-refusals-preserve-all-stroke-values", TerrainStateValue() == terrainBefore);
    core::SavedLibrarySnapshot current;
    Require(core::RefreshSavedLibrary(current).ok(), "terrain-file-current-snapshot");
    auto row = std::find_if(current.entries.begin(), current.entries.end(), [&](const auto& r) { return r.file.path == file.path; });
    Require(row != current.entries.end(), "terrain-file-current-row");
    Check("terrain-ownership-separate-from-visible-hidden", row->ownership.terrain == 1 && row->ownership.visible == 0 && row->ownership.hidden == 0);
    core::TerrainReplaceProject(pid, {});
    Require(Act(selection, FA::Archive).ok(), "archive-after-actual-terrain-reference-removed");
    const auto archived = Entry(FK::Project, true, file.filename);
    Check("terrain-file-move-keeps-exact-bytes", FileText(archived.path) == "#terrain|0|10|20|2|1|1|10|20|0\n");
}
