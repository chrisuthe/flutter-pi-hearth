# Warm WPE Tabs — Phase 0 Spike Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove on the dev Pi whether **two concurrent, kept-alive `wpevideosrc` pipelines** — both sharing one `GstGLDisplay` **and** one `GstGLContext` — coexist and render steadily without crashing. This single result decides whether warm webview tabs are achievable via the low-effort pool (spike passes) or require heavyweight direct WPE embedding (spike fails).

**Architecture:** Re-apply the reverted Phase A shared-`GstGLDisplay` code, add the Phase B shared wrapped-`GstGLContext` (`gst.gl.app_context`), then stand up a throwaway env-gated native harness that runs two `wpevideosrc` pipelines to PLAYING and keeps both alive while pulling/releasing frames. Validate coexistence on the Pi. The crash we hit before was on **teardown**; this spike deliberately never tears down, testing only concurrent creation + steady-state — the exact condition warm tabs rely on.

**Tech Stack:** C, GStreamer 1.26 (`gstreamer-gl-1.0`, gstwpe), WPE WebKit 2.48, EGL/GLES2, CMake, on-Pi build over SSH.

**Spec:** `docs/specs/2026-07-01-wpe-warm-webview-tabs-design.md`

**Scope:** Phase 0 only. The Phase 1 warm-tab pool (native keep-alive semantics + Hearth bounded warm pool reversing PR #170's cap) is a separate plan, written only if this spike passes.

## Global Constraints

- **GStreamer 1.26 / WPE 2.48.3**; the webview element is `wpevideosrc` (named `websrc`).
- All new GL code guarded behind **`HAVE_GSTREAMER_GL`**, mirroring the existing `HAVE_WPE_WEBKIT` guard (compiles out cleanly when `gstreamer-gl-1.0` is absent).
- The bus sync handler runs on the GStreamer **streaming thread** — it may read only the immutable `player->flutterpi`; no other player state, no locking.
- Commits authored as the user — no AI/Claude/`Co-Authored-By` self-reference.
- **No off-device test exists** for this WPE-integration behavior; verification is the on-Pi build gate + the Phase 0 validation protocol (Task 3). Do not fabricate unit tests.
- **Build/verify loop:** push branch → on the Pi (`ssh hearthdev@10.0.1.13`) rebuild in `~/flutter-pi-verify` (`git fetch/reset origin hearth`, `make -j4`), install + restart for on-device runs.
- The harness in Task 3 is **throwaway scaffolding** — it is removed (or left dormant behind the env flag, off by default) before any Phase 1 work; it must never run unless `FLUTTERPI_WPE_SPIKE` is set.

---

### Task 1: Re-apply the shared `GstGLDisplay` (revive reverted Phase A)

**Files:**
- Modify: `CMakeLists.txt` (optional `gstreamer-gl-1.0` dep), `src/plugins/gstreamer_video_player/player.c` (singleton + sync handler for `gst.gl.GLDisplay`)

**Interfaces:**
- Produces: `HAVE_GSTREAMER_GL` compile def + link of `PkgConfig::LIBGSTREAMER_GL`; file-static `get_shared_gl_display(struct flutterpi *)` returning `GstGLDisplay *`; a bus sync handler `on_bus_sync_message` answering `GST_GL_DISPLAY_CONTEXT_TYPE`. Task 2 extends the same singleton file-section and the same sync handler.

- [ ] **Step 1: Re-apply the three reverted Phase A commits verbatim**

These commits were implemented, reviewed clean, and confirmed to compile on the Pi; they were reverted only because display-sharing alone was insufficient. Re-apply them exactly:

```bash
git cherry-pick a50f04c 84a53dc 1a61ff5
```

If any cherry-pick reports a conflict (mainline unchanged since, so none expected), abort and re-apply by hand from `git show <sha>`. The result must reintroduce, in `player.c` (guarded by `#ifdef HAVE_GSTREAMER_GL`): the includes (`gst/gl/gl.h`, `gst/gl/egl/gstgldisplay_egl.h`, `gl_renderer.h`), `create_shared_gl_display`/`get_shared_gl_display`, `on_bus_sync_message` (answering `GST_GL_DISPLAY_CONTEXT_TYPE`), and the `init()` wiring (proactive `gst_element_set_context` + `gst_bus_set_sync_handler`) right after `bus = gst_pipeline_get_bus(...)`; and in `CMakeLists.txt` the optional `gstreamer-gl-1.0` block.

- [ ] **Step 2: Push and build-gate on the Pi**

```bash
git push origin HEAD:hearth && git push gitea HEAD:hearth
```

Then on the Pi:

```bash
ssh -o BatchMode=yes hearthdev@10.0.1.13 'cd ~/flutter-pi-verify && git fetch -q origin hearth && git reset -q --hard origin/hearth && cd build && make -j4 2>&1 | tail -5'
```

Expected: builds clean; the earlier Pi build already proved this exact code compiles with `HAVE_GSTREAMER_GL` active (gstreamer-gl 1.26.2 present).

---

### Task 2: Add the shared wrapped `GstGLContext` (`gst.gl.app_context`, Phase B)

**Files:**
- Modify: `src/plugins/gstreamer_video_player/player.c` (extend the singleton section + `on_bus_sync_message` + `init()` wiring)

**Interfaces:**
- Consumes: `get_shared_gl_display(struct flutterpi *)` (Task 1); `gl_renderer_create_context(struct gl_renderer *)` → `EGLContext` (from `gl_renderer.h`).
- Produces: `get_shared_gl_context(struct flutterpi *)` returning `GstGLContext *`; the sync handler and `init()` now also answer/set `"gst.gl.app_context"`.

- [ ] **Step 1: Add the wrapped-context singleton**

In `player.c`, inside the `#ifdef HAVE_GSTREAMER_GL` block, immediately after `get_shared_gl_display`, add:

```c
/// Builds the process-global wrapped GstGLContext over one of flutter-pi's
/// shared EGL contexts, so gstwpe's internal GL context is created sharing GL
/// objects with flutter-pi (required for the exported-image/appsink handoff
/// and for multiple wpevideosrc to agree on one context). g_once; may return NULL.
static gpointer create_shared_gl_context(gpointer userdata) {
    struct flutterpi *flutterpi = userdata;

    GstGLDisplay *display = get_shared_gl_display(flutterpi);
    if (display == NULL) {
        LOG_ERROR("Can't create shared GstGLContext: no shared GstGLDisplay.\n");
        return NULL;
    }

    struct gl_renderer *renderer = flutterpi_get_gl_renderer(flutterpi);
    EGLContext egl_context = gl_renderer_create_context(renderer);
    if (egl_context == EGL_NO_CONTEXT) {
        LOG_ERROR("Can't create shared GstGLContext: gl_renderer_create_context failed.\n");
        return NULL;
    }

    GstGLContext *context =
        gst_gl_context_new_wrapped(display, (guintptr) egl_context, GST_GL_PLATFORM_EGL, GST_GL_API_GLES2);
    if (context == NULL) {
        LOG_ERROR("gst_gl_context_new_wrapped() returned NULL.\n");
    }

    return context;
}

/// Returns the process-global wrapped GstGLContext, creating it on first use.
/// Does not transfer ownership. May return NULL if EGL/display isn't ready.
static GstGLContext *get_shared_gl_context(struct flutterpi *flutterpi) {
    static GOnce once = G_ONCE_INIT;
    return g_once(&once, create_shared_gl_context, flutterpi);
}
```

- [ ] **Step 2: Answer `gst.gl.app_context` in the sync handler**

In `on_bus_sync_message`, replace the single display-only `if` block that returns `GST_BUS_DROP` with handling for **both** context types. After the existing `gst_message_parse_context_type(msg, &context_type);`, use:

```c
    if (g_strcmp0(context_type, GST_GL_DISPLAY_CONTEXT_TYPE) == 0) {
        GstGLDisplay *display = get_shared_gl_display(player->flutterpi);
        if (display == NULL) {
            return GST_BUS_PASS;
        }
        GstContext *context = gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
        gst_context_set_gl_display(context, display);
        gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(msg)), context);
        gst_context_unref(context);
        return GST_BUS_DROP;
    }

    if (g_strcmp0(context_type, "gst.gl.app_context") == 0) {
        GstGLContext *gl_context = get_shared_gl_context(player->flutterpi);
        if (gl_context == NULL) {
            return GST_BUS_PASS;
        }
        GstContext *context = gst_context_new("gst.gl.app_context", TRUE);
        gst_structure_set(gst_context_writable_structure(context), "context", GST_TYPE_GL_CONTEXT, gl_context, NULL);
        gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(msg)), context);
        gst_context_unref(context);
        return GST_BUS_DROP;
    }

    return GST_BUS_PASS;
```

- [ ] **Step 3: Proactively set the app context in `init()`**

In `init()`, inside the existing `#ifdef HAVE_GSTREAMER_GL` block where the display context is set proactively, add — right after the display `if (gl_display != NULL) { ... }` block and before `gst_bus_set_sync_handler(...)`:

```c
    GstGLContext *gl_app_context = get_shared_gl_context(player->flutterpi);
    if (gl_app_context != NULL) {
        GstContext *app_ctx = gst_context_new("gst.gl.app_context", TRUE);
        gst_structure_set(gst_context_writable_structure(app_ctx), "context", GST_TYPE_GL_CONTEXT, gl_app_context, NULL);
        gst_element_set_context(pipeline, app_ctx);
        gst_context_unref(app_ctx);
    }
```

- [ ] **Step 4: Build-gate on the Pi, and investigate the wrapped-context `fill_info` question**

Push, then rebuild on the Pi (same commands as Task 1 Step 2). Expected: compiles clean.

**Investigation (resolve during the on-Pi run in Task 3, not now):** a wrapped `GstGLContext` may need its GL info populated before gstwpe can share with it. Default: rely on GstGL populating it lazily inside `gst_gl_context_create` (no manual activation). If the Task 3 run logs errors such as gstwpe failing to share/create its context, or falling back to its own context, then the fix is a one-time `gst_gl_context_activate(ctx, TRUE)` + `gst_gl_context_fill_info(ctx, &error)` + `gst_gl_context_activate(ctx, FALSE)` in `create_shared_gl_context`, performed on a thread where making `egl_context` current does not conflict with flutter-pi's render/raster threads. Record which path was needed.

- [ ] **Step 5: Commit** (folded with Steps 1–3 if not already committed by the build-gate iteration)

```bash
git add src/plugins/gstreamer_video_player/player.c
git commit -m "feat(gstreamer): also share a wrapped GstGLContext (gst.gl.app_context) with wpevideosrc pipelines"
```

---

### Task 3: Two-`wpevideosrc` coexistence harness + on-Pi decision gate

**Files:**
- Modify: `src/plugins/gstreamer_video_player/plugin.c` (env-gated spike entry point) — or `player.c` if a helper there is cleaner. The harness is throwaway scaffolding, off unless `FLUTTERPI_WPE_SPIKE` is set.

**Interfaces:**
- Consumes: `get_shared_gl_display` / `get_shared_gl_context` and `on_bus_sync_message` (Tasks 1–2) via a small shared helper, or replicates the sync-handler install on each spike pipeline.

- [ ] **Step 1: Add the env-gated spike harness**

Add a function that, when `FLUTTERPI_WPE_SPIKE` is set to two `;`-separated URLs, builds **two** independent pipelines and keeps both alive. Gate it so it never runs in normal operation. Concrete shape:

```c
#ifdef HAVE_GSTREAMER_GL
// THROWAWAY Phase-0 spike: prove two concurrent wpevideosrc coexist. Enabled
// only when FLUTTERPI_WPE_SPIKE="url1;url2" is set. Remove after Phase 0.
static void wpe_spike_maybe_run(struct flutterpi *flutterpi) {
    const char *spec = getenv("FLUTTERPI_WPE_SPIKE");
    if (spec == NULL) {
        return;
    }
    char *dup = strdup(spec);
    char *sep = strchr(dup, ';');
    if (sep == NULL) { LOG_ERROR("WPE_SPIKE needs 'url1;url2'\n"); free(dup); return; }
    *sep = '\0';
    const char *url1 = dup, *url2 = sep + 1;

    for (int i = 0; i < 2; i++) {
        const char *url = (i == 0) ? url1 : url2;
        char desc[1024];
        snprintf(desc, sizeof desc,
                 "wpevideosrc name=websrc location=%s draw-background=false ! videoconvert ! fakesink sync=false", url);
        GError *err = NULL;
        GstElement *pipeline = gst_parse_launch(desc, &err);
        if (pipeline == NULL) { LOG_ERROR("spike pipeline %d failed: %s\n", i, err->message); continue; }

        // Provide the shared display + context via the same mechanism as real pipelines.
        struct gstplayer stub = { .flutterpi = flutterpi };
        GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
        gst_bus_set_sync_handler(bus, on_bus_sync_message, &stub, NULL);   // stub lives for process lifetime below
        GstGLDisplay *d = get_shared_gl_display(flutterpi);
        if (d != NULL) { GstContext *c = gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE); gst_context_set_gl_display(c, d); gst_element_set_context(pipeline, c); gst_context_unref(c); }
        GstGLContext *gc = get_shared_gl_context(flutterpi);
        if (gc != NULL) { GstContext *c = gst_context_new("gst.gl.app_context", TRUE); gst_structure_set(gst_context_writable_structure(c), "context", GST_TYPE_GL_CONTEXT, gc, NULL); gst_element_set_context(pipeline, c); gst_context_unref(c); }
        gst_object_unref(bus);

        GstStateChangeReturn r = gst_element_set_state(pipeline, GST_STATE_PLAYING);
        LOG_DEBUG("WPE_SPIKE pipeline %d (%s) -> set_state PLAYING = %d\n", i, url, r);
        // Intentionally leak: keep both pipelines alive for the whole process (no teardown).
    }
    // dup intentionally leaked (referenced by pipelines' location); process-lifetime spike only.
}
#endif
```

Note the `struct gstplayer stub` must outlive the pipeline — since we keep pipelines alive for the process lifetime, allocate the two stubs statically or heap-leak them (throwaway). Adjust to two file-static `struct gstplayer` stubs so the sync handler's `player->flutterpi` stays valid. Call `wpe_spike_maybe_run(flutterpi)` once from the plugin's init, after the GL renderer is available.

- [ ] **Step 2: Deploy and run on the Pi**

Push, rebuild+install on the Pi, then run flutter-pi (via the service or manually) with the env set to two real HA dashboard URLs, e.g. `dashboard-home` and `dashboard-cameras`. Because the service `ExecStart` is fixed, run manually for the spike:

```bash
ssh -o BatchMode=yes hearthdev@10.0.1.13 'sudo systemctl stop hearth; \
  FLUTTERPI_WPE_SPIKE="https://ha.home.chrisuthe.com/dashboard-home;https://ha.home.chrisuthe.com/dashboard-cameras" \
  /usr/local/bin/flutter-pi --release --mirror-connector HDMI-A-2 /opt/hearth/bundle > /tmp/spike.log 2>&1 & \
  sleep 90; echo "--- alive? ---"; pgrep -a flutter-pi | head; echo "--- crash/EGL/context signatures ---"; \
  grep -iE "SPIKE|Multiple EGL|SIGSEGV|segfault|WPEContextTh|assert|share|app_context|gl context|error" /tmp/spike.log | tail -40'
```

- [ ] **Step 3: Evaluate against the decision gate**

Record, from `/tmp/spike.log`, the running process, and `journalctl`/RAM:

1. Both pipelines reach `set_state PLAYING` and produce frames (no NULL-buffer / `view->buffer()` crash on the 2nd) — the `#1386` condition.
2. **No** `Multiple EGL displays`, **no** SIGSEGV, over the ≥90s soak with both alive.
3. Note whether the wrapped-context `fill_info` fallback (Task 2 Step 4) was required.
4. RAM for two live WPE web processes (`ps -o rss` on the two `WPEWebProcess` + flutter-pi) — is it acceptable on this Pi?

**Gate:**
- **All pass →** warm tabs are viable via the pool. Stop here; the Phase 1 pool plan is written next. Leave Tasks 1–2 code in place (guarded, inert without real pool wiring); **disable/remove the Task 3 harness** (it must not run without the env flag — confirm `FLUTTERPI_WPE_SPIKE` unset in the service).
- **Any fail →** capture the exact failure (log + backtrace via the core-dump procedure from the prior debugging: raise `LimitCORE`, reproduce, `sudo gdb` the core). Warm tabs via concurrent `wpevideosrc` is not viable on this stack → the spec's Direct WPE multi-view embedding fallback (or accept status quo) is the path. Do not proceed to a pool plan.

- [ ] **Step 4: Restore the Pi** to the service-managed binary regardless of outcome (`sudo systemctl start hearth`), and if the gate failed, revert Tasks 1–2 on `hearth` so mainline stays stable (as was done after the first on-Pi finding).

---

## Self-Review

- **Spec coverage (Phase 0):** shared `GstGLDisplay` (Task 1) ✓ · shared wrapped `GstGLContext`/`gst.gl.app_context` (Task 2) ✓ · two concurrent kept-alive `wpevideosrc`, no teardown (Task 3 harness) ✓ · the four success criteria incl. RAM (Task 3 Step 3) ✓ · wrapped-context `fill_info` investigation (Task 2 Step 4) ✓ · decision gate + fallback routing (Task 3 Step 3) ✓ · Pi restored / mainline safe on failure (Task 3 Step 4) ✓. Phase 1 pool + Hearth changes intentionally out of scope.
- **Placeholder scan:** the two investigative points (fill_info path; the coexistence result itself) are inherent spike unknowns, framed as "do X, observe Y, if not then Z" with concrete fallbacks — not vague TODOs. All code steps carry literal code.
- **Type consistency:** `get_shared_gl_display`/`get_shared_gl_context` return `GstGLDisplay *`/`GstGLContext *` and are used identically in the sync handler, `init()`, and the harness; the `"gst.gl.app_context"` type string and `GST_GL_DISPLAY_CONTEXT_TYPE` macro are used consistently across all three sites.
