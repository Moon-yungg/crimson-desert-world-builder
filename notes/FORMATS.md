# Crimson Desert – Format-Notizen (Stand 2026-09-17)

## Archiv-Schicht (gelöst durch pycrimson)
- `meta/0.papgt` = Gruppenliste (Ordner `0000`…`0037`, Mods ab `0036+`, nur numerische Namen werden geladen).
  Sprach-Flag ist inzwischen 15 Bit breit (`0x7FFF`), pycrimson lokal gepatcht (`UNKNOWN_14`).
- `NNNN/0.pamt` = Dateiindex (Trie-Strings, Chunks = `N.paz`), Einträge: ChaCha20-Poly1305 optional, LZ4 optional.
- Gesamt: 2.120.279 Dateien. Top-Ordner: character, sound, leveldata (349k), actionchart, object, gamedata, ui, sequencer.
- `notes/filelist.txt` = komplette Liste `gruppe | pfad (crypto, compression)`.

## Reflection-Serializer (pycrimson `ReflectionParser`)
Selbstbeschreibend: jede Datei enthält Typtabelle (Name + Properties mit Typname, Kind, fixed_size, Flags),
danach Objekt-Infos (type_index, offset, size) und Objektdaten (Property-Bitmap + Werte).
Wichtige Basistypen: `Transform` = 10 floats (scale3, quat4, pos3), `TiledTransform` = 11 floats, `float3`, `SceneObjectUid` (4 B), `SceneObjectUuid` (16 B).

## PARC-Container (.palevel, .prefab, .pae, .parg, .pasg …)
```
Datei-Header (16 B): "PARC" u16 0x0008  u64 hash  u16 0
Chunk (16 B):        "PARC" u16 kind    10 × 0x00   -> direkt danach Reflection-Block (beginnt mit 0xFFFF)
```
- Objekt-Offsets im Reflection-Block sind relativ zum Reflection-Start (Parser muss mit `reader.seek(chunk+16)` starten, `has_shared_strings=True`).
- `.palevel`: Chunk kind `0x0800` = `SceneLevelDataReflect` (Name, Bounding-Box `_minVerts/_maxVerts`, Flags).
  Chunk kind `0x0802` = SceneObject-Baum: Wurzel `SceneObject`, Kinder in `_childSceneObjects`, Komponenten in `_components`.
  Typname eines platzierten Objekts = Prefab-Pfad (`/object/.../xyz.prefab`), Felder wie `SceneObject`:
  `_worldTransform`, `_tiledTransform`, `_sceneObjectUid`, `_sceneObjectUuid`, `_collisionWorldFilterLayer`, `_tag`.
  Komponenten gesehen: MeshComponent, EditorMeshComponent, SocketComponent, DecalComponent, TreeComponent, GimmickSpawnDataComponent.
- Parser: `scripts/parse_parc.py <datei> [out.json] [--types] [--debug]`.
- Weltkoordinaten sind groß (z.B. x=-7878, z=-2631); `_tiledTransform` enthält die Position relativ zur Kachel (x=-878).

## Level-Verzeichnisse
- `leveldata/bin__/rootlevel/*.palevel` (5.592) – benannte Level (Quests, Gimmicks, Straßen `roadlevel_*`, Phasen `*_phase00_00`).
- `leveldata/bin__/rootlevel/sectorlevel/…` – Sektor-Level der offenen Welt.
- `leveldata/rootlevel/sceneobjectphase/levelinfos/*.levelinfo` (15.659) – KEIN Reflection-Format; beginnt `02 00`, Einträge `u16 idx, 0xFFFF, u64, u64, u32` (vermutlich UID-Index). Noch offen.
- `.pas` (Splines), `.pastage` (Sequencer-Skripte), `.pai` (AI-Charts).

## EXE / RTTI
- `bin64/CrimsonDesert.exe` 375 MB, Sektionen umbenannt (`.idata` 84 MB exec, `.sbss` 253 MB RWX). Imagebase 0x140000000.
- 15.828 TypeDescriptors, 15.067 CompleteObjectLocators, 15.068 vtables (offline: `scripts/rtti_static.py` -> `notes/rtti_static.txt`).
- Keine Skriptsprache; PAScript = C++ Klassen in Namensräumen gameClientScript/engineScript/scriptComponent/uiCommonScript.
- Ultimate ASI Loader (winmm.dll, Vortex-Deploy) lädt `bin64/*.asi`. Unser Plugin: `asi/cdmodkit` -> `bin64/cdmodkit.asi`, Log in `bin64/cdmodkit/`.
- Achtung: winmm-Loader hängt sich auch in `crashpad_handler.exe` – Plugin muss Prozessnamen prüfen.

## Laufzeit: Objekte spawnen (EXE 1.0.0.2850, alle RVAs relativ zu 0x140000000)
- `SceneObjectManager::createSceneObjectFrom` = RVA `0x3A6E600`, 9 Argumente (MSVC x64):
  `SceneObject* create(SceneObjectManager* mgr, StaticString* tag, WeakPtr<SceneLevel>* level, RefCountedPtr<LoadingInfo>* info, ResourceReferencePath_Prefab* prefab, Transform* xf(10 float: scale3, quat4, pos3), bool, bool, bool)`
  Housing-Aufrufer (0x10EEB30) uebergibt: tag="Housing", level+info = genullte Puffer, flags 1,1,1. Rueckgabe wird als SceneObject* weiterverwendet (Refcount-Block bei +0x28, Refcount +0x10).
- `mgr` = `[[0x6C2D9F0] + 0xE0] + 0xEB0` (Muster aus 8 von 9 Aufrufern).
- String-Objekt = 1 Zeiger auf StringData `{ char* str; int32 len(-1=unbekannt); int32 pad; int32 refcount(+0x10) }`, leerer Singleton bei RVA 0x692E4C0.
- `StaticString::ctor` = RVA `0x1231110` `(void* this8, const char*, int=1, int=0x2FFFF)`.
- `ResourceReferencePath_Prefab::ctor` = RVA `0x1319EA0` `(void* this(~0x70 B), const StringObj* src)`; vtable 0x54B8F48. Prueft Endung (xml / prefab).
- Spieler: `[[[0x6C29760]] + 0x30] + 0x58` = Actor; Comps `+0x68` -> Transform-Komponente `+0x1A0` -> Pos `+0xB4` (parent-relativ), ParentEid `+0xC8`, ParentPos `+0xEC` (master-looter, MIT).
- Game-Thread-Pump: Pattern `48 8B C4 4C 89 48 ?? 48 89 50 ?? 55 41 56` (Movement-Tick, master-looter/Trinity).
- Tools: `scripts/xref.py <rva|string --str>` (rip-relative Xrefs, numpy), `scripts/disasm.py <rva> [n] [--func]`, `scripts/rtti_hier.py <Klasse>`.
- KORREKTUR Spieler (dieser Build): Global `0x6C2D9F0` -> `+0x30` ClientActorManager -> `+0x58` ClientUserActor -> `+0xD0` ClientChildOnlyInGameActor (Koerper in der Welt)
  -> `+0x68` Komponententabelle -> `+0x1A0` ClientTransformSyncActorComponent -> `+0xB4` Pos (3 float), `+0xC8` ParentEid. Verifiziert live mit `scripts/peek.py`.
  master-looters 0x6C29760 ist in diesem Build null. Komponententabelle des Kind-Actors: +0x20 Status, +0x40 CharacterControl, +0x58 Ai, +0xB8 Inventory, +0x1A0 TransformSync, +0x1B0 Input.
- `scripts/peek.py chain <rva> <off>...` / `dump <addr> <n>` liest den laufenden Prozess (kein Neustart noetig).
- Movement-Tick-Hook MUSS 8 uint64-Argumente durchreichen und das Original zuerst aufrufen (4-Arg-Wrapper = Crash beim Laden).
- KACHELN: Transform-Komponente speichert kachelrelative Position (+0xB4) und Kachelindex als int16-Paar (+0xC0 x, +0xC2 z). Kachel = 1000 Einheiten,
  world = tiled + tile*1000 (trunc toward zero). createSceneObjectFrom erwartet WELT-Koordinaten.
- createSceneObjectFrom Flags: (1,0,0) = synchroner Pfad (so laden die Sektor-Objekte), arg8=1 = asynchroner Task-Pfad (crasht von fremdem Thread).
  arg4 = Delegate {ctx +0, fn +8, ..., u8 +0x18}; wenn fn==0 wird nichts weiter aufgerufen. Rueckgabe = SceneObjectClient*.
- Tag (arg2) = 4-Byte IndexedString aus 0x1231110(this, "text", 1, 0x2FFFF).
- Prefab-Pfad bauen: sd = 0x1391FD0(len); strncpy_s(sd->str, len+1, path); holder=sd; 0x1238AB0(&tmp, &holder); 0x1319EA0(rrp, &tmp).

## MEILENSTEIN 2026-09-17 22:46: Live-Spawn funktioniert (Plugin v0.13)
Rezept (alles auf dem Game-Thread via Movement-Tick-Hook):
1. mgr = [[0x6C2D9F0]+0xE0]+0xEB0
2. tag = Zeiger auf genullten 0x80-Block (wie der Sektor-Loader bei 0x8593EB), NICHT der IndexedString-Index
3. arg3 (WeakPtr<SceneLevel>) = 64 Nullbytes, arg4 (Delegate) = 64 Nullbytes (nie aus einem Template kopieren: enthaelt Funktionszeiger)
4. Prefab-Pfad: 0x1391FD0(len) -> strncpy_s -> holder -> 0x1238AB0(&tmp,&holder) -> 0x1319EA0(rrp,&tmp)
5. Transform = {1,1,1, 0,0,0,1, world x,y,z}; Flags = (1,1,0)  -> mittleres Flag startet den Add-to-Level-Task; (1,0,0) erzeugt nur das Objekt unsichtbar
6. Rueckgabe SceneObjectClient*. Objekt ist rein visuell/statisch; Gimmick-Verhalten (breakable etc.) laeuft ueber die Actor-Schicht (GimmickSpawnDataComponent / Server TrocTr...), noch offen.
Sektor-Loader: Funktion 0x8590C0, Aufruf ueber vtable-Slot 0x158 des Objekts [[[0x6C2D9F0]+0xC8]]+0x200 (Wrapper 0x3A5B430 -> createSceneObjectFrom).
Add-to-Level-Pipeline (Plan B / spaeter): Funktionen um 0x3A6B2CF..0x3A6BFE2 (ProcessAddToLevelStart/Step0/1/2).

## SceneObject-API (Laufzeit, EXE 1.0.0.2850)
- `SceneObject::setWorldTransform` = RVA 0x261AF00 `(this, const Transform*, u8 a=0, u8 b=1)`: schreibt +0x1A4 (Welt) und +0x1CC (Kachel), dann
  vslot151 isInLevel? -> vslot119 remove -> vslot120 insert(b). Fuer unsere Objekte flackert das Ergebnis (halb entfernt), Kollision weg.
- `SceneObject::setEnable` = RVA 0x261B9A0 `(this, u8 enable)`: Bit 3 in Flags +0xFD, propagiert an Kinder (+0x118 Liste) und Render-Instanzen.
  Housing ruft (obj,1) nach dem Platzieren und (obj,0) beim Abriss (0x10F0040), danach nur noch Ref-Release (0x385020 auf Slot +0xF8).
- `SceneObject::attachChild` = 0x2621DC0 (parent, child) -> 0x2620F20(parent, child, identity, 0).
- `SceneObject::getWorldTransform` = 0x2619D00 (this, out40) (Kachel -> Welt).
- Housing-Zustandsmaschine: 0x10EDD50, Zustand in Datensatz +0xC0; Objekt-Ref bei +0xF8 (Control-Block = obj+0x28).
- Direkte Schreibzugriffe auf +0xA0/+0x1C0 aendern nichts Sichtbares (Render-Instanz liest eigene Kopie).
- Kollision wird beim Erzeugen aus der Transform gebaut; setWorldTransform dreht nur die Render-Instanz. Verifiziert v0.28: Yaw/Scale-Aenderung -> Objekt neu erzeugen, Position -> in-place (disable/setTransform/enable).
- Release-Paket: release/cdmodkit-vX.zip (bin64/cdmodkit.asi + bin64/cdmodkit/prefabs.txt + README). Build-Pruefung per Byte-Signaturen an 6 RVAs.

## Build 1.0.0.2944 (Update 2026-09-19) und Laufzeit-Signaturen (Plugin v0.31)
- Neue RVAs: createSceneObjectFrom 0x3B58120, setWorldTransform 0x26D4AC0, setEnable 0x26D5560, StringDataAlloc 0x1420470,
  PrefabPathCtor 0x13A7C50, PathNormalizeCtor 0x12C6B00, StaticStringCtor 0x12BED30, attachChild 0x26DB980, sectorLoader 0x8CFAE0, WorldGlobal 0x6D691B0.
- Das Plugin sucht seit v0.31 alles zur Laufzeit: 5 Prolog-Signaturen (notes/sigs.json, scripts/gen_sigs.py), Welt-Global ueber das
  Take-or-Steal-Muster, createSceneObjectFrom ueber den Namens-String + Unwind-Tabelle (FuncStartOf). Spielerkette per RTTI-Suche.
- Bei neuem Build: scripts/rebase.py (alte EXE als CrimsonDesert_old.exe) prueft, ob die Signaturen noch eindeutig treffen.


## Thumbnails / sizes (v0.37)
- scripts/render_thumbs.py renders data/thumbs/<fnv1a64(logical prefab path)>.png (256px, flat shaded, PIL painter's) and data/prefab_size.tsv (path	w	h	d in m).
  Geometry: prefab (PARC or reflection) -> SceneObject tree, _worldTransform (scale3, quat4, pos3) -> .pami XML <StaticMesh Path=...pam> -> cdmw parse_pam.
  Run from PowerShell or with MSYS_NO_PATHCONV=1 in Git Bash, otherwise "--only /object/" is rewritten to a Windows path and nothing is rendered.
- Plugin: core::ThumbFile() = same FNV-1a 64 over UTF-8; overlay::Thumb() decodes with stb_image, uploads via the DX12 command list, LRU cache (700 textures).
- Live dragging: setWorldTransform(xf, 0, 0) without setEnable toggling (console "livemode 0..3" switches the method); the disable/enable sequence
  re-adds the object asynchronously and shows the change only seconds later.

## In-game thumbnail generator (v0.38, asi/cdmodkit/thumbgen.cpp)
- Previews are rendered on the player's machine from the installed pack files; no images or sizes ship with the mod.
- Pack access in C++: meta/0.papgt (12-byte header, 12-byte entries, i32 name buffer length, cstring names) -> <group>/0.pamt memory
  mapped (header 12 bytes: crc u32, chunk count u16, u16, u8 + 3 encrypt bytes; chunk table 12 B each; u32-prefixed dir/file name tries;
  u32 dir count + 16 B dirs (name crc, name off, file start, file count); u32 file count + 20 B files (name off, chunk off, csize, usize,
  chunk id u16, flags u8 (low nibble compression 2=LZ4, high nibble crypto 3=ChaCha20), u8)).
  Directory lookup by Jenkins hashlittle (init len+0xDEBA1DCD) of the directory path, verified against the decoded trie string.
- Entry decrypt: counter = checksum(file name), nonce16 = counter x4, key = base key ^ nonce16[i%16] ^ (enc0^enc1^enc2),
  ChaCha20 with RFC 7539 layout (state[12] = counter, state[13..15] = nonce words). Then LZ4 block decompress to usize.
- Worker thread (below normal priority) renders on demand (selection in the Browser) first, then walks the whole index; results are
  recorded in bin64\cdmodkit\prefab_size.tsv (0 0 0 = no usable geometry) so nothing is retried on later starts.

## Preview formats (v0.91)
- .pam vertex: pos u16x3 over the header bbox, uv = two halves at +8/+10 (as CDMW). Until v0.90 uv was read as u16/65535 at +10:
  wrong for every mesh, only visible on atlas textures (cloth, props). Cache version 4 re-renders all old images.
- .pac (skinned mesh, CDMW parse_pac): PAR header, 8 section slots at 0x10 {u32 compressed size (0 = stored), u32 size}, sections
  back to back from 0x50, each LZ4 on its own when compressed ("partial" pack entries arrive raw like that). Section 0: one descriptor
  per submesh, found 35 bytes before the LOD pattern 04 00 01 02 03 (3/2-LOD variants too): u8 1, floats at +3 (bbox min at [2..4],
  extent [5..7]), u16 vertex counts at +40, u32 index counts at +44/46/48. Sections 4..1 = LOD 0..3: 40-byte vertices (pos u16x3,
  v = min + u16/32767 * extent, uv halves at +8/+10, packed normal u32 at +16, bone slots +20/+24) then u16 indices.
  Validated: 300/300 random .pac give CDMW's vertex and face counts. Characters face the other way than props (preview camera 215 deg).
- Materials: .pami <MaterialParameterX Name= Value=>; .pac -> character/modelproperty/<same path>.pac_xml,
  <SkinnedMeshMaterialWrapper _subMeshName> with <MaterialParameterX _name= _value=>, textures as nested _path=; the first
  <ModelProperty Index="0"> is the default look, later ones are variants. Submesh names match case-insensitively.
- Dye: many character/monster materials have no _baseColorTexture. Colour = _colorBlendingMaskTexture (_ma, DXT1) r/g/b weights of
  _tintColorR/G/B ("#rrggbbaa"), overlay-blended with the grey _overlayColorTexture (_o). Props: base texture * _tintColor ("r g b").
- Textures: _n normal maps BC5 (x, y; DirectX green), _sp = r ambient occlusion, g roughness, b metal (DXT1), emissive BC4.
- Sub-prefab: a child object whose reflection type name is a prefab path ("/object/.../x.prefab") with its own _worldTransform;
  the preview expands it in place. prefabs.tsv tags it "SubPrefab" and does not count its meshes.
- Characters are assembled at runtime from character/appearance/.../*.app_xml (<Nude>, <Head>, <Hair>, <Armor> list prefab names).
- Game data tables: gamedata/binarystaticinfo__/bin/<name>.staticinfoheader + .staticinfobody (characterinfo, faction,
  factionrelationgroup, allygroupinfo, aiactionattributeinfo, dropsetinfo, ...), 134 files. Header = row directory: count
  (1, 2 or 4 bytes) + per row key (1/2/4/8/12 bytes) + u32 body offset; widths resolved against the body, where every row
  repeats its key first (CDMW structured_binary_editor). Rows are packed structs without a type table; field names exist in
  the exe as (class, field) string pairs ("FactionInfo" / "_factionRelationGroupInfo"), types and order not decoded.
  scripts/staticinfo_dump.py writes all tables as TSV (gitignored notes/staticinfo/).
- String tables (v0.93): gamedata/stringtable/binary__/<lang>/<table>.paloc (eng ger fre spa-es por-br rus tur kor jpn
  zho-cn zho-tw ara ita pol spa-mx; 39 files each). "paloc" header, u32 stored size @9, u32 declared size @13, LZ4 from 17.
  The stream starts with offset-0 matches (invalid LZ4; the reference decoder copies its zeroed buffer: a run of zeros of
  decoded - declared bytes), then CDMW's records: u32 category, u32 reserved, u32 key length + key, u32 text length + text,
  u32 count at the end. Keys are global u64 in text form: (row key << 32) | field tag. Only the key a table row stores itself
  (or row key << 32 for gimmicks/characters) is reliable; guessing tags returns other tables' texts.
- In-game names in the browser: gimmickinfo row strings (prefab path + name key) + <lang>/gimmick.paloc, read at runtime
  through the game's loader. 11,114 of 13,941 gimmicks share a name; the tokens after the group's common ones are appended.
- Appearances (v0.93): prefabs.tsv rows "/character/appearance/.../x.app_xml" (scripts/build_appearance_index.py);
  the preview is the union of the prefabs listed under <Nude>/<Head>/<Hair>/<Armor> (flight cloak left out).
- Decals: DecalComponent _offsetTransform (box: scale, quat, pos; missing = 1 m box) + DecalInfo _textureFilename
  (DXT5 _dec.dds); previewed as a textured quad in the box's XZ plane, camera from above.
- Partial textures (v0.93): ~13 % of DDS entries are stored "partial": the first up to four mips are LZ4 blocks of their own,
  stored sizes in the DDS header's reserved1[0..3] (one block {stored, full} in reserved1[0..1] when mips <= 5 or an array).
  The game's loader hands them over as stored. The renderer plans the stored offset of its mip (PlanDds) and reads only the
  header plus that tail (core::GameReadFileRange, read() with offset/length), enabled by a start-up byte-compare self test.
  Offline: 1093 -> 219 MB read for 220 prefabs, identical images.

## TiledTransform und setWorldTransform (v0.45, Build 2944)
- SceneObject::setWorldTransform(this, TiledTransform*, u8 a, u8 b) erwartet 44 Bytes: scale3, quat4, pos3 (relativ zur Kachel), int16 tileX, int16 tileZ.
  Die Funktion liest das Kachel-Paar bei +0x28 (mov eax,[rbx+0x28]). Bis v0.44 uebergab das Plugin 40 Bytes, das Kachel-Paar war Stack-Muell:
  in-place bewegte Objekte landeten in zufaelligen Kacheln (unsichtbar / "Flackern"), nur Neu-Erzeugen war zuverlaessig.
- Housing-Code (VHousingPlaceContext, vtable 0x5575f60; Aufrufer z.B. rva 0x3cb3ee, 0x3cbd59, 0x3cc799) ruft vor jedem setWorldTransform
  rva 0x50ea00 = TiledTransform::normalize: tx = trunc(pos.x * 0.001), tz = trunc(pos.z * 0.001), tile += (tx,tz), pos -= tile*1000. Flags immer (0,1).
  Objektzeiger = Control-Block - 0x28 (lea rcx,[r8-0x28]).
- Plugin: MakeTransform() erzeugt jetzt die normalisierte TiledTransform (xf[12]), fuer create und setWorldTransform.
- RE-Hilfe: Konsole "trace on|off" loggt die setWorldTransform/setEnable-Aufrufe des Spiels mit Aufrufer-RVA und die Transform des zuletzt
  vom Spiel erzeugten Objekts pro Tick.

## Dateien ueber den Spiel-Loader lesen (v0.53, kein Archiv-Code / Schluessel mehr in der Mod)
- ResourceLoader::load = rva 0x12d0130 (Signatur "48 89 5C 24 18 48 89 54 24 10 55 56 57 41 56 41 57 48 81 EC 90 ..."): (this, Resource** out, NormalizedPath* path, u32 flags)
  fragt alle LoadWorker (this+0x10 Array, Anzahl +0x18) per vslot2 und liefert einen ResourceHandler_Paz.
  Instanz: per Hook auf dieselbe Funktion aus den Aufrufen des Spiels abgegriffen (this). Oeffentlicher Einstieg der Spiel-Systeme = vslot8 (0x12d03d0).
- ResourceHandler_Paz: +0x20 Worker (ResourceLoadWorker_Package), +0x34 / +0x38 Groessen (partial: +0x34, sonst +0x38 = entpackt), +0x3c Flags
  (Low-Nibble Kompression 0/1 partial/2 LZ4, High-Nibble Crypto). Freigabe: vslot0(handler, 1).
- Worker vslot5 (0x13a5d30) = read(worker, handler, u8* buf, u32 capacity, u32 offset, u32 length): liest, entschluesselt, entpackt in den Puffer.
  vslot4 (0x13a5c70) ist die Variante mit game-eigenem Buffer-Objekt {ptr, u32 size, u32 cap, u8 own} + Allocator (statisches Objekt rva 0x557346c).
- Pfadobjekt wie in DoSpawn: StringDataAlloc -> strncpy -> PathNormalizeCtor. Pfade ohne fuehrenden Slash ("object/bin__/...", "PAConfig.txt").
- Eigene Entschluesselung, LZ4, pamt/paz-Index aus thumbgen.cpp entfernt. Validiert: 107/117 neu gerenderte Bilder byte-identisch, Rest nur Flaechen-Stichprobe.
- Vorsicht: ein ReadFile-Hook (kernel32) laesst NvMessageBus.dll abstuerzen; nur mit traceio.flag fuer Analyse installieren.

## Editor-Architektur v0.54+ (Kurz)
- Registry: SpawnedObj.uid stabil (Indizes verschieben sich bei Forget), group; SpawnAt reserviert den Eintrag sofort und liefert die uid.
- MoveMany(reqs, final): ein Game-Thread-Job fuer viele Objekte (Gruppen-Grab). Live-Updates werden verworfen, wenn >2 Jobs warten.
- Undo/Redo im Editor (Spawn/Move/Delete als Act-Listen), Copy/Paste relativ zum Auswahl-Schwerpunkt, Linie/Kreis ueber SpawnSet.
- Projekt v2: prefab|x|y|z|yaw|scale|group, Gruppen-IDs werden beim Laden neu vergeben.
- Query_TerrainForHousing ist nur der Name eines Collision-Query-Enums (Registrierung bei rva 0x267bac7 / 0x267e936, Wert 0x5d/0x5e), keine Raycast-Funktion.
- PlayerCameraComponent vtable 0x560bca0 (145 Slots); Blickrichtung noch unbekannt -> Konsole "camtrace" loggt Kandidaten.
