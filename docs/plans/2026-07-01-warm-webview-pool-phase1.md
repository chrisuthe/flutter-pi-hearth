# Warm Webview Pool (Phase 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep the N (=4) most-recently-used webviews live and warm so switching between them is instant with state preserved, replacing PR #170's one-live-session cap.

**Architecture:** Native — re-apply the Phase-0-validated shared `GstGLDisplay` + wrapped `GstGLContext` (lets N concurrent `wpevideosrc` coexist). Dart — turn `WebviewSessionPool` from a cap-1 LRU into a cap-N LRU that **creates then evicts** (never tears down on switch), and change `WebviewScreen` so offscreen pages stay warm instead of detaching.

**Tech Stack:** Dart/Flutter 3.41 (Hearth app), C/GStreamer/WPE (flutter-pi-hearth embedder), Riverpod.

**Spec:** `flutter-pi-hearth/docs/specs/2026-07-01-warm-webview-pool-phase1-design.md`

## Global Constraints

- **Two repos.** Native lives in `flutter-pi-hearth` (branch `hearth`). Dart lives in the **Hearth** repo at `Harth/Harth-main` (default branch `main`; PRs target `main` — create a feature branch `feat/warm-webview-pool` there, do **not** commit Dart work to `hearth` or to Hearth `main` directly).
- **Warm count `N` = 4** (a named constant, default via constructor).
- **Create-then-evict**: the newly-resolved session is inserted and marked MRU *before* the over-capacity check, so eviction never targets the view being switched to. This is a deliberate reversal of PR #170's dispose-then-create ordering.
- **Warm sessions stay PLAYING** (live), not paused.
- Dart lint rules (Hearth `flutter analyze`): `prefer_const_constructors`, `prefer_const_declarations`, `avoid_print` (use `debugPrint`). Keep changed files analyze-clean.
- Commits authored as the user — no AI/Claude/`Co-Authored-By` self-reference.
- **Dart TDD loop (local):** `/c/flutter/bin/flutter test <path>` and `/c/flutter/bin/flutter analyze` run in `Harth/Harth-main` (verified working — 21 pool tests green, Flutter 3.41.6).
- **Native build gate:** `ssh hearthdev@10.0.1.13` → `~/flutter-pi-verify` (`git fetch/reset origin hearth`, `make -j4`).

---

### Task 1: Native — re-apply shared display + context groundwork

**Repo:** `flutter-pi-hearth` (branch `hearth`). No Dart.

**Files:** `CMakeLists.txt`, `src/plugins/gstreamer_video_player/player.c` (via cherry-pick).

**Interfaces:**
- Produces: the embedder provides a shared `GstGLDisplay` (`gst.gl.GLDisplay`) and wrapped `GstGLContext` (`gst.gl.app_context`) to every pipeline — enabling N concurrent `wpevideosrc`. Consumed at runtime by the Dart warm pool (Tasks 2–3), not at compile time.

- [ ] **Step 1: Cherry-pick the four validated commits (NOT the harness)**

```bash
cd /c/Users/chris/code/flutter-pi-hearth
git cherry-pick 6f348c3 be060af 0b188cb 66f3e45
```

Expected: applies cleanly (mainline unchanged since the revert). This reintroduces the optional `gstreamer-gl-1.0` CMake dep + `HAVE_GSTREAMER_GL`, the shared `GstGLDisplay` singleton + bus sync handler, the doc clarifications, and the wrapped `GstGLContext` (`gst.gl.app_context`). Do **not** cherry-pick `7dab197` (the throwaway spike harness).

- [ ] **Step 2: Push and build-gate on the Pi**

```bash
git push origin HEAD:hearth && git push gitea HEAD:hearth
ssh -o BatchMode=yes hearthdev@10.0.1.13 'cd ~/flutter-pi-verify && git fetch -q origin hearth && git reset -q --hard origin/hearth && cd build && make -j4 2>&1 | tail -3'
```

Expected: `BUILD_OK`-equivalent (compiles clean; this exact code already built on the Pi during the spike).

---

### Task 2: Dart — `WebviewSessionPool` cap-N LRU (create-then-evict)

**Repo:** Hearth (`Harth/Harth-main`), branch `feat/warm-webview-pool`.

**Files:**
- Modify: `lib/modules/webview/webview_session_pool.dart`
- Modify: `test/modules/webview/webview_session_pool_test.dart`

**Interfaces:**
- Consumes: `WebviewSession` (`.shutdown()`, `.isDisposed`, `.initScript`, `.initScriptAllowOrigin`, `.renderWidth`, `.renderHeight`, `.useSizeCaps`), `WebviewSession.testing` factory.
- Produces: `WebviewSessionPool({WebviewSessionFactory? sessionFactory, int capacity = 4})`; `Future<WebviewSession> getOrCreate(...)` (unchanged signature); LRU eviction to `capacity`; `activeUrls` now returns up to `capacity` URLs in LRU→MRU order.

- [ ] **Step 1: Write failing tests for cap-N semantics**

Replace the `// ---- single-live-session cap ----` block (the four tests from `requesting a different URL disposes...` through `serializes concurrent resolves so the cap holds`) in `test/modules/webview/webview_session_pool_test.dart` with this `// ---- warm pool (cap-N LRU) ----` block:

```dart
    // ---- warm pool (cap-N LRU) ----

    test('keeps N distinct sessions warm without tearing any down', () async {
      final pool = WebviewSessionPool(
          sessionFactory: WebviewSession.testing, capacity: 4);
      final a = await pool.getOrCreate('https://a.example');
      final b = await pool.getOrCreate('https://b.example');
      final c = await pool.getOrCreate('https://c.example');
      final d = await pool.getOrCreate('https://d.example');
      expect([a, b, c, d].every((s) => !s.isDisposed), isTrue);
      expect(pool.activeUrls.length, 4);
    });

    test('the (N+1)th resolve evicts exactly the LRU', () async {
      final pool = WebviewSessionPool(
          sessionFactory: WebviewSession.testing, capacity: 2);
      final a = await pool.getOrCreate('https://a.example');
      final b = await pool.getOrCreate('https://b.example');
      final c = await pool.getOrCreate('https://c.example');
      expect(a.isDisposed, isTrue); // A was LRU
      expect(b.isDisposed, isFalse);
      expect(c.isDisposed, isFalse);
      expect(pool.activeUrls.toSet(), {'https://b.example', 'https://c.example'});
    });

    test('re-resolving a warm URL marks it MRU so it survives the next eviction',
        () async {
      final pool = WebviewSessionPool(
          sessionFactory: WebviewSession.testing, capacity: 2);
      final a = await pool.getOrCreate('https://a.example');
      await pool.getOrCreate('https://b.example');
      await pool.getOrCreate('https://a.example'); // touch A -> MRU; B now LRU
      final c = await pool.getOrCreate('https://c.example'); // evicts B
      expect(a.isDisposed, isFalse);
      expect(c.isDisposed, isFalse);
      expect(pool.activeUrls.toSet(), {'https://a.example', 'https://c.example'});
    });

    test('create-then-evict never disposes the just-created session', () async {
      final pool = WebviewSessionPool(
          sessionFactory: WebviewSession.testing, capacity: 1);
      final a = await pool.getOrCreate('https://a.example');
      final b = await pool.getOrCreate('https://b.example');
      expect(b.isDisposed, isFalse); // the new one survives
      expect(a.isDisposed, isTrue); // the old LRU is evicted
      expect(pool.activeUrls.single, 'https://b.example');
    });

    test('never holds more than capacity live sessions', () async {
      final pool = WebviewSessionPool(
          sessionFactory: WebviewSession.testing, capacity: 3);
      for (final u in ['a', 'b', 'c', 'd', 'e']) {
        await pool.getOrCreate('https://$u.example');
      }
      expect(pool.activeUrls.length, 3);
      expect(pool.activeUrls.toSet(),
          {'https://c.example', 'https://d.example', 'https://e.example'});
    });

    test('concurrent resolves serialize and respect capacity', () async {
      final pool = WebviewSessionPool(
          sessionFactory: WebviewSession.testing, capacity: 2);
      await Future.wait([
        pool.getOrCreate('https://a.example'),
        pool.getOrCreate('https://b.example'),
        pool.getOrCreate('https://c.example'),
      ]);
      expect(pool.activeUrls.length, 2);
      expect(pool.activeUrls.toSet(),
          {'https://b.example', 'https://c.example'});
    });
```

Also update the now-inverted teardown-ordering test `awaits the previous session teardown before constructing the replacement` — under create-then-evict the new session is built **before** the LRU tears down. Replace that whole test with:

```dart
    test('builds the replacement before tearing down the evicted LRU', () async {
      final created = <_ProbeSession>[];
      final pool = WebviewSessionPool(
        capacity: 1,
        sessionFactory: ({
          required String url,
          String? initScript,
          String? initScriptAllowOrigin,
          int renderWidth = 1920,
          int renderHeight = 1080,
          bool useSizeCaps = false,
        }) {
          final s = _ProbeSession(
            url: url,
            initScript: initScript,
            initScriptAllowOrigin: initScriptAllowOrigin,
            renderWidth: renderWidth,
            renderHeight: renderHeight,
            useSizeCaps: useSizeCaps,
          );
          created.add(s);
          return s;
        },
      );

      final a = await pool.getOrCreate('https://a.example') as _ProbeSession;
      // Resolve B: with create-then-evict, B is constructed, THEN A is evicted.
      final bFuture = pool.getOrCreate('https://b.example');
      await a.shutdownStarted.future; // A's eviction teardown has begun...
      expect(created.length, 2); // ...and B already exists (created first)
      a.allowShutdown.complete();
      final b = await bFuture;
      expect(a.isDisposed, isTrue);
      expect(identical(a, b), isFalse);
    });
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `/c/flutter/bin/flutter test test/modules/webview/webview_session_pool_test.dart`
Expected: FAIL — `WebviewSessionPool` has no `capacity` parameter yet, and the current cap-1 `_resolve` disposes on every different-URL resolve, so the new expectations don't hold.

- [ ] **Step 3: Implement cap-N create-then-evict in the pool**

In `lib/modules/webview/webview_session_pool.dart`: add a `capacity` field/param, replace `_resolve`'s `await _disposeLive()` step with create-then-evict, add `_touch`/`_evictToCapacity`, and delete the now-unused `_disposeLive`. Concretely:

Change the constructor:

```dart
  final WebviewSessionFactory _factory;
  final int capacity;
  final Map<String, WebviewSession> _sessions = {};

  Future<void> _pending = Future<void>.value();

  WebviewSessionPool({WebviewSessionFactory? sessionFactory, this.capacity = 4})
      : _factory = sessionFactory ?? _defaultFactory;
```

Replace the body of `_resolve` (everything after the `reqW`/`reqH` locals are computed) with:

```dart
    final existing = _sessions[url];
    if (existing != null) {
      final sizeMatches = !useCaps ||
          (existing.useSizeCaps &&
              (existing.renderWidth - reqW).abs() <= 2 &&
              (existing.renderHeight - reqH).abs() <= 2);
      if (existing.initScript == initScript &&
          existing.initScriptAllowOrigin == initScriptAllowOrigin &&
          sizeMatches) {
        _touch(url);
        return existing;
      }
      // Same URL, different injector/size — rebuild this one session.
      _sessions.remove(url);
      await existing.shutdown();
    }

    final session = _factory(
      url: url,
      initScript: initScript,
      initScriptAllowOrigin: initScriptAllowOrigin,
      renderWidth: reqW,
      renderHeight: reqH,
      useSizeCaps: useCaps,
    );
    _sessions[url] = session; // inserted last => most-recently-used
    await _evictToCapacity();
    return session;
```

Replace `_disposeLive` with `_touch` + `_evictToCapacity`:

```dart
  /// Moves [url] to the most-recently-used position (Dart Maps preserve
  /// insertion order, so remove+reinsert = touch).
  void _touch(String url) {
    final s = _sessions.remove(url);
    if (s != null) _sessions[url] = s;
  }

  /// Disposes least-recently-used sessions until at most [capacity] remain,
  /// awaiting each teardown (serialized via [_pending]). Runs after the new
  /// session is inserted+MRU, so it never evicts the just-resolved view.
  Future<void> _evictToCapacity() async {
    while (_sessions.length > capacity) {
      final lruUrl = _sessions.keys.first; // oldest = LRU
      final lru = _sessions.remove(lruUrl);
      await lru?.shutdown();
    }
  }
```

Update the class doc comment (the block describing "capped at one live session") to describe the cap-N warm pool + create-then-evict + LRU. Keep `release`/`releaseAll`/`pauseAll`/`resumeAll`/`reconcile`/`activeUrls` as-is.

- [ ] **Step 4: Run tests + analyze to verify green**

Run: `/c/flutter/bin/flutter test test/modules/webview/webview_session_pool_test.dart && /c/flutter/bin/flutter analyze lib/modules/webview/webview_session_pool.dart`
Expected: all pool tests pass; analyze clean (no unused `_disposeLive`).

- [ ] **Step 5: Commit**

```bash
cd /c/Users/chris/code/flutter-pi-hearth/Harth/Harth-main
git add lib/modules/webview/webview_session_pool.dart test/modules/webview/webview_session_pool_test.dart
git commit -m "feat(webview): warm session pool (cap-N LRU, create-then-evict)"
```

---

### Task 3: Dart — `WebviewScreen` keeps offscreen pages warm

**Repo:** Hearth (`Harth/Harth-main`), branch `feat/warm-webview-pool`.

**Files:**
- Modify: `lib/modules/webview/webview_screen.dart`

**Interfaces:**
- Consumes: the cap-N `WebviewSessionPool` (Task 2) — resolving multiple screens no longer evicts each other while within capacity.

- [ ] **Step 1: Let offscreen pages resolve and stay warm**

In `lib/modules/webview/webview_screen.dart`:

1. In `_ensureSession`, remove the active-gate at the top:

```dart
  Future<void> _ensureSession(Size? renderPx) async {
    final gen = ++_resolveGen;
```

(delete the `if (!widget.isActive) return;` line; keep the rest).

2. In the post-await guard, drop the `!widget.isActive` clause so an offscreen resolve still attaches:

```dart
    if (!mounted || gen != _resolveGen) return;
```

3. In `didUpdateWidget`, stop detaching on scroll-away — keep the session warm. Replace the whole `if/else if` with just the become-active reclaim (harmless when already resolved):

```dart
  @override
  void didUpdateWidget(WebviewScreen oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (widget.isActive && !oldWidget.isActive && _lastRenderPx != null) {
      _ensureSession(_lastRenderPx);
    }
  }
```

4. Delete the now-unused `_detachSession()` method.

5. Update the `_ensureSession` doc comment: it no longer no-ops when inactive; note that offscreen pages resolve and stay warm in the cap-N pool, and that `isActive` now governs only input routing (touch/navigation events go to the visible page).

- [ ] **Step 2: Verify it analyzes clean and existing webview tests pass**

Run: `/c/flutter/bin/flutter analyze lib/modules/webview/webview_screen.dart && /c/flutter/bin/flutter test test/modules/webview/`
Expected: analyze clean (no unused `_detachSession`, no unused field); all existing webview tests pass. (The screen's warm-vs-detach behavior is integration-level — its real verification is the on-Pi run in Task 4; there is no widget test that pins detach-on-inactive today, so none needs updating.)

- [ ] **Step 3: Confirm offscreen sessions receive no input**

Read `_TouchableWebviewView` / the touch-mapping path in this file and confirm pointer/navigation events are only dispatched for the visible page (a `PageView`'s offscreen children don't receive pointer events; if any navigation dispatch is not already guarded by `isActive`, gate it on `widget.isActive`). Run: `/c/flutter/bin/flutter test test/modules/webview/webview_touch_mapping_test.dart` — expected: pass.

- [ ] **Step 4: Commit**

```bash
git add lib/modules/webview/webview_screen.dart
git commit -m "feat(webview): keep offscreen webview pages warm (no detach on scroll-away)"
```

---

### Task 4: On-Pi integration validation (acceptance gate)

Requires the native binary (Task 1) built/installed on the Pi **and** the Hearth bundle built from the `feat/warm-webview-pool` branch deployed to the Pi (via the DEV bundle-deploy loop — push the branch; build+deploy the bundle the same way Hearth DEV builds are normally flashed).

- [ ] **Step 1: Deploy** the Task 1 native binary (`sudo make install` in `~/flutter-pi-verify/build`) and the warm-pool Hearth bundle; restart `hearth.service`.

- [ ] **Step 2: Warm switching + state preservation.** Configure 5 webviews. Visit 4, scroll/drill into some. Switch among them — verify each of the 4 MRU is **instant** with **state preserved** (scroll position / in-page location retained), no loading placeholder. In logs (`journalctl -u hearth -f`): multiple `[I/Webview] state -> PLAYING` for different URLs coexisting; **no** `status=11/SEGV`, **no** `Multiple EGL displays`.

- [ ] **Step 3: Eviction.** Visit the 5th webview → confirm the LRU (1st) is evicted (its WPE web process exits); returning to it reloads (cold). Watch for whether the eviction teardown crashes (the residual risk); note frequency.

- [ ] **Step 4: RAM + soak.** `ps -o rss -C WPEWebProcess` stays ~N×400 MB; no unbounded growth over a few minutes of switching. HA dashboards stay authenticated (token injection). Idle → all pause; wake → resume.

- [ ] **Step 5: Record** the result (state-preservation confirmed, eviction crash frequency, RAM). If eviction crashes on essentially every eviction, escalate to the Approach B (recycle-by-navigation) fallback per the spec; if it's clean or rare, Phase 1 is done.

---

## Self-Review

- **Spec coverage:** native re-apply (Task 1) ✓ · pool cap-1→cap-N LRU + create-then-evict (Task 2) ✓ · `WebviewScreen` keep-warm / no-detach + `isActive` repurposed to input-only (Task 3) ✓ · warm=PLAYING (design; Task 2/3 make no pause change) ✓ · injector/size-change rebuild preserved (Task 2 `_resolve`) ✓ · `reconcile`/`release`/`pauseAll`/`resumeAll` unchanged (Task 2) ✓ · Dart unit tests incl. LRU/eviction/create-then-evict/serialization (Task 2 Step 1) ✓ · on-Pi state-preservation + eviction + RAM + injection + idle (Task 4) ✓ · residual eviction risk + Approach-B escalation (Task 4 Step 5) ✓.
- **Placeholder scan:** all steps carry literal code/commands; the on-Pi steps are observational acceptance criteria, not code placeholders.
- **Type consistency:** `capacity` (int, default 4) is defined in Task 2's constructor and used in `_evictToCapacity` and every Task 2 test; `_touch`/`_evictToCapacity` defined and called in `_resolve`; `activeUrls` semantics (LRU→MRU order, ≤capacity) consistent across tests; `WebviewSession.testing` factory shape matches the existing `_ProbeSession` super-constructor and `_defaultFactory`.
