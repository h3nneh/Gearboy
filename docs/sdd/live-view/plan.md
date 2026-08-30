# live-view — Implementation Plan

**Source spec:** docs/sdd/live-view/spec.md (authoritative; on conflict the spec wins)
**Status:** draft (pending approval)
**Initiative budget:** 6h (provisional — keine früheren Ledger in diesem Repo)

**No literal implementation or test code in this plan.** Properties say what must hold; the
implementing side writes the tests.

## Global constraints

- Spec D7 gilt für jeden Task: C++11, `-fno-exceptions`, warnungsfrei unter
  `-Wall -Wextra`, GPLv3-Header, Branch `live-view`.
- Build-Kommando: `make -C platforms/linux -j$(nproc)`; Tests: `make -C tests` plus
  Ausführung der gebauten Test-Binaries. Guard: `bash tests/guards/liveview_guard.sh`
  (ab T1).
- Voraussetzung für alle Tasks: Toolchain aus Spec D8 ist installiert (Devbox-Rebuild).
- E2E-Gates: Binary aus `platforms/linux/`, ROM aus `GEARBOY_TEST_ROM` (Default siehe
  Spec D8), Probes als Node-Skripte unter `tests/e2e/`.

## Task DAG

| ID | Task | Class | Risk | Effort intent | Depends on |
|----|------|-------|------|----------------|------------|
| T1 | Conformance-Guard liveview | S | none | low | — |
| T2 | SHA-1 + Base64 Utility-Modul | M | none | low | T1 |
| T3 | WebSocket-Handshake & Framing | M | none | medium | T2 |
| T4 | Gate Phase 1: Build, Tests, Guards grün | S | none | low | T3 |
| T5 | LiveView-Server-Kern | M | none | medium | T4 |
| T6 | Emulator-Integration & CLI-Flags | M | external | medium | T5 |
| T7 | Gate Phase 2: Stream-Probe e2e | S | none | low | T6 |
| T8 | MCP-Tool set_agent_status + Status-Push | M | contract | medium | T7 |
| T9 | Viewer-Seite (embedded HTML/JS) | M | none | medium | T7 |
| T10 | Finales Produkt-Gate: End-to-End | S | none | low | T8, T9 |

## Phases

| Phase | Tasks (DAG order) | Gate task | Expected phase diff |
|-------|-------------------|-----------|---------------------|
| 1 — WebSocket-Grundlage | T1, T2, T3, T4 | T4 | ~660 (provisional) |
| 2 — Server & Stream | T5, T6, T7 | T7 | ~560 (provisional) |
| 3 — Status & Viewer | T8, T9, T10 | T10 | ~740 (provisional) |

## T1 — Conformance-Guard liveview

- **Goal:** Ein Guard-Skript `tests/guards/liveview_guard.sh` anlegen, das die
  maschinenprüfbaren Spec-Invarianten erzwingt und mit Exit-Code ≠ 0 samt klarer Meldung
  fehlschlägt, wenn eine verletzt ist. Geprüfte Invarianten: (1) kein
  `throw`/`try`/`catch` in `platforms/shared/desktop/liveview/` (Spec D7); (2) die
  eingebettete Viewer-Seite referenziert keine externen `http(s)://`-Ressourcen (Spec
  D5); (3) keine neuen Einträge unter `platforms/shared/dependencies/` gegenüber dem
  committeten Stand (Spec D1: keine neuen Dependencies). Das Skript muss auf dem
  aktuellen Repo-Stand (Verzeichnis `liveview/` existiert noch nicht) grün laufen.
- **Interfaces:** Neues Skript `tests/guards/liveview_guard.sh` (bash, keine weiteren
  Abhängigkeiten als grep/find). Verzeichnis `tests/guards/` ist neu.
- **Acceptance properties:** Aufruf auf sauberem Stand endet mit Exit 0. Ein temporär
  eingeschleustes `throw` in einer Datei unter `liveview/` bzw. eine `https://`-URL in
  der Viewer-Seite lässt das Skript mit Exit ≠ 0 und benennender Meldung fehlschlagen
  (im Task per Selbsttest demonstriert, nicht als bleibende Testdatei).
- **Scope:** `tests/guards/` (neu).
- **Expected diff:** ~50 (provisional)
- **Cadence:** task close

## T2 — SHA-1 + Base64 Utility-Modul

- **Goal:** Socketfreies Utility-Modul für den WebSocket-Handshake: SHA-1 über beliebige
  Byte-Puffer und Base64-Encoding, als reine Funktionen ohne Heap-Überraschungen, plus
  Unit-Tests nach dem Muster der bestehenden Tests in `tests/`.
- **Interfaces:** Neue Dateien `platforms/shared/desktop/liveview/liveview_crypto.h/.cpp`.
  Vertrag: SHA-1 liefert 20 Bytes Digest für beliebige Eingaben (inkl. leerer Puffer,
  Puffer > 64 Bytes über Blockgrenzen); Base64 encodiert beliebige Bytefolgen inkl.
  Padding-Fälle (Länge mod 3 = 0/1/2). Beide Funktionen sind thread-sicher (kein
  globaler Zustand). Registrierung der neuen `.cpp` in
  `platforms/shared/makefiles/Makefile.sources`; neues Test-Target in `tests/Makefile`
  nach dem Muster der bestehenden Targets.
- **Acceptance properties:** Unit-Tests prüfen SHA-1 gegen mindestens drei bekannte
  Referenzvektoren (leerer String, kurzer String, Eingabe > 64 Bytes) und Base64 gegen
  Referenzvektoren aller drei Padding-Fälle; zusätzlich der zusammengesetzte
  WebSocket-Accept-Vektor aus RFC 6455 (bekannter `Sec-WebSocket-Key` →
  bekannter Accept-Wert). Tests laufen im bestehenden `tests/`-Build grün.
- **Scope:** `platforms/shared/desktop/liveview/` (neu);
  `platforms/shared/makefiles/Makefile.sources` (Eintrag der neuen Quellen);
  `tests/Makefile` (neues Test-Target).
- **Expected diff:** ~280 (provisional)

## T3 — WebSocket-Handshake & Framing

- **Goal:** Socketfreies WebSocket-Protokollmodul: Handshake-Verarbeitung und
  Frame-Codec für die Serverseite, plus Unit-Tests.
- **Interfaces:** Neue Dateien `platforms/shared/desktop/liveview/liveview_ws.h/.cpp`,
  baut auf T2 auf. Vertrag: (a) Handshake — aus einem HTTP-Upgrade-Request-Header den
  `Sec-WebSocket-Key` extrahieren und die vollständige 101-Response erzeugen; fehlender
  Key oder fehlendes Upgrade-Header-Paar ergibt einen Fehlerstatus, nie undefiniertes
  Verhalten. (b) Encoding — Server-Frames (unmaskiert) für Text, Binary, Ping, Pong,
  Close mit korrekten Längenformen (7-bit, 16-bit, 64-bit Payload-Länge). (c) Decoding —
  eingehende maskierte Client-Frames inkrementell aus einem Bytestrom parsen
  (Teil-Frames über mehrere reads, Demaskierung, Opcode, Close-Code); unmaskierte
  Client-Frames und reservierte Opcodes werden als Protokollfehler gemeldet.
  Registrierung in `Makefile.sources` und Test-Target in `tests/Makefile` wie in T2.
- **Acceptance properties:** Unit-Tests decken beobachtbar ab: Handshake-Response für
  den RFC-6455-Beispiel-Key; Roundtrip encode→decode für Text- und Binary-Payloads der
  Längen um die Grenzen 125/126/65535/65536; inkrementelles Decoding eines in beliebige
  Stücke zerteilten Frames liefert dieselbe Payload wie am Stück; unmaskierter
  Client-Frame wird als Fehler gemeldet. Alles im `tests/`-Build grün.
- **Scope:** `platforms/shared/desktop/liveview/`;
  `platforms/shared/makefiles/Makefile.sources` (Einträge);
  `tests/Makefile` (neues Test-Target).
- **Expected diff:** ~330 (provisional)

## T4 — Gate Phase 1: Build, Tests, Guards grün

- **Goal:** Phasen-Checkpoint: der Fork baut mit den neuen Modulen, alle Tests und
  Guards sind grün.
- **Interfaces:** Keine neuen; reine Verifikation.
- **Acceptance properties:** `make -C platforms/linux -j$(nproc)` endet mit Exit 0 und
  erzeugt `platforms/linux/gearboy`; `make -C tests` endet mit Exit 0 und alle
  Test-Binaries (bestehende plus die aus T2/T3) enden mit Exit 0;
  `bash tests/guards/liveview_guard.sh` endet mit Exit 0;
  `./platforms/linux/gearboy --version` zeigt die Versionszeile.
- **Scope:** Keine Produktänderungen; nur Korrekturen, die aus rot laufenden obigen
  Kommandos folgen, in den Scopes von T1–T3.
- **Expected diff:** ~0 (provisional)

## T5 — LiveView-Server-Kern

- **Goal:** Der eigentliche Server als eigenständiges Modul, noch ohne
  Emulator-Verdrahtung: Listener-Thread, HTTP- und WebSocket-Handling, Broadcast.
- **Interfaces:** Neue Dateien
  `platforms/shared/desktop/liveview/liveview_server.h/.cpp`. Vertrag: Ein
  Server-Objekt mit Start(address, port)/Stop()-Lebenszyklus; Start bindet und lauscht
  in einem eigenen Thread, Stop beendet Thread und alle Verbindungen deterministisch
  (kein Leak, kein Hängen — Vorbild für Socket-/Shutdown-Muster:
  `platforms/shared/desktop/mcp/mcp_transport.h`). `GET /` antwortet mit einer
  HTTP-200-HTML-Response (Seiteninhalt kommt als String von außen, in dieser Phase
  Platzhalter); `GET /ws` vollzieht das Upgrade über das T3-Modul; andere Pfade → 404.
  Publikationsschnittstelle für den Mainloop (Spec D2-Invariante): eine Methode
  publiziert einen RGBA-Frame-Snapshot (Pointer+Breite+Höhe, wird kopiert), eine
  weitere den Status-JSON-String; beide sind nicht-blockierend bis auf kurze
  Mutex-Griffe. Der Server-Thread encodiert den jeweils neuesten Snapshot per
  `stb_image_write` zu PNG, gedrosselt auf ~20 fps, und broadcastet ihn als
  Binary-Frame an alle verbundenen Clients; Status-Strings gehen als Text-Frame bei
  Änderung, gedrosselt auf ~4 Hz. Client-Pings werden mit Pong beantwortet; tote
  Verbindungen werden entfernt. Registrierung in `Makefile.sources`.
- **Acceptance properties:** Baut warnungsfrei; Guards grün. Beobachtbares Verhalten
  wird in T7 per e2e-Probe abgenommen (dieser Task liefert dafür die Voraussetzung);
  auf Modulebene gilt: Start auf Port 0 (ephemeral) gefolgt von Stop kehrt zurück,
  wiederholtes Start/Stop leakt keine Threads — als Unit-Test im `tests/`-Build, sofern
  dort ohne SDL-Abhängigkeit baubar, sonst als dokumentierter Bestandteil der
  T7-Probe.
- **Scope:** `platforms/shared/desktop/liveview/`;
  `platforms/shared/makefiles/Makefile.sources` (Eintrag); optional `tests/Makefile`
  (neues Test-Target, gleiches Muster).
- **Expected diff:** ~380 (provisional)

## T6 — Emulator-Integration & CLI-Flags

- **Goal:** Live-View von außen nutzbar machen: CLI-Flags, Emulator-API und
  Mainloop-Hooks in windowed wie headless Betrieb.
- **Interfaces:** `platforms/shared/desktop/main.cpp` — neue Flags nach Spec D6
  (`--live-view`, `--live-view-port N`, `--live-view-address A`), Parsing und
  Help-Text analog zu den MCP-Flags, Durchreichung über die bestehende
  `ApplicationParams`-Struktur. `platforms/shared/desktop/emu.h/.cpp` — neue
  `emu_live_view_*`-Funktionen (start/stop/is_running/publish) im Stil der
  `emu_mcp_*`-Familie; publish liest `emu_frame_buffer` plus Runtime-Dimensionen und
  reicht sie an den T5-Server; Statuspublikation in dieser Phase noch minimal
  (leerer Agent-Text, `media`-Block gefüllt). Hooks: je ein Publish-Aufruf pro
  Iteration in `application_mainloop` (`application.cpp`) und
  `application_headless_mainloop` (`application_headless.cpp`); Start/Stop im
  Init/Destroy-Pfad beider Betriebsarten. Headless-Exit-Bedingung („No service
  running") berücksichtigt einen laufenden Live-View-Server als Service.
- **Acceptance properties:** `--help` listet die drei neuen Flags. Binary startet mit
  `--headless --live-view` plus ROM und beendet sich nicht sofort (Service-Bedingung).
  `curl` auf `http://127.0.0.1:<port>/` liefert HTTP 200 mit HTML; unbekannter Pfad
  liefert 404. Ohne `--live-view` lauscht nichts auf dem Port. Beobachtbar per
  Kommandos; formal abgenommen in T7.
- **Scope:** `platforms/shared/desktop/` (main.cpp, application.cpp,
  application_headless.cpp, application.h, emu.h, emu.cpp);
  `platforms/shared/desktop/liveview/`.
- **Expected diff:** ~180 (provisional)

## T7 — Gate Phase 2: Stream-Probe e2e

- **Goal:** Phasen-Checkpoint mit beobachtbarer Stream-Abnahme: ein Node-Skript
  `tests/e2e/liveview_stream_probe.mjs`, das den laufenden Emulator prüft, plus die
  Standard-Grün-Kommandos.
- **Interfaces:** Neues Skript `tests/e2e/liveview_stream_probe.mjs` (Node ≥22,
  globaler WebSocket-Client, keine npm-Dependencies). Es startet das Binary
  `platforms/linux/gearboy` mit `GEARBOY_TEST_ROM` (Spec D8), `--headless
  --live-view --live-view-port <freier Port>`, verbindet sich, misst, beendet den
  Prozess und meldet Exit 0/≠0.
- **Acceptance properties:** Probe-Assertions, alle als Exit-Code beobachtbar:
  (1) `GET /` → 200 und Content-Type HTML; (2) WebSocket-Verbindung auf `/ws` kommt
  zustande; (3) innerhalb von 5 s treffen ≥ 40 Binärframes ein (~20 fps-Nachweis);
  (4) jeder geprüfte Binärframe beginnt mit der PNG-Signatur; (5) nach SIGTERM endet
  der Emulator-Prozess binnen 3 s. Dazu: Build, `tests/`-Suite und Guard grün wie in
  T4.
- **Scope:** `tests/e2e/` (neu); nur aus rot laufenden Kommandos folgende Korrekturen
  in den Scopes von T5/T6.
- **Expected diff:** ~120 (provisional)

## T8 — MCP-Tool set_agent_status + Status-Push

- **Goal:** Der Agent kann Analysetext posten, und der Status-Kanal führt Agent-Text,
  Memory-Watch-Werte und Input-Zustand zusammen (Spec D3/D4).
- **Interfaces:** `platforms/shared/desktop/mcp/mcp_server.cpp` — Tool-Eintrag
  `set_agent_status` (Beschreibung, Schema mit Pflichtfeld `text`) in der Tool-Tabelle
  und im Dispatch, im Muster der bestehenden Tools;
  `platforms/shared/desktop/mcp/mcp_debug_adapter.h/.cpp` — Handler, der den Text
  (gekappt auf 4096 Bytes, Spec D4) in den Live-View-Status übernimmt; Tool ist auch
  ohne laufenden Live-View-Server gültig (No-op mit Bestätigung).
  `platforms/shared/desktop/emu.cpp` bzw. `liveview/` — die Statuspublikation aus T6
  wird auf die volle D3-Shape ausgebaut: `agent_text`/`agent_ts`/`seq` aus dem Tool,
  `watches` aus den im MCP registrierten Memory-Watches (Adresse, Größe, Label,
  aktueller Wert), `inputs` aus dem Pad-Zustand Player 1, `media` wie gehabt.
  Die D3-Feldnamen sind verbindlicher Vertrag für die Viewer-Seite (T9) und das finale
  Gate (T10).
- **Acceptance properties:** `tools/list` des MCP-Servers enthält `set_agent_status`.
  Ein `tools/call` mit `text` liefert eine Erfolgsantwort; ein anschließend über den
  Live-View-WebSocket empfangener Status-Textframe ist gültiges JSON der D3-Shape und
  trägt genau diesen Text; ein zweiter Call erhöht `seq` strikt. Nach `add_memory_watch`
  enthält der nächste Status-Frame den Watch mit plausiblem Wert. Fehlender
  `text`-Parameter ergibt eine MCP-Fehlerantwort, keinen Crash. Beobachtbar über die
  bestehende MCP-HTTP-Schnittstelle plus WebSocket; automatisiert im T10-Skript.
- **Scope:** `platforms/shared/desktop/mcp/` (mcp_server.cpp, mcp_debug_adapter.h/.cpp);
  `platforms/shared/desktop/liveview/`; `platforms/shared/desktop/emu.cpp`.
- **Expected diff:** ~280 (provisional)

## T9 — Viewer-Seite (embedded HTML/JS)

- **Goal:** Die eingebettete Viewer-Seite nach Spec D5: Spielbild, Agent-Status mit
  Historie, Watches, Inputs, Verbindungsstatus.
- **Interfaces:** Neue Datei `platforms/shared/desktop/liveview/liveview_page.h`
  (Seite als String-Konstante), vom T5-Server für `GET /` ausgeliefert (ersetzt den
  Platzhalter). Verhalten der Seite: verbindet auf `ws://<host>/ws`; Binary-Frames
  werden als PNG dekodiert und auf ein Canvas mit Nearest-Neighbor-Skalierung
  gezeichnet (Dimensionen aus dem `media`-Block, GB wie SGB); Status-Frames (D3-Shape)
  aktualisieren Agent-Text (neuester oben, clientseitige Historie über `seq`
  dedupliziert), Watch-Tabelle (Label, Adresse hex, Wert dezimal+hex) und die acht
  Button-Indikatoren; Abriss der Verbindung zeigt einen sichtbaren Zustand und
  reconnectet automatisch mit Backoff. Keine externen Ressourcen (Guard aus T1 prüft).
- **Acceptance properties:** Guard grün (keine externen URLs). Seite enthält die
  DOM-Anker, die das T10-Skript prüft: ein Canvas-Element und je ein per `id`
  adressierbares Element für Agent-Text, Watch-Tabelle und Input-Anzeige. Verhalten
  (Frames gezeichnet, Status aktualisiert) wird in T10 headless über die
  WebSocket-Ebene plus statische Seitenprüfung abgenommen — kein Browser-Test in
  dieser Initiative.
- **Scope:** `platforms/shared/desktop/liveview/`.
- **Expected diff:** ~340 (provisional)

## T10 — Finales Produkt-Gate: End-to-End

- **Goal:** Abschluss-Gate der Initiative: das komplette Zusammenspiel — Emulator,
  MCP, Live-View, Viewer-Auslieferung — an einem laufenden Prozess nachgewiesen, per
  Skript `tests/e2e/liveview_full_probe.mjs`.
- **Interfaces:** Neues Skript `tests/e2e/liveview_full_probe.mjs` (Node ≥22, keine
  npm-Dependencies). Es startet `platforms/linux/gearboy` mit `GEARBOY_TEST_ROM`,
  `--headless --mcp-http --mcp-http-port <frei>` und `--live-view --live-view-port
  <frei>` und spricht MCP über HTTP sowie den Live-View über WebSocket.
- **Acceptance properties:** Alle als Exit-Code beobachtbar: (1) T7-Assertions gelten
  weiter (200 auf `/`, ≥ 40 Binärframes in 5 s, PNG-Signatur); (2) die ausgelieferte
  Seite enthält Canvas und die T9-DOM-Anker und referenziert keine externen URLs;
  (3) MCP `tools/list` enthält `set_agent_status`; (4) nach `tools/call
  set_agent_status` mit einem Markertext trifft binnen 2 s ein Status-Frame mit genau
  diesem `agent_text` und strikt gewachsener `seq` ein; (5) nach `add_memory_watch`
  auf eine WRAM-Adresse enthält ein Status-Frame binnen 2 s den Watch mit Wert;
  (6) der Status-Frame trägt einen vollständigen `inputs`-Block, und nach
  `controller_button` (press) via MCP meldet ein Status-Frame binnen 2 s den Button
  als gedrückt; (7) Prozess endet sauber auf SIGTERM. Dazu Build, Suite und Guards
  grün (T4-Kommandos).
- **Scope:** `tests/e2e/`; nur aus rot laufenden Kommandos folgende Korrekturen in den
  Scopes von T5–T9.
- **Expected diff:** ~120 (provisional)

## Amendment Log

<!-- One dated entry per amendment, newest last. Empty until the first amendment lands. -->
