# live-view — Spec

**Status:** approved (2026-08-30)
**Date:** 2026-08-30
**Authority:** this file. Changes land as dated entries in the Amendment Log at the bottom —
never as silent edits to settled sections. On conflict, the newest amendment wins.

## Goal

Ein im Gearboy-Fork eingebauter Live-View-Server, mit dem ein Mensch im Browser in
Echtzeit zusieht, wie ein MCP-Agent das Spiel spielt und analysiert: WebSocket-Stream des
Spielbilds (~20 fps), dazu ein Status-Kanal mit dem vom Agenten per neuem MCP-Tool
`set_agent_status` geposteten Analysetext, den Live-Werten der registrierten
Memory-Watches und dem aktuellen Button-Zustand — alles auf einer eingebetteten
Viewer-HTML-Seite ohne externe Abhängigkeiten.

## Non-goals

- Kein Auth, kein TLS, kein Multi-Client-Management über simples Broadcast hinaus
  (localhost-Tool; Bind-Adresse bleibt konfigurierbar).
- Keine Aufzeichnung/Replay des Streams.
- Kein Upstream-PR in dieser Initiative (Code bleibt PR-fähig sauber, mehr nicht).
- Keine Persistenz der Live-View-Flags in config.ini — reine CLI-Flags.
- Kein Audio-Streaming.
- Keine Änderungen an bestehendem MCP-Verhalten außer dem additiven Tool
  `set_agent_status`.

## Decisions, constraints, and invariants

### D1 — Transport: WebSocket, von Hand, ohne neue Dependencies
Ein TCP-Listener-Thread im Emulator (Vorbild für Socket-Handling:
`platforms/shared/desktop/mcp/mcp_transport.h`). `GET /` liefert die eingebettete
Viewer-Seite als HTTP-Response, `GET /ws` macht das WebSocket-Upgrade. Handshake
(SHA-1 + Base64 des `Sec-WebSocket-Key`) und Frame-Encoding/-Decoding (Text, Binary,
Ping/Pong, Close; maskierte Client-Frames) werden selbst implementiert — als reine,
socketfreie Funktionen in einem eigenen Modul, damit sie unit-testbar sind. Keine neuen
Third-Party-Dependencies.

### D2 — Frame-Kanal: PNG-Binärframes, ~20 fps, Snapshot-Übergabe
Videoframes gehen als rohe PNG-Bytes in WebSocket-Binary-Frames. PNG-Encoding über das
bereits vendorte `stb_image_write.h` (gleiches Muster wie `emu_get_screenshot_png` in
`platforms/shared/desktop/emu.cpp`). Der Emulator-Mainloop (windowed wie headless)
publiziert pro Iteration eine Kopie des Framebuffers (`emu_frame_buffer`, RGBA;
Dimensionen aus der Runtime-Info — GB 160×144, SGB 256×224) in einen mutex-geschützten
Snapshot; der Server-Thread drosselt selbst auf ~20 fps, encodiert und broadcastet.

**Invariante (Threading):** Der Server-Thread liest niemals Emulator-Zustand direkt —
ausschließlich den publizierten Frame-Snapshot und den Status-Struct (beide
mutex-geschützt). Der Mainloop blockiert nie auf Encoding oder Sockets.

### D3 — Status-Kanal: JSON-Textframes auf derselben WebSocket-Verbindung
Der Server pusht Status als JSON-Textframe bei Änderung, gedrosselt auf max. ~4 Hz.
Shape (Feldnamen verbindlich):
`{"type":"status", "seq":<u64>, "agent_text":<string>, "agent_ts":<unix_ms>,
"inputs":{"a":bool,"b":bool,"start":bool,"select":bool,"up":bool,"down":bool,
"left":bool,"right":bool}, "watches":[{"address":<u16>,"size":<1|2>,"label":<string>,
"value":<int>}], "media":{"title":<string>,"paused":<bool>,"width":<int>,
"height":<int>}}`. Watch-Quelle sind die im MCP registrierten Memory-Watches;
Input-Quelle der aktuelle Pad-Zustand von Player 1. `seq` wächst streng monoton.

### D4 — MCP-Tool `set_agent_status` (additiv)
Neues Tool im bestehenden Registrierungsmuster (`mcp_server.cpp` Tool-Tabelle +
Dispatch): Parameter `text` (string, erforderlich, serverseitig auf 4096 Bytes gekappt).
Ersetzt den aktuellen Statustext, setzt `agent_ts`, erhöht `seq`. Antwort ist eine
schlichte Bestätigung. Historie hält der Browser (Viewer akkumuliert), nicht der Server.

### D5 — Viewer-Seite: eingebettet, ohne externe Ressourcen
Eine HTML/JS/CSS-Seite als String im Binary (eigener Header unter
`platforms/shared/desktop/liveview/`). Canvas mit Nearest-Neighbor-Skalierung fürs
Spielbild, Panel mit Agent-Status inkl. clientseitiger Historie, Watch-Tabelle,
Button-Anzeige, Verbindungsstatus mit Auto-Reconnect. **Invariante:** Die Seite lädt
keinerlei externe Ressourcen (keine http(s)-URLs außer dem eigenen WebSocket).

### D6 — CLI-Oberfläche
`--live-view` (aktiviert den Server), `--live-view-port N` (Default 7778),
`--live-view-address A` (Default 127.0.0.1). Parsing in
`platforms/shared/desktop/main.cpp`, Durchreichung analog zu den MCP-Flags. Live-View
funktioniert kombiniert mit `--headless`, `--mcp-stdio` und `--mcp-http`.

### D7 — Code-Constraints (Bestand des Repos, gilt für allen neuen Code)
C++11, `-fno-exceptions`, `-Wall -Wextra` warnungsfrei; kein `throw`/`try`/`catch` in
`platforms/shared/desktop/liveview/`. Neue Quellen werden in
`platforms/shared/makefiles/Makefile.sources` registriert; Unit-Tests folgen dem
bestehenden Muster in `tests/` (eigene Targets in `tests/Makefile`). GPLv3-Header wie im
Bestand. Arbeit auf Branch `live-view` ab `master`; Upstream-Remote bleibt unangetastet.

### D8 — Build- und Testumgebung
Build: `make -C platforms/linux -j$(nproc)`; Tests: `make -C tests` und Ausführung der
Test-Binaries. Die Toolchain (`make gcc-c++ pkgconf-pkg-config sdl3-devel
mesa-libGL-devel`) kommt per Devbox-Rebuild; bis dahin ist kein Task ausführbar.
End-to-End-Gates starten das gebaute Binary direkt aus dem Repo (keine Installation nach
`/usr/local` nötig) mit einem ROM aus `GEARBOY_TEST_ROM`
(Default `/workspace/python/pokered_planner/pokemon_red.gb`; ROMs werden nie committet)
und prüfen per Node-Skript (Node ≥22, globaler WebSocket-Client) beobachtbare
Eigenschaften.

## Amendment Log

<!-- One dated entry per amendment, newest last. Empty until the first amendment lands. -->
