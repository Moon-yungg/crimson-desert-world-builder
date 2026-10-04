# World Builder – In-Game Prefab-Editor für Crimson Desert (Testversion)

Spawnt, verschiebt, dreht, skaliert und löscht Objekte (Prefabs) live in der Spielwelt, speichert Aufbauten als Projekte
und bietet anderen Mods eine kleine C-API (`sdk\cdmodkit_api.h`).
Die Spielfunktionen werden beim Start über Byte-Signaturen gesucht (getestet mit CrimsonDesert.exe 1.0.0.2850 und 1.0.0.2944).
Findet das Plugin nach einem Spiel-Update eine Funktion nicht, schaltet es sich selbst ab: im Overlay erscheint ein roter Hinweis,
im Log steht `RESOLVE FAILED`, und nichts wird gehookt.

## Installation

1. **Ultimate ASI Loader** installieren, falls noch nicht vorhanden. Er liegt im Spielordner unter
   `bin64\winmm.dll`. Wer den Definitive Mod Manager (DMM) oder Vortex für Crimson Desert benutzt, hat ihn meistens schon.
   Manuell: `Ultimate-ASI-Loader_x64.zip` von https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases laden,
   die enthaltene `dinput8.dll` nach `bin64\winmm.dll` kopieren (umbenennen).
2. Aus diesem Zip nach `<Spiel>\bin64\` kopieren:
   - `cdmodkit.asi`
   Der Ordner `cdmodkit\` und `settings.txt` werden beim ersten Start automatisch erzeugt. Der Ordner `sdk\` im Zip ist nur
   für Mod-Entwickler und muss nicht kopiert werden.
3. Spiel normal über Steam starten. Das Log liegt in `bin64\cdmodkit\cdmodkit.log`; ein Konsolenfenster gibt es nur mit `console=1` in der automatisch erzeugten `settings.txt`.

Prefab-Index, Fehlernamen und Übersetzungen sind direkt in `cdmodkit.asi` eingebettet und werden nicht als separate Dateien installiert oder erzeugt.

Die Oberflächensprache lässt sich im Tab **Settings** unter **Language** ändern. Es gibt Englisch, vereinfachtes und traditionelles Chinesisch, Deutsch, Französisch, Koreanisch, Japanisch, Spanisch, brasilianisches Portugiesisch, Russisch und Türkisch sowie die automatische Erkennung der Systemsprache. Die Namen in der Auswahlliste werden in der jeweils gewählten Oberflächensprache angezeigt.

## Vorschaubilder

Die Mod enthält keine Spieldaten und keinen Archiv-Code. Die Dateien lädt das Spiel selbst über seinen eigenen Resource-Loader,
die Mod rendert daraus beim ersten Start die Vorschaubilder und unter `bin64\cdmodkit\thumbs\` abgelegt (ca. 47.000 Bilder inklusive Figuren
und Rüstungen, rund eine Stunde im Hintergrund mit niedriger Priorität).
Sichtbare Kacheln und das im Browser ausgewählte Prefab werden immer zuerst gerendert. Den Fortschritt zeigt die Kachelansicht
oben an, mit eingeschaltetem Detailbereich steht er auch neben dem ausgewählten Prefab ("previews x of y").
Im Tab "Settings" stellt "preview quality" die Qualität ein (Grundfarbe, + Färbung, + Normal-Maps, + Glanz und Leuchten; Standard ist die beste Stufe).
Eine niedrigere Stufe macht nur den Hintergrund-Durchlauf schneller.
Vorschauen von Figuren, Monstern und Rüstungsteilen erscheinen erst, wenn der Filter "meshes only" aus ist.
Zum Neuaufbau einfach den Ordner `thumbs` und die Datei `prefab_size.tsv` löschen.

## Bedienung

- Spielstand laden, dann **Einfg** (Insert) drücken: Fenster "World Builder" erscheint im **Bearbeitungsmodus**. Die Welt läuft
  weiter, aber Maus und Tastatur gehören komplett dem Menü (die Figur reagiert nicht, der Cursor ist sichtbar).
- **Pos1** (Home) schaltet bei offenem Editor die **freie Kamera** ein und aus (wie der Button "free camera"). In Settings kann
  sie optional beim Öffnen des Editors automatisch starten ("start free camera when opening the editor"). Beide Tasten sind im
  Tab "Settings" umbelegbar, dort lässt sich auch das Konsolenfenster abschalten (gespeichert in `bin64\cdmodkit\settings.txt`).
- **Browser:** Kategorien links (Breite ziehbar), Suche und Tag-Filter oben, Favoriten mit dem Stern. Eintrag anklicken zeigt
  Vorschau, Größe in Metern und Tags. Versatz zum Spieler, Yaw und Scale einstellen, dann "SPAWN" oder Doppelklick.
  "meshes only" blendet Prefabs ohne sichtbares Mesh aus (Standard). Rechtsklick auf einen Eintrag: Favorit, Sammlung hinzufügen.
  "show selected item details panel" in Settings blendet unter der Liste einen Detailbereich zum gewählten Prefab ein.
  Unter den Kategorien stehen deine **Sammlungen**: Name eingeben, "add". SPAWN setzt das Objekt vor dir ab, Abstand nach Objektgröße.
  Interaktive Objekte (Gimmicks) stehen unter ihrem Namen aus dem Spiel, in der Sprache der Oberfläche (z.B. "Privates Lager");
  gleichnamige bekommen den unterscheidenden Teil des Dateinamens angehängt. Die Suche findet sie auch über diesen Namen,
  die Listenansicht zeigt den Dateinamen in der Spalte "prefab".
- **Figuren:** Unter character › appearance stehen alle Figuren des Spiels (Spielfiguren, NPCs, Monster, Tiere, Reittiere)
  mit einer Vorschau der kompletten Figur aus Körper, Kopf, Haaren und Rüstung. Vorerst nur als Vorschau, Spawnen folgt später.
- **Kachelansicht:** Knopf "cards" links neben der Suche zeigt die Treffer als Kacheln mit Vorschaubild (Größe per Regler),
  gleiche Filter wie die Liste. Die Vorschauen werden mit den echten Texturen des Spiels gerendert; beim ersten Start mit
  dieser Version werden vorhandene Bilder im Hintergrund einmal neu erzeugt, sichtbare Kacheln zuerst.
- **Dock:** Knopf "dock" oben im Editor schaltet auf ein schmales Seitenfenster. Dort kannst du zwischen Browser, Scene,
  NPCs, Terrain, Blueprints, Travel, Project sowie Time & Weather wechseln; "full editor" schaltet zurück.
- **Anklicken:** Im Edit-Modus wählt ein Klick auf ein platziertes Objekt es aus (Strg fügt hinzu), Doppelklick greift es.
  Ausgewählte Objekte bekommen einen orangen Rahmen.
- **Tasten:** Objekte werden standardmäßig mit dem Maus-Gizmo platziert. Wer lieber mit der Tastatur arbeitet, schaltet in
  Settings die optionale Tastatursteuerung beim Platzieren ein (Ziffernblock, Tasten umbelegbar; `keyboard_placement=1` in
  der settings.txt). Konfigurierbar sind außerdem die Hotkeys für Editor und Kameramodus.
- **Snap to ground:** "To ground" im Platzierungs-HUD oder im Scene-Tab setzt Objekte auf die Fläche darunter.
  Nutzt die Bodenprobe der Spielphysik.
- **Place** (Browser) bzw. **Grab** (Scene): das Objekt erscheint vor der Figur bzw. Kamera und das Maus-Gizmo ist sofort aktiv.
  Pfeile verschieben entlang der Achsen, der Mittelpunkt schiebt über den Boden, die Würfel skalieren, der grüne Ring dreht und
  der rote bzw. blaue Ring kippt das Objekt (Pitch/Roll). Im Platzierungs-HUD stehen Drop, Cancel, To ground, level und Snap bereit.
  Das Objekt bleibt außerdem stehen, sobald du etwas Neues platzierst, ein anderes Objekt anklickst oder ins Leere doppelklickst.
- **Linie / Kreis** (Browser, unten): setzt N Kopien des gewählten Prefabs in einer Reihe oder auf einem Kreis vor dir ab, als Gruppe,
  und übergibt sie dem Platzierungsmodus. Ausrichtung wählbar (fest, entlang/nach innen, nach außen).
- **Scene:** Objekte anklicken (Strg+Klick mehrere, Umschalt+Klick Bereich). Gruppieren mit "Group", eine Gruppe wird beim
  Anklicken als Ganzes ausgewählt. "Grab" trägt die ganze Auswahl, gedreht wird um den gemeinsamen Mittelpunkt.
  Strg+Z / Strg+Y machen Spawnen, Verschieben und Löschen rückgängig bzw. wieder. Mit der Maus über der Welt (nicht über einem
  Fenster) gelten außerdem: Strg+A wählt alles, Strg+G gruppiert, Strg+D dupliziert, Strg+C / Strg+V kopieren die Auswahl mit
  Ausrichtung und setzen die Kopie vor dir ab (Platzierungsmodus), Entf löscht. "snap" schaltet Raster und Winkelschritte für das Maus-Gizmo
  ein. Einzelnes Objekt: Position/Yaw/Neigung/Scale ziehen, "level" nimmt die Neigung raus. Mit "live" folgt das Objekt sofort, beim Loslassen
  wird die Kollision nachgezogen (bei Yaw/Scale-Änderung wird das Objekt dafür kurz neu erzeugt). "Delete" entfernt es, "Duplicate" kopiert es.
- **Project:** Objekte und verwaltete NPCs sind gleichwertige Projekt-Entitäten. Beim Laden werden beide erzeugt; **Unload**
  entfernt beide nur aus der laufenden Szene und lässt die `.cdproj`-Datei bestehen. Die Projektseite bietet klare Aktionen für
  **Load, Reload, Save, Unload und Autoload**. Unload und Reload verwerfen ungespeicherte Änderungen ohne Rückfrage, vorher also
  speichern. Gespeicherte Projekte lassen sich archivieren (**Archive**), wiederherstellen (**Restore**) und löschen.
- **Bearbeitetes Projekt:** Alles, was du neu spawnst oder platzierst, gehört zum oben angezeigten bearbeiteten Projekt. Ist noch
  keins gewählt, legt World Builder eins mit dem Namen "Untitled N" an. Das bearbeitete Projekt wird bei jedem Start wieder
  geladen, andere Projekte über Autoload. Ältere, noch nicht zugeordnete Objekte und NPCs lassen sich mit **Add unassigned** in
  ein geladenes Projekt übernehmen.
- **Echtzeit-Autosave:** Ein eigener Schalter auf der Projektseite aktiviert oder deaktiviert das automatische Speichern.
  Ist er aktiv, wird jede bestätigte Änderung an einer Entität sofort beim nächsten Editor-Frame in ihr Projekt zurückgeschrieben.
- **Blueprints:** wiederverwendbare Objektgruppen (`.cdgroup`), getrennt von den Projekten gespeichert. "Create blueprint"
  schreibt die aktuelle Auswahl unter einem Namen, "Import blueprint" holt eine Datei herein; Doppelklick oder Ziehen in die
  Welt setzt einen Blueprint ins bearbeitete Projekt.
- **History:** listet die Einträge von Undo und Redo mit der Anzahl ihrer Schritte; aufgeklappt zeigt jeder Eintrag die
  Einzelheiten.
- **Kompatibilität:** Alte `.cdproj`-Objektzeilen bleiben lesbar. Das aktuelle Format behält diese Zeilen kompatibel bei und speichert
  zusätzlich Gruppennamen, Notizen und verwaltete NPCs als optionale Metadaten.
- Konsole: `help` listet die Befehle.

## Interaktive Objekte

Alle Prefabs unter `/object/cd_gimmick/` (Tag "Gimmick": Fackeln, Lampen, Türen, Truhen, Lagerfeuer, Hebel ...) erzeugt World
Builder über den Spawn-Weg des Spiels selbst, so wie das Spiel seine Feld-Objekte anlegt. Sie verhalten sich dann wie im Spiel:
eine Standfackel lässt sich an- und ausmachen und fällt um, eine Truhe geht auf. Auswahl, Rahmen, Gizmo, Undo, Gruppen und
Projekte funktionieren wie bei jedem anderen Objekt. Beim Ziehen folgt ein statischer Platzhalter der Maus, beim Loslassen
erscheint das interaktive Objekt an der neuen Stelle. Löschen entfernt es so, wie das Spiel ein aufgehobenes Item entfernt.

Dafür braucht die Mod eine Vorlage, die das Spiel von selbst liefert: einen Spawn, den es beim Herumlaufen macht, oder eine
Platzierung im Baumenü. Direkt nach dem Laden zeigt der Scene-Tab "N interactive objects waiting for a spawn template: walk a
few meters", bis der erste passiert ist. Lehnt das Spiel ein Prefab ab, wird es als normales Objekt gesetzt, das Log sagt es.
"interactive objects through the game" im Log-Tab unter "Developer: how moves are applied" (bzw. `gimmick_spawn=0` in der
settings.txt) schaltet das ab. Ein Spiel-Update kann
den Weg blockieren; das Log nennt dann den fehlenden Hook, alles andere läuft weiter.

## NPCs und Kreaturen

Der Tab **NPCs** listet alle rund 7.250 Figuren des Spiels (Menschen, Goblins, Tiere, Reittiere, Monster, Bosse) mit ihrem
Spielnamen in deiner Sprache, dem internen Namen und dem Schlüssel. Suche und Kategorie grenzen die Liste ein, **SPAWN** (oder
ein Doppelklick) setzt die Figur mit dem eingestellten Abstand vor dich. Mit "count" können bis zu 500 Figuren pro Vorgang erzeugt werden;
als Formation stehen **Line**, **Matrix** und **Circle** mit einstellbarem Abstand bzw. Radius zur Verfügung. Der Spawn-Abstand
kann auch für weit entfernte Gruppen direkt eingegeben werden. Eine Figur kann außerdem aus der Liste oder Kachelansicht direkt
in die Spielwelt gezogen werden; beim Loslassen wird ein einzelner NPC am markierten Bodenpunkt erzeugt. Die Liste
liest die Mod zur Laufzeit aus deinem installierten Spiel, es wird nichts davon mitgeliefert.

Die Figuren entstehen weiterhin über die Spawn-Anfrage des Spiels selbst, werden vom World Builder aber als **verwaltete NPCs**
registriert. Schon beim Erzeugen kannst du normale KI oder **Hold position (AI paused)** wählen. Danach lassen sich NPCs mit Ctrl und Shift
mehrfach auswählen oder komplett markieren und gemeinsam bewegen, löschen, gruppieren, benennen, mit Notizen versehen sowie
die KI ein- und ausschalten sowie zwischen normalem Verhalten und Hold wechseln. Dieselben verwalteten NPCs erscheinen auch im
**Scene**-Tab direkt zwischen den normalen Objekten; ihre Änderungen laufen durch Undo und Redo.

Verwaltete NPCs werden zusammen mit dem Projekt gespeichert: Position, KI-Zustand, Verhalten, Gruppe, eigener Name und Notiz
werden beim Laden wiederhergestellt. Ist die Server-Figur des Spielers nach einem Ladebildschirm noch nicht bekannt, bleibt der
NPC kurz als "pending" eingetragen und wird automatisch erzeugt, sobald das Spiel wieder eine gültige Server-Session liefert.

Der NPC-Browser funktioniert auch im Dock. Dort wird die Trefferliste automatisch als kompakte Kachelansicht gezeigt; Suche,
Kategorie, Abstand, Anzahl, Formation, KI beim Spawn und SPAWN bleiben verfügbar. Der NPC-Tab bleibt dabei bewusst ein
Browser und Spawner; bereits erzeugte, verwaltete NPCs werden ausschließlich im **Scene**-Tab bearbeitet.

Beim Schließen des World-Builder-Fensters – per Hotkey oder über das X in der Titelleiste – wird ein aktiver Freikamera-/Flugmodus
sofort beendet und die Steuerung vollständig an das Spiel zurückgegeben.

Nach dem Laden eines Spielstands zeigt der Tab "walk a few steps first", bis du dich kurz bewegt hast: Die Anfrage braucht
die Server-Figur deines Charakters, und die liefert das Spiel beim Laufen. Ein Spiel-Update kann den Weg blockieren; dann sagt
der Tab das, alles andere läuft weiter.

## Terrain

Der Tab **Terrain** (auch im Dock) formt den Boden mit einem Pinsel: **Raise** hebt an, **Lower** senkt ab, **Flatten** zieht den
Boden auf die Höhe, an der du angefangen hast. Radius 2 bis 60 m, bis zu 5 m pro Strich; Striche addieren sich. Mit
"Brush active" malt die linke Maustaste in der Welt, der Ring zeigt, wo der Pinsel trifft. Boden und Kollision ändern sich
sofort, ohne Neuladen: In eine gerade gemalte Mulde kann man hineinlaufen, auf einen Hügel hinauf. Undo, Redo und
"Clear project terrain" (entfernt die Striche des bearbeiteten Projekts) stehen im Tab. Die Striche werden mit dem Projekt gespeichert und sind beim Autoload sofort wieder da.

**Apply** (Schnellreise 5 km weg und wieder zurück, zwei Ladebildschirme, etwa eine halbe Minute) ist nur nötig, wenn ein
Strich einmal nicht live angezeigt werden konnte; der Tab sagt das dann. Die Spieldateien werden nicht verändert: Die
Änderung wird beim Einlesen der Höhenkarten im Speicher angewendet.

## Reisen

Der Tab **Travel** nutzt die Schnellreise des Spiels: Koordinaten eingeben (x, Höhe, z; die Höhe muss nur ungefähr
stimmen) oder einen gespeicherten Punkt wählen, dann folgt ein Ladebildschirm und du stehst am Ziel. Eigene Punkte speicherst
du unter einem Namen an der aktuellen Position; sie liegen in `bin64\cdmodkit\`. Die erste Reise einer Sitzung braucht bis zu
einer halben Minute Vorbereitung, danach geht es sofort. In der Konsole setzt `tp x y z` die Figur direkt an die Position
(ohne Schnellreise und Ladebildschirm; die Umgebung lädt danach nach).

## Spielmodus (isolierte Welt)

Liegt beim Start eine `bin64\cdmodkit\playmode.json` bereit (zum Beispiel vom "Play here"-Knopf eines Editors geschrieben),
startet das Spiel direkt in eine leere Welt: World Builder drückt im Titelbildschirm selbst "Fortsetzen" (das Spielfenster
muss dafür im Vordergrund sein), nach dem Laden bringt dich die Schnellreise zum angegebenen Punkt, und es bleiben nur Terrain,
Himmel, Wetter, deine Figur und die gewünschten Objekte oder das Projekt. NPCs, Tiere, Gebäude und Requisiten der Welt werden
nicht erzeugt. Schickt der Editor seine offene Szene mit, laden genau deren Level mit Gebäuden, Requisiten und
NPCs, der Rest der Welt bleibt leer. Wälder und Gras gehören zum Terrain und bleiben. Speichern ist in dieser Sitzung gesperrt:
Das Spiel kann deinen Spielstand nicht überschreiben (gesperrt sind die Dateizugriffe des Spiels selbst auf die Spielstände).
Die Datei wird beim Start sofort in `playmode.last.json` umbenannt; der nächste normale Start ist also wieder ganz normal.
Findet das Plugin nach einem Spiel-Update seine Funktionen nicht, verwirft es die Anfrage, und das Spiel startet normal.
Beenden: Strg+Umschalt+Ende schließt das Spiel (im Dialog des Spiels die Leertaste gedrückt halten); das geht erst in der
Spielwelt, nicht im Titel- oder Ladebildschirm. Verlässt ein Programm den Spielmodus über die HTTP-Schnittstelle per Reise
zurück in die normale Welt, bleibt Speichern bis zum Neustart gesperrt. Das Dateiformat beschreibt `notes/FORMATS.md`
(Abschnitt "Play mode") im Quellcode-Repository.

## Zeit und Wetter

Der Tab **Time & Weather** steuert die visuelle Tageszeit und grundlegende Wetterwerte. Die Uhrzeit lässt sich frei einstellen
oder mit 06:00, 12:00, 18:00 und 00:00 schnell setzen. **freeze time (lighting only)** hält die visuelle Tageszeit und damit
Sonne/Himmel/Beleuchtung fest, pausiert aber nicht das Spiel: NPCs, Physik, Kampf und andere Simulation laufen weiter. Mit
**use native time** übernimmt wieder der normale Zeitverlauf des Spiels.

Für das Wetter gibt es Clear Sky sowie optionale Overrides für Regen, Schnee, Wolkenmenge und Wind. Schnee wird nur angeboten,
wenn neben der Wettertabelle auch die Partikel-Steuerung des Spiels sicher aufgelöst wurde. **use native weather** schaltet alle
World-Builder-Wettervorgaben wieder ab. Zeit und Wetter sind auch im Dock verfügbar. Die benötigten Spielpfade werden beim Start
per Signatur aufgelöst; wenn ein Patch sie verändert, wird nur der betroffene Bereich deaktiviert.

## Freie Kamera

Der Button **free camera** oben im Editor schaltet den Kameramodus ein: eine frei fliegende Kamera, während
der Editor offen bleibt. W/A/S/D bewegen, E oder Leertaste hoch, Q runter, linke Umschalttaste vierfach schneller, das Mausrad fährt
vorwärts. Mit gedrückter rechter Maustaste über der Welt ziehen dreht den Blick; ein Rechtsklick ohne Bewegung öffnet das
Kontextmenü. Editor-Kürzel wie Strg+Z bleiben aktiv. Deine Figur bleibt stehen, neue Objekte erscheinen vor der Kamera,
Gizmo und Rahmen folgen ihr. Pos1 (Home) beendet den Kameramodus wieder, der Editor bleibt offen. Tempo und
Mausempfindlichkeit stehen im Tab Settings. Die Welt lädt Details weiterhin rund um deine Figur, bei sehr weiten Flügen wird es daher gröber.

## Bekannte Einschränkungen

- Prefabs außerhalb von `/object/cd_gimmick/` sind rein visuell mit Kollision.
- Platzierte Objekte sind erst mit Save (oder Autosave) im Projekt gespeichert. Nach einem Neustart lädt World Builder das
  bearbeitete Projekt wieder, andere Projekte über Autoload oder von Hand.
- Mit DLSS Frame Generation ist das Overlay unter Umständen nicht sichtbar. Dann FG kurz abschalten.
- Andere Overlays (CrimsonRoute, ReShade, Master Looter) sollten funktionieren, sind aber nicht getestet.
- Terrain-Striche über eine Kachelgrenze (alle 1024 m) werden pro Kachel angewendet; in seltenen Fällen ist dort ein Apply nötig.
- Steam-Integritätsprüfung entfernt `cdmodkit.asi` wieder. Danach einfach erneut kopieren.

## Für Mod-Entwickler

`sdk\cdmodkit_api.h` beschreibt die exportierten Funktionen (`cdk_spawn`, `cdk_move`, `cdk_remove`, `cdk_player_pos`, ...).
Eigene ASI-Mods holen sich die Adressen per `GetModuleHandleA("cdmodkit.asi")` + `GetProcAddress`.

**HTTP-Schnittstelle** für Programme auf diesem PC (zum Beispiel den Crimson Desert Mod Framework Editor): in Settings
"HTTP API for programs on this PC" einschalten (`http_api=1`, Port `http_port=8765` in der settings.txt). Sie ist
standardmäßig aus und nur unter 127.0.0.1 erreichbar; Anfragen aus dem Browser werden abgelehnt. Die Endpunkte (Objekte,
Projekte, Zeit, Reisen, Spielmodus) beschreibt `HTTP_API.md` im Quellcode-Repository. Achtung: Die Forschungs-Endpunkte
unter `/api/research/` lesen und schreiben Speicher des Spiels; die Schnittstelle also nur einschalten, wenn ein Programm
sie braucht.

## Fragen, Wünsche, Probleme

Discord-Server **Crimson Desert Modding**: https://discord.gg/HfkShRJZU

## Log und Fehler

Alles landet in `bin64\cdmodkit\cdmodkit.log`. Bei einem Absturz die letzten 30 Zeilen davon schicken,
insbesondere Zeilen mit `[fault]`, `[imgui assert]` oder `RESOLVE FAILED`.

## Danke

- **dofo7777** für ausgiebiges Testen, Ideen und Videos.
- **Nostyxx** für [CrimsonWeather](https://github.com/Nostyxx/CrimsonWeather): danke für das Finden der Offsets und Signaturen der
  Tageszeit- und Wetterdaten, auf denen die Zeit- und Wettersteuerung aufbaut (kein Code übernommen).
- Dem Autor von **CrimsonRoute** für die Erlaubnis, den Overlay-Capture-Code zu verwenden.
- **Shin234** für Master Looter (die Eingabeschicht dieser Mod ist daraus abgeleitet) und die ganze Crimson-Desert-Modding-Community,
  insbesondere die Projekte pycrimson und CDMW für die Formatforschung.

## Deinstallation

`bin64\cdmodkit.asi` und den Ordner `bin64\cdmodkit\` löschen. `winmm.dll` nur entfernen, wenn keine anderen ASI-Mods genutzt werden.
