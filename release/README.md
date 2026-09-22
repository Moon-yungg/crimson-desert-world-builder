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
   - Ordner `cdmodkit\` (enthält `prefabs.tsv` und `settings.txt`; bei einem Update kannst du deine eigene `settings.txt` behalten)
3. Spiel normal über Steam starten. Das Log liegt in `bin64\cdmodkit\cdmodkit.log`; ein Konsolenfenster gibt es nur mit `console=1` in der `settings.txt`.

## Vorschaubilder

Die Mod enthält keine Spieldaten und keinen Archiv-Code. Die Dateien lädt das Spiel selbst über seinen eigenen Resource-Loader,
die Mod rendert daraus beim ersten Start die Vorschaubilder und unter `bin64\cdmodkit\thumbs\` abgelegt (ca. 33.000 Bilder, rund 20 Minuten im Hintergrund mit niedriger Priorität).
Das im Browser ausgewählte Prefab wird immer sofort gerendert. Der Fortschritt steht unten im Browser-Tab ("previews x / y").
Zum Neuaufbau einfach den Ordner `thumbs` und die Datei `prefab_size.tsv` löschen.

## Bedienung

- Spielstand laden, dann **Einfg** (Insert) drücken: Fenster "World Builder" erscheint im **Bearbeitungsmodus**. Die Welt läuft
  weiter, aber Maus und Tastatur gehören komplett dem Menü (die Figur reagiert nicht, der Cursor ist sichtbar).
- **Pos1** (Home) schaltet in den **Spielmodus**: das Fenster bleibt halbtransparent stehen, alle Eingaben gehen ans Spiel.
  Nochmal Pos1 schaltet zurück, Einfg blendet das Fenster aus. Beide Tasten sind im Tab "Settings" umbelegbar, dort lässt sich auch das Konsolenfenster abschalten
  (gespeichert in `bin64\cdmodkit\settings.txt`).
- **Browser:** Kategorien links (Breite ziehbar), Suche und Tag-Filter oben, Favoriten mit dem Stern. Eintrag anklicken zeigt
  Vorschau, Größe in Metern und Tags. Versatz zum Spieler, Yaw und Scale einstellen, dann "SPAWN" oder Doppelklick.
  "fold variants" fasst Geschwister wie wall_01 / wall_02 / wall_01_broken zu einer aufklappbaren Zeile zusammen, "meshes only"
  blendet Prefabs ohne sichtbares Mesh aus (Standard). Rechtsklick auf einen Eintrag: Favorit, Sammlung hinzufügen.
  Unter den Kategorien stehen deine **Sammlungen**: Name eingeben, "add". SPAWN setzt das Objekt vor dir ab, Abstand nach Objektgröße.
- **Kachelansicht:** Knopf "cards" links neben der Suche zeigt die Treffer als Kacheln mit Vorschaubild (Größe per Regler),
  gleiche Filter wie die Liste. Die Vorschauen werden mit den echten Texturen des Spiels gerendert; beim ersten Start mit
  dieser Version werden vorhandene Bilder im Hintergrund einmal neu erzeugt, sichtbare Kacheln zuerst.
- **Dock:** Knopf "dock" oben im Editor schaltet auf ein schmales Seitenfenster mit Suche, ein bis vier Spalten Kacheln und PLACE.
  Es bleibt beim Platzieren offen, "full editor" schaltet zurück.
- **Anklicken:** Im Edit-Modus wählt ein Klick auf ein platziertes Objekt es aus (Strg fügt hinzu), Doppelklick greift es.
  Ausgewählte Objekte bekommen einen orangen Rahmen.
- **Tasten:** Alle Platzierungstasten sind im Tab "Settings" oder in der settings.txt (key_move_fwd= ...) frei belegbar,
  Numpad ist nur die Vorgabe. "fast" kann Shift, Strg oder Alt sein.
- **Snap to ground:** Numpad / setzt das getragene Objekt auf die Fläche darunter,
  "To ground" im Scene-Tab die Auswahl. Nutzt die Bodenprobe der Spielphysik.
- **Place** (Browser) bzw. **Grab** (Scene): das Menü schließt sich, das Objekt erscheint vor der Figur (Abstand nach Objektgröße)
  und bleibt dort stehen. Du kannst frei herumlaufen und es mit dem Numpad verschieben, relativ zu deiner Position:
  8/2 weg/näher, 4/6 links/rechts, 9/3 hoch/runter, 7/1 drehen, +/- Größe, 0 holt es wieder vor dich, Shift = schnell.
  **Numpad 5** gibt die Maus an World Builder: am Objekt erscheint ein Gizmo. Pfeile ziehen verschieben entlang der Achsen,
  der Mittelpunkt schiebt über den Boden, die Würfel skalieren, der grüne Ring dreht, der rote und der blaue Ring kippen das Objekt
  (Pitch/Roll). **Numpad *** stellt es wieder gerade, Numpad 5 gibt die Maus an die Kamera zurück. Numpad-Punkt schaltet Snap.
  Das Objekt bleibt stehen, sobald du etwas Neues platzierst, ein anderes Objekt anklickst oder ins Leere doppelklickst (**Enter** geht auch); **Rücktaste** bricht ab (bei Grab springt das Objekt zurück). "Vor der Figur" ist die letzte Laufrichtung.
- **Linie / Kreis** (Browser, unten): setzt N Kopien des gewählten Prefabs in einer Reihe oder auf einem Kreis vor dir ab, als Gruppe,
  und übergibt sie dem Platzierungsmodus. Ausrichtung wählbar (fest, entlang/nach innen, nach außen).
- **Scene:** Objekte anklicken (Strg+Klick mehrere, Umschalt+Klick Bereich, Strg+A alle). Gruppieren mit Strg+G oder "Group",
  eine Gruppe wird beim Anklicken als Ganzes ausgewählt. "Grab" trägt die ganze Auswahl, gedreht wird um den gemeinsamen Mittelpunkt.
  Strg+Z / Strg+Y machen Spawnen, Verschieben und Löschen rückgängig bzw. wieder. Strg+C / Strg+V kopieren die Auswahl mit Ausrichtung
  und setzen die Kopie vor dir ab (Platzierungsmodus), Entf löscht. "snap" schaltet Raster und Winkelschritte für den Platzierungsmodus
  ein (Numpad-Punkt schaltet dort um). Einzelnes Objekt: Position/Yaw/Neigung/Scale ziehen, "level" nimmt die Neigung raus. Mit "live" folgt das Objekt sofort, beim Loslassen
  wird die Kollision nachgezogen (bei Yaw/Scale-Änderung wird das Objekt dafür kurz neu erzeugt). "Delete" entfernt es, "Duplicate" kopiert es.
- **Project:** Aufbau unter einem Namen speichern/laden (`bin64\cdmodkit\projects\*.cdproj`, absolute Weltkoordinaten, Gruppen
  bleiben erhalten), optional Autoload beim Spielstart – beliebig viele Projekte gleichzeitig (Häkchen pro Zeile; die Liste steht in
  `bin64\cdmodkit\autoload.txt`, ein Projektname pro Zeile). "Import .cdproj" übernimmt eine Datei von jemand anderem, "Open folder" zeigt deine.
  Jedes Objekt merkt sich, aus welchem Projekt es stammt: im Scene-Tab klickst du dich über eine Tab-Leiste durch "all", "new" und die
  geladenen Projekte, ein Stern zeigt ungespeicherte Änderungen, und "Save the changes" schreibt nur die Objekte dieses Projekts zurück.
  Beim Speichern wählst du zwischen "everything in the scene" und "only the new objects" – so baust du ein zweites Projekt in einem
  geladenen, ohne das erste vorher löschen zu müssen. "New" leert die Szene für einen echten Neuanfang.
- Konsole: `help` listet die Befehle.

## Experimentell: interaktive Objekte

Platzierte Prefabs sind rein visuell. Zusätzlich kann World Builder Objekte über den Spawn-Weg des Spiels selbst erzeugen,
so wie das Spiel seine Feld-Objekte anlegt. Die reagieren dann wie im Spiel: eine so erzeugte Standfackel lässt sich an- und
ausmachen. **Bild auf** (Page Up) erzeugt zwei Meter vor dir eine Standfackel, oder das Prefab, dessen Pfad du im Log-Tab
in das Feld "replay prefab path override" einträgst (das Spiel kennt 13.941 Gimmick-Prefabs: Fackeln, Lampen, Türen,
Truhen, Lagerfeuer ...). "remove" in der Liste "spawned by replay" nimmt ein Objekt wieder weg.
Dafür braucht die Mod eine Vorlage: sie kopiert die Parameter eines Spawns, den das Spiel gerade selbst gemacht hat, und
tauscht Prefab, Position und Identität aus. Solche Spawns passieren beim Herumlaufen ständig, ein paar Schritte nach dem Laden
reichen; vorher meldet Bild auf "walk a few meters first". Stand der Dinge: keine Rotation, kein Gizmo, nicht im Projekt
gespeichert, und ein Spiel-Update kann die Funktion abschalten (das Log sagt dann, welcher Hook fehlt).

## Bekannte Einschränkungen

- Platzierte Objekte sind rein visuell mit Kollision. Gimmick-Verhalten gibt es nur über den experimentellen Spawn-Weg (siehe oben).
- Objekte existieren nur für die laufende Sitzung. Projekte müssen nach einem Neustart geladen werden (oder Autoload nutzen).
- Mit DLSS Frame Generation ist das Overlay unter Umständen nicht sichtbar. Dann FG kurz abschalten.
- Andere Overlays (CrimsonRoute, ReShade, Master Looter) sollten funktionieren, sind aber nicht getestet.
- Steam-Integritätsprüfung entfernt `cdmodkit.asi` wieder. Danach einfach erneut kopieren.

## Für Mod-Entwickler

`sdk\cdmodkit_api.h` beschreibt die exportierten Funktionen (`cdk_spawn`, `cdk_move`, `cdk_remove`, `cdk_player_pos`, ...).
Eigene ASI-Mods holen sich die Adressen per `GetModuleHandleA("cdmodkit.asi")` + `GetProcAddress`.

## Fragen, Wünsche, Probleme

Discord-Server **Crimson Desert Modding**: https://discord.gg/HfkShRJZU

## Log und Fehler

Alles landet in `bin64\cdmodkit\cdmodkit.log`. Bei einem Absturz die letzten 30 Zeilen davon schicken,
insbesondere Zeilen mit `[fault]`, `[imgui assert]` oder `RESOLVE FAILED`.

## Unterstützen

Wenn dir World Builder gefällt und du dich bedanken möchtest: https://ko-fi.com/daebak91. Nie erwartet, immer willkommen.

## Danke

- **dofo7777** für ausgiebiges Testen, Ideen und Videos.
- **Shin234** für Master Looter (die Eingabeschicht dieser Mod ist daraus abgeleitet) und die ganze Crimson-Desert-Modding-Community,
  insbesondere die Projekte pycrimson und CDMW für die Formatforschung.

## Deinstallation

`bin64\cdmodkit.asi` und den Ordner `bin64\cdmodkit\` löschen. `winmm.dll` nur entfernen, wenn keine anderen ASI-Mods genutzt werden.
