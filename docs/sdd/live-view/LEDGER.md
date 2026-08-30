# live-view — Ledger

**Lease:** session_0143mRxmbctLNYLWy7bSS9gp | 2026-08-30T16:55:05+00:00
*(`unleased` at init — `sdd:plan` never writes a session id; format when held:
`<session-id> | <ISO timestamp>` — stale after 2h, taken/refreshed/cleared only by
`sdd:run`, spec §6)*
**Session model:** fable (above recommended orchestrator tier `opus` — cost signal noted to user)

**Plan:** docs/sdd/live-view/plan.md — **approved (2026-08-30)**.
**Budget:** 0.6 of 6h (provisional)
**Checkpoints:** sdd/live-view/phase-1 @ 0d5ad7da; sdd/live-view/phase-2 @ 0fb299e1

| ID | Status | Class | Model used | Commits | Deviations | Parked findings | Escalations | Rounds | Fix-passes | Dispatches | Wall-clock | Diff |
|----|--------|-------|------------|---------|------------|------------------|-------------|--------|------------|------------|------------|------|
| T1 | done | S | opus | 856f48d9; 0fb299e1 (fix-pass, D1-Baseline lt. Amendment) | D5-Viewer-Heuristik (basename *viewer* oder `<html`), GPL-URL im Header erlaubt; D1 schlägt ohne git-Checkout fehl | Baseline pinnt Präsenz der 6 Deps nicht (Spec-Frage); Baseline-Literal dupliziert Amendment-Liste | — | 0 | 1 | d1; fp1 | — | 140 |
| T2 | done | M | opus | 1e95cd85 | tests/Makefile: neue Targets zusätzlich in all:/clean: | — | — | 0 | 0 | d1 | — | 570 |
| T3 | done | M | opus | 0d5ad7da | Payload-Cap 4 MiB; zusätzl. RFC-6455-Protokollfehler (RSV, Control-Frames, 64-bit-Länge, Close-Byte); BAD_REQUEST für non-GET/Headerzeile ohne Doppelpunkt; keine Sec-WebSocket-Version-Prüfung; tests/Makefile all:/clean: | Diff 1273 ≫ M-Budget 400 (Kalibrierung) | — | 0 | 0 | d1 | — | 1273 |
| T4 | done | S | opus | (kein Commit — Gate grün auf committed state) | — | Pre-existing: (1) `make -C tests` solo rot — miniz.c unter -std=c99 (ftello/fseeko), repro @3ae71c08; (2) tests/ und platforms/linux teilen miniz.o mit versch. Flags; (3) Testbinaries nicht gitignored → `-dirty` in --version. Vorschlag: je eigener Task. Review-Findings: [Notable] D1-Guard prüft nur untracked files — behoben im Fix-Pass 0fb299e1. [Cosmetic] D5-Zeilenfilter der GPL-URL könnte externe URL auf derselben Zeile verdecken — nur geloggt | — | 0 | 0 | d1; review r1 (fable): APPROVE_WITH_FINDINGS | 16 (batch of 4) | 0 |
| T5 | done | M | opus | 11594eed | PublishFrame mit Kanalzahl statt RGBA (Spec-Amendment D2); eingebauter Platzhalter bis SetPage (T8); Unit-Test deckt mehr als Start/Stop ab; stb_image_write-Impl im Testbinary | Diff 1261 ≫ M-Budget 400 (Kalibrierung) | — | 0 | 0 | d2 | — | 1261 |
| T6 | done | M | opus | d9af734f | --headless-Helptext um „live view" ergänzt; live_view_sanitize für ROM-Titel im Status-JSON; seq inkrementiert nur bei Statusänderung (D3-konform) | Status noch D3-Teilmenge (inputs/watches fehlen — planmäßig, T8) | — | 0 | 0 | d2 | — | 147 |
| T7 | done | S | opus | 94fa7a6f | Probe prüft zusätzlich T6-Observables (Helpflags, 404, kein Listener ohne Flag) | Pre-existing: 4× LTO-Warnung Processor_inline.h (@upstream); ALSA-Fehlerzeile bei headless im Container | — | 0 | 0 | d2; review r2 (fable) dispatched | 20 (batch of 3) | 305 |
| T8 | in_progress | M | — | — | — | — | — | — | — | — | — | — |
| T9 | in_progress | M | — | — | — | — | — | — | — | — | — | — |
| T10 | in_progress | S | — | — | — | — | — | — | — | — | — | — |

## Amendments

<!-- One dated entry per amendment, newest last. Empty until the first amendment lands. -->

- 2026-08-30: Spec D2 „RGBA"→RGB (mechanisch, GB_Color ist 3 Bytes); Plan T5 gleichlautend.
- 2026-08-30: Guard-Korrektur D1-Baseline-Check autorisiert (Phase-1-Review Notable); Umsetzung im Phase-2-Fix-Pass.
