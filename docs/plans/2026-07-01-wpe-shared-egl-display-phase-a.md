# WPE Shared EGL Display — Phase A Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Share one `GstGLDisplay` across every `wpevideosrc` pipeline so multiple WPE WebViews coexist on flutter-pi's single process EGL display, eliminating the "Multiple EGL displays are not supported" SIGSEGV and letting Hearth revert PR #170's one-live-session cap.

**Architecture:** Add a process-global `GstGLDisplay` singleton (inside the `gstreamer_video_player` plugin) that wraps the embedder's existing EGL display. Inject it into each pipeline two ways: proactively via `gst_element_set_context()` right after `gst_parse_launch`, and reactively via a per-pipeline bus **sync** handler that answers `GST_MESSAGE_NEED_CONTEXT` for `gst.gl.GLDisplay`. The existing dmabuf→EGLImage frame path is untouched.

**Tech Stack:** C, GStreamer 1.26 (`gstreamer-gl-1.0`), EGL/GLES2, GBM, CMake + pkg-config, Unity (existing C test harness).

**Scope:** Phase A only. Phase B (zero-copy `GLMemory` handoff, wrapped `gst.gl.app_context`) is deferred to its own plan pending the two spikes in the design's "Open questions" section and on-Pi validation of Phase A.

**Design doc:** `docs/specs/2026-07-01-wpe-shared-egl-display-design.md`

## Global Constraints

- **GStreamer 1.26**; the WebView element is `wpevideosrc` (named `websrc`), never `wpesrc`.
- **Optional-dependency pattern:** all new GL code is guarded behind a compile define **`HAVE_GSTREAMER_GL`**, mirroring the existing `HAVE_WPE_WEBKIT` guard, so a build without `gstreamer-gl-1.0` still compiles (the sharing simply compiles out).
- **The bus sync handler runs on the GStreamer streaming thread:** it must touch **no** `struct gstplayer` state — only parse the message and set a context. No locking.
- **Match existing style:** `LOG_ERROR`/`LOG_DEBUG` from `util/logging.h`, `#ifdef` guards, existing brace/format conventions.
- **Commits:** author as the user — no self-reference, no `Co-Authored-By` lines naming any AI/model.
- **Push before requesting on-Pi testing** — the dev Pi's Hearth install script builds from the pushed branch.

## Verification Strategy (read before starting)

This change is **integration-level**: its effect (a second `wpevideosrc` sharing the display instead of tripping "Multiple EGL displays") only manifests with real GStreamer GL + WPE WebKit on the Pi. The Unity harness links `flutterpi_module` but constructing the singleton needs a live EGL/GBM display, which isn't present off-device, so there is **no meaningful off-device unit test** for this behavior — do not manufacture one with heavy mocks.

Two honest gates instead:
1. **Compile gate (local):** cross-compile cleanly after each code task.
2. **Behavioral gate (on-Pi):** the acceptance checklist in Task 3, run over SSH on the dev Pi.

---

### Task 1: Build system — add optional `gstreamer-gl-1.0` dependency

**Files:**
- Modify: `CMakeLists.txt:352-358` (the WPE WebKit block inside `BUILD_GSTREAMER_VIDEO_PLAYER_PLUGIN`)

**Interfaces:**
- Produces: the compile definition `HAVE_GSTREAMER_GL` and links `PkgConfig::LIBGSTREAMER_GL` when `gstreamer-gl-1.0` is found. Task 2's code is guarded on this define.

- [ ] **Step 1: Add the optional dependency next to the WPE block**

In `CMakeLists.txt`, inside the `if (LIBGSTREAMER_FOUND AND ...)` body (immediately after the existing `pkg_check_modules(LIBWPE_WEBKIT ...)` / WPE `if` block that ends at line 358), add:

```cmake
      # Optional: gstreamer-gl lets the embedder hand every wpevideosrc pipeline
      # a single shared GstGLDisplay wrapping flutter-pi's one EGL display. WPE
      # WebKit allows only one EGL display per process, so without a shared
      # display a second live webview trips "Multiple EGL displays are not
      # supported" and SIGSEGVs. If gstreamer-gl isn't present the sharing
      # compiles out (single-webview behavior), so this never breaks a build.
      pkg_check_modules(LIBGSTREAMER_GL IMPORTED_TARGET gstreamer-gl-1.0)
      if (LIBGSTREAMER_GL_FOUND)
        target_link_libraries(flutterpi_module PUBLIC PkgConfig::LIBGSTREAMER_GL)
        target_compile_definitions(flutterpi_module PRIVATE HAVE_GSTREAMER_GL)
      else()
        message(NOTICE "gstreamer-gl (gstreamer-gl-1.0) not found; multiple concurrent WPE webviews will not be supported (single-display sharing disabled).")
      endif()
```

- [ ] **Step 2: Configure and confirm detection**

Run (local aarch64 cross-config; use the preset matching your Pi):

```bash
cmake --preset cross-aarch64-default
```

Expected: configure succeeds; if `gstreamer-gl-1.0` is available in the sysroot, no `NOTICE` about it is printed (and `HAVE_GSTREAMER_GL` is defined). If it prints the NOTICE, install `libgstreamer-gl1.0-dev` (or the sysroot equivalent) before proceeding — Phase A is inert without it.

- [ ] **Step 3: Commit**

```bash
git add CMakeLists.txt
git commit -m "build: add optional gstreamer-gl-1.0 dependency for shared GL display"
```

---

### Task 2: Shared `GstGLDisplay` singleton, proactive context, and bus sync handler

**Files:**
- Modify: `src/plugins/gstreamer_video_player/player.c` (includes near line 21; new statics + helpers before `init()` at line 903; wiring inside `init()` between line 1034 and line 1040)

**Interfaces:**
- Consumes: `flutterpi_get_gl_renderer(struct flutterpi *)` (from `flutter-pi.h`), `gl_renderer_get_egl_display(struct gl_renderer *)` (from `gl_renderer.h`), `HAVE_GSTREAMER_GL` (from Task 1).
- Produces: file-static `get_shared_gl_display(struct flutterpi *)` and sync handler `on_bus_sync_message(...)`, both internal to `player.c`.

- [ ] **Step 1: Add guarded includes**

In `player.c`, immediately after the existing `HAVE_WPE_WEBKIT` include block (line 21), add:

```c
#ifdef HAVE_GSTREAMER_GL
    #include <gst/gl/egl/gstgldisplay_egl.h>
    #include <gst/gl/gl.h>

    #include "gl_renderer.h"
#endif
```

(`gl_renderer.h` provides `gl_renderer_get_egl_display` and the `EGLDisplay` type; `player.c` does not currently include it.)

- [ ] **Step 2: Add the singleton + sync handler above `init()`**

Insert this block immediately before `static int init(struct gstplayer *player, bool force_sw_decoders) {` (currently line 903):

```c
#ifdef HAVE_GSTREAMER_GL
/// Builds the process-global GstGLDisplay wrapping flutter-pi's single EGL
/// display. Called exactly once via g_once. Returns NULL (logged) if the GL
/// renderer or EGL display isn't available.
static gpointer create_shared_gl_display(gpointer userdata) {
    struct flutterpi *flutterpi = userdata;

    struct gl_renderer *renderer = flutterpi_get_gl_renderer(flutterpi);
    if (renderer == NULL) {
        LOG_ERROR("Can't create shared GstGLDisplay: no GL renderer available.\n");
        return NULL;
    }

    EGLDisplay egl_display = gl_renderer_get_egl_display(renderer);
    if (egl_display == EGL_NO_DISPLAY) {
        LOG_ERROR("Can't create shared GstGLDisplay: no EGL display available.\n");
        return NULL;
    }

    GstGLDisplay *display = GST_GL_DISPLAY(gst_gl_display_egl_new_with_egl_display(egl_display));
    if (display == NULL) {
        LOG_ERROR("gst_gl_display_egl_new_with_egl_display() returned NULL.\n");
    }

    return display;
}

/// Returns the process-global shared GstGLDisplay, creating it on first use.
/// Shared across every wpevideosrc pipeline so all WPE instances reference the
/// same EGL display (WPE WebKit enforces one EGL display per process). Never
/// torn down; lifetime matches the process EGL display. Does not transfer
/// ownership — callers must not unref. May return NULL if EGL isn't ready.
static GstGLDisplay *get_shared_gl_display(struct flutterpi *flutterpi) {
    static GOnce once = G_ONCE_INIT;
    return g_once(&once, create_shared_gl_display, flutterpi);
}

/// Bus SYNC handler: answers GstGL's context negotiation synchronously, on the
/// streaming thread, before the element falls back to creating its own display.
/// Handles only the gst.gl.GLDisplay request; everything else passes through to
/// the async on_bus_message watch. Touches no player state.
static GstBusSyncReply on_bus_sync_message(GstBus *bus, GstMessage *msg, gpointer userdata) {
    struct gstplayer *player = userdata;

    (void) bus;

    if (GST_MESSAGE_TYPE(msg) != GST_MESSAGE_NEED_CONTEXT) {
        return GST_BUS_PASS;
    }

    const gchar *context_type = NULL;
    gst_message_parse_context_type(msg, &context_type);

    if (g_strcmp0(context_type, GST_GL_DISPLAY_CONTEXT_TYPE) != 0) {
        return GST_BUS_PASS;
    }

    GstGLDisplay *display = get_shared_gl_display(player->flutterpi);
    if (display == NULL) {
        // Can't satisfy it; let the element self-provision (falls back to
        // today's single-webview behavior rather than crashing here).
        return GST_BUS_PASS;
    }

    GstContext *context = gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
    gst_context_set_gl_display(context, display);
    gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(msg)), context);
    gst_context_unref(context);

    return GST_BUS_DROP;
}
#endif
```

- [ ] **Step 3: Wire it into `init()` — proactive context + sync handler**

In `init()`, the bus is fetched at line 1034 (`bus = gst_pipeline_get_bus(...)`). Immediately **after** that line and **before** `gst_bus_get_pollfd(bus, &fd);` (line 1036), insert:

```c
#ifdef HAVE_GSTREAMER_GL
    // Provide the shared GstGLDisplay two ways: proactively on the pipeline
    // (present before wpevideosrc first probes) and via a sync handler (answers
    // a NEED_CONTEXT synchronously, before the element self-provisions its own
    // display). Both are needed so multiple WPE webviews share one EGL display.
    GstGLDisplay *gl_display = get_shared_gl_display(player->flutterpi);
    if (gl_display != NULL) {
        GstContext *gl_context = gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
        gst_context_set_gl_display(gl_context, gl_display);
        gst_element_set_context(pipeline, gl_context);
        gst_context_unref(gl_context);
    }

    gst_bus_set_sync_handler(bus, on_bus_sync_message, player, NULL);
#endif
```

This runs before the `gst_element_set_state(...)` calls (lines 1047/1050), so the context is in place for the `NULL→READY/PAUSED/PLAYING` transition that triggers WPE/GL init.

- [ ] **Step 4: Cross-compile to verify it builds**

Run:

```bash
cmake --build --preset cross-aarch64-default 2>&1 | tail -20
```

(or `cmake --build build-cross-aarch64-default` if you configured without a build preset)

Expected: `player.c` compiles and `flutter-pi` links with no errors. Common failures to check: missing `gstreamer-gl-1.0` in the sysroot (Task 1 NOTICE fired → `HAVE_GSTREAMER_GL` undefined → this code is inert, which is not what we want on the Pi), or a missing `gl_renderer.h` include (Step 1).

- [ ] **Step 5: Commit**

```bash
git add src/plugins/gstreamer_video_player/player.c
git commit -m "feat(gstreamer): share one GstGLDisplay across wpevideosrc pipelines

Provides a process-global GstGLDisplay wrapping flutter-pi's single EGL
display to every pipeline, proactively via gst_element_set_context and
reactively via a bus sync handler answering the gst.gl.GLDisplay
NEED_CONTEXT. Because all wpevideosrc instances now resolve the same
display, WPE WebKit's one-EGL-display-per-process invariant holds and
multiple concurrent webviews no longer trip \"Multiple EGL displays are
not supported\" / SIGSEGV. Guarded behind HAVE_GSTREAMER_GL so builds
without gstreamer-gl-1.0 are unaffected."
```

- [ ] **Step 6: Push (required before on-Pi testing)**

```bash
git push
```

---

### Task 3: On-Pi behavioral validation + coordinate the Hearth cap revert

This is the acceptance gate. It requires the **Hearth** repo change (A-dart) that reverts PR #170's one-live-session cap, so two WebViews can actually be warm at once. That change lives in the Hearth repo, not here — coordinate it as a paired branch.

**Files:** none in this repo. (Hearth: revert/relax the `WebviewSessionPool` capacity-1 LRU so ≥2 sessions stay warm.)

- [ ] **Step 1: Build on the dev Pi**

SSH to the dev Pi and run the Hearth install script so the embedder rebuilds from the pushed branch. Confirm the build log shows `gstreamer-gl-1.0` was found (no "single-display sharing disabled" NOTICE) — otherwise Phase A is compiled out.

- [ ] **Step 2: Reproduce the original crash condition — now fixed**

With the Hearth cap lifted, open **two** WebView pages (an HA dashboard and a custom URL). Verify in the service logs (`journalctl -u hearth -f`):
- **No** `Multiple EGL displays are not supported.`
- **No** `status=11/SEGV` / restart loop.
- Both pages render simultaneously.

- [ ] **Step 3: Verify warm switching restored**

Switch back and forth between the two WebView pages. Expected: instant swap — **no** multi-second WPE re-init / loading placeholder (the regression PR #170 traded away).

- [ ] **Step 4: Verify HA token injection still works**

Confirm both HA-dashboard WebViews load already authenticated (the `configure-web-view` document-start script still runs). The shared display must not have disturbed the existing `HAVE_WPE_WEBKIT` injection path.

- [ ] **Step 5: Record the result**

Note in the PR: number of concurrent WebViews validated, and confirmation of no SIGSEGV over a few minutes of switching. If a crash or a new GStreamer context warning appears, capture the full log and stop — that reshapes the design before Phase B.

---

## Self-Review

- **Spec coverage (Phase A):** shared `GstGLDisplay` singleton (Task 2 Step 2) ✓ · bus sync handler for `gst.gl.GLDisplay` (Task 2 Step 2) ✓ · proactive `gst_element_set_context` on the pipeline (Task 2 Step 3) ✓ · `gstreamer-gl-1.0` build dep guarded like `HAVE_WPE_WEBKIT` (Task 1) ✓ · error handling: NULL display logged and passes through, unknown context types pass through (Task 2 Step 2) ✓ · frame path untouched ✓ · on-Pi acceptance incl. cap-revert coordination + token-injection check (Task 3) ✓. Phase B intentionally out of scope.
- **Placeholder scan:** none — all code and commands are literal.
- **Type consistency:** `get_shared_gl_display(struct flutterpi *)` returns `GstGLDisplay *` and is called identically in the sync handler and in `init()`; `GST_GL_DISPLAY_CONTEXT_TYPE` used consistently for both the proactive context and the sync-handler match; `create_shared_gl_display` matches the `GThreadFunc`/`GOnce` signature (`gpointer(gpointer)`).
