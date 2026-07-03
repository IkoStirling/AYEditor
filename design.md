# AYEditor Design

**Version:** v0.3  
**Date:** 2026-07-03  
**Status:** E3 — `EditorApp` + AYDevice host window; GDI chrome + child viewport HWND (see [§2.3](#23-viewport-presentation-interim-vs-target))

> The editor is a **cross-module system**, not a single UI library.  
> Chrome is drawn by [AYUI](../AYUI/design.md); simulation control follows [AYExtension §3](../AYExtension/design.md) and [AYApplication §3](../AYApplication/design.md).

---

## 1. Overview

AYEditor is the **minimal editor product layer** on top of the runtime:

| Responsibility | Owner module |
|----------------|--------------|
| Toolbar, panels, layout JSON | **AYUI** (editor chrome) |
| Edit / Play / Paused / Simulate | **AYEditor** (`EditorSession`, `EditorGameView`) |
| Application entry, subsystem filter | **AYApplication** (`BuildType::Editor`) |
| OS window, input poll, native handles | **AYDevice** ([WindowManager](../AYDevice/design.md)) — **E3 prerequisite** |
| World update, rendering | **AYGameLoop**, **AYEntity**, **AYRenderer** |
| Scene/asset persistence, property grid | **Deferred** — AYSerializer + reflection metadata (E4+) |

### 1.1 Goals (v0)

- See a **credible editor shell** (menu bar, toolbar, dock placeholders) as early as possible.
- **Clearly separate Edit mode from Play mode** without duplicating engine code.
- Keep each phase **small and testable** (standalone demo exe before full `EditorApp`).

### 1.2 Non-goals (v0)

- Full SceneView gizmos, asset browser, undo/redo stack.
- Inspector driven by reflection (planned E4+).
- Level/scene save-load via serializer (planned E4+).
- Docking drag-resize, multi-window, plugin SDK.
- Replacing AYExtension Replay/Timeline (orthogonal; share `tickOnce` only).

---

## 2. Architecture

```
┌────────────────────────────────────────────────────────────────────┐
│  EditorApp (AYApplication, BuildType::Editor)                     │
├────────────────────────────────────────────────────────────────────┤
│  EditorSession                                                      │
│    ├─ EditorGameView (Mode: Edit | Play | Paused | Simulate)       │
│    ├─ UIManager + editor_shell.ui.json  ← AYUI chrome              │
│    └─ ViewportRect → RendererSubSystem  ← 3D game view             │
├────────────────────────────────────────────────────────────────────┤
│  GameLoop                                                           │
│    Edit:     pause/stop — no World::update (or static preview only) │
│    Play:     run() — same path as EngineIntegration_Demo           │
│    Paused:   pause() — frozen sim, UI still live                    │
│    Simulate: run logic, optional skip Present (later)               │
└────────────────────────────────────────────────────────────────────┘
```

### 2.1 Mode matrix

Aligned with [AYExtension/design.md §3.2](../AYExtension/design.md). v0 implements **Edit**, **Play**, **Paused** first; **Simulate** is optional.

| Mode | GameLoop | Entity / World | Renderer (viewport) | AYUI chrome |
|------|----------|----------------|---------------------|-------------|
| **Edit** | `stop()` or idle tick | No `update()` | Clear / static scene optional | Always `update` + `render` |
| **Play** | `run()` or per-frame tick | Normal `update()` | Full 3D + UI overlay | Always `render` |
| **Paused** | `pause()` | No `update()` | Last frame frozen | Always `render` |
| **Simulate** | `run()` without swap | Normal `update()` | Skip Present (deferred) | Optional |

**Time domains:** Editor chrome uses **Unscaled** delta (see [AYGameLoop/design.md](../AYGameLoop/design.md)). Game simulation may use Scaled time inside GameLoop; UI must not slow down when `timeScale != 1`.

### 2.2 Frame order (target, U2+)

```
1. Poll OS input
2. EditorSession::update(dt)     // mode machine, toolbar handlers
3. If Play|Simulate: GameLoop tick (Entity, …)
4. RendererSubSystem::renderFrame
     a. 3D into viewport sub-rect
     b. UIManager::render full window (or chrome-only regions)
5. Present
```

v0 demo (E0) may use **UI-only window** with a gray `Image`/rect as viewport placeholder.

### 2.3 Viewport presentation: interim vs target

Editor logic (modes, GameLoop, viewport **geometry**) is independent of how pixels are composited.  
Two presentation stacks exist in the roadmap; **only the demo/compositor layer differs**.

| Layer | Interim (current, E2-interim) | Target (E2-composite + E3) |
|-------|-------------------------------|------------------------------|
| **Editor chrome** | GDI on **host** HWND via `GdiRenderBackend` + `EditorShellDemo` | `AYUIRenderBackend` (AYUI U2) composited in `RendererSubSystem::renderFrame` |
| **3D viewport** | bgfx on **child** HWND (`EditorPlayRuntime::_viewportWindow`) | bgfx on **main** window with `RendererSubSystem` viewport sub-rect (`setViewRect`) |
| **Native window owner** | Raw Win32 in demo + `EditorPlayRuntime` | **AYDevice** `WindowManager` (SDL2) → `getWindowHandle()` |
| **Why interim** | GDI `BitBlt` onto a D3D/bgfx swap-chain **host** surface is unreliable on Windows; child HWND keeps GDI and GPU separate | Single present path, no Z-order / region hacks |

```
Interim (EditorShell_Demo today):

  ┌─ Host HWND (GDI: full editor_shell.ui.json) ─────────────────┐
  │ Toolbar │ Hierarchy │  ┌─ Child HWND (bgfx) ─┐ │ Inspector   │
  │         │             │  rotating cube       │ │             │
  └─────────┴─────────────┴──────────────────────┴─┴─────────────┘
         ▲                        ▲
    EditorSession::render   EditorPlayRuntime::tick → GameLoop

Target (E2-composite + AYDevice):

  ┌─ AYDevice main window (single client area) ────────────────────┐
  │  RendererSubSystem: 3D in viewport rect                        │
  │  AYUIRenderBackend: UI texture / chrome over full client       │
  └────────────────────────────────────────────────────────────────┘
         ▲
    AYDevice::pollEvents → EditorSession → GameLoop
```

**Encapsulation rule:** Demo Win32 details stay in `demo/EditorShellDemo.cpp`.  
`EditorSession` / `EditorGameView` / `EditorPlayRuntime` must not spread `CreateWindowEx` outside the runtime viewport host.  
Migrating to target = change **bootstrap window handle source** and **UI backend**, not the mode machine.

**What is already “E2-ready” in engine code (reuse as-is):**

- `RendererSubSystem::setBootstrapViewport` / `setViewportRect` / `resize`
- `GameLoop::preparePlaySession`, `tickOnce`, `stepOnce`, `getElapsedTime()`
- Persistent shader cache under `ayeditor_cache/`
- `EditorSession::isChromePoint` (chrome vs viewport hit routing)
- Play/Stop keeps renderer alive (`enterEdit()` hides viewport; `shutdownEngine()` on session exit)

### 2.4 AYDevice: when to introduce

[AYDevice/design.md](../AYDevice/design.md) owns **SDL2 window creation**, `getWindowHandle()` for bgfx, resize/focus callbacks, and input poll.

| Question | Answer |
|----------|--------|
| Block current E2-interim demo? | **No.** Raw Win32 in the demo is acceptable until E3. |
| Build AYDevice skeleton before E3? | **Yes — minimal WindowManager only** (see below). |
| Build full input mapping / OpenXR now? | **No.** Defer to after editor shell is on `EditorApp`. |

**Recommended AYDevice Phase-1 skeleton (before `EditorApp`, can parallel U2):**

- `WindowManager`: `createWindow`, `destroyWindow`, `getWindowHandle`, `getSize`, resize/title callbacks
- `DeviceManager`: `initialize` / `shutdown`, `pollEvents` (SDL queue → window events only)
- **No** Action/Axis mapping, Gamepad, XR in the first skeleton

**Migration from interim demo:**

1. Replace `EditorShellDemo` Win32 loop with `DeviceManager::pollEvents` + frame callback.
2. Replace `EditorPlayRuntime` child `CreateWindowEx` with either:
   - **Option A (short term):** `WindowManager::createChildSurface(viewportRect)` returning native handle for bgfx; or
   - **Option B (E2-composite):** drop child HWND; bootstrap main handle + viewport sub-rect only.
3. `EditorSessionDesc::hostWindow` becomes `IWindow*` / `WindowHandle` from AYDevice, not raw `HWND`.

Do **not** duplicate SDL window creation inside AYRenderer or AYEditor — single owner remains AYDevice per device design §3.1.

## 3. Phase roadmap

| Phase | Scope | Exit criteria | Blocks |
|-------|--------|---------------|--------|
| **E0** | `EditorShell_Demo` + `editor_shell.ui.json` | Window shows toolbar + panel placeholders; buttons fire callbacks | AYUI **U0–U1** |
| **E1** | `EditorSession` + `EditorGameView` mode switch | Play runs rotating-cube demo; Stop returns to Edit; toolbar reflects mode | E0 + GameLoop `tickOnce` / play session |
| **E2-interim** | Viewport host + engine play session (**current**) | Child HWND bgfx + GDI host chrome; shader cache; chrome hit-test in Play | E1 (no AYDevice required) |
| **E2-composite** | Single-window UI + 3D composite | 3D in viewport sub-rect on **one** swap chain; AYUI draws chrome via renderer | AYUI **U2** (`AYUIRenderBackend`) |
| **E3** | `EditorApp` + `BuildType::Editor` | Same behaviour as E2 via `IApplication`; no raw Win32 in demo | **AYDevice WindowManager skeleton** + AYApplication |
| **E4+** | Hierarchy, Inspector, scene I/O | See [§8](#8-deferred-metadata--serialization) | Reflection + AYSerializer |

**Prerequisite chain:** E0–E2-interim do **not** require AYDevice, serializer, or Inspector.  
**E3** is the first phase that should **require** AYDevice window ownership (not necessarily full input mapping).

---

## 4. Core types (spec)

Implementation lives under `AYEditor/` (not inside AYUI).

### 4.1 `EditorGameView`

Mode owner; does **not** own widgets. Toolbar buttons call into this type.

```cpp
namespace ayt::editor {

enum class EditorMode : uint8_t {
    Edit,
    Play,
    Paused,
    Simulate,
};

class EditorGameView {
public:
    explicit EditorGameView(IAYGameLoop& loop);

    EditorMode mode() const;
    void setMode(EditorMode mode);

    void stepOnce();   // calls loop.tickOnce() when Paused — requires GameLoop impl

private:
    IAYGameLoop& _loop;
    EditorMode _mode = EditorMode::Edit;
};

} // namespace ayt::editor
```

**Note:** `IAYGameLoop::tickOnce()` / `preparePlaySession()` / `stepOnce()` are implemented for editor play sessions (see `AYGameLoopImpl`).

### 4.2 `EditorSession`

Top-level editor controller for demos and later `EditorApp`.

```cpp
class EditorSession {
public:
    void initialize(const EditorDesc& desc);
    void shutdown();

    void update(float unscaledDt);
    void render();

    EditorGameView& gameView();
    ayt::ui::UIManager& ui();

    // Viewport in client pixels (E2+)
    void setViewportRect(const Rect& r);
    Rect viewportRect() const;

private:
    EditorGameView _gameView;
    // UIManager, viewport rect, bound toolbar handlers
};
```

### 4.3 Toolbar → mode (AYUI binding)

Chrome is JSON; logic is C++ `bindEvent` (same pattern as [AYUI §4.2](../AYUI/design.md)):

```cpp
loader.bindEvent("btn_play",  "onClick", [&]{ session.gameView().setMode(EditorMode::Play); });
loader.bindEvent("btn_pause", "onClick", [&]{ session.gameView().setMode(EditorMode::Paused); });
loader.bindEvent("btn_stop",  "onClick", [&]{ session.gameView().setMode(EditorMode::Edit); });
loader.bindEvent("btn_step",  "onClick", [&]{ session.gameView().stepOnce(); });
```

Do **not** encode mode transitions inside JSON.

---

## 5. Editor chrome (AYUI)

### 5.1 Contract

- **File:** `assets/ui/editor_shell.ui.json` (or path via `EditorDesc`).
- **Loader:** existing `UILayoutLoader` + `WidgetFactory` — no second layout format.
- **Styles:** optional `editor_shell.ui.styles.json` when AYUI U1 StyleSheet lands.
- **i18n keys:** `ui.editor.*` (e.g. `ui.editor.play`, `ui.editor.stop`).

AYUI scope is documented in [AYUI/design.md §13](../AYUI/design.md#13-editor-chrome). AYEditor owns **which** layout to load and **what** handlers do.

### 5.2 Minimal shell layout (E0)

Logical regions only — no real data yet:

```
┌──────────────────────────────────────────────────────────────┐
│ MenuBar (HBox) — File / Edit / View — placeholders           │
├──────────────────────────────────────────────────────────────┤
│ Toolbar: [Play] [Pause] [Step] [Stop]   |  mode label        │
├──────────┬───────────────────────────────────────┬───────────┤
│ Hierarchy│         Viewport (placeholder)        │ Inspector │
│ (static) │         gray panel / future 3D        │ (static)  │
│          │                                       │           │
└──────────┴───────────────────────────────────────┴───────────┘
```

Example JSON skeleton (abbreviated):

```json
{
  "type": "Window",
  "id": "editor_root",
  "size": { "w": 1280, "h": 720 },
  "children": [
    {
      "type": "VBox",
      "id": "editor_column",
      "children": [
        {
          "type": "HBox",
          "id": "toolbar",
          "children": [
            { "type": "Button", "id": "btn_play",  "text": "ui.editor.play",  "onClick": "play" },
            { "type": "Button", "id": "btn_pause", "text": "ui.editor.pause", "onClick": "pause" },
            { "type": "Button", "id": "btn_step",  "text": "ui.editor.step",  "onClick": "step" },
            { "type": "Button", "id": "btn_stop",  "text": "ui.editor.stop",  "onClick": "stop" },
            { "type": "TextLabel", "id": "lbl_mode", "text": "EDIT" }
          ]
        },
        {
          "type": "HBox",
          "id": "main_row",
          "children": [
            { "type": "Window", "id": "panel_hierarchy", "text": "Hierarchy", "size": { "w": 220, "h": 600 } },
            { "type": "Window", "id": "panel_viewport",  "text": "Viewport",  "size": { "w": 740, "h": 600 } },
            { "type": "Window", "id": "panel_inspector", "text": "Inspector", "size": { "w": 280, "h": 600 } }
          ]
        }
      ]
    }
  ]
}
```

E0: Hierarchy/Inspector show static labels. E4+: bind to entity selection and reflection.

### 5.3 Mode indicator

`EditorSession` updates `lbl_mode` text (`EDIT` / `PLAY` / `PAUSED`) from code when `setMode` runs — not from simulation state.

---

## 6. Application & subsystems

From [AYApplication/design.md](../AYApplication/design.md):

- **`BuildType::Editor`** — compile-time `AY_BUILD_TARGET_EDITOR`.
- Register game subsystems (Entity, Renderer, …) **plus** editor-only:
  - `EditorToolsSubSystem` — Unscaled tick: `EditorSession::update`, UI hot-reload.
  - `SceneEditorSubSystem` — deferred (scene editing logic).

E3 wires `EditorApp : IApplication` to construct `EditorSession` after GameLoop init.

**Do not** put `EditorGameView` inside AYUI widgets.

---

## 7. Demos & build targets

| Target | Phase | Purpose |
|--------|-------|---------|
| `AYEditorShell_Demo` | E0–E2-interim | Win32 + GDI host + optional child viewport; proves chrome JSON and Play modes |
| `AYEditor` (app) | E3 | Production entry via AYDevice + AYApplication; replaces demo Win32 loop |

CMake: add `AYRuntime/AYEditor/CMakeLists.txt` when E0 starts; link `AYUI`, `AYGameLoop`, `AYEntity`, `AYRenderer` as needed per phase.

---

## 8. Deferred: metadata & serialization

The editor **will** use reflection metadata and [AYSerializer](../../AYFoundation/AYSerializer/README.md) for:

| Feature | Mechanism | Phase |
|---------|-----------|-------|
| Inspector property grid | `AYReflection` / component `AY_PROPERTY` metadata | E4+ |
| Hierarchy entity list | `World` query API + optional display names in metadata | E4+ |
| Save / load level | `SerializerFor<T>` + Entity component registration | E4+ |
| Undo/redo | Command pattern over serialized snapshots or property deltas | E5+ |

**E0–E3 must not block on serializer work.** Static placeholder panels are intentional.

Existing building blocks (already used by Entity):

- Components: `AY_PROPERTY` + `AY_FINALIZE_REGISTRATION_METADATA(T)` ([AYEntity/design.md](../AYEntity/design.md), [AYSerializer README](../../AYFoundation/AYSerializer/README.md)).
- UI layout export: `WidgetSerializer` round-trip ([AYUI §4.1](../AYUI/design.md)) — separate from **game scene** serialization.

When E4 starts, define:

- `SceneDocument` JSON schema (entities + component blobs).
- Inspector adapter: `IPropertyEditor` reading `ITypeInfo` — lives in **AYEditor**, not AYUI.

---

## 9. Directory structure (planned)

```
AYEditor/
├── design.md              ← this file
├── README.md
├── CMakeLists.txt         ← E0
├── interface/
│   └── IAYEditorSession.h
├── include/
│   ├── AYEditorSession.h
│   ├── AYEditorGameView.h
│   └── AYEditorPlayRuntime.h   ← viewport host (child HWND interim; AYDevice later)
├── src/
│   ├── AYEditorSession.cpp
│   ├── AYEditorGameView.cpp
│   └── AYEditorPlayRuntime.cpp
├── assets/
│   └── ui/
│       ├── editor_shell.ui.json
│       └── editor_shell.ui.styles.json   ← optional U1
└── demo/
    └── EditorShellDemo.cpp    ← E0–E2-interim Win32 message loop (replaced at E3)
```

Long term, [AYExtension/Editor](../AYExtension/design.md) may thin-wrap or re-export AYEditor APIs for replay/timeline; **AYEditor/design.md is authoritative** for shell and modes.

---

## 10. Testing

- **E0:** AYTest or demo smoke — load `editor_shell.ui.json`, `findById("btn_play")` non-null.
- **E1:** Mode transition unit tests on `EditorGameView` with mock `IAYGameLoop`.
- **E2-interim:** Manual — child viewport animates in Play; GDI chrome intact; Pause/Stop/Step work.
- **E2-composite:** Manual — single-window composite; no child HWND.
- No compile in AI agent loop (project rule).

---

## 11. Decisions log

| Date | Decision |
|------|----------|
| 2026-07-03 | AYEditor is a separate module; not embedded in AYUI |
| 2026-07-03 | Chrome via existing `UILayoutLoader` JSON; ImGui samples in Extension doc are non-normative |
| 2026-07-03 | E0–E3 without Inspector/scene I/O; metadata + serializer deferred to E4+ |
| 2026-07-03 | Fastest visual path: `EditorShell_Demo` before `EditorApp` |
| 2026-07-03 | **E2 split:** E2-interim = child HWND + GDI host (Windows-safe); E2-composite = U2 single-window target |
| 2026-07-03 | Viewport native ownership migrates to **AYDevice** at E3; interim Win32 encapsulated in demo + `EditorPlayRuntime` |
| 2026-07-03 | AYDevice **WindowManager skeleton** recommended before E3; full input/XR not required for editor shell |

---

## 12. References

- [AYUI/design.md §13 Editor chrome](../AYUI/design.md#13-editor-chrome)
- [AYExtension/design.md §3 Editor Integration](../AYExtension/design.md)
- [AYApplication/design.md §3 BuildType / subsystems](../AYApplication/design.md)
- [AYGameLoop/design.md](../AYGameLoop/design.md) — `tickOnce`, Unscaled editor time
- [AYDevice/design.md §3 WindowManager](../AYDevice/design.md#3-窗口管理-windowmanager) — native handle owner for E3+
- [AYSerializer/README.md](../../AYFoundation/AYSerializer/README.md) — future scene/Inspector I/O
