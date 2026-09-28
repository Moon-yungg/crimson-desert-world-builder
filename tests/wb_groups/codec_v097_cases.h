// Released v0.97 disk schema exercised through the actual codec and Windows transactional writer.
#pragma once
static const std::string kV097Mixed = kV097Header +
    "#group|42|7465616d20ed959c\n#group|7|7365636f6e64\n"
    "#npc|4294967295|4|5|6|255|4294967295|0|1|42|4e504320ed959c|6e70637c6e6f74650aed959c\n"
    "#terrain|1|10|20|3|-2|0.5|11|21|9\n"
    "/object/a.prefab|1.25|2.5|3.75|37|1.5|42|19|-8|6f626a6563747c6e6f74650aed959c\n"
    "#npc|17|7|8|9|1|0|1|0|7\n";
static void CheckV097Mixed(const Document& doc, bool canonical) {
    Check(doc.kind == Kind::Project && doc.records.size() == 1 && doc.npcs.size() == 2 && doc.terrain.size() == 1,
          "mixed v4 keeps separate object/NPC/terrain record families");
    if (doc.records.size() != 1 || doc.npcs.size() != 2 || doc.terrain.size() != 1) return;
    const auto& object = doc.records[0]; const auto& npc = doc.npcs[0]; const auto& terrain = doc.terrain[0];
    const int first = canonical ? 1 : 42, second = canonical ? 2 : 7;
    Check(object.prefab == "/object/a.prefab" && object.pos.x == 1.25 && object.pos.y == 2.5 && object.pos.z == 3.75 &&
          object.yaw == 37 && object.scale == 1.5 && object.pitch == 19 && object.roll == -8 &&
          object.note == "object|note\n\xED\x95\x9C", "noted object retains every disk value");
    Check(npc.key == UINT32_MAX && npc.extra == UINT32_MAX && npc.pos.x == 4 && npc.pos.y == 5 && npc.pos.z == 6 &&
          npc.type == 255 && !npc.aiEnabled && npc.behavior == 1 && npc.label == "NPC \xED\x95\x9C" &&
          npc.note == "npc|note\n\xED\x95\x9C", "NPC retains key/type/extra/control and UTF-8 metadata");
    Check(doc.npcs[1].key == 17 && doc.npcs[1].aiEnabled && doc.npcs[1].behavior == 0 &&
          doc.npcs[1].label.empty() && doc.npcs[1].note.empty(), "optional NPC text fields take empty defaults");
    Check(object.group == first && npc.group == first && doc.npcs[1].group == second,
          "one partition mapping spans object and NPC records");
    const auto firstName = doc.groupNames.find(first), secondName = doc.groupNames.find(second);
    Check(doc.groupNames.size() == 2 && firstName != doc.groupNames.end() && secondName != doc.groupNames.end() &&
          firstName->second == "team \xED\x95\x9C" && secondName->second == "second", "group names follow the shared partition mapping");
    Check(terrain.mode == 1 && terrain.x == 10 && terrain.z == 20 && terrain.r == 3 && terrain.amount == -2 &&
          terrain.strength == .5 && terrain.ax == 11 && terrain.az == 21 && terrain.y == 9, "all terrain disk values survive");
}
static void RunV097Cases(const std::string& dir) {
    std::string error;
    BeginCase("CODEC-V097-MIXED");
    {
        Document parsed;
        Check(Parse(kV097Mixed, "mixed.cdproj", Kind::Project, parsed, error), "real upstream-shaped mixed v4 parses");
        CheckV097Mixed(parsed, false);
        std::vector<EngineRow> rows;
        Check(NarrowForEngine(parsed, rows, error) && rows.size() == 1 && rows[0].group == 42,
              "NPC and terrain never consume object engine ordinals");
        std::string encoded; Document back;
        Check(Serialize(parsed, encoded, error) && encoded.rfind(kV097Header, 0) == 0, "projects emit the upstream v4 header");
        Check(Parse(encoded, "mixed.cdproj", Kind::Project, back, error), "serialized mixed v4 reparses");
        CheckV097Mixed(back, true);
        std::string again;
        Check(Serialize(back, again, error) && again == encoded, "mixed metadata write/read/write is byte stable");
        Document wrapped = FullGroupDocument(); wrapped.kind = Kind::Project;
        wrapped.npcs = parsed.npcs; wrapped.groupNames = parsed.groupNames; wrapped.terrain = parsed.terrain;
        wrapped.records[0].note = parsed.records[0].note;
        Check(Serialize(wrapped, encoded, error) && CountOf(encoded, "# wb-member record=") == 5,
              "WB envelopes coexist with NPC/terrain without shifting member ordinals");
        Check(Parse(encoded, "wrapped.cdproj", Kind::Project, back, error) && back.records.size() == 5 &&
              back.envelopes.size() == 2 && back.npcs.size() == 2 && back.terrain.size() == 1,
              "WB project envelope relations survive the v4 superset");
    }
    EndCase();

    BeginCase("CODEC-V097-SINGLE-KIND");
    {
        for (const std::string& suffix : {std::string(), std::string("|6c6162656c"), std::string("|6c6162656c|6e6f7465")}) {
            Document npcOnly;
            Check(Parse(kV097Header + "#npc|17|1|2|3|1|0|1|0|0" + suffix + "\n", "npc.cdproj", Kind::Project, npcOnly, error),
                  "NPC-only v4 accepts each optional text width");
            Check(npcOnly.records.empty() && npcOnly.npcs.size() == 1 && !npcOnly.hasBounds,
                  "NPC-only project needs no object metadata");
        }
        Document terrainOnly, ordinary;
        Check(Parse(kV097Header + "#terrain|0|1|2|3|4|1\n", "terrain.cdproj", Kind::Project, terrainOnly, error) &&
              terrainOnly.records.empty() && terrainOnly.terrain.size() == 1, "terrain-only v4 retains legacy trailing defaults");
        Check(Parse(kV097Header + "/object/a.prefab|1|2|3|0|1|0|0|0\n", "plain.cdproj", Kind::Project, ordinary, error) &&
              ordinary.records.size() == 1 && !ordinary.hasBounds, "ordinary v4 object rows need no WB bounds");
        std::string text;
        Check(Serialize(ordinary, text, error), "metadata-free ordinary v4 project is writable");
        Document group = FullGroupDocument(); group.groupNames[42] = "group \xED\x95\x9C"; group.records[4].note = "member|note";
        Check(Serialize(group, text, error) && text.rfind(kGroupHeader, 0) == 0 && CountOf(text, "# wb-note record=5 ") == 1,
              "object-only group keeps WB v3 rows with explicit note metadata");
        Document back;
        Check(Parse(text, "group.cdgroup", Kind::Group, back, error) && back.records.size() == 5 &&
              back.records[4].note == "member|note" && back.groupNames.at(1) == "group \xED\x95\x9C",
              "group notes and named partitions roundtrip");
        group.npcs.push_back(NpcRecord{}); group.npcs[0].key = 1;
        text = "unchanged";
        Check(!Serialize(group, text, error) && text == "unchanged", "group refuses NPC data transactionally");
        group.npcs.clear(); group.terrain.push_back({0, 1, 2, 3, 4, 1, 0, 0, 0});
        Check(!Serialize(group, text, error) && text == "unchanged", "group refuses terrain data transactionally");
    }
    EndCase();

    BeginCase("CODEC-V097-MALFORMED");
    {
        for (const auto& row : std::vector<std::string>{
            "#npc|0|1|2|3|1|0|1|0|0", "#npc|4294967296|1|2|3|1|0|1|0|0",
            "#npc|1|nan|2|3|1|0|1|0|0", "#npc|1|1|2|3|0|0|1|0|0",
            "#npc|1|1|2|3|256|0|1|0|0", "#npc|1|1|2|3|1|4294967296|1|0|0",
            "#npc|1|1|2|3|1|0|2|0|0", "#npc|1|1|2|3|1|0|1|2|0",
            "#npc|1|1|2|3|1|0|1|0|-1", "#npc|1|1|2|3|1|0|1|0|0|0",
            "#npc|1|1|2|3|1|0|1|0|0|zz", "#npc|1|1|2|3|1|0|1|0|0|c080",
            "#npc|1|1|2|3|1|0|1|0|0|00", "#npc|1|1|2|3|1|0|1|0|0|||extra",
            "#group|7|6e616d65", "#group|0|6e616d65", "#group|7|zz"})
            Bad(kV097Header + row + "\n", "bad.cdproj", Kind::Project, "0", "malformed reserved v4 metadata is refused");
        Bad(kV097Header + "#group|7|61\n#group|7|62\n#npc|1|1|2|3|1|0|1|0|7\n",
            "bad.cdproj", Kind::Project, "0", "duplicate named partition is refused");
        for (const char* invalid : {"0", "zz", "c080", "00"})
            Bad(kV097Header + "/object/a.prefab|1|2|3|0|1|0|0|0|" + invalid + "\n",
                "bad.cdproj", Kind::Project, "1", "invalid object note hex/UTF-8 is refused at its object ordinal");
        Document huge; NpcRecord npc; npc.key = 1; huge.npcs.push_back(npc);
        for (double coordinate : {1e300, 1e10}) {
            huge.npcs[0].pos.x = coordinate; std::vector<EngineRow> rows(1); rows[0].group = 123;
            Check(!NarrowForEngine(huge, rows, error) && rows.size() == 1 && rows[0].group == 123,
                  "NPC float/tile overflow rejects before any engine-row output mutation");
        }
    }
    EndCase();

    BeginCase("CODEC-V097-SEMANTIC-VALUES");
    {
        Document original; Check(Parse(kV097Mixed, "mixed.cdproj", Kind::Project, original, error), "semantic fixture parses");
        Document dialect = original; dialect.legacy = !dialect.legacy;
        Check(SameValues(original, dialect), "semantic equality ignores only the dialect flag");
        const std::vector<std::function<void(Document&)>> changes = {
            [](Document& d){d.records[0].note += "!";}, [](Document& d){d.groupNames[42] += "!";},
            [](Document& d){d.npcs[0].key = 9;}, [](Document& d){d.npcs[0].pos.x += 1;},
            [](Document& d){d.npcs[0].pos.y += 1;}, [](Document& d){d.npcs[0].pos.z += 1;},
            [](Document& d){d.npcs[0].type = 2;}, [](Document& d){d.npcs[0].extra = 9;},
            [](Document& d){d.npcs[0].aiEnabled = true;}, [](Document& d){d.npcs[0].behavior = 0;},
            [](Document& d){d.npcs[0].group = 7;}, [](Document& d){d.npcs[0].label += "!";},
            [](Document& d){d.npcs[0].note += "!";}, [](Document& d){d.terrain[0].mode = 0;},
            [](Document& d){d.terrain[0].x += 1;}, [](Document& d){d.terrain[0].z += 1;},
            [](Document& d){d.terrain[0].r += 1;}, [](Document& d){d.terrain[0].amount += 1;},
            [](Document& d){d.terrain[0].strength += .1;}, [](Document& d){d.terrain[0].ax += 1;},
            [](Document& d){d.terrain[0].az += 1;}, [](Document& d){d.terrain[0].y += 1;}
        };
        for (const auto& change : changes) { Document changed = original; change(changed); Check(!SameValues(original, changed), "appended metadata participates in semantic equality"); }
    }
    EndCase();

    BeginCase("CODEC-V097-TRANSACTION");
    {
        const auto workspace = std::filesystem::path(dir) / "v097-write"; std::filesystem::create_directories(workspace);
        const std::string destination = (workspace / "mixed.cdproj").string();
        Document original; Check(Parse(kV097Mixed, "mixed.cdproj", Kind::Project, original, error), "transaction mixed fixture parses");
        std::string oldBytes; Check(Serialize(original, oldBytes, error), "transaction old mixed snapshot serializes");
        Document changed = original; changed.records[0].note = "new object note"; changed.npcs[0].label = "new label"; changed.npcs[0].note = "new NPC note"; changed.groupNames[42] = "new group";
        std::string next; Check(Serialize(changed, next, error), "transaction changed metadata serializes");
        for (int fault = 0; fault < 6; ++fault) {
            Check(WriteFile(destination, oldBytes), "seed exact prior mixed bytes");
            const WriteStage stages[] = {WriteStage::Write, WriteStage::Flush, WriteStage::Close, WriteStage::Readback, WriteStage::Replace};
            WriteHooks hooks;
            hooks.fault = [&](WriteStage stage, const std::string& temp, size_t& length, std::string& failure) {
                if (fault == 5) { if (stage == WriteStage::Write) length /= 2; return true; }
                if (stage != stages[fault]) return true;
                if (stage == WriteStage::Readback) return WriteFile(temp, oldBytes); // valid but wrong semantic snapshot
                failure = "record 0: injected mixed write refusal"; return false;
            };
            Check(!WriteTransactional(destination, Kind::Project, next, &hooks, error), "each mixed-file write fault is reported");
            Check(ReadFile(destination) == oldBytes && FileCount(workspace.string()) == 1,
                  "mixed-file failure preserves prior bytes and removes owned temporary");
        }
        Check(WriteTransactional(destination, Kind::Project, next, nullptr, error) && ReadFile(destination) == next,
              "mixed-file transaction recovers after every injected failure");
        Document readback; Check(Parse(ReadFile(destination), destination, Kind::Project, readback, error), "successful mixed-file replacement reparses");
        Check(readback.records.size() == 1 && readback.npcs.size() == 2 && readback.records[0].note == "new object note" &&
              readback.npcs[0].label == "new label" && readback.npcs[0].note == "new NPC note" && readback.groupNames.at(1) == "new group",
              "successful replacement retains every changed metadata family");
    }
    EndCase();
}
