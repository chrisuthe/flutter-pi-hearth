# Phase 1 — Bounded warm webview pool

- **Date:** 2026-07-01
- **Repos:** Hearth (Dart app — the substance) + flutter-pi-hearth (embedder — re-apply validated groundwork)
- **Status:** Design approved, pending implementation plan
- **Parent:** `flutter-pi-hearth/docs/specs/2026-07-01-wpe-warm-webview-tabs-design.md` (Phase 0 spike PASSED)
- **Related:** Hearth PR #170 (the one-live-session cap this replaces)

## Goal

Keep the N most-recently-used webviews **live and warm** so switching between them is instant with state preserved (scroll position, in-page navigation, DOM/JS). Replaces PR #170's cap of one live session (which fully re-initializes on every switch, losing state and showing a multi-second placeholder).

## Background

Phase 0 proved on the dev Pi (Pi 5, 8 GB; gst 1.26.2 / WPE 2.48.3) that **multiple concurrent `wpevideosrc` pipelines coexist** when the embedder shares one `GstGLDisplay` + one wrapped `GstGLContext` — 3 kept-alive WPE views soaked >130 s with zero crashes, ~400 MB each. The SIGSEGV that sank the original approach is **teardown-specific** (an upstream WPEBackend-fdo use-after-free in `releaseImage` during view destruction, triggered by PR #170's dispose-on-every-switch). The warm pool's core bet: **stop tearing down on switch.**

The user runs 5+ webviews, but most deployments are 2–4. So the pool is **bounded** (default N=4): the common case stays fully warm and never evicts; eviction is a rare, gracefully-handled event, not the hot path.

## Non-goals

- Two webviews on screen at once (confirmed: one visible at a time; others warm in the background).
- The recycle-by-navigation architecture (Approach B) — deliberately not built; eviction teardown is accepted as a rare, best-effort operation.
- Pausing/"freezing" offscreen tabs — warm sessions stay **PLAYING** (live). YAGNI unless CPU proves a problem on-device.

## Architecture

### 1. Native (flutter-pi-hearth) — re-apply validated groundwork

Cherry-pick the four Phase-0-validated commits back onto `hearth` (reverted for kiosk safety in `9ac548b`, recoverable):

- `6f348c3` — optional `gstreamer-gl-1.0` CMake dependency (`HAVE_GSTREAMER_GL`).
- `be060af` — shared `GstGLDisplay` singleton + bus sync handler (`gst.gl.GLDisplay`).
- `0b188cb` — doc-comment clarifications.
- `66f3e45` — wrapped `GstGLContext` singleton (`gst.gl.app_context`).

Do **not** bring back the throwaway harness (`7dab197`). This is the entire native change; it is what lets N concurrent pipelines coexist. Eviction uses the existing gstplayer dispose path — no new native teardown code.

### 2. Dart — `WebviewSessionPool`: cap-1 → cap-N LRU

Keep the URL-keyed `_sessions` map and the `_pending` / `await session.shutdown()` serialization exactly as-is (that machinery is now used only for eviction and settings-driven rebuilds, not every switch). Change only the eviction policy:

- **Add** a capacity constant `N` (default **4**) and LRU access-ordering (e.g. a `LinkedHashMap` reinserted on access, or a parallel access-order list).
- **`_resolve`:**
  - Requested URL already warm (matching injector + size, per the existing match logic) → mark MRU and **return it; no teardown**.
  - Miss → construct the new session, insert it, mark it MRU; **then** while `_sessions.length > N`, evict the **LRU** session via the existing serialized `shutdown()`. Because the new session is inserted and marked MRU before the capacity check, eviction never targets the view being switched to, and the evicted view is a *different, idle* LRU session — not racing its own recreation (the condition that crashed under PR #170).
- **`reconcile`/`release`/`releaseAll`/`pauseAll`/`resumeAll`:** unchanged — they now operate over ≤N sessions instead of ≤1.

### 3. Dart — `WebviewScreen`: let offscreen pages stay warm

Today only the active page resolves, and it detaches its session on scroll-away (that was the cap-1 enforcement). Change:

- Allow offscreen / PageView-prebuilt pages to resolve (so neighbours warm up). Remove the `if (!widget.isActive) return` gate on resolving.
- **Do not detach** on scroll-away — the session stays warm in the pool. Remove the `_detachSession()` call from the inactive transition.
- Keep the `_resolveGen` stale-resolve guard (still correct — a superseded resolve must not attach).
- Keep `isActive`, **repurposed**: it no longer gates resolve/detach; it now only routes touch / navigation events to the visible webview (offscreen warm tabs render but receive no input).

## Data flow / lifecycle

- Each session owns a stable `VideoPlayerController` + texture for its warm lifetime; the active screen paints its session's texture.
- **Injector/size change for an already-warm URL** (HA token changed in Settings, or a different render size) still rebuilds that one session (dispose→recreate) via the serialized path — the only same-URL teardown trigger, and rare.
- **Idle** pauses/resumes all warm sessions (unchanged).
- **Create-then-evict** ordering guarantees the just-created session is never the eviction target.

## Eviction — the residual risk

Evicting the LRU still calls WPE's `releaseImage` teardown path (the upstream UAF). Mitigations: it is serialized, and it tears down an *idle* view that is not racing a creation (unlike PR #170). It is not eliminated. Per the "handle gracefully" decision: eviction is rare (only past N distinct webviews), and if it ever crashes, systemd restarts the kiosk — acceptable degradation. On-Pi validation measures how real the risk is; it is not a design blocker.

## Testing

### Dart unit tests (real TDD — pure logic, no device)

Extend `test/modules/webview/webview_session_test.dart` (or a pool test) using the existing `WebviewSession.testing` factory:

- N distinct URLs all stay warm — no `shutdown()` called.
- The (N+1)th resolve evicts exactly the LRU and retains the other N.
- Re-resolving a warm URL returns the **same** instance with no teardown.
- An injector change for a warm URL rebuilds that session (dispose old, create new).
- Create-then-evict never disposes the just-created session.
- Serialization holds under concurrent resolves (the existing `_pending` guarantee).

`flutter test` and `flutter analyze` clean on changed files.

### On-Pi validation

Configure 5 webviews; swipe through them:

1. The 4 MRU switch **instantly with state preserved** (scroll position / in-page location retained) — no reload, no placeholder.
2. Visiting the 5th evicts the oldest; returning to the evicted one reloads it (cold) — expected.
3. **No crash during warm switching**; observe whether/how often eviction teardown crashes (residual risk).
4. RAM bounded (~N × 400 MB) and steady over a soak.
5. HA token injection authenticates each warm dashboard; idle pause/resume works.

## Sequencing / rollback

- **(native)** re-apply the four groundwork commits — self-contained, revertible.
- **(Dart)** pool cap-N + screen keep-warm — the substance; gated on the native change being deployed.
- If eviction teardown proves to crash too often in practice, the fallback is the recycle-by-navigation architecture (Approach B in the parent spec) — a larger effort held in reserve.

## Open questions

- Does an *isolated* eviction teardown (idle LRU view, not racing a creation) actually crash, or was the original crash specifically PR #170's dispose-then-immediately-recreate overlap? On-Pi validation answers this; it decides whether Approach A is sufficient or Approach B is eventually needed.
- Final default for `N` — 4 is the proposed start; on-Pi RAM/CPU under real dashboards may adjust it.
