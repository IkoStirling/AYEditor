# AYEditor Design

**Version:** v0.3.1
**Date:** 2026-08-19
**Status:** E2-composite + §4.2.x Editor 持 Edit Scene + §4.3.x Transport bar UX（v0.3 PR-4；Q-G 收口延续）

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
│  EditorApp / EditorShellDemo (host frame loop)                    │
│    poll input → EditorSession::update(real dt) → composite present │
├────────────────────────────────────────────────────────────────────┤
│  EditorSession                                                      │
│    ├─ EditorGameView (Mode: Edit | Play | Paused | Simulate)       │
│    ├─ UIManager + editor_shell.ui.json  ← AYUI chrome              │
│    └─ ViewportRect → RendererSubSystem  ← 3D game view             │
├────────────────────────────────────────────────────────────────────┤
│  GameLoop                                                           │
│    PresentationOwnership::ExternalHost                              │
│    Edit:     prepared + paused — no World::update                  │
│    Play:     tickOnce() once per host frame                        │
│    Paused:   pause() — frozen sim, UI still live                   │
│    Simulate: host tick, optional skip Present (later)              │
└────────────────────────────────────────────────────────────────────┘
```

The editor is the outer host: it owns OS event polling, editor wall-clock
delta, and the one composite/present call.  `GameLoop` remains the simulation
clock and phase scheduler.  In hosted mode its Presentation phase builds the
`RenderScene` packet, but the host consumes that packet together with editor UI
through `RendererSubSystem::renderCompositeFrame`; `GameLoop` does not submit a
second standalone render frame.

### 2.1 Mode matrix

Aligned with [AYExtension/design.md §3.2](../AYExtension/design.md). v0 implements **Edit**, **Play**, **Paused** first; **Simulate** is optional.

| Mode | GameLoop | Entity / World | Renderer (viewport) | AYUI chrome |
|------|----------|----------------|---------------------|-------------|
| **Edit** | prepared + `pause()`; no tick | No `update()` | Clear / static scene optional | Always `update` + `render` |
| **Play** | `tickOnce()` once per host frame | Normal `update()` | Full 3D + UI overlay | Always `render` |
| **Paused** | `pause()` | No `update()` | Last frame frozen | Always `render` |
| **Simulate** | host-driven tick without gameplay input (deferred) | Normal `update()` | Skip Present (deferred) | Optional |

**Time domains:** The host measures editor chrome delta with a monotonic wall
clock and passes that unscaled value to `EditorSession`.  Game simulation keeps
its own Scaled/Unscaled/RealWall domains inside `GameLoop`; editor UI must not
slow down when `timeScale != 1`.

### 2.2 Frame order (target, U2+)

```
1. Host polls OS input.
2. Host measures monotonic `realDt` and calls `EditorSession::update(realDt)`.
3. In Play, `EditorPlayRuntime::tick()` calls `GameLoop::tickOnce()` exactly once.
4. GameLoop Presentation builds its `RenderScene` packet; it does not present.
5. `RendererSubSystem::renderCompositeFrame` draws the 3D viewport and AYUI
   chrome into the same bgfx frame.
6. bgfx submits/presents that composite frame.
```

v0 demo (E0) may use **UI-only window** with a gray `Image`/rect as viewport placeholder.

### 2.3 Viewport presentation: interim vs target

Editor logic (modes, GameLoop, viewport **geometry**) is independent of how pixels are composited.  
Two presentation stacks exist in the roadmap; **only the demo/compositor layer differs**.

| Layer | Interim (E2-interim, legacy demo) | Current (E2-composite) |
|-------|-----------------------------------|------------------------|
| **Editor chrome** | GDI on **host** HWND via `GdiRenderBackend` | `AYUIRenderBackend` composited in `RendererSubSystem::renderCompositeFrame` |
| **3D viewport** | bgfx on **child** HWND | bgfx on **main** window with `RendererSubSystem` viewport sub-rect |
| **Native window owner** | Raw Win32 in demo | **AYDevice** `WindowManager` → `getWindowHandle()` |
| **Why interim existed** | GDI `BitBlt` onto a D3D/bgfx swap-chain **host** surface is unreliable on Windows | Single present path, no Z-order / region hacks |

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
- `GameLoop::prepareHostedSession`, `tickOnce`, `stepOnce`, `getElapsedTime()`
- `GameLoop::setPresentationOwnership(PresentationOwnership::ExternalHost)`
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

### 4.3.z UI Layout Editor (v0.5)

Tools → **UI Layout Editor…** opens an `EditorChildWindowManager` child that loads `assets/ui/layout_editor.ui.json` and attaches shared `ayt::ui::LayoutEditorSession` (same core as standalone `AYUI_LayoutEditor`). Format remains `*.ui.json` via `UILayoutLoader` — see [AYUI design §20](../AYUI/design.md#20-ui-layout-editor).

### 4.3.aa Audio Editor (v0.5)

Tools → **Audio Editor…** opens a child that loads `assets/ui/audio_editor.ui.json` and attaches shared `ayt::audio::AudioEditorSession` (same core as standalone `AYAudio_AudioEditor`). Mixer desk: transport, stream BGM, bus gains, duck/reverb/timeScale, spatial L/C/R. Requires `AudioSubSystem` from `registerDefaultEditorModules` (skipped with `-no-audio`).

### 4.2.x Editor Session 持 Edit Scene（v0.3 PR-4）

`EditorSession` 持 `std::unique_ptr<ayt::scene::Scene> _editScene`（SceneMode::Edit），
与 EditorSession 同寿。`initialize()` 末尾（line 124）：

```cpp
if (auto* host = ayt::app::currentEngineHost()) {
    if (auto* sm = host->scenes()) {
        _editScene = std::make_unique<ayt::scene::Scene>(
            ayt::scene::SceneMode::Edit, "<editor_default>");
        sm->setEdit(_editScene.get());
        sm->setCurrent(_editScene.get());
    }
}
```

`shutdown()` 末尾 reverse（setCurrent(nullptr) → setEdit(nullptr) → reset）。

**不接 EditorPlayRuntime 私有通路**：`EditorGameView::applyMode` Play/Edit 切换
仍走 `EditorPlayRuntime::startPlay()/enterEdit()`，**不**调
`host->scenes()->beginPlay()`。理由：EditorPlayRuntime 私有通路直接操作 World
（spawn cube / ground / glass / playerController），不走 Scene::load——与
SceneManager 的 "Edit ↔ Play Scene 切" 语义不同。v0.3 PR-4 仅 ship
"Editor 持 Edit Scene + transport bar UX"，不改 EditorPlayRuntime 业务。

**EditorMode 与 SceneMode 分离**（决策 3a）：EditorMode 3 态（Edit / Play /
Paused）vs SceneMode 2 态（Edit / Play）。Paused 不接 SceneManager；Editor 状态
切换走 `_gameView.setMode()` 私有通路。

### 4.3.x Transport bar UX（v0.3 PR-4 / Q4=b 最小 Hierarchy）

`bindTransportBar` 改造（PR-4）：

1. **`btn_play` enable 条件** = `host->scenes()->canBeginPlay()`（决策 1a；
   内置逻辑；caller 仍可点击；enable 是 UI 提示）
2. **`btn_play` click handler 头部 dirty prompt**（决策 4a）：
   - `host->scenes()->requireSaveBeforePlay()` →
     弹 Win32 `MessageBoxW(MB_YESNOCANCEL | MB_ICONWARNING)`
   - **Cancel** → 早返（不切 mode）
   - **Save** → 调 `host->scenes()->edit()->save(path)`；失败弹错 + 早返
   - **Discard** → 继续（切 EditorMode::Play）
3. **`lbl_unsaved` TextLabel**（`editor_shell.ui.json` 新增；id=`lbl_unsaved`）：
   - dirty → 显示 "•"（warning 色 `(0.85, 0.55, 0.10, 1.0)`）
   - clean → 隐藏（visible=false）
   - refresh 时机：`onModeChanged` + `bindTransportBar` 末尾 + 编辑器主动调
     `refreshUnsavedIndicator()`（决策 5a）；**不**每帧轮询（避免每帧调用
     host facade）
4. **Scene 列表最小 Hierarchy**（Q4=b）：
   - `lbl_mode` 已有 + `lbl_unsaved`（dirty 指示）
   - 显示当前 Edit Scene name（`host->scenes()->edit() ? ... : ""`）+
     Play Scene name（`host->scenes()->play() ? ... : ""`）
   - **不**展开 entity 树；entity 树推迟后续 PR

**决策镜像**（PR-4 7 项 decision）：

| 决策 | 内容 |
|------|------|
| 1a | caller 持 _editScene ownership（std::unique_ptr） |
| 2a | 不接 EditorPlayRuntime 私有通路 |
| 3a | EditorMode 3 态 vs SceneMode 2 态分离 |
| 4a | Save/Discard/Cancel 三选项（Win32 MessageBoxW） |
| 5a | lbl_unsaved 不每帧轮询；mode 切换 + 编辑器主动 refresh |
| 6a | PR-4 不 ship Scene 树（entity 树推迟后续 PR） |
| 7a | EditorShellDemo 同步加启动日志验证 wiring |

**测试矩阵**（v0.3 PR-4 ship 时）：

- `editor_transport_edit_scene_injected_after_initialize` — _editScene 创建 + host 反映
- `editor_transport_can_begin_play_true_after_initialize` — canBeginPlay 翻转
- `editor_transport_lbl_unsaved_visible_when_edit_dirty` — dirty 显 "•"
- `editor_transport_lbl_unsaved_hidden_when_edit_clean` — clean 隐藏
- `editor_transport_lbl_unsaved_follows_on_mode_changed` — mode 切换同步

**Deferred**（不 ship PR-4）：
- ❌ MessageBoxW 拦截（Win32 API hook 复杂；e2e 验证）
- ❌ Save/Discard/Cancel 3 选项 click handler 完整路径（依赖 _hostWindow + MessageBoxW）
- ❌ Entity 树 Hierarchy 面板（PR-4 仅 Scene 列表最小版）
- ❌ 接 EditorPlayRuntime 私有通路到 SceneManager（私有通路直接操作 World）
- ❌ dirty 信号订阅（PR-1/2/3 决策 7a/5a/3a 三层锁）

### 4.3.y Scene runtime bridge（v0.4 PR-1 / 通路接线收口）

PR-4 ship 了 `_editScene` ownership + transport bar UX；PR-5 ship 了 Hierarchy entity tree。但 **btn_play / btn_stop click handler 完全不调 `sm->beginPlay()` / `sm->endPlay()`**，entity spawn 仍走 `World::instance()` —— 两条路径并存导致 `_current` 永远指向 Edit Scene，但 Play 模式实际上挂在 `World::instance()` 上。**PR-1 收口最后一刀** = 让 EditorPlayRuntime 真正接通 SceneManager，让 spawn 路径走 `sm->play()->world()`（收口 LM-1）。

**决策**（v0.4 PR-1）：

| # | 议题 | 裁定 |
|---|------|------|
| Q1 = a | **adapter 模式** — EditorPlayRuntime 做 adapter，不直接 EditorSession 调 SM | single source of truth = `_runtime.startPlay/enterEdit`；applyMode/btn_play/btn_stop 不直接调 SM |
| Q2 = b | **保持双 tick**（Play 仍 `GameLoop::tickOnce()`，**不切** `SM::tick`） | renderer 帧提交 + system tick + network poll 在 GameLoop 内耦合；切 SM::tick 破坏 pipeline 顺序 |
| Q3 = a | **endPlay idempotent**（SM 内部 `_play==nullptr` 时 no-op；三处触发安全） | btn_stop + enterEdit + shutdownEngine 三处都可能触发；idempotent = 兜底 |
| Q4 = a | **net-client 路径不调 beginPlay**（推迟到 v0.5） | client consume 服务端 EntitySpawn，本地不持久化 |
| Q5 = a | G6 / G7 / G8 推到 v0.4 PR-2 / PR-3 | 本 PR 最小切片 = G1+G2+G3+G4+G5+G9 |
| Q6 = a | **1 PR ship** | 避免拆多刀 |
| Q7 = a | 新增 7 case + 既有 173 不 regress | target 175-177 PASS |

**Gap 收口表**：

| ID | gap | 收口点 |
|----|-----|--------|
| **G1** | btn_play 不调 `sm->beginPlay()` | `_runtime.startPlay()` 头部插入 `sm->beginPlay()`（非 Client 路径） |
| **G2** | btn_stop 不调 `sm->endPlay()` | `_runtime.enterEdit()` 头部插入 `sm->endPlay()` |
| **G3** | applyMode 不调 SM | adapter 模式：EditorPlayRuntime 做单一入口 |
| **G4** | entity spawn 走 `World::instance()` | 新增 `resolvePlayWorld()` helper：Server → `sm->play()->world()`；Client / fallback → `World::instance()` |
| **G5** | `_runtime.enterEdit` 不调 `sm->endPlay()` | F3.b 头部兜底（与 shutdownEngine 自动覆盖） |
| **G6** | `_gameView.stepOnce()` 不走 SM | defer v0.4 PR-2（D1） |
| **G7** | `refreshOutliner` Play mode 仍走 `World::instance()` | defer v0.4 PR-2（D2） |
| **G8** | mode label 反映 `EditorMode` 而非 `SceneMode` | defer v0.4 PR-2/3（D3） |
| **G9** | `--net-client` 走 `autoEnterNetClientPlay` | 同 G1：`sm->beginPlay()` Client 短路 |

**5 个新 landmine**：

| ID | 风险 | 缓解 |
|----|------|------|
| **LM-X1** | beginPlay save 失败静默吞错 | startPlay 头部 fprintf stderr + return false；btn_play UX 不动 |
| **LM-X2** | net-client 路径 fallback 噪音 stderr | helper 优先判 `_netPlayRole == Client` → 直接返 `World::instance()` 静默 |
| **LM-X3** | endPlay 后 entity 裸指针 dangle | `releaseOwnedEntity(ptr, world&)` 走 resolvePlayWorld；endPlay 后 `sm->play()` 返 nullptr → fallback `World::instance().isInitialized()` 检查 + 置 nullptr |
| **LM-X4** | Tick 路径未切 SM::tick → Play Scene 无独立 tick 入口 | §6 明确"PR-1 不切"；system tick = GameLoop::TickSystems() 遍历 World::instance() 系统注册器 |
| **LM-X5** | Test runner double-include LNK2005 | main.cpp `#include` only；不进 add_executable（PR-4/5 同 landmine） |

**7 个新 test case**（`Test_EditorSceneBridge.cpp`）：

- T1: `editor_scene_bridge_btn_play_invokes_begin_play` — G1
- T2: `editor_scene_bridge_btn_stop_invokes_end_play` — G2 + idempotent
- T3: `editor_scene_bridge_play_world_resolve_no_panic` — G4 fallback
- T4: `editor_scene_bridge_enter_edit_fallback_calls_end_play` — G5
- T5: `editor_scene_bridge_net_client_path_skips_begin_play` — G9 + 决策 4a
- T6: `editor_scene_bridge_end_play_destroys_play_scene_world` — G4 destroy + LM-X3
- T7: `editor_scene_bridge_is_edit_dirty_survives_play_round_trip` — INV-3/4 锁

**不在 PR-1 范围**（明确 defer）：

- D1: G6 `_gameView.stepOnce()` 不走 SM（v0.4 PR-2）
- D2: G7 `refreshOutliner()` Play 模式切 `sm->play()->world()`（v0.4 PR-2）
- D3: G8 mode label 改用 `SceneMode`（v0.4 PR-2/3）
- D4: G9 net-client 路径显式 beginPlay（v0.5）
- D5: LM-X3 端到端清理顺序重构（v0.4 PR-2）

**AYScene 0 改动**：v0.3 PR-3 已 ship 完整公共契约；本 PR 仅消费 `sm->beginPlay()` / `sm->endPlay()` / `sm->play()` / `sm->canBeginPlay()` / `sm->edit()` / `sm->isEditDirty()` / `sm->currentMode()`。

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
│   └── AYEditor/EditorSession.h
├── include/
│   ├── AYEditor/EditorSession.h
│   ├── AYEditor/EditorGameView.h
│   └── AYEditor/EditorPlayRuntime.h   ← viewport host (child HWND interim; AYDevice later)
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
| 2026-08-06 | v0.3 PR-4 Editor 持 _editScene + transport bar UX（决策 1a/2a/3a/4a/5a/6a/7a） |

---

## 12. References

- [AYUI/design.md §13 Editor chrome](../AYUI/design.md#13-editor-chrome)
- [AYExtension/design.md §3 Editor Integration](../AYExtension/design.md)
- [AYApplication/design.md §3 BuildType / subsystems](../AYApplication/design.md)
- [AYGameLoop/design.md](../AYGameLoop/design.md) — `tickOnce`, Unscaled editor time
- [AYDevice/design.md §3 WindowManager](../AYDevice/design.md#3-窗口管理-windowmanager) — native handle owner for E3+
- [AYSerializer/README.md](../../AYFoundation/AYSerializer/README.md) — future scene/Inspector I/O
