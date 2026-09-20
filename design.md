# AYEditor Design

**Version:** v0.3.2
**Date:** 2026-09-18
**Status:** E2-composite + native SVG shell icons + Transform Gizmo baseline + §4.2.x Editor 持 Edit Scene + §4.3.x Transport bar UX
**Owner:** AYEditor

> **2026-09-21 实现状态**：§4.3.skeletal 已落地 UI-free `AYAnimationEditorCore` 与 AYEditor 薄适配，覆盖源骨架检查、线框交互、骨树搜索、57 角色映射/诊断、mapping/template/最小 retarget RigProfile、目标/platform 配置、双状态标志和动画/时间轴预览；实际重定向求解与引用安全的生产清理烘焙仍按 SKA 后续项实施。SKA 优先级和验收统一见 [骨骼动画资源管线设计](../../AYDocs/SKELETAL-ANIMATION-RESOURCE-PIPELINE.md)。

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

- Multi-selection/pivot editing, transform snapping and parent-space gizmos.
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
│    Play:     tickHostedFrame(host frame) once per host frame       │
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

Each outer iteration constructs one `HostedFrameContext` *after*
`DeviceManager::pollEvents() and passes it through `EditorSession` to
`GameLoop::tickHostedFrame()`.  Its delta is the only time sample used by the
Play simulation for that host frame; `hostFrameIndex` and `inputFrameIndex`
are copied into every subsystem `FrameContext`.  The indices establish the
input-snapshot boundary without coupling AYGameLoop to AYDevice.  The current
`DeviceInputProvider` remains a named-query adapter over that stable,
already-polled device state; a future copied/action-mapped input snapshot can
replace it without changing the GameLoop boundary.

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

### 4.3.z UI Layout Editor (v0.6)

Tools → **UI Layout Editor…** 打开或聚焦唯一的 **AYUI Designer 独立工具窗**。它是由主编辑器
HWND owner 持有的 modeless 顶层窗口，不占用 Scene View 的 Center Dock，也不伪装成可拆卸
`DockCard`。窗口创建、激活、标题更新、关闭事件以及键鼠/滚轮/焦点都通过 AYDevice
`WindowManager` 与 `EditorChildWindowManager`；AYUI 只接收已经归一化的逻辑坐标。

`EditorUiLayoutExtension` 仍注册 `*.ui.json` 文档类型，`EditorUiLayoutDocument` 继续进入统一的
路径、标题、revision、dirty、Save/Save As 与关闭策略。新的
`EditorUiLayoutController` 包装共享 `ayt::ui::LayoutEditorSession`，由独立窗的 UIManager 绑定；
扩展 View 只保留为通用 Host/测试兼容路线，正常 Shell 打开路径不再创建 Center 页面。独立
`AYUI_LayoutEditor` 继续作为同一编辑核心的模块级回归宿主，因此保存、输入和撤销状态机没有
第二套实现。

Chrome 来自 `EngineAssets/AYUI/ui/layout_editor.ui.json`，采用 File/Edit 菜单栏、Widget Library +
Document Outline、Canvas、可滚动 Inspector、状态栏布局。文件操作进入 File；撤销、复制粘贴、
删除和层级调整进入 Edit；对齐、分布与 Snap 进入 Inspector 的选择上下文。Widget Library 是
单列紧凑列表，每个类型使用独立 SVG 图标，列表行同时支持点击创建和拖放到画布。属性以紧凑
label/control 行和动态 section 显隐呈现；空属性行不会继续占位，选择变化在同一输入事务内完成
invalidate + layout，避免 Inspector 显示上一控件的结构。

Widget Library 现已覆盖 Image、集合/树、Tab、Grid/Scroll 及 Window/Modal 等常用运行时类型。
Image 的 Browse 由 AYEditor 提供原生路径选择，预览图经 stb_image 解码后上传到该 Designer 子窗
自己的 GDI DIB 纹理表并以 premultiplied-alpha `AlphaBlend` 绘制；句柄生命周期停留在 GDI backend，
布局只保存 texture name。Controller 与类型有效事件可在 Inspector 中编辑为声明式名字，真正的
C++ 回调仍由使用该布局的宿主通过 AYUI Loader 注册，AYEditor 不在 JSON 中生成或执行游戏脚本。

画布选择装饰是透明、像素对齐的单层轮廓与控制点，不改变被选 Widget 的填充，也不重复绘制
第二层边框。AYEditor 链接 editor-only `AYUILayoutEditorCore`；其中 `LayoutDocumentModel`、
`LayoutSelectionModel`、公共 `EditorCommandHistory` 和 `LayoutCanvasViewport` 分别承载 authoring 状态，
`LayoutEditorSession` 只做 chrome/手势协调。Workspace Document 的保存仍委托绑定 Controller，且
Document 自身不持有 Widget、HWND 或渲染对象。

Widget Library、默认创建参数和 Inspector 属性集合统一读取 `WidgetAuthoringRegistry`；属性 section
由 `PropertySchema` 生成。布局编辑命令接入引擎统一的文档历史；高频属性保留细粒度命令，结构编辑
在迁移期保留完整 JSON snapshot 兜底，因此 standalone 与 AYEditor 的 undo/redo 和保存点行为一致。

第三阶段的产品编辑能力仍由共享 core 提供。Collections、完整 Tree source、Tab page 与 RichText run
使用 Structured Content Inspector 增删、重命名和排序；AYEditor 只为 `TextureResourceProvider` 枚举
Project/Assets 与 EngineAssets，搜索、missing/invalid 状态和选中赋值由通用资源目录模型处理。
Canvas 可切换 Desktop/HiDPI/Phone/Tablet 或自定义物理分辨率、DPI 与 Safe Area，预览 extent 不会
写回文档 root。F6/Interact 暂时把输入交给运行时控件，退出后从进入前 snapshot 回滚交互状态；
因此 Designer 无需复制一套游戏 UI host，也不会让试点操作污染 Workspace Document dirty 状态。

文档根节点是固定 authoring origin，不提供 X/Y 或排列入口，方向键和 Ctrl+滚轮不会改写它的
位置。普通自由定位控件支持方向键 1px 微调，Shift+方向键使用网格步长，且不会被 Snap 抵消。

### 4.3.aa Audio Editor (v0.5)

Tools → **Audio Editor…** opens a child that loads `EngineAssets/AYAudio/ui/audio_editor.ui.json` and attaches shared `ayt::audio::AudioEditorSession` (same core as standalone `AYAudio_AudioEditor`). Mixer desk: transport, stream BGM, bus gains, duck/reverb/timeScale, spatial L/C/R. Requires `AudioSubSystem` from `registerDefaultEditorModules` (skipped with `-no-audio`).

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
| **G7** | `refreshOutliner` Play mode 仍走 `World::instance()` | `EditorWorldContext` Play slot：Scene World 优先、显式 fallback |
| **G8** | mode label 反映 `EditorMode` 而非 `SceneMode` | defer v0.4 PR-2/3（D3） |
| **G9** | `--net-client` 走 `autoEnterNetClientPlay` | 同 G1：`sm->beginPlay()` Client 短路 |

**5 个新 landmine**：

| ID | 风险 | 缓解 |
|----|------|------|
| **LM-X1** | beginPlay save 失败静默吞错 | startPlay 头部 fprintf stderr + return false；btn_play UX 不动 |
| **LM-X2** | net-client 路径 fallback 噪音 stderr | helper 优先判 `_netPlayRole == Client` → 直接返 `World::instance()` 静默 |
| **LM-X3** | endPlay 后 entity 裸指针 dangle | runtime-owned entity 先在 Play World 内释放，再调用 `endPlay()` 销毁 Scene World |
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
- D2: G7 已由 `EditorWorldContext` Play slot 收口。
- D3: G8 mode label 改用 `SceneMode`（v0.4 PR-2/3）
- D4: G9 net-client 路径显式 beginPlay（v0.5）
- D5: LM-X3 已收口：清 runtime entity 在前，`endPlay()` 在后。

**AYScene 0 改动**：v0.3 PR-3 已 ship 完整公共契约；本 PR 仅消费 `sm->beginPlay()` / `sm->endPlay()` / `sm->play()` / `sm->canBeginPlay()` / `sm->edit()` / `sm->isEditDirty()` / `sm->currentMode()`。

### 4.3.world Explicit editor world context

`EditorWorldContext` is the non-owning resolution boundary shared by
`EditorSession`, hierarchy/inspector tools, and `EditorPlayRuntime`:

- Edit resolves only `SceneManager::edit()->world()` and never falls back.
- Server Play prefers `SceneManager::play()->world()`.
- Network-client and standalone runtime paths use an explicitly configured
  process-world fallback.
- `EditorSession` owns the context and binds the host `SceneManager`; neither
  the context nor editor tools own a Scene or World.
- Runtime entities are released while the Play Scene World is still alive,
  before `SceneManager::endPlay()` destroys it.

This boundary removes per-panel global world lookup and is the injection point
for future preview worlds and multiple editor viewports.

### 4.3.network P2P authority migration

`NetPlayRole` is the immutable launch/world-ownership role. Runtime authority
is queried from AYNetwork's `P2PSessionInfo`: a client promoted by Host
Migration becomes authoritative without being reclassified as a server-owned
Play Scene. This distinction keeps `enterEdit()` teardown on the World that
originally created each entity.

When AYNetwork is preconfigured for P2P, Play uses `listenP2P()` or
`connectP2P()` and enables host migration; otherwise the existing IP
listen/connect path remains unchanged. `EditorPlayRuntime` subscribes to P2P
session lifecycle events. On `AuthorityChanged` to Host it marks retained
client replication objects as owned, stops client polling, and rebroadcasts all
registered spawn announcements to later connections. The listener is removed
before runtime shutdown so no callback can target a destroyed editor object.
While AYNetwork reports `isP2PMigrationFrozen()`, the Play runtime update
listener does not poll replication, rotate demo state, spawn late joins, or run
other editor-owned authority mutations. Replication's final Full and migration
control frames continue through the network subsystem independently.

---

### 4.3.skeletal 骨骼映射与重定向作者工作区（第一版已实现）

完整资源、状态和验收契约见 [统一设计](../../AYDocs/SKELETAL-ANIMATION-RESOURCE-PIPELINE.md) §3～§7。
工作区使用通用文档/扩展宿主及 AYDevice 窗口生命周期，不在 AYUI 中实现骨骼业务，也不另建资源加载器。

- **P0 / SKA-04**：打开完整源骨架与绑定的映射/profile 作者资源；树/角色槽、搜索、左右侧与缺失高亮、模板预览/手工覆盖、目标选择、参考姿势修正。配置保存不改写源骨名、层级或 Bind Pose。
- **P0 / SKA-05**：Content Browser 图标与 Inspector 同时展示醒目的适配、烘焙两个标志；展示 profile/目标/platform 范围、未配置/不完整/失效和未烘焙/当前/过期/失败。原生与非人形状态明确；不以 canonicalName 或 importState 的 Ready 判断已重定向。
- **P0 / SKA-06**：源/目标并排骨轴与动画预览、时间拖动和逐帧检查，复用 AYAnimation 转换核心；不通过实体补偿或修改源绑定掩盖问题。
- **P0 / SKA-07～09 协作**：发起检查、清理预览和异步烘焙，显示保留/删除及受影响依赖、取消/失败诊断；迟到结果按请求 generation 丢弃。发布使用同一 manifest 门禁。
- **P1 / SKA-10/13**：批量 clip/模板库、依赖导航、直接兼容/需重定向/不兼容分级、配置比较和迁移影响分析。

映射修改、模板应用、参考姿势与绑定选择接入文档级 `EditorCommandHistory` 和保存游标。
重新导入只迁移可无歧义关联的条目；dirty 文档不被磁盘操作静默覆盖，索引重排不沿用旧映射。
标志读取统一校验/构建记录，以文字和图形共同表达，不只依靠颜色或维护 UI 私有布尔值。
当前已具备 Skeleton Editor 第一版与规范 `.ayrig` Rig Profile 作者资源；编辑器仍可读取 Legacy `.aysmap`，但会醒目标记并在保存时迁移为 `.ayrig`，旧文件不被静默覆盖。不得据此声明已有完整运行时重定向求解器或引用安全的生产清理烘焙器。
项目元数据 `.ayeditor/skeleton-profile-bindings.json` 现以工程相对路径保存骨架到默认 mapping/retarget profile 的显式绑定；同一骨架可在工作区切换多个 `.ayrig`，同名 sidecar 仅保留为首次建议。`kind=template` 的 `.ayrig` 可被发现、预览命中/保留/缺失/冲突计数并显式应用，已有手工槽不会被模板静默覆盖。骨树支持按名称/索引搜索，角色槽显示左右侧、必需、缺失和重复占用。`kind=retarget` 的最小结构保存目标骨架、`BakeToTarget` 和 platform；目标/模式/platform 进入 profile 指纹和输出 scope，求解器交付前预检必须以 `retargetSolverUnavailable` 阻止烘焙。普通资源 Inspector 与 Content Browser 读取同一 authoring/build 状态来源展示 Mapping/Bake 两个状态及 retarget 目标/platform。

## 5. Editor chrome (AYUI)

### 5.1 Contract

- **File:** `EngineAssets/AYEditor/ui/editor_shell.ui.json` (or an explicit
  `EditorSessionDesc::layoutPath`).
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

### 5.4 Render Settings verification contract

Every user-visible rendering effect mounted by an Editor pipeline must retain a
direct verification control in `Render Settings`. One visual effect maps to one
enable switch even when it owns multiple implementation passes (for example
Bloom Extract + Blur). The current panel exposes Bloom, Depth Haze, SSAO, FXAA,
LUT Color Grading and Shadows; Tonemap uses its `None` mode as the disabled
state. Color Grading exposes Neutral/Warm/Cool/Cinematic presets plus strength
and defaults off. Controls remain live in Edit/Play and are reapplied after
presentation or pipeline recreation.

### 5.5 FreeCam viewport navigation

Mouse wheel input over the unobstructed 3D viewport dollies FreeCam along the
world ray under the pointer. This makes the pointer the visible zoom anchor and
does not change FOV. AYUI logical wheel pixels are converted back to native
notches before navigation so high-resolution touchpads preserve fractional
motion without multiplying sensitivity. UI overlays, menus and scrollable
panels retain wheel priority, so camera navigation cannot consume their input.

Continuous flight is an explicit RMB-held mode. While RMB is held, mouse
movement changes view direction, W/S move along the full camera forward vector
(including pitch), A/D strafe, Q/E descend/ascend and Shift boosts speed.
Releasing RMB ends both mouse-look and keyboard movement. Ctrl is never a
camera key, so Edit-mode Ctrl+Z/Ctrl+Y/Ctrl+S remain unambiguous. LMB is reserved
for selection and Universal Gizmo handles; dragging an object or empty surface
does not silently enter FreeCam.

### 5.6 Native SVG shell icons

AYEditor owns command-to-icon semantics while AYUI owns SVG parsing, retained
path recording and rendering. `EditorSession::bindShellIcons` maps the window
controls, Play/Pause/Step/Stop transport and
viewport-options button to shared immutable `AYUI::SvgDocument` instances. A
button's JSON text is cleared only after its SVG parses successfully; failures
retain the existing text placeholder. Icon-only buttons set explicit
accessibility labels, so replacing visible text does not remove their command
names from UI Automation.

The canonical icon root is `EngineAssets/Icons/Tabler`; it is deliberately not
owned by AYEditor, AYUI, AYVideo or another feature module. Any engine surface
may map its own command semantics to these shared files. `AY_EDITOR_ICON_ROOT`
remains an explicit development/test override and must contain `outline/` and
`filled/`; there is no implicit executable-ancestor scan or `AssetRepo`
fallback.

This is intentionally a direct SVG path, not SVG→PNG conversion: icons remain
resolution-independent and no raster cache or SVG converter belongs in
AYEditor. The selected Tabler files are versioned engine assets and their MIT
notice is installed from `EngineAssets/Licenses/Tabler/LICENSE.txt`.

The supported SVG grammar and explicit rejection boundary are authoritative in
[AYUI design](../AYUI/design.md); AYEditor must not grow a second parser or a
private NanoSVG-style rasterizer.

### 5.7 Dock tabs and promoted-window lifecycle

Every non-empty dock leaf renders the same tab strip, including a leaf that
contains only one card. Tabs keep the editor preferred width (140 logical px)
while room remains, then compress and middle-elide their titles; unused strip
space is not assigned to the final tab. Only the active tab paints a close
button, and that button is an AYUI vector path rather than a font glyph. The
viewport card keeps the stable `Scene View` title even though its own embedded
header height is zero.

A promoted DockCard is the same live widget tree hosted by a borderless child
HWND. AYUI paints SVG/fallback-vector minimize, maximize/restore, and close
buttons; `EditorChildWindowManager` maps those semantic requests to AYDevice.
Dragging the promoted title moves the child HWND and publishes a drop guide on
the primary DockArea. A valid drop reparents the same card back into the dock
and removes child-only chrome state, so detaching and docking never serialize
or recreate panel contents. The child host derives the authoritative screen
point from each Win32 mouse message's client coordinates, so follow-window
movement and synthetic input cannot leave `GetCursorPos` stale. Those Win32
coordinates are physical pixels and must pass through the primary UIManager's
`physicalToLogical` boundary before DockArea hit-testing; this keeps guides and
drops aligned at 125%/150% display scaling.

### 5.8 Installed product layout and path authority

AYEditor uses a staged install tree rather than treating a build directory as
the product. `EditorProductPaths` is the single path authority for both the
root launcher and the hosted editor:

```text
AYEditor/
├─ AYEditor.exe                 root launcher; system DLLs only
├─ EngineAssets/                immutable, repository-owned engine assets
│  ├─ Icons/Tabler/             shared across all engine modules
│  ├─ AYEditor/                 editor config and shell layout
│  ├─ AYUI/ and AYAudio/        child-tool layouts
│  ├─ AYRenderer/               editor-required renderer data
│  ├─ AYScript/                 editor-required script templates
│  └─ Licenses/                 third-party asset notices
├─ AYRuntime/                   hosted editor executable and runtime DLLs
├─ UserAssets/                  writable user project/cache root
│  └─ Assets/
└─ logs/                        writable product logs
```

The launcher sets `AY_EDITOR_PRODUCT_ROOT` and starts
`AYRuntime/AYEditorShell_Demo.exe`; keeping the host beside its DLL closure
avoids Windows loader failures before `WinMain`. Installed runtime lookups never
walk the source tree. Development builds use the CMake-provided
`AY_ENGINE_ASSETS_SOURCE_HINT`; tests may explicitly override
`AY_EDITOR_ENGINE_ASSETS_ROOT`, `AY_EDITOR_USER_WORKSPACE_ROOT` or
`AY_EDITOR_ICON_ROOT`.

`cmake --install <build> --config <config> --component AYEditorProduct` creates
the product tree. `AYEditorStage` is the build convenience target and stages to
`AY_EDITOR_INSTALL_ROOT`. Reinstall replaces only product-owned `EngineAssets`
and `AYRuntime`; it preserves `UserAssets` and `logs`. Debug staging is a local
validation layout and still requires the matching MSVC debug runtime. A release
installer must provide the supported VC runtime prerequisite separately.

Closing a promoted window is deferred until platform/UI dispatch has unwound.
Before destroying the child host, the manager returns the card to the primary
DockArea and invokes its normal close policy. Persistent tool panels are parked
and remain reopenable from Window; DSL documents run their dirty Save/Discard/
Cancel path. Editor shutdown is the exception: it tears child hosts down
directly and must not re-enter the primary dock.

---

## 6. Application & subsystems

From [AYApplication/design.md](../AYApplication/design.md):

- **`BuildType::Editor`** — compile-time `AY_BUILD_TARGET_EDITOR`.
- Register game subsystems (Entity, Renderer, …) **plus** editor-only:
  - `EditorToolsSubSystem` — Unscaled tick: `EditorSession::update`, UI hot-reload.
  - `SceneEditorSubSystem` — deferred (scene editing logic).

E3 wires `EditorApp : IApplication` to construct `EditorSession` after GameLoop init.

**Do not** put `EditorGameView` inside AYUI widgets.

### 6.1 Startup presentation and validation-scene boundary

The production main HWND is created hidden. `EditorStartupSplash` owns a small
borderless Win32/GDI window and its own message thread, so progress remains
responsive while synchronous renderer and importer work runs on the editor
thread. Startup ordering is fixed:

1. show the startup progress window;
2. create the final editor HWND hidden;
3. initialize `EditorSession`, renderer, UI backend, and input against that
   final HWND;
4. submit one complete composite warm-up frame while the HWND is hidden;
5. close the progress window, then reveal the editor HWND.

The splash is presentation only: it must not own AYUI, a renderer subsystem, or
a temporary swap chain. `WM_ERASEBKGND` on the final editor surface uses the
editor bootstrap colour as a failure/resize fallback, so a slow or failed first
present cannot expose the generic white window-class brush.

Reference Character, Ground, Cube, and Glass entities are not product document
defaults. They are authored only by the clearly marked
`EditorPlayRuntime::initializeEditorTestScene()` function. Generic editor hosts
leave `EditorSessionDesc::editorTestSceneEnabled` false; the
`AYEditorShell_Demo` integration fixture opts in explicitly at its composition
root. Keep future renderer-validation objects inside that one function.

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

### 10.1 Editor visual system

AYEditor owns the product-facing visual policy while AYUI owns reusable
rendering primitives:

- `EditorVisualStyle` registers the `aliyat-editor-dark` palette through
  AYUI `ThemeManager` and applies an editor density profile after layout load.
- Theme, density, and UI scale are independent persisted preferences. A color
  change must not resize widgets; a density change must not fork the palette.
- `Compact` is the default profile: 13px body text, 12px secondary chrome,
  shorter buttons/tabs, while the default global UI scale remains 1.0 so
  viewport input and renderer coordinates keep their existing pixel contract.
- The primary editor uses a borderless Win32 surface with AYUI-owned chrome.
  Platform hit testing stays in `EditorApp`: blank title space drags/double-
  clicks, outer edges resize, and custom minimize/maximize/close buttons remain
  ordinary AYUI controls.
- If SVG/theme/font/density behavior becomes useful to more than one host, move
  the generic mechanism into AYUI. AYEditor keeps only palette choice, metrics,
  semantic icon mapping, and persistence policy.

### 10.2 Transform Gizmo baseline

- Edit 模式选中带 `Transform` 的实体后直接显示一个 Universal Gizmo；不再要求
  切换 Select/Move/Rotate/Scale 工具。Play/Paused 或无选择时不显示。
- Universal 同时提供 X/Y/Z 平移箭头、XY/YZ/ZX 平面方片、三个轴向旋转圆环、
  三个局部轴缩放方块和中心统一缩放方块。空间层级固定为：中心统一缩放、平面
  移动边框、内段缩放细杆与方块、留白、外段平移箭头、最外圈旋转环。平移箭头
  不再从原点出发，缩放和平移之间的留白也不接受任一语义的命中；几何由
  AYRenderer 程序化生成，不依赖贴图或模型资产。
- 缩放可见段约为 `[0.09, 0.38]`，平移可见段约为 `[0.50, 0.95]`，使外侧
  平移明显长于内侧缩放。CPU 轴向命中带分别覆盖 `[0.13, 0.40]`（中心附近由
  统一缩放优先）和 `[0.47, 0.97]`，径向热区使用 `0.105`，大于实际几何；缩放
  细杆本身、方块和完整箭头都可点击。平面边框视觉范围 `[0.18, 0.32]`、命中
  范围 `[0.16, 0.34]`，旋转环同样使用大于线宽的 `0.085` 热区。
- 视觉上平移杆半径由 `0.016` 收细至 `0.008`，旋转环半宽收细至 `0.010`，
  平面移动从不透明方片改为细边框；视觉变细不以牺牲鼠标可点击性为代价。
- 操作类型由命中的 handle 决定。命中优先级为中心统一缩放、轴缩放方块、平面、
  平移箭头、旋转环，避免投影重叠时随机切换语义。
- Universal Gizmo 按当前相机计算每个 handle 的屏幕投影可操作性。平移/轴缩放在
  轴向投影长度低于 `0.25`（约 14°）时禁用；平面与旋转环在投影面积低于同一阈值
  时禁用。已禁用 handle 必须恢复到 `0.32`（约 19°）才重新启用，形成滞回并避免
  相机位于临界角时闪烁。中心统一缩放始终可用。
- 禁用 handle 不参与 CPU 命中和拖动起始，但 Renderer 保留 36% 亮度的暗色轮廓，
  让用户仍能判断轴向。`EditorSession` 每帧把同一个禁用位掩码传给 Renderer，保证
  视觉状态与拾取状态一致；正在拖动的 handle 不会因局部旋转改变基向而半途变暗。
- 对象表面点击只负责选择，LMB 空白/对象拖动不旋转相机也不直接修改 Transform；
  RMB 按住才进入 FreeCam look，Transform 只从明确命中的 Gizmo handle 开始。
- Gizmo hover/drag 始终使用普通箭头系统光标；可操作性由具体 handle 的黄色高亮
  表达。通用四向 Move 光标无法区分平移、旋转和缩放，因此不用于 Transform。
- `EditorTransformGizmo` 是无 UI/GPU 依赖的 CPU 状态机，负责射线拾取、轴/平面
  约束、圆环角度和缩放计算；`EditorSession` 负责选择、鼠标捕获和 Renderer 状态同步。
- 拖动期间直接预览实体 Transform；释放时先恢复起始值，再通过
  `EditorSceneDocument::executeTransform` 提交最终值，因此一次连续拖动只产生一条
  Undo/Redo 命令并统一更新 document dirty。失焦、离开视口、切空间或切模式
  会回滚未提交拖动。
- World/Local 对 Move 与 Rotate 生效。Scale 固定使用局部轴，因为当前 TRS 组件无法
  无损表达任意世界轴非均匀缩放产生的 shear。
- 当前基线不包含 snapping、数值 HUD、多选 pivot、父子层级空间补偿和可编辑 pivot；
  这些在视觉与输入闭环稳定后迭代。

### 10.3 Project Content Browser P0

- `EditorApp` 将显式 `--project`、`Editor.ProjectRoot`、
  `AY_EDITOR_PROJECT_ROOT` 或自动探测到的仓库根传给 `EditorSession`，并同步
  `AYProject::Project`。编辑器不再以 exe 当前目录作为资源身份的一部分。
- `EditorAssetDatabase` 是 AYEditor 拥有的轻量项目索引，不是第二个
  `ResourceManager`。它异步扫描 `<project>/Assets` 与
  `<project>/.ayeditor_cache/assets`，在 UI 中分别映射为 `Assets` 和
  `Imported`，提供稳定 ID、目录浏览、递归搜索和类型过滤；解码、加载与热重载
  仍由 AYResource 负责。
- Content Browser 使用左侧目录树、右侧虚拟化 `AYUI::TileView` 与路径/搜索/类型工具条。
  单击选择资源，双击文件夹后把目录跳转延迟到下一次 editor update 消费，禁止在
  `TileCell` 回调仍在派发时重绑同一虚拟池。
- 选中资源会清除实体选择并切换 Inspector 到资源详情；选中实体则恢复组件
  Inspector。当前详情包含类型、来源、大小、逻辑路径和 AYResource 加载状态。
- `+` 通过 AYResource 导入管线把受支持的源文件写入 Imported 根并立即重扫。
  P0 不直接复制任意文件，也不引入私有格式转换器。
- Session 使用 `TileView::setDragPayloadBuilder()` 把选中的 Mesh tile 映射为
  `EditorAsset` drag payload。将 Mesh 拖到 Scene View 后，Session 用视口射线与 y=0 工作平面确定
  放置点，创建带 `Transform`/`MeshComponent` 的实体，选中它并把场景标记为 dirty。
- `EditorAssetTilePresenter` 是无状态、无 UI 输入的展示映射：保留包含扩展名的
  完整文件名，输出类型简称、资源类别与类别色，并按引擎原生扩展名输出右上角
  标记。Session binder 把这些值写入 AYUI 的通用 `InfoStrip`/`CornerMarker`；AYUI
  不包含 EngineAsset、Mesh 等编辑器语义。Presenter 不读取 SVG/PNG、不创建控件，
  也不执行重命名或其他文件系统操作；预览图来源与缓存延后到缩略图阶段实现。
- 当前 Content Browser 已包含异步缩略图缓存、Shift/Ctrl/框选、多选删除确认、
  小型文本资源的引用提示、项目内可恢复 trash，以及 Scene/UI/Tilemap/DSL 的
  双击打开。Mesh/Material/Animation 预览读取实际资源数据并按路径、大小、mtime
  缓存；PNG/JPEG/BMP/TGA 使用真实图像预览。
- 当前已包含重命名、目录选择式移动/复制、启动磁盘索引、文件系统 watcher、
  顺序异步导入队列、源依赖失效重导、引用修复、可浏览回收站与资源操作历史。
  仍未包含右键菜单、`.meta` GUID 和跨所有资源格式的完整依赖图。项目身份、
  导入策略和资源语义继续留在 AYEditor/AYProject/AYResource。

### 10.4 Phoskia / Logia DSL document tabs

- Content Browser 将 `.phoskia` 归类为 Shader、`.logia` 归类为 Script；双击两类
  文件时把打开请求延迟到下一次 editor update，避免在 `TileCell` 事件派发期间修改
  Dock 树。同一资源只保留一个 Center `DockCard`，再次打开只聚焦已有页签。
- `EditorDockViewHost` 是无 DSL 语义的通用 DockCard View Host：它通过
  `EditorDescriptor::createView` 创建视图，统一持有文档/视图/卡片关联，并负责激活、
  焦点命令路由、脏标题、关闭决策、tick 与两阶段 UI shutdown。`EditorSession` 不再
  复制一套 DSL 控件树，也不保存源码区、诊断区等裸控件别名。
- `EditorDslDocument` 是独立于控件的源码模型。它读取 UTF-8，编辑缓冲统一为 LF，
  保存时保留原文件的 UTF-8 BOM 与 CRLF 风格，并以原子替换写回；8 MiB 上限与
  Content Browser 的资源身份保持在 AYEditor，而不是下沉到 AYUI。
- 页签提供源码区、只读 Diagnostics、Save/Compile 工具条。修改后标题显示 `*`；
  `Ctrl+S` 保存，`F7` 编译当前内存缓冲，关闭脏页签时提供 Save/Discard/Cancel。
- 源码区启用行号 gutter 与竖向分隔线；行号、文本、选择、光标和鼠标命中共享
  TextArea 的同一测量坐标。Tab 插入到下一个四空格制表位，选区 Tab/Shift+Tab
  对涉及行统一缩进/反缩进，不再触发普通控件焦点遍历。
- AYEditor 按 Logia/Phoskia 语义配置逐行高亮：语言关键字、内建类型、数字/布尔
  字面量、字符串与行注释使用不同颜色。AYUI 只执行宿主提供的 span 绘制，不
  认识两种 DSL 的 token 或编译器。
- Phoskia 使用 AYShader 的 production frontend 与 BGFX source backend 生成并验证
  后端源码。该页面尚不拥有目标平台、shader profile、include 路径和 shaderc 配置，
  因而不在此处产出最终平台二进制。Logia 通过 AYScript production compiler 编译为
  Lua，并把结构化错误、行列与 hint 显示在 Diagnostics。
- Dock、TextArea 和按钮仍是 AYUI 通用能力；DSL 类型、编译器选择、文件保存语义与
  诊断格式只存在于 AYEditor/AYShader/AYScript。TextArea 的绘制、命中、选择框与光标
  必须共享同一有效字号和内容内边距，避免紧凑编辑器行高下产生累计偏移。

### 10.5 UI Layout workspace document

- `EditorUiLayoutExtension` 把 `.ui.json` 注册为文档；Tools 菜单使用固定 untitled workspace key，
  重复打开时激活同一 Document 并 bring-to-front 同一独立工具窗，不创建第二份 Controller。
- `EditorChildWindowManager` 创建 owner 指向主编辑器的普通顶层窗口，并提供 per-window UIManager、
  GDI backend、AYDevice 输入、逻辑坐标转换、动态标题、关闭 veto 和延迟 teardown。它不是 Scene
  Dock 的 floating-card 路径，关闭后也不会 redock。
- `EditorUiLayoutController` 是 AYEditor 与通用 Layout Session 的唯一行为适配层。Canvas/Palette
  手势优先经过 Controller；普通 Button、TextInput、ComboBox、ScrollView 与 popup 仍由该窗口的
  UIManager 正常处理。
- `LayoutEditorSession::attach(UIManager&, Widget* chromeRoot)` 支持 scoped chrome；独立 Designer
  当前绑定整个 child UIManager，通用 View fallback 则传子树 root。两条宿主路径共享保存、dirty、
  undo/redo、序列化和输入状态机。
- AYEditor 不再直接编译 demo 下的 Session 源码，而是链接 `AYUILayoutEditorCore`。Document、Selection、
  Command 与 Viewport 模型的所有权保持在共享 core，子窗口 manager 只持宿主资源与 native 生命周期。
- 图片选择器与预览 loader 同样通过 Controller config 注入。独立 Designer 的 GDI backend 持有
  preview bitmap，Layout Session 只持 `ImageTextureHandle` 和持久化名字；更换生产资源系统时无需
  修改 AYUI 控件或 Serializer。
- Inspector 可创作 controller/event handler 名字；运行时回调解析遵循 widget-id 精确覆盖、
  controller/handler、全局 handler 的顺序。AYEditor 只编辑契约，不拥有游戏 controller。
- 用户关闭 dirty Designer 时，由 DocumentManager 执行 Save/Discard/Cancel；Cancel 阻止原生窗口
  关闭。安全点先 detach Controller，再销毁 child UI tree/backend/HWND；Shell shutdown 使用强制
  teardown，但仍执行同样的 detach 顺序。
- Chrome 的 fill 区只声明主轴 `h=0`；同时声明 `w` 与 `h` 会按 Loader 契约固定尺寸，禁止用于
  Document Outline、Canvas、Inspector Scroll 这类需要父布局拉伸的区域。集成测试锁定三者实际
  高度/宽度，防止再次出现“控件存在但区域为 0”的视觉退化。

### 10.6 Tilemap workspace first integration

- Tilemap 的作者文件保存与运行时烘焙是两个不同结果。只要
  `.aytilemap.json` 已成功落盘，文档保存就成功并清除 dirty；位于项目 `Assets` 下时随后
  自动烘焙 v3 `.aytilemap`。v3 完整承载多图层、精确图集切片、逐 Tile tint 与语义阴影；
  cook 失败仍以独立 notice 报告，不能把已经完成的作者保存误报为失败，也不能阻止关闭。
- `TilemapWorkspaceDocument` 继续唯一持有共享 `AY2DEditorCore::TilemapEditorModel`。AYEditor View
  只负责控件、输入和状态映射，不复制 paint/fill/terrain/history/serialization 规则。
- 首次集成把原尺寸摘要页替换成可操作的三栏工作区：Tile 列表、中心正交画布、图层/Tile
  属性；工具栏提供 Pencil、Eraser、Fill、Rectangle、Grid、Collision 和 Frame。画布支持连续
  笔划、矩形预览、右键取样、滚轮缩放，以及中键或 Space+拖动平移。
- 无图像来源或来源暂时不可用的 Tile 使用作者数据中的确定性预览色；正常路径通过宿主图片
  服务显示完整 Source Sheet 与共享 PNG texture。View 不直接持有 Renderer 私有对象，AY2D
  core 也不依赖 AYUI。
- 视图命令继续进入统一 `EditorCommandRouter`；Save/Undo/Redo、脏页签、自动恢复和关闭决策
  不建立第二套状态。快捷键 P/E/F/R、Space 和 Home 由 `IEditorViewInputTarget` 映射，文本输入
  获得焦点时不得截获字母工具键。

### 10.7 Tilemap atlas authoring in the main editor

- `IEditorHostServices` exposes an editor-authoring image service and an image
  file picker. `EditorAuthoringImage` carries a borrowed `ImageTextureHandle`,
  original dimensions, and shared immutable BGRA8 source pixels. The Session
  cache owns the GPU handle and keeps authoring handles stable until Session
  teardown; extension views never release renderer resources.
  Because this extends a public virtual interface, AYEditor Source ABI advances
  from 2 to 3.
- Authoring images are decoded at original resolution with the same
  64-megapixel budget as AY2D import planning. Content Browser thumbnails stay
  independently capped at 512 px; atlas slicing must never inspect the resized
  thumbnail because it would change transparency and boundary decisions.
- Tile-sheet parameters live in a blocking import dialog, not Inspector. The
  preview receives the flexible majority of that dialog, numeric grid controls
  stay in a fixed side column, and commit delegates to
  `AY2DEditorCore::TileAtlasImportModel` before the document performs its atomic
  duplicate-ID validation.
- The permanent left side is source-first: it shows the original sheet with its
  spatial grid, including skipped transparent cells, and direct cell clicks
  select a Tile and return to Pencil. The textual Tile list remains a secondary
  lookup surface. Reopening a document resolves saved atlas paths and restores
  those textures through the same host cache.
- Canvas rendering uses one borrowed atlas texture per source, exact persisted
  source rectangles, render tint, and the established half-texel UV inset.
  Missing source files or unavailable GPU services fall back to deterministic
  preview color and produce a status message instead of invalidating the
  authoring document.

### 10.8 Smart atlas import and Stamp authoring

- On image selection the Tilemap workspace inspects bounded sibling `.json`,
  `.tsj`, and `.tsx` files through the shared AY2D metadata parser. It accepts only a
  Tiled tileset or TexturePacker atlas whose declared image matches the chosen
  bitmap, reports the exact metadata file, and keeps Grid/Free Regions as
  explicit user-selectable modes. The host owns discovery and file I/O; no
  filesystem dependency enters the picker or document model.
- The import preview supports integer source-space rectangle creation. Free
  Regions have local undo/clear before commit, while metadata regions are
  read-only. The preview and committed atlas use the same shared plan and the
  main Source Sheet renders arbitrary region outlines instead of a fabricated
  uniform grid.
- A regular-grid Source Sheet can enter Stamp Select mode. Dragging a rectangle
  creates one persisted `TileStampDefinition`; skipped source cells remain
  holes. A Stamp selector recalls definitions, delete is undoable, and the
  toolbar exposes Stamp as a distinct active tool so painting cannot be
  confused with Pencil or Eraser.
- Stamp placement delegates to `TilemapEditorModel` and remains a single
  history gesture. Canvas clipping skips only cells that land outside the map;
  it never shifts the selected pattern to fit.

### 10.9 Semantic Tilemap shadow authoring

- AYEditor binds the shared AY2D quarter-cell shadow plane rather than creating
  shadow Tile assets or invoking the 3D shadow-map settings. `Shadow` is a
  distinct toolbar mode and therefore cannot be confused with an inactive Tile
  brush.
- A labelled mask selector exposes the complete 4-bit brush space, including
  full/half/quarter presets and `Clear`. Selection changes only the Shadow brush;
  painting replaces the cell mask and is grouped by the core gesture history.
- The Tilemap canvas draws the persisted RGBA mask as four pixel-aligned
  half-cell overlays and previews the selected mask under the pointer. Runtime
  dynamic-light occluders remain outside this authoring integration.

### 10.10 Tilemap tool-level shell entry

- Registering a Tilemap document editor is not, by itself, complete shell
  integration. The main AYEditor exposes the same Tilemap workspace from both
  `Tools -> 2D Tilemap Editor...` and the second-row tool-launcher toolbar.
- The toolbar uses a dedicated grid icon and an explicit `Open 2D Tilemap
  Editor` accessibility label. A text fallback remains visible when the SVG
  asset cannot be loaded.
- Both entry points open the same stable untitled Tilemap resource key in one
  owned, modeless Tilemap tool window. Repeated activation focuses that live
  window rather than creating nested windows or duplicate documents.
  Asset-backed Tilemaps continue to open per resource through the Content
  Browser or `File -> New Tilemap`; test/headless hosts without an AYDevice
  child-window manager retain the Center-dock fallback.

Verification after this slice: `AYEditor_UnitTests` and `AYEditorShell_Demo`
link, the 993-check `AYEditor_Shell` suite passes, the shell JSON parses, and
the toolbar/menu regression opens then refocuses one live Tilemap workspace.

### 10.11 Tilemap compact controls and modal ownership

- Tile-sheet import is a Tilemap-window modal owned by the Tilemap view's
  explicit `UIManager`. Opening is scoped to that manager, the logical dialog size is
  clamped to the current client area, and its position is recomputed from the
  live logical client size on every open. This keeps the scrim, focus trap,
  hit-testing and dialog on the same window and prevents a previously active
  secondary window from receiving only part of the modal interaction.
- The Tilemap paint toolbar is icon-first: Pencil, Eraser, Fill, Rectangle,
  Stamp, Shadow, Grid, Collision overlay, Shadow overlay and Frame are compact
  square SVG buttons. Active tool and enabled overlays use accent icon color;
  the permanent canvas badge remains the authoritative textual mode signal.
  Every icon button keeps an accessibility label, hover tooltip and a short
  text fallback if its SVG cannot be parsed.
- Render-layer add/remove/reorder/visibility/rename actions use the same square
  icon-button contract. The layer-name field remains textual. This change is a
  local density and clipping correction, not a wider Tilemap visual redesign.
- Semantic icon mapping remains AYEditor-owned while SVG parsing/rendering stays
  in AYUI. Missing Tabler outline icons are vendored from the shared AssetRepo
  into immutable `EngineAssets/Icons/Tabler/outline`; production code never
  depends on the developer-only AssetRepo path.

### 10.12 Hosted-canvas shortcut ownership

- A hosted canvas can own keyboard input without assigning focus to a regular
  AYUI widget. When AYUI focus is empty, the Dock host therefore preserves the
  command target of its input-focused document; a real focused widget inside or
  outside a hosted view still takes precedence. This keeps Tilemap history
  commands attached to the canvas selected by pointer/tool input without
  stealing commands from Inspector, Content Browser or text fields. A press in
  the hosted input surface explicitly ends stale widget focus before acquiring
  this lease; normal AYUI dispatch may immediately focus a child field when the
  field itself was clicked.
- Editor shortcuts with modifiers are resolved before view-local plain tool
  keys. Tilemap P/E/F/R/S/H and navigation keys remain local commands, while
  Ctrl+Z, Ctrl+Y and Ctrl+Shift+Z are handled by the active document command
  router. Ctrl+Shift+Z is a default redo alias only while Redo keeps its default
  binding; a user-defined Redo binding replaces both default forms.

### 10.13 Progressive Tilemap shadow controls

- The persisted shadow plane remains one 4-bit quarter-cell mask per map cell.
  Keeping the complete mask space preserves RPG-style imported data and future
  automatic shadow generation without making all sixteen combinations primary
  authoring modes.
- The normal toolbar exposes only `Full Shadow` and `Clear Shadow`; choosing
  either also activates the Shadow tool, and `H` always restores the predictable
  full-cell brush. The canvas badge states `FULL`, `CLEAR`, or `CUSTOM` instead
  of exposing the mask's implementation number.
- Arbitrary masks move into a compact blocking dialog containing a spatial 2 x 2
  quadrant editor. Each quadrant is toggled where it appears in the cell, and
  `Use Brush` applies that composed mask and activates Shadow. The editor does
  not list sixteen textual permutations such as diagonals or `Except` variants.
- This is an interaction simplification only. Painting still replaces the
  complete cell mask in one history gesture, the global RGBA shadow color is
  unchanged, and runtime dynamic lights remain a separate rendering feature.

### 10.14 Tilemap spatial orientation and modal surfaces

- Every blocking Tilemap dialog owns an explicitly styled opaque content
  surface. The dimmer lowers the rest of the editor, while the dialog plate
  itself must fully occlude the workspace below it; a layout-only transparent
  `VBox` is not a valid modal root.
- Atlas pixels and source rows use the conventional top-left origin with Y
  increasing downward. Tilemap cells use world-space Y increasing upward.
  Creating a Stamp from a source rectangle therefore negates its row delta:
  the source rectangle's top-left cell remains the placement anchor and rows
  below it remain below it after placement instead of being vertically
  reordered.
- `Frame Map` means fit the complete map bounds into the canvas and center the
  camera; `Fit Sheet` performs the equivalent reset for the Source Sheet.
  Tooltips and labels use those explicit terms instead of the ambiguous bare
  `Frame` name.

### 10.15 Dedicated and resilient Tilemap workspace

- Tilemap and UI Designer share the same host foundations: both are owned,
  modeless top-level windows created by `EditorChildWindowManager`, each has a
  private `UIManager`, and both keep document/dirty/save/undo ownership in the
  common `EditorWorkspace`. They intentionally do not share view construction:
  UI Designer loads JSON chrome around `EditorUiLayoutController`, while
  Tilemap mounts its registered `IEditorView` into a private `DockArea` through
  `EditorDockViewHost` so several Tilemap documents can remain tabbed together.
- The Tilemap body is a three-pane authoring surface. Source Sheet/assets and
  Render Layers/Inspector are vertical `ScrollView`s; two horizontal
  `SplitterHandle`s let the user resize them around the canvas. The center
  canvas keeps the flexible remainder and remains the primary work area.
- Automatic compression caused by a narrow native window is transient. AYUI
  box layout may reduce fixed side panes to protect the fill pane, but it no
  longer overwrites their requested widths; maximizing or growing the window
  therefore restores the prior authored/splitter width. An explicit splitter
  drag still updates the requested width and remains persistent for that live
  workspace.
- The paint command row uses AYUI `ToolBar` horizontal overflow rather than
  clipping commands. Side-pane vertical scrolling and toolbar horizontal
  overflow are independent, so a small window never makes commands or lower
  Inspector fields permanently unreachable.
- The dedicated Tilemap host is borderless and uses an outer AYUI `DockCard`
  as editor-painted window chrome. Its title strip owns minimize,
  maximize/restore and close; the private `DockArea` lives inside that frame.
  Window movement is armed only when the drag source is this outer frame.
  Dragging an inner Tilemap document tab therefore remains a Dock operation
  and can never move the native window.
- The outer-frame/private-`DockArea` split is also the compatibility boundary
  for later tool-window joining: another independent editor window can expose
  its DockArea as a cross-window drop target without confusing a document tab
  with the OS-window movement handle. Cross-window transfer is deliberately a
  later phase; this change establishes the host shape and fixes local input.
- Authoring texture handles are renderer-local. `EditorDockViewHost` retains
  the source BGRA pixels from the shared authoring-image cache, uploads one
  path-keyed copy into a child GDI backend, and releases those copies before
  child UI teardown. Import preview, Source Sheet selection, and painted map
  tiles therefore never pass a primary-window GPU handle to another backend.

### 10.16 Project UI Flow authoring（阶段四）

- `.uiflow.json` 是项目级 UI 编排资产，不是 Scene 内的 Widget 树。Content Browser 会识别该
  后缀，`File -> New UI Flow` 创建可立即通过 validator 的 Boot 模板，双击资产或
  `Tools -> UI Flow Editor...` 在独立 AYDevice 非模态子窗中打开；Tools 入口优先解析项目
  描述符声明的 `ui.flow`，不存在时才创建未命名文档。
- `EditorUiFlowDocument` 直接持有 AYUI `UIFlowDocument`，提供统一 dirty/revision、100 步
  snapshot undo/redo、原子保存/恢复副本、引用安全重命名和被引用对象删除保护。Layer、Slot、
  Screen、Context、Entry、Signal、Action、Region、State、Transition 与 Graph 都使用生产
  serializer/validator，没有编辑器私有 wire format。
- 三栏工具窗左侧是完整模型 Outline，中间画布绘制并行 Region/State、Transition 连线、Graph
  node/link、活动 State 和 mounted Screen，右侧 Inspector 编辑 Layer 输入/容量、Context
  `slot=screen`/`!slot` 赋值、Screen Scope/动画以及 Transition guard/action graph。所有修改后
  即时运行同一个 validator，诊断列表显示稳定 JSON path 和消息。
- `EditorUiFlowPreview` 的逻辑预览直接实例化生产 `AYApplicationUI::UIFlowRuntime`。模拟 Signal
  使用声明的默认 payload，Mock Action 使用声明的默认 inputs；Context、Slot、Scope、并行 Region
  和 graph request 因而不会在编辑器内形成第二套状态机。当前 Screen Host 记录挂载关系和 trace，
  真实 Widget 像素加载/动画交接属于阶段五生产视觉集成。
- 阶段四测试锁定跨引用重命名、删除保护、历史往返、Context 编排、资产创建/分类、磁盘往返，
  以及 Signal 驱动生产 Runtime 后活动 State 和 mounted Screen 的变化。

### 10.17 UI Flow production preview（阶段五）

- Flow 工具窗中间区域拆分为编排图和真实 Screen 预览。预览使用项目描述符的 `assetRoot`，通过生产
  `UIManagerFlowScreenHost` 加载同一 `*.ui.json`、动画库、Layer 顺序和输入策略，不维护编辑器专用
  Widget 模型。
- 真实预览挂在编辑器持有的 `CompoundWidget` 视口中；渲染和 hit-test 共用 client clip，设计尺寸大于
  面板时不会泄漏到图画布、Outline 或 Inspector。逻辑记录 Host 仍并行保留 mounted Screen 和 trace，
  因此视觉预览不可用时错误也能稳定显示。
- Controller 每帧同步预览视口大小并推进 Screen enter/exit Timeline；系统 reduced-motion 设置沿生产
  Timeline 生效。关闭工具窗时先停止 Flow、销毁生产 Host，再释放视口，确保无外部 Widget 悬挂。

### 10.18 UI Flow asset closure（阶段六）

- Flow Editor 的 Diagnostics 合并模型 validator 与生产资产 audit。每次文档 revision 只审计一次，
  可在未触发对应 Context/State 前发现 Screen 布局缺失、路径越界、Loader 拒绝、动画片段缺失以及
  动画轨道 Widget ID 失效；标题同时显示去重后的 Layout 依赖数量。
- `EditorProjectRuntimeValidator` 解析项目 `ui.flow`，校验 descriptor 的 Entry 和 World Context 引用，
  并把每个 `Flow -> layout -> Screens` 关系放入 `uiFlowDependencies`。独立验证器会打印这份稳定闭包，
  后续 packager 无需扫描编辑器视图或实际运行状态。
- Headless profile 只确认 Flow schema、引用、根目录约束和文件存在；Full Client profile 才创建真实
  Widget 树并解析动画。两者共用同一 portable dependency key，避免验证结果与部署输入分叉。

### 10.19 UI Flow interaction authoring（阶段七）

- Screen Inspector 增加可往返的 `handler=signal; ...` 映射。Signal 重命名会原子更新映射，被 Screen
  引用的 Signal 不允许删除；模型 validator 检查 Signal 存在及 command-event 所需默认 payload，
  Full Client 资产审计再确认 handler 确实存在于 Screen 布局。
- 真实预览把生产 Runtime emitter 透传到内部 `UIManagerFlowScreenHost`。用户点击带声明式事件的真实
  Button 会立即进入同一 Signal 队列、驱动 Region/State/Context 和 Screen 重挂，而不是由编辑器模拟。
- Graph 工具栏的节点类型、输出端和输入端改为 ComboBox。内建通用节点词汇和
  `EditorUiFlowExtensionConfig::graphNodeTypes` 共用 AYUI `UIFlowGraphNodeRegistry`；宿主可覆盖同名定义，
  Inspector diagnostics 对节点类型、Pin 方向/类型和属性做严格检查，但执行语义仍由游戏/插件注册。

### 10.20 UI Flow executable graph preview（阶段八）

- `EditorUiFlowPreview` 不再只记录整张 Graph request，而是把 Controller 的节点词汇表注册到生产
  `AYApplicationUI::UIFlowGraphExecutor`。预览节点使用明确的 mock host handler；
  `flow.invokeAction`/`host.action` 会进入同一 Runtime Action registry，`flow.emitSignal` 会进入同一
  Signal queue，节点执行、失败和状态迁移因而共享生产调度路径。
- Graph 画布从固定卡片升级为按 registry 展示 input/output Pin。Execution 与 bool/integer/number/
  string/entity/asset value 使用不同颜色，link 以连续贝塞尔 Path 绘制；连接 UI 选定 source 后只保留
  `areUIFlowGraphPinsCompatible()` 接受且不属于自身的 target，避免先写入无效连接再依赖诊断纠错。
- 生产执行器测试覆盖 value propagation、异步 continuation、completion callback、cycle reject、
  interrupt 失效和 Runtime pipeline；编辑器 contract 测试覆盖真实节点 trace、类型过滤和 Path 绘制。

### 10.21 UI Flow graph debugger（阶段十）

- 断点由 `graphId + nodeId` 定位，执行器在调用 host handler 前暂停；暂停快照因此包含最终解析的
  property/default/value-link 输入，同时保证未产生节点副作用。无断点和手动 pause 时该路径完全惰性。
- `Pause Next` 在下一可运行节点停下；`Step` 只执行当前节点，并在下一可运行节点前再次暂停；
  `Continue` 跳过当前断点一次，随后恢复普通断点行为。异步节点单步后会在 continuation 完成并产生下一
  ready 节点时暂停，不伪造同步完成。
- Runtime Trace 保存 node started inputs 与 completed outputs；编辑器底部调试栏显示当前暂停原因和输入，
  Graph 画布以琥珀色三像素边框标记暂停节点。调试控制复用生产 `UIFlowGraphExecutor`，不建立第二套执行器。

### 10.22 UI Flow integration and visual contract（阶段十一）

- 独立工具窗的生产链接路径由 `AYEditorShell_Demo` 和完整 `AYEditor_UnitTests` 共同覆盖；项目内容验证器
  继续以 headless/full-client 两种 profile 验证 World、Layout 与 Flow 资产闭包。
- Preview 以单调 `presentationRevision` 暴露 mounted Screens、Region state、Runtime Trace 和调试暂停
  的显示变化。Controller 每帧只做 O(1) revision 比较，有变化才更新列表和 Graph 画布，异步 continuation
  不再出现运行时已暂停但编辑器仍显示 Running 的延迟，同时避免对不断增长的 Trace 做逐帧扫描。
- Flow chrome 建立 1280×720、1440×860、1920×1080 三档确定性视觉契约：主栏与两条 Graph 工具行必须
  完整落在各自容器内，Canvas/真实 Screen Preview 必须保有正面积，所有操作文字必须进入渲染提交，
  裁剪栈必须成对闭合。1280 档的 Graph 编排控件采用紧凑宽度，避免 Pin 选择器越界覆盖 Inspector。
- Flow 工具窗的静态 chrome 与对象类型、Inspector 字段、诊断类别、预览/调试状态、画布提示和操作状态
  统一使用 AYEditor 宿主持有的本地化 resolver；语言切换只原位刷新显示文本，保持 Outline 选择、预览和
  调试状态。ID、资产路径、Scope 及运行时/插件原始诊断仍保持 wire 文本，避免本地化改变资产语义。

#### UI Flow authoring quality gate

- Inspector 的 Enter、失焦、保存、预览及选择切换统一先提交当前合法输入；提交失败保留编辑缓冲并阻止
  破坏性导航。工具栏打开操作不得覆盖 dirty 文档。正在运行的 Preview 记录源文档 revision，文档变化后
  进入明确的过期状态并禁用 Signal、Action 与调试命令，直至用户重新启动预览。
- 枚举和数值不得依赖静默 fallback。Scope、Layer Input Policy、32 位整数及 Transition Interrupt Policy
  在进入命令历史前严格校验；Transition 暴露 `queue`、`cancelPrevious`、`reversePrevious`、
  `ignoreIfRunning`、`coalesce` 全集，不能再用单个 Coalesce 布尔值压缩生产 wire contract。
- Add 列表只展示当前依赖已满足的对象种类；所有选择相关命令依据 Document/Preview 状态禁用，避免把
  “先点击再读错误”当作正常发现机制。后续任务导向改版继续以该 gate 为底线，不改变 AYUI wire model、
  AYApplication runtime 或生产 Preview 路径。

### 10.23 UI Designer project workflow

- `EditorUiDesignerWorkflow` 扫描项目 asset root 下的 `.ui.json` 与 `.uiflow.json`，建立
  `Screen -> Layout` 和 `Layout -> Screens` 双向索引。它是 editor-only 项目服务，不进入 AYUI
  runtime，也不复制 Flow 执行语义。
- Layout Designer 的 Workflow 菜单可定位第一个 owning Screen，并可将布局中声明的 Widget handler
  补全为 Screen event 与 Flow Signal；已有同名 Signal 优先复用，新 Signal 使用稳定的
  `screen.handler` 名字。Flow Editor 的 **Open Layout** 对选中 Screen 执行反向导航。
- 项目重构只处理已知 schema 字段：Layout asset、Flow signal、Widget handler、Widget ID 及动画轨道
  target。计划阶段记录每个文件的原始内容；提交前全部复核，随后用同目录 temporary/backup 文件安装，
  任一失败则恢复所有已安装文件。字符串相同但语义无关的普通文本不会被替换。
- 文档所有权保持在 Workspace：跳转优先聚焦已有文档；补全或重构若会覆盖一个已打开的 dirty Flow，
  必须拒绝并要求用户先保存或放弃。磁盘事务完成后只重载未修改的打开文档。

---

## 11. Decisions log

| Date | Decision |
|------|----------|
| 2026-09-21 | Skeleton Editor 增加骨树搜索及左右/必需/缺失/重复诊断；`.ayrig kind=retarget` 最小作者结构保存目标骨架、输出模式和 platform，并按该范围隔离 dry-run/回执身份；真实重定向求解器交付前预检阻止 BakeToTarget。 |
| 2026-09-19 | 骨骼映射作者资源迁移到规范 `.ayrig` Rig Profile；Content Browser 与 Skeleton Editor 保留 `.aysmap` 只读兼容入口，旧资源打开后标记为待迁移，保存生成 `.ayrig` 且保留原文件。 |
| 2026-09-19 | 骨骼作者工作区第一版采用 `AYAnimationEditorCore` 独立核心 + AYEditor 文档/视图薄适配；源 `.ayskel` 只读，映射保存为绑定的 `.ayrig`，Content Browser 与工作区分别展示适配和烘焙状态，动画预览复用公共时间轴能力。 |
| 2026-09-18 | **计划，待实施**：骨骼作者工作区采用源保留 + 映射/profile 资源绑定 + 手工/模板非破坏式编辑；Content Browser/Inspector 使用独立适配和烘焙标志，公共文档历史、统一转换核心和发布门禁；SKA 优先级在跨模块设计维护，见 §4.3.skeletal。 |
| 2026-07-03 | AYEditor is a separate module; not embedded in AYUI |
| 2026-07-03 | Chrome via existing `UILayoutLoader` JSON; ImGui samples in Extension doc are non-normative |
| 2026-07-03 | E0–E3 without Inspector/scene I/O; metadata + serializer deferred to E4+ |
| 2026-07-03 | Fastest visual path: `EditorShell_Demo` before `EditorApp` |
| 2026-07-03 | **E2 split:** E2-interim = child HWND + GDI host (Windows-safe); E2-composite = U2 single-window target |
| 2026-07-03 | Viewport native ownership migrates to **AYDevice** at E3; interim Win32 encapsulated in demo + `EditorPlayRuntime` |
| 2026-07-03 | AYDevice **WindowManager skeleton** recommended before E3; full input/XR not required for editor shell |
| 2026-08-06 | v0.3 PR-4 Editor 持 _editScene + transport bar UX（决策 1a/2a/3a/4a/5a/6a/7a） |
| 2026-08-30 | Render Settings 保留每个可见效果的验证开关；FreeCam 视口滚轮使用沿视线 dolly |
| 2026-08-30 | Render Settings 新增 LUT Color Grading 开关、四种预设与强度实时验证控件 |
| 2026-08-31 | Shell 图标改用 AYUI 原生 SVG path；AYEditor 只保留语义映射、资产查找和文字降级 |
| 2026-08-31 | 主窗口改为无原生标题栏的 AYUI 自绘 Chrome；保留拖动、双击最大化和边缘缩放 |
| 2026-08-31 | 引入 `aliyat-editor-dark` + `Compact`，主题、密度和 UI Scale 分离持久化 |
| 2026-08-31 | Transform 编辑使用选中即出现的 Universal Gizmo：平移轴/平面、旋转圆环、轴/统一缩放同时可用；RMB 独占 FreeCam，Ctrl 只作为编辑快捷键修饰符 |
| 2026-08-31 | Universal Gizmo 对朝向相机的轴和侧视退化的平面/圆环使用 0.25/0.32 投影滞回门限；暗色禁用并从 CPU 拾取中排除 |
| 2026-08-31 | Universal Gizmo 调整为短缩放/长平移比例；热区覆盖全部可见几何并保留外扩容差；hover/drag 保持普通箭头，仅由 handle 高亮反馈 |
| 2026-08-31 | Content Browser P0 采用 AYEditor 轻量项目索引：Assets/Imported 双根、搜索过滤、资源 Inspector、导入与 Mesh 拖入视口；加载继续复用 AYResource |
| 2026-08-31 | Content Browser 右侧切换到 AYUI 通用 TileView；AYEditor Presenter 独占类型简称、类别色和引擎原生角标语义，AYUI 只提供 InfoStrip/CornerMarker 展示契约 |
| 2026-08-31 | `.phoskia` / `.logia` 双击打开唯一 Center DSL DockCard；AYEditor 保留源码、保存和编译语义，AYUI 仅提供 Dock/TextArea 通用控件 |
| 2026-09-01 | 2D authoring 先落 UI-independent 模型：`Editor2DViewportModel` 负责正交视口换算/网格吸附，`EditorTilemapDocument` 负责 paint/fill/collision/animation 与 `.aytilemap.json` 保存加载；AYUI 后续只绑定通用控件。 |
| 2026-09-01 | Dock 单页签保持固定宽度且仅活动页显示矢量关闭图标；浮动 DockCard 使用 AYUI 矢量窗口控件，关闭先回主 DockArea 执行持久面板/DSL 文档策略，拖回时迁移同一 live card。 |
| 2026-09-01 | AYEditor 改用 install 产品树：根启动器、`AYRuntime` 运行库、只读 `EngineAssets`、可写 `UserAssets`/`logs` 分离；`EngineAssets/Icons` 是跨模块共享图标根，不归属任何具体模块。 |
| 2026-09-01 | 通用 `EditorDockViewHost` 落地并成为 DSL Shell 的唯一展示路径；DSL widget/command 实现在扩展 View 内，Session 只提供工程根、状态文本、重绘与原生关闭选择。 |
| 2026-09-01 | UI Layout Editor 迁入通用 Center DockCard Host；限定根绑定隔离 chrome id，画布输入通过 `IEditorViewInputTarget` 接入，独立 `AYUI_LayoutEditor` 保留为回归宿主。 |
| 2026-09-01 | Dock 模板在 self-heal 后显式恢复 mid/Center fill，防止 Bottom 临时折叠把默认中心文档区压缩为 120px。 |
| 2026-09-02 | UI Layout Editor 从 Center DockCard 迁为主窗口 owner 持有的 modeless AYDevice 工具窗；`EditorUiLayoutController` 复用同一 Layout Session，文档仍归 Workspace 管理。Chrome 重构为现代六区布局，Scene Center 不再承载 UI authoring。 |
| 2026-09-02 | Designer 命令收敛到 File/Edit 与 Inspector 上下文；Widget Library 改为可拖放 SVG 图标列表；选择装饰改为透明像素对齐单线，Inspector 切换在同一输入事务内完成布局。 |
| 2026-09-08 | Designer 工具箱扩展到 Image、集合/树、Tab、Grid/Scroll 和 Modal；加入原生图片选择、GDI 实图预览，以及可往返的 controller/event 交互契约。 |
| 2026-09-08 | Designer 保存/重开验证升级为对象级结构检查；生产 Loader 对称重建 Tab/Modal/Dialog payload 与深层 ID，并修复未挂载 Tab page 的重复 ID。 |
| 2026-09-09 | Tilemap 作者保存与运行时烘焙结果分离；共享 `AY2DEditorCore` 的 Tilemap 文档页由摘要占位升级为可绘制、可取样、可缩放平移、可管理图层和 Tile 属性的 AYEditor 工作区。 |
| 2026-09-09 | Tilemap 主编辑器图集工作流统一走宿主 authoring-image cache；原图选砖、模态切片和画布纹理共享 AY2D 的切片规划；随后 Timeline 公共编辑契约加入，AYEditor Source ABI 最终升至 4。 |
| 2026-09-09 | Tilemap 从仅资源触发的文档扩展补齐为主壳层工具入口：Tools 菜单和第二行网格图标打开同一未命名 Center Dock 工作区，重复点击只聚焦现有页。 |
| 2026-09-10 | Tilemap 改为 `EditorChildWindowManager` 管理的独立非模态工具窗；内部复用 `EditorDockViewHost` 文档页签，并以左右 Splitter、侧栏 ScrollView、工具栏横向溢出和可逆窄窗压缩保证缩放后布局恢复。 |
| 2026-09-10 | Tilemap 独立窗改用无原生标题栏的 AYUI 自绘外框与最小化/最大化/关闭按钮；窗口移动只接受外框标题拖拽，内部文档标签继续由 Dock 系统处理，并以私有 DockArea 作为后续跨窗口合并边界。 |
| 2026-09-10 | Tilemap 子窗口不再直接复用主渲染器的 authoring texture 句柄；`EditorDockViewHost` 依据共享 BGRA 原图为子窗口 GDI 后端创建并缓存本地副本，统一修复导入预览、原图选砖和画布 Tile 空白。 |
| 2026-09-10 | 2D 制作闭环继续使用通用 Scene/World：模板和创建命令只装配 Sprite、Tilemap、OrthoCamera 与 Transform；Scene View 以组件平面边界进行拾取并向 Renderer 提交世界空间选择轮廓，2D Universal Gizmo 固定屏幕尺寸且复用统一 Undo/Redo 命令栈；AYEditor Source ABI 升至 7。 |
| 2026-09-11 | UI Flow 阶段四采用独立 AYDevice 工具窗；创作、诊断与逻辑预览复用同一 `UIFlowDocument`/serializer/validator/`UIFlowRuntime`，编辑器只以 mock Screen Host/Action 显示状态，不另建运行格式或状态机。 |
| 2026-09-11 | UI Flow 阶段五将生产 `UIManagerFlowScreenHost` 挂入独立裁剪预览视口，并以项目 `assetRoot` 加载真实 Screen；异步 Graph、中断策略、动画交接、reload/replay 与 reduced-motion 共用运行时契约。 |
| 2026-09-11 | UI Flow 阶段六以 `validateUIFlowAssets` 建立 Screen 布局/动画的生产资产闭包；Flow Editor、项目验证器和后续 packager 共用去重依赖与反向 Screen 引用，不再等到状态实际挂载后才暴露缺失资产。 |
| 2026-09-11 | UI Flow 阶段七以 Screen `handler -> Signal` 映射闭合真实 Widget 交互；Graph 节点与 Pin 选择统一由可扩展 registry 提供，基础 wire format 保持开放、编辑器按宿主词汇表执行严格校验。 |
| 2026-09-11 | UI Flow 阶段八以 AYApplication `UIFlowGraphExecutor` 执行宿主节点、传播类型化值并支持异步续跑/中断；Flow Editor 预览复用该执行器，Graph 画布按同一 registry 绘制彩色 Pin、贝塞尔 link 并过滤不兼容目标。 |
| 2026-09-12 | UI Flow 阶段九升级为依赖感知执行：Flow Editor 预览逐帧驱动 Graph timeout，并在 Runtime/Document 销毁前先取消异步 continuation，保持与生产执行器的数据依赖、并行和 Join 语义一致。 |
| 2026-09-12 | UI Flow 阶段十加入副作用前断点、Pause Next、Step/Continue 和输入/输出快照；Flow Editor 仅呈现生产执行器调试状态，并在 Graph 画布高亮暂停节点。 |
| 2026-09-12 | UI Flow 阶段十一以 presentation revision 同步异步预览显示，并把三档窗口尺寸、操作文字、双预览区和裁剪栈固化为自动化视觉契约。 |
| 2026-09-12 | UI Designer 工作流建立 Layout/Flow 双向项目索引、一键导航、handler-to-Signal 补全及 schema-aware 多文件重命名事务；dirty 打开文档禁止被磁盘操作覆盖。 |
| 2026-09-12 | Layout Designer 增加可视化 Project Safe Rename；AYUI 只承载中性预览/提交表单，AYEditor 负责引用类型映射、原子磁盘事务与干净文档重载。 |
| 2026-09-12 | UI Designer 项目索引改为带文件指纹/revision 的长生命周期服务；仅 authoring 文件变化时重解析，并自动同步干净的 Workspace 文档，dirty 冲突只告警不覆盖。 |
| 2026-09-14 | B-8 公共命令历史抽为无 AYUI/World/Renderer 依赖的 `AYEditorCommandCore`；统一原子 execute/undo 失败契约、事务取消与无副作用丢弃、256 条默认容量、保存游标和过期 owner 熔断。全局路由保留在 AYEditor，各文档各自持有一份历史；公共接口变化使 AYEditor Source ABI 升至 15。 |
| 2026-09-14 | B-8 生产迁移完成：Scene、GameFlow、UIFlow 与 UI Layout 均改用文档级 `EditorCommandHistory`，删除 `EditorCommandStack`、`LayoutCommandStack` 及 Flow 私有 undo/redo 容器；保存游标统一驱动 dirty，活动文档统一接入命令路由。公共布局变化使 AYEditor Source ABI 升至 18、AYUI Source ABI 升至 128。 |
| 2026-09-14 | B-8 收口：Timeline 的关键帧/clip 编辑迁入公共历史，Scene 删除重复 dirty 标志；主菜单 Save/Save As/Undo/Redo 每帧读取活动文档命令目标并统一执行。Tilemap 保留 AY2D 领域模型内带内存预算的增量历史，文本控件保留局部输入历史，两者均不构成 Workspace 文档第二套命令栈。AYEditor Source ABI 升至 19。 |

---

## 12. References

- [AYUI/design.md §13 Editor chrome](../AYUI/design.md#13-editor-chrome)
- [AYExtension/design.md §3 Editor Integration](../AYExtension/design.md)
- [AYApplication/design.md §3 BuildType / subsystems](../AYApplication/design.md)
- [AYGameLoop/design.md](../AYGameLoop/design.md) — `tickOnce`, Unscaled editor time
- [AYDevice/design.md §3 WindowManager](../AYDevice/design.md#3-窗口管理-windowmanager) — native handle owner for E3+
- [AYSerializer/README.md](../../AYFoundation/AYSerializer/README.md) — future scene/Inspector I/O
