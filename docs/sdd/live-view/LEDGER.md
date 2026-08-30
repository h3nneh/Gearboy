# live-view — Ledger

**Lease:** session_0143mRxmbctLNYLWy7bSS9gp | 2026-08-30T16:32:30+00:00
*(`unleased` at init — `sdd:plan` never writes a session id; format when held:
`<session-id> | <ISO timestamp>` — stale after 2h, taken/refreshed/cleared only by
`sdd:run`, spec §6)*
**Session model:** fable (above recommended orchestrator tier `opus` — cost signal noted to user)

**Plan:** docs/sdd/live-view/plan.md — **approved (2026-08-30)**.
**Budget:** 0.3 of 6h (provisional)
**Checkpoints:** sdd/live-view/phase-1 @ 0d5ad7da

| ID | Status | Class | Model used | Commits | Deviations | Parked findings | Escalations | Rounds | Fix-passes | Dispatches | Wall-clock | Diff |
|----|--------|-------|------------|---------|------------|------------------|-------------|--------|------------|------------|------------|------|
| T1 | done | S | opus | 856f48d9 | D5-Viewer-Heuristik (basename *viewer* oder `<html`), GPL-URL im Header erlaubt; D1 schlägt ohne git-Checkout fehl | — | — | 0 | 0 | d1 | — | 140 |
| T2 | done | M | opus | 1e95cd85 | tests/Makefile: neue Targets zusätzlich in all:/clean: | — | — | 0 | 0 | d1 | — | 570 |
| T3 | done | M | opus | 0d5ad7da | Payload-Cap 4 MiB; zusätzl. RFC-6455-Protokollfehler (RSV, Control-Frames, 64-bit-Länge, Close-Byte); BAD_REQUEST für non-GET/Headerzeile ohne Doppelpunkt; keine Sec-WebSocket-Version-Prüfung; tests/Makefile all:/clean: | Diff 1273 ≫ M-Budget 400 (Kalibrierung) | — | 0 | 0 | d1 | — | 1273 |
| T4 | done | S | opus | (kein Commit — Gate grün auf committed state) | — | Pre-existing: (1) `make -C tests` solo rot — miniz.c unter -std=c99 (ftello/fseeko), repro @3ae71c08; (2) tests/ und platforms/linux teilen miniz.o mit versch. Flags; (3) Testbinaries nicht gitignored → `-dirty` in --version. Vorschlag: je eigener Task | — | 0 | 0 | d1; review r1 (fable) dispatched | 16 (batch of 4) | 0 |
| T5 | pending | M | — | — | — | — | — | — | — | — | — | — |
| T6 | pending | M | — | — | — | — | — | — | — | — | — | — |
| T7 | pending | S | — | — | — | — | — | — | — | — | — | — |
| T8 | pending | M | — | — | — | — | — | — | — | — | — | — |
| T9 | pending | M | — | — | — | — | — | — | — | — | — | — |
| T10 | pending | S | — | — | — | — | — | — | — | — | — | — |

## Amendments

<!-- One dated entry per amendment, newest last. Empty until the first amendment lands. -->
