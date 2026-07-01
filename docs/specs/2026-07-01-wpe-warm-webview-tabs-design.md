# Warm WPE webview "tabs" — instant, live, state-preserving switching

- **Date:** 2026-07-01
- **Repo:** flutter-pi-hearth (embedder) + Hearth (Dart app) — cross-repo
- **Status:** Phase 0 spike **PASSED** on-device (2026-07-01) — see "Phase 0 result" at the end. Phase 1 (warm-tab pool) pending; it is primarily a Hearth (Dart) repo change plus re-applying the validated native shared display+context.
- **Supersedes direction of:** `docs/specs/2026-07-01-wpe-shared-egl-display-design.md` (Phase A/B "EGL display sharing"), whose on-Pi validation reframed the problem (see Background).
- **Related:** Hearth PR #170 (one-live-session cap, the current stable behavior)

## Goal

Let the user keep **multiple webviews live at once** ("tabs"): open webview A, navigate/scroll within it, switch to webview B, and switch back to A **instantly with A's live state intact** (no reload, no re-init). Today (Hearth PR #170) only one WPE session is live and every switch fully re-initializes it — losing state and costing a multi-second placeholder.

## Non-goals

- Displaying two webviews **on screen simultaneously**. The requirement is confirmed: exactly one webview visible at a time; "tabs" are live in the background but only one is composited. (A `PageView` swipe momentarily mounts an offscreen neighbour — that counts as "another live tab," not simultaneous display.)
- Rewriting the non-WPE (RTSP/file/Plex) video paths.
- Truly unbounded tabs. A small bounded pool (e.g. 2–3 warm tabs) is the target; RAM is the limit (each tab is a full WebKit web process).

## Background: what the Phase A on-Pi validation taught us

The earlier "share one `GstGLDisplay`" work (Phase A) was implemented, reviewed, and deployed to the dev Pi. Result:

- It **eliminated** the `Multiple EGL displays are not supported.` error (0 occurrences).
- But the process still **SIGSEGV'd during webview switching**. Core dumps (gdb) put the crash in
  `wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image` (libWPEBackend-fdo) ← `libgstwpe.so`, on the `GstWPEContextTh` thread. **No flutter-pi code was in any crash stack.**
- This is a **known upstream bug**: WPEBackend-fdo #175 ("Invalid write in `releaseImage`") and gst-plugins-bad #1372/#1386 ("crash on 2nd WPE src"). It is a **use-after-free during teardown** — an exported EGL image is released *after* its backend/Wayland resource has been destroyed.

Phase A was therefore reverted on `hearth` (mainline restored to the stable PR #170 behavior).

### The reframe

Two facts change the approach:

1. **The crash is teardown-specific.** PR #170 disposes the WPE session on *every switch*, so PR #170's own cap is what keeps feeding views into the teardown UAF. If we **never tear down on switch** (keep tabs warm), we never hit that code path.
2. **gstwpe is designed for multiple concurrent instances.** Reading `ext/wpe/WPEThreadedView.cpp` (current gstwpe): all `wpevideosrc` instances share **one** `WPEContextThread` (a `g_once` singleton — one WebKit GLib main loop, all WebKit calls serialized), and each `WPEView` **refs and uses a provided `gst.display` (GstGLDisplay) and `gst.context` (GstGLContext)**. That is exactly the Phase A + Phase B context-sharing mechanism, and it is the *intended* way to feed multiple instances.

So the plausible path to warm tabs: **share one `GstGLDisplay` + one `GstGLContext` across N `wpevideosrc` pipelines that are kept alive (never torn down on switch); switching just composites an already-live texture.** This revives the reverted Phase A (shared display), adds the previously-deferred Phase B (shared context), and adds the one thing never tried: keep-alive.

The single unproven assumption is whether **2–3 concurrent, kept-alive `wpevideosrc` actually coexist on our gst 1.26 / WPE 2.48.3.** The source says yes; only the Pi can confirm. So the design **leads with a spike.**

## Approach: spike-first, then warm-tab pool

### Phase 0 — Spike (de-risk the core assumption). BLOCKING.

Prove, on the dev Pi, that multiple concurrent kept-alive `wpevideosrc` pipelines coexist without the teardown UAF or a creation crash.

- **Native:** provide a shared `GstGLDisplay` **and** a wrapped `GstGLContext` (`gst.gl.app_context`) to every pipeline via the bus sync handler + proactive `gst_element_set_context` (the reverted Phase A code plus the Phase B context wrap).
- **Minimal harness:** stand up **two** `wpevideosrc` webview pipelines at once (two different HA dashboard URLs), both driven to PLAYING, and **keep both alive** (do not dispose either). This can be a temporary native/Dart test path — it does **not** require the full warm-pool rework yet.
- **Success criteria (all must hold, observed on-device):**
  1. Both pipelines reach PLAYING and both render (two live textures).
  2. **No** `Multiple EGL displays` log, **no** SIGSEGV, over a multi-minute soak while both stay alive.
  3. Compositing/switching *which* texture is shown (without tearing either down) is stable.
  4. RAM with 2 live WPE web processes is acceptable on the target Pi (record the number).
- **Decision gate:**
  - **Pass →** proceed to Phase 1 (warm-tab pool).
  - **Fail (concurrent creation/coexistence still crashes) →** the low-effort path is dead; fall back to Direct WPE multi-view embedding (see Fallback) or accept the status quo. Do **not** build the pool on a failed spike.

### Phase 1 — Warm-tab pool (only if Phase 0 passes)

**Native (flutter-pi-hearth):**
- Shared `GstGLDisplay` singleton (revive reverted Phase A) — answer `gst.gl.GLDisplay` NEED_CONTEXT + proactive set.
- Shared wrapped `GstGLContext` singleton (Phase B) — answer `gst.gl.app_context`, wrapping a dedicated context from `gl_renderer_create_context()`. (Resolve the wrapped-context `fill_info` threading question flagged in the prior spec during Phase 0.)
- No per-switch teardown from the native side; pipelines live until explicitly disposed.
- **Careful teardown for the *removal* case:** when a tab is genuinely removed (config change / pool eviction), that single dispose still risks the WPEBackend-fdo `releaseImage` UAF. Handle removals rarely and serialized (drive to NULL and await, as PR #170 does for its cap) so at most one teardown is in flight and no image release races a destroyed backend.

**Dart (Hearth):**
- Replace PR #170's capacity-1 LRU with a **bounded warm pool** (N tabs, N≈2–3). Sessions for configured/recent webview URLs stay **alive and warm**; switching selects an already-live session and composites its texture — no dispose, no re-init.
- Eviction (when the pool exceeds N) is the only path that disposes a session → routes through the native serialized-teardown path.
- HA token injection is unaffected (the document-start user script stays registered on each live view).

### Fallback — Direct WPE multi-view embedding (only if Phase 0 fails)

If concurrent `wpevideosrc` cannot be made stable, the "correct" architecture is to embed WPE directly in flutter-pi, bypassing gstwpe: one `WebKitWebContext`, N `WebKitWebView`s (`SHARED_SECONDARY_PROCESS`), each with a WPE backend + frame callback delivering exported images to Flutter textures, compositing the active one. This is how production multi-tab WPE apps work, but it is a **large native project** (weeks: backend management, EGLImage import, per-view input routing) and still rides on WPEBackend-fdo's export path. Documented here as the escape hatch; **not** to be started unless the spike rules out the pool.

## Risks

- **Spike may fail** — concurrent creation (not just teardown) may still crash on 2.48.3 (#1386 was "2nd src → `view->buffer()` NULL"). Mitigated by making Phase 0 blocking and cheap.
- **Memory** — N live WebKit web processes. 2–3 HA-dashboard tabs on the target Pi must be measured (Phase 0 criterion 4).
- **Removal teardown UAF** — the WPEBackend-fdo bug still exists for genuine tab removal; mitigated by rarity + serialized single-teardown, not eliminated. An upstream WPE/WPEBackend-fdo update is the only true fix and is out of scope.
- **Wrapped-context threading** — the `gst.gl.app_context` `fill_info`/activation-thread question (carried from the prior spec) must be resolved in Phase 0.

## Test plan

- **Local:** cross-compile / on-Pi build gate (no off-device unit test exists for this WPE-integration behavior — same rationale as the prior spec).
- **Phase 0:** the four success criteria above, on the dev Pi, over a multi-minute two-tab soak.
- **Phase 1:** open ≥2 webviews; switch back and forth repeatedly → instant, **state preserved** (scroll/in-page navigation intact), no reload, no SIGSEGV; then remove a tab (config change) → clean serialized teardown, no crash; RAM steady over a soak.

## Open questions

- Does gst 1.26 / WPE 2.48.3 permit ≥2 concurrent live `wpevideosrc`? (Phase 0 answers this — the whole gate.)
- Wrapped `GstGLContext` `fill_info` threading — lazy in 1.26, or explicit activation on a controlled thread? (Resolve in Phase 0.)
- Practical warm-tab count N on the target Pi given RAM. (Measure in Phase 0.)
- Is serialized single-teardown sufficient to avoid the `releaseImage` UAF on tab removal, or is removal inherently unsafe until an upstream fix?

## Phase 0 result (2026-07-01) — PASSED

On the dev Pi (gst 1.26.2 / WPE 2.48.3), with the native shared `GstGLDisplay` + wrapped `GstGLContext` (`gst.gl.app_context`) provided to every pipeline, a throwaway harness (`FLUTTERPI_WPE_SPIKE`) stood up **two** `wpevideosrc` pipelines and kept them alive alongside the Dart app's one live webview:

- **3 concurrent WPE web processes coexisted** (≈372–416 MB each) for a >130 s soak with **zero** SIGSEGV and **zero** "Multiple EGL displays" — no creation crash (#1386 does not bite on this stack), no teardown crash (nothing was torn down).
- The wrapped `app_context` worked **without** the `fill_info` activation fallback.
- The earlier switch-crash is thereby **isolated to teardown** (PR #170's dispose-on-switch), confirming the design's central bet: keep views alive, never tear down on switch.
- **RAM:** ~400 MB per live tab (heavy HA dashboards); N≈2–3 warm tabs is the practical ceiling on a 4 GB+ Pi. Phase 1 must bound the pool accordingly.

**Decision: proceed to Phase 1 (warm-tab pool), not the direct-embedding fallback.** The validated native groundwork (shared display + wrapped context) lives in git history — commits `6f348c3`, `be060af`, `0b188cb`, `66f3e45` (reverted from mainline for kiosk safety in `9ac548b`, since without the pool it still crashes on PR #170's teardown) — to be re-applied together with the pool. The throwaway harness (`7dab197`) is not reused.
