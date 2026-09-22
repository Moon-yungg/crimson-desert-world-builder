# Quick Guide: Crimson Desert Modding Discord

Ziel: ein allgemeiner Modding-Server für Crimson Desert, der klein startet (World Builder im Fokus) und
ohne Umbau wachsen kann. Alles auf Englisch benennen, die Community ist international.

## 1. Server anlegen (10 Minuten)

1. Discord: **+** in der Serverliste, "Create My Own", "For a club or community".
2. Name: `Crimson Desert Modding`. Icon: dein World-Builder-Hero-Bild oder ein Screenshot, quadratisch zugeschnitten.
3. **Server Settings, Enable Community** aktivieren. Das schaltet Rules-Screening, Onboarding, Foren-Kanäle,
   Announcement-Kanäle, Server-Insights und Discovery frei. Verlangt einen Rules-Kanal und einen Moderator-Kanal, die
   der Assistent automatisch anlegt.
4. **Server Settings, Safety Setup**:
   - Verification Level: **Medium** (E-Mail verifiziert, 5 Minuten auf Discord). Hält Spam-Accounts raus, nervt echte Nutzer nicht.
   - Explicit Media Filter: für alle Mitglieder scannen.
   - 2FA-Pflicht für Moderation: an.
5. **AutoMod** (Server Settings, AutoMod): die vorgefertigten Regeln "Block Spam Content", "Block Mention Spam" und
   "Commonly Flagged Words" aktivieren. Eigene Regel: Nachrichten mit `discord.gg/` Links von Nicht-Moderatoren blocken
   (verhindert die üblichen Fake-Nitro- und Server-Werbebots).
6. **Onboarding** (Server Settings, Onboarding): Default Channels = welcome, announcements, general. Eine Frage:
   "What are you here for?" mit Antworten "World Builder", "Other mods", "I make mods" und passenden Rollen (siehe unten).

## 2. Rollen

| Rolle | Farbe | Rechte | Wofür |
| --- | --- | --- | --- |
| Owner | rot | alles | du |
| Moderator | orange | Nachrichten löschen, Timeout, Kick, Ban, Threads verwalten | zwei, drei vertrauenswürdige Leute, z.B. dofo7777 |
| Mod Author | gold | Kanäle in "Mod Authors" schreiben, eigene Threads in showcase anpinnen | jeder, der eine Mod veröffentlicht hat |
| World Builder | grün | keine Sonderrechte, nur Ping-Gruppe | wählbar im Onboarding |
| Tester | blau | Zugang zu beta | wer Vorabversionen testet |
| @everyone | grau | lesen, schreiben in den offenen Kanälen, keine @everyone-Pings, keine Links in showcase? nein: Links erlaubt | Standard |

Rollen sind unter Server Settings, Roles. "Mention @everyone" bei @everyone abschalten.

## 3. Kanäle (Startaufbau)

Kategorien in Großbuchstaben, Kanäle klein. Kanäle mit `#` sind Text, `[forum]` sind Foren-Kanäle, `[ann]` Announcement.

**INFO**
- `#welcome` (schreibgeschützt): kurzer Text, was der Server ist, Links zu Nexus, Regeln, Rollen-Auswahl.
- `#rules` (vom Community-Setup): siehe Vorlage unten.
- `#announcements` [ann]: Releases, Server-News. Nur Owner/Moderator schreiben. Nutzer können ihn in ihre eigenen Server folgen.

**WORLD BUILDER**
- `#wb-general`: Fragen, Austausch.
- `#wb-support` [forum]: ein Thread pro Problem. Post-Guideline im Kanal: Version, Spielversion, letzte 30 Zeilen aus `bin64\cdmodkit\cdmodkit.log`. Tags: bug, question, solved.
- `#wb-showcase` [forum]: Bauten zeigen. Tags: build, video, project-file. Nutzer hängen ihre `.cdproj` an, andere importieren sie.
- `#wb-projects`: reine Dateiablage für geteilte `.cdproj`-Dateien, Slowmode 30 s, nur Anhänge und ein Satz Beschreibung.
- `#wb-feature-requests` [forum]: Wünsche, ein Thread pro Wunsch, Reaktionen als Abstimmung.
- `#wb-beta` (nur Rolle Tester): Vorabversionen, Feedback.

**MODDING (allgemein)**
- `#general`: alles rund um Crimson-Desert-Mods.
- `#mod-help` [forum]: Installationsprobleme aller Mods (ASI Loader, DMM, Vortex, Steam-Integritätsprüfung).
- `#mod-showcase` [forum]: Releases anderer Autoren, ein Thread pro Mod.
- `#tools-and-formats`: pycrimson, CDMW, Formatforschung, Reverse Engineering. Hier landen die technischen Leute.

**MOD AUTHORS** (Rolle Mod Author)
- `#authors-chat`: Abstimmung untereinander, Kompatibilität, gemeinsame Probleme nach Spiel-Updates.
- `#sdk`: World-Builder-C-API, Fragen zu `cdmodkit_api.h`.

**OFF TOPIC**
- `#off-topic`: der Rest.
- `#screenshots`: schöne Bilder ohne Mod-Bezug.

**STAFF** (nur Moderator)
- `#mod-log`: der Kanal aus dem Community-Setup, plus AutoMod-Alerts.
- `#staff-chat`.

Später bei Wachstum: pro große Mod eine eigene Kategorie, Sprachkanäle (ein "Lounge"-Voice reicht anfangs), `#patch-notes`
für Spiel-Updates und Kompatibilitätsstatus aller Mods.

## 4. Kanal-Einstellungen, die sich lohnen

- `#wb-support` und `#mod-help` als Foren mit Pflicht-Tag und "Require tags" an, "Solved"-Tag, Standardsortierung "Latest activity".
- Slowmode 5 s in `#general` und `#wb-general` (bremst Bots, stört Menschen nicht).
- In Foren-Kanälen die "Guidelines" ausfüllen, sie werden beim Erstellen eines Posts angezeigt.
- `#announcements`: Follow-Button ist automatisch da, Nutzer bekommen so Releases in ihre eigenen Server.
- Alle Kanäle: Pin-Recht nur für Moderator und Mod Author.

## 5. Vorlagen

**#welcome**

> Welcome to Crimson Desert Modding.
> This server is for everyone who mods Crimson Desert or uses mods: help, releases, tools, format research.
> It started around World Builder (in-game prefab editor), you find everything about it under WORLD BUILDER.
> Pick your roles in Channels & Roles, read #rules, and post problems in #wb-support or #mod-help as a forum thread.
> World Builder on Nexus: <Link>   Ko-fi: <Link>

**#rules**

> 1. Be decent. No harassment, no slurs, no drama across channels.
> 2. Modding only: no piracy, no game files or extracted assets, no cheats for online features.
> 3. Support requests go into the forum channels, one thread per problem, with your log.
> 4. No advertising or invite links without asking a moderator.
> 5. Mod authors: you own your releases, credit what you build on, respect other people's licenses.
> 6. Moderators can remove content and members that hurt the server. Ask if something is unclear.

**#wb-support Guidelines**

> Title: one line with the problem. Body: World Builder version, game version, what you did, what happened,
> the last 30 lines of bin64\cdmodkit\cdmodkit.log (lines with [fault], [imgui assert] or RESOLVE FAILED are the important ones).

## 6. Bots (optional, alles kostenlos)

- **Carl-bot** oder **Dyno**: Reaction-Roles falls du Onboarding nicht magst, Logging, Auto-Roles. Anfangs nicht nötig, Discords eigenes Onboarding und AutoMod reichen.
- **Statbot** nur, wenn du Aktivität sehen willst. Server Insights (Community-Feature) zeigen das Wichtigste bereits.
- Keine Musik- oder Spaßbots am Anfang, sie ziehen Moderationsaufwand nach sich.

## 7. Einladungslink und Verbreitung

- Server Settings, Invites: einen Link ohne Ablauf und ohne Limit erzeugen (`Edit invite link`, Expire "Never", Max uses "No limit").
- Vanity-URL (`discord.gg/crimsonmodding`) gibt es ab Community-Level 3 (Boosts); anfangs den normalen Link nehmen.
- Link auf der Nexus-Seite in den Abschnitt "Questions, requests, problems" setzen, im Mod-Changelog und in der README.
- Beim Start ein Posting im Nexus-Forum der Mod und ein Hinweis in den bestehenden Crimson-Desert-Modding-Communities.

## 8. Erste Woche

- Zwei Moderatoren ernennen, bevor der Link öffentlich geht.
- Selbst je einen Beispiel-Thread in `#wb-support`, `#wb-showcase` und `#wb-feature-requests` anlegen, damit die Foren nicht leer wirken.
- Release-Ankündigung 0.65 in `#announcements` mit dem Changelog.
- Nach einer Woche: Onboarding-Frage und Kanalstruktur anhand der tatsächlichen Nutzung anpassen, tote Kanäle zusammenlegen.
