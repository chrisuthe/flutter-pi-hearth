# Native EGL-display sharing across WPE instances

- **Date:** 2026-07-01
- **Repo:** flutter-pi-hearth (embedder / native side)
- **Status:** Implemented and REVERTED — on-Pi validation showed sharing the `GstGLDisplay` alone is insufficient (it eliminates the "Multiple EGL displays" error but exposes an upstream WPEBackend-fdo teardown use-after-free in `wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image`). Superseded by `2026-07-01-wpe-warm-webview-tabs-design.md`, which reframes the fix around keeping views alive (no per-switch teardown) plus shared display **and** context.
- **Related:** Hearth PR #170 (Dart-side workaround); `2026-07-01-wpe-warm-webview-tabs-design.md` (successor)

## Problem

On the Pi, running more than one WebView at a time crashes the kiosk in a
restart loop (`hearth.service: Main process ... status=11/SEGV`). Root cause,
confirmed on-device: **flutter-pi initializes exactly one EGL display**, and
`wpevideosrc` / WPE WebKit enforce a **single EGL display per process**. When a
second live `wpevideosrc` pipeline initializes, GStreamer's GL stack creates its
*own* second `GstGLDisplay`, WPE logs `Multiple EGL displays are not supported.`
and the process SIGSEGVs.

Hearth PR #170 works around this in the Dart layer by capping the WebView
session pool to **one live WPE session** (an LRU of capacity 1) and serializing
teardown so two WPE pipelines never hold an EGL display at the same instant. The
cost: switching between two WebView pages fully re-initializes WPE (a few seconds
of loading placeholder), losing warm-cache instant switching.

The fix belongs in the embedder: share **one** `GstGLDisplay` across every
`wpevideosrc` pipeline so all instances reference the single process EGL display.

## Goals

1. **Lift the cap.** N warm WPE WebViews coexist without SIGSEGV; instant
   (warm) switching restored. This lets Hearth revert PR #170's 1-session cap.
2. **Zero-copy frames.** Remove the per-frame GPU→CPU→GPU round-trip that the
   current `... ! videoconvert ! appsink` path incurs, by delivering `GLMemory`
   straight to the appsink and handing GstGL's texture to Flutter directly.

## Non-goals

- Reworking the non-WPE (RTSP/file) video paths. They keep the existing dmabuf
  import path unchanged.
- Multi-process WPE isolation. This design keeps the single-process model and
  makes it correct, not the WebKit multi-process split.

## Background: current architecture

- The embedder creates **one** EGL display from GBM in `gl_renderer.c`
  (`gl_renderer_get_egl_display()`).
- `gstreamer_video_player` builds pipelines from Dart-supplied
  `gst_parse_launch` strings. The WebView pipeline (Hearth) is:
  ```
  wpevideosrc name=websrc location=$url draw-background=false \
    [ ! video/x-raw(memory:GLMemory),width=W,height=H ] ! videoconvert ! appsink name=sink
  ```
  on **GStreamer 1.26** (element must be `wpevideosrc`, not `wpesrc` — only
  `wpevideosrc` owns the `configure-web-view` signal used for HA token injection).
- The appsink handlers (`on_appsink_new_sample` / `_preroll`) call
  `frame_new` (`frame.c`), which imports the buffer as a
  `EGL_LINUX_DMA_BUF_EXT` EGLImage → GL texture (`frame.c:1026`) — zero **CPU**
  copy *if* the buffer arrives as dmabuf.
- The pipeline's bus is drained asynchronously via an `sd_event` bus-fd source
  (`on_bus_fd_ready` → `on_bus_message`). **Nothing answers
  `GST_MESSAGE_NEED_CONTEXT`.** Each `wpevideosrc` therefore self-provisions its
  own `GstGLDisplay` — the root cause.
- With the optional `memory:GLMemory` caps pinned before `videoconvert` (a CPU
  element), GStreamer must insert a `gldownload` (GPU→CPU); `frame.c` then wraps
  system memory back into a gbm bo (CPU→GPU) — a full round-trip per frame. That
  round-trip is what zero-copy removes.

## Approach

Staged, because the two GStreamer GL context types map cleanly onto the two
goals and let us isolate risk:

- **`gst.gl.GLDisplay`** (the shared *display*) → Phase A. Answering only this
  lets GstGL create its own context on our EGL display, which alone satisfies
  WPE's single-display-per-process rule. Lifts the cap. No cross-thread context
  activation.
- **`gst.gl.app_context`** (the wrapped *context*) → Phase B. Answering this
  makes GstGL's internal context share GL objects with flutter-pi's context, so
  textures are cross-usable → zero-copy. Needs fence + lifetime care.

Phase A is independently shippable and revertible; Phase B builds on the proven
foundation and degrades gracefully back to Phase A if its trickier parts don't
land cleanly.

---

## Phase A — shared `GstGLDisplay` (lifts the cap)

### Component: process-global GL-display singleton

A lazily-initialized (`g_once`) `static GstGLDisplay *` living **inside the
`gstreamer_video_player` plugin**, built from the embedder's existing EGL
display:

```c
gst_gl_display_egl_new_with_egl_display(gl_renderer_get_egl_display(renderer))
```

The plugin already holds the `gl_renderer` (passed to `frame_interface_new`), so
no GStreamer types leak into the GStreamer-agnostic `gl_renderer` core. The
singleton is never torn down; its lifetime matches the process EGL display.

### Wiring: per-pipeline bus **sync** handler

Context negotiation is **synchronous** — GstGL posts `NEED_CONTEXT` inside a
state change and, if unanswered *in that same call*, immediately falls through to
`gst_gl_display_new()` and creates its own display. The existing async
`on_bus_message` observes the message too late to prevent that. So:

- Add `gst_bus_set_sync_handler()` per pipeline, alongside the existing async
  `sd_event` watch.
- The sync handler matches `GST_MESSAGE_NEED_CONTEXT` with type
  `"gst.gl.GLDisplay"`, builds a `GstContext`, calls
  `gst_context_set_gl_display(ctx, shared_display)` and
  `gst_element_set_context(GST_MESSAGE_SRC(msg), ctx)`, then `GST_BUS_DROP`s that
  one message (fully consumed). **Everything else returns `GST_BUS_PASS`** so
  `on_bus_message` keeps handling errors / EOS / state / buffering exactly as
  today.
- The handler runs on the **streaming thread**: it must touch **no** player
  state — only build and set the context. No lock needed.

Belt-and-suspenders: also call `gst_element_set_context(pipeline, display_ctx)`
immediately after `gst_parse_launch`, so the display is already present when
`wpevideosrc` first looks (this proactive set is the most deterministic path; the
sync handler is the fallback for lazy probes).

### Frame path

**Unchanged.** The dmabuf → EGLImage import in `frame.c` is untouched in Phase A.

### Build

Adds a `gstreamer-gl-1.0` dependency (`#include <gst/gl/gl.h>`,
`<gst/gl/egl/gstgldisplay_egl.h>`), guarded like the existing optional
`HAVE_WPE_WEBKIT` so non-WPE builds are unaffected.

### Error handling

- Shared-display creation failure → log and fail pipeline `init` (a `wpevideosrc`
  pipeline cannot run without it).
- Unknown `NEED_CONTEXT` types → pass through.

### What lands

- **(A-native)** shared display singleton + sync handler (this repo).
- **(A-dart)** revert PR #170's 1-session cap (Hearth repo), gated on A-native
  landing.

---

## Phase B — zero-copy `GLMemory` handoff

Coordinated **two-repo** change; both halves land together.

- **Native (this repo):** answer `gst.gl.app_context` + add a `GLMemory` branch
  to the frame path + sync/lifetime handling.
- **Dart (Hearth):** change the pipeline to deliver `GLMemory` to the appsink,
  e.g. `wpevideosrc ! glcolorconvert ! video/x-raw(memory:GLMemory),format=RGBA
  ! appsink`, dropping `videoconvert`.

### 1. Wrapped app context (answer `gst.gl.app_context`)

Built once, same singleton home as the display:

```c
gst_gl_context_new_wrapped(display, (guintptr) ctx, GST_GL_PLATFORM_EGL, GST_GL_API_GLES2)
```

wrapping a **dedicated** context from `gl_renderer_create_context()` (shares
flutter-pi's sharegroup without touching the raster or `frame_interface`
contexts). The sync handler answers `"gst.gl.app_context"` with this, parallel to
Phase A's display answer.

> **Risk #1 — wrapped-context `fill_info`.** A wrapped context's GL API/version
> is unpopulated until it is activated once (`gst_gl_context_activate` +
> `gst_gl_context_fill_info`), which requires making our EGL context current on a
> thread we control. **Spike first:** determine whether GStreamer 1.26 fills this
> lazily during `gst_gl_context_create`; if not, perform a one-time activate on a
> controlled setup thread. If it can't be made clean, fall back (see §5).

### 2. Frame path — `frame_new` branches on memory type

- `gst_is_gl_memory(mem)` → pull `tex_id` via `gst_gl_memory_get_texture_id`,
  emit a `texture_frame` with `target = GL_TEXTURE_2D`, `name = tex_id`, RGBA —
  **no EGLImage**. Ref the `GstSample`/`GstBuffer`; unref it in
  `on_destroy_texture_frame`. Because the raster context shares GstGL's
  sharegroup, `tex_id` is directly valid there — the zero-copy.
- dmabuf branch → **unchanged**, retained as the fallback path.

### 3. Cross-thread sync

GstGL writes on the streaming thread; Flutter samples on the raster thread. Use
**`GstGLSyncMeta`**: on consume, `gst_gl_sync_meta_wait(sync_meta,
wrapped_context)` inserts a server-side wait so sampling cannot outrun WPE's
render. (This is the consume-time use of the wrapped context.) A buffer missing
the sync meta → request it via the allocation query, or fall back to dmabuf.

### 4. Buffer lifetime / pool starvation — Risk #2

GstGL's pool is bounded; Flutter holds frames across its own multi-buffering.
Holding too many too long starves the pool → pipeline stalls. Mitigation: release
the `GstBuffer` promptly in the destroy callback; bump pool `min-buffers` via the
allocation query if needed; soak-test.

### 5. Graceful degradation

If GLMemory negotiation, wrapped-context setup, or sync can't be made solid, the
pipeline keeps the Phase A dmabuf path (Dart keeps `videoconvert`). Phase B then
degrades exactly to "cap lifted, no zero-copy" — deliberately held in reserve.

---

## Test plan

**Local, every phase:** cross-compile builds clean (compile-gate only — WPE
can't be exercised off-device).

**On the dev Pi (SSH; Hearth install script compiles on-device):**

*Phase A:*
1. With PR #170's cap reverted, open 2–3 WebViews (HA dashboard + custom URL).
2. **No** `Multiple EGL displays are not supported` in logs; **no** `status=11/SEGV`; all pages render.
3. Switching between pages is instant/warm — no multi-second WPE re-init.
4. HA token injection still authenticates both dashboards.

*Phase B:*
5. Frames render correctly — no tearing/garbage; RGBA vs BGRA sanity.
6. Perf: CPU% and frame timing, Phase A (round-trip) vs Phase B (zero-copy), with 2+ live WebViews.
7. Soak: N WebViews running for an extended period — watch for pool-starvation stalls and memory growth (buffer leak).

## Sequencing / rollback

Three independently revertible commits:

- **(A-native)** shared display + sync handler
- **(A-dart)** cap revert
- **(B)** zero-copy (native + Dart caps change, together)

If Phase B proves costly, ship Phase A and stop — the crash is fixed and warm
switching restored; zero-copy becomes a follow-up.

## Open questions / spike items

- **B-Risk #1:** does GStreamer 1.26 fill the wrapped context's info lazily, or
  must we activate it on a controlled thread? Resolve before building §B.1.
- **B-Risk #2:** does the default GstGL pool depth survive Flutter's
  multi-buffering with N live WebViews, or must we raise `min-buffers`?
- Confirm `wpevideosrc` on 1.26 requests `gst.gl.app_context` (not only
  `gst.gl.GLDisplay`) when a shared display is already provided — determines
  whether Phase B's sync handler branch is exercised as designed.
