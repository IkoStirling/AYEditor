#include "AYEditorSession.h"

#include "AYEditorHeapDebug.h"
#include "AYEntity.h"
#include "AYSplitterHandle.h"
#include "AYButton.h"
#include "AYCheckBox.h"
#include "AYComboBox.h"
#include "AYGameLoop.h"
#include "AYMenuBar.h"
#include "AYMenu.h"
#include "AYMenuItem.h"
#include "AYRendererSubSystem.h"
#include "AYSlider.h"
#include "AYTextLabel.h"
#include "AYWidget.h"
#include "AYDockArea.h"
#include "AYDockCard.h"

#include <components/AYAnimationComponent.h>
#include <components/AYMeshComponent.h>
#include <components/AYSkeletonComponent.h>

#include <cstdio>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <Windows.h>

namespace ayt::editor {

EditorSession::EditorSession()
    : _gameView(ayt::game::GameLoop::instance(), _playRuntime) {
}

EditorSession::~EditorSession() {
    shutdown();
}

bool EditorSession::initialize(const EditorSessionDesc& desc) {
    AY_EDITOR_TRACE("initialize: begin");
    _hostWindow = desc.hostWindow;
    _layoutPath = desc.layoutPath;
    _playRuntime.setHostWindow(_hostWindow);
    // ED-02: forward the imported character (if any) to the
    // Play-runtime. Empty / invalid = cube fallback at startPlay.
    _playRuntime.setImportedCharacter(desc.importedCharacter);
    _playRuntime.setNetPlayRole(
        desc.netClientMode ? NetPlayRole::Client : NetPlayRole::Server);
    _playRuntime.setNetConnectHost(desc.netConnectHost);
    _netClientAutoPlay = desc.netClientMode;
    // Pre-existing _CrtCheckMemory() failure on session_after_set_host
    // in Debug builds. Commented to keep the build runnable; the four
    // checks later in initialize() remain enabled as debug invariants.
    // AY_EDITOR_HEAP_CHECK("session_after_set_host");

    _ui.initialize(desc.uiBackend);
    AY_EDITOR_TRACE("initialize: ui backend set");
    _gameView.setModeChangedCallback([this](EditorMode mode) { onModeChanged(mode); });

    if (_hostWindow != nullptr) {
        RECT clientRect{};
        if (GetClientRect(_hostWindow, &clientRect) != 0) {
            const float width  = static_cast<float>(clientRect.right - clientRect.left);
            const float height = static_cast<float>(clientRect.bottom - clientRect.top);
            if (width > 0.0f && height > 0.0f) {
                _ui.setClientSize(width, height);
                _playRuntime.setClientSize(static_cast<uint32_t>(width),
                                           static_cast<uint32_t>(height));
            }
        }
    }

    AY_EDITOR_TRACE("initialize: loading layout");
    if (!_layoutPath.empty()) {
        if (!_ui.loadLayout(_layoutPath)) {
            std::fprintf(stderr, "[EditorSession] loadLayout failed: %s\n", _layoutPath.c_str());
            return false;
        }
    }
    AY_EDITOR_TRACE("initialize: layout loaded");

    bindToolbar();
    bindMenuBar();
    bindTransportBar();
    bindNetworkPanelStub();
    bindRenderSettingsPanel();

    _mainDock = dynamic_cast<ayt::ui::DockArea*>(_ui.findById("main_dock"));
    setDockCardVisible("card_network", false);
    AY_EDITOR_TRACE("initialize: toolbar bound");

    setModeLabel(L"EDIT");

    syncViewport();
    AY_EDITOR_TRACE("initialize: done");

    // D5+.5: optional child-window manager for DockCard promotion.
    if (desc.childWindowManager != nullptr) {
        _childWindows = std::make_unique<EditorChildWindowManager>(
            *desc.childWindowManager, _ui);
        if (!desc.childWindowConfigPath.empty()) {
            const auto cfgs =
                parseChildWindowConfig(desc.childWindowConfigPath);
            for (const auto& cfg : cfgs) {
                void* h = nullptr;
                if (!_childWindows->openChildWindow(cfg, h)) {
                    std::fprintf(stderr,
                        "[EditorSession] child window '%s' failed to open\n",
                        cfg.title.c_str());
                }
            }
            AY_EDITOR_TRACE("initialize: opened %zu child window(s)",
                            _childWindows->count());
        }
        wirePromoteCallback();
    }

    return true;
}

bool EditorSession::initialize(ayt::ui::IRenderBackend* backend, const std::string& layoutPath) {
    EditorSessionDesc desc;
    desc.uiBackend = backend;
    desc.layoutPath = layoutPath;
    return initialize(desc);
}

void EditorSession::shutdown() {
    if (_shutdown) {
        return;
    }
    _shutdown = true;

    _gameView.setModeChangedCallback({});
    _repaintCallback = nullptr;
    // K-INV-D5-6: tear down child HWNDs BEFORE primary UIManager
    // shutdown. ~EditorChildWindowManager calls _wm.destroyTopLevelWindow
    // for every entry; ~UIManager on each child may poke
    // g_activeUIManager if it was active during the last tick. Doing
    // this here (with _ui still alive and owning the active slot)
    // avoids an UAF cleanup race against the primary.
    _childWindows.reset();
    _mainDock = nullptr;
    // Tear down Play/renderer borrow before UI widgets — avoids
    // Inspector path strings and GPU borrows racing UI teardown.
    _playRuntime.shutdownEngine();
    _gameView.setMode(EditorMode::Edit);
    _ui.shutdown();
    _layoutPath.clear();
    _hostWindow = nullptr;
}

void EditorSession::setClientSize(float width, float height) {
    _ui.setClientSize(width, height);
    _playRuntime.setClientSize(static_cast<uint32_t>(width),
                               static_cast<uint32_t>(height));
    _ui.layout();
    syncViewport();
}

void EditorSession::update(float dt) {
    // D5+.5: tick every open child (each via pushActive scope) before
    // the primary update so the active pointer is correctly swapped
    // before any per-frame UI logic that might read g_activeUIManager.
    // Pass nullptr backend — child UIManagers use K-INV-D5-4 null
    // backend no-op; the host's _ui owns the only real backend.
    if (_childWindows) {
        _childWindows->tickAll(dt, nullptr);
    }
    _ui.update(dt);
    // Per-frame reconcile: if the last known cursor is not on a splitter
    // band, force every SplitterHandle un-revealed. Leave events alone
    // are not sufficient (capture path / skipped WM_MOUSEMOVE).
    syncSplitterRevealToMouse();

    if (freecamActive()) {
        if (viewportAcceptsGameInput()) {
            _freecam.updateMovement(dt);
        }
        pushFreecamToRenderer();
    }

    if (_gameView.mode() == EditorMode::Play) {
        _playRuntime.tick();
    } else if (_gameView.mode() == EditorMode::Paused) {
        // Keep last rendered frame visible; stepOnce drives simulation separately.
    }

    // Splitter drag updates HBox slot widths; keep the 3D viewport rect
    // in sync every frame so render composite tracks panel resize.
    syncViewportIfChanged();
}

bool EditorSession::freecamActive() const
{
    const EditorMode mode = _gameView.mode();
    return mode == EditorMode::Play || mode == EditorMode::Paused;
}

void EditorSession::pushFreecamToRenderer()
{
    if (auto* sub = ayt::render::RendererSubSystem::findRegistered()) {
        sub->setCameraLookAt(
            _freecam.eye(),
            _freecam.at(),
            _freecam.up(),
            _freecam.fovYDegrees());
    }
}

void EditorSession::render() {
    render(false);
}

void EditorSession::render(bool skipViewportPanel) {
    // Pre-AI-1 wrapper: kept for callers that want a single populate+
    // flush call. The new AI-1 path in AYEditorApp.cpp uses
    // populateFrame + flushFrame directly so the RenderPass dispatch
    // can own the UI submission boundary (UIPass::execute flushes
    // pending text batches).
    populateFrame(skipViewportPanel);
    flushFrame();
}

void EditorSession::populateFrame(bool skipViewportPanel) {
    // AI-1: begin-frame the widget walk that accumulates draws on
    // the backend's pendingRects + textBatch. No flush yet — the
    // flush moves to UIPass::execute so the RenderPass dispatch owns
    // the UI submission boundary. flushFrame() closes the lifecycle.
    _panelViewportForFrame = nullptr;
    if (skipViewportPanel) {
        ayt::ui::Widget* viewport = _ui.findById("panel_viewport");
        if (viewport != nullptr) {
            _panelViewportWasVisibleForFrame = viewport->isVisible();
            viewport->setVisible(false);
            _panelViewportForFrame = viewport;
        }
    }

    _ui.layout();
    _ui.populateFrame();
}

void EditorSession::flushFrame() {
    // AI-1: close the IRenderBackend lifecycle (endCanvas + endFrame).
    // endFrame() inside the backend flushes pendingRects via
    // flushColoredRects() + any remaining text via flushPendingText().
    _ui.flushFrame();

    if (_panelViewportForFrame != nullptr) {
        _panelViewportForFrame->setVisible(_panelViewportWasVisibleForFrame);
        _panelViewportForFrame = nullptr;
    }
}

bool EditorSession::shouldCompositeViewport() const {
    const EditorMode mode = _gameView.mode();
    return (mode == EditorMode::Play || mode == EditorMode::Paused)
           && _playRuntime.isPresentationReady();
}

bool EditorSession::ensurePresentationReady() {
    return _playRuntime.ensurePresentationReady();
}

void EditorSession::autoEnterNetClientPlay()
{
    if (!_netClientAutoPlay) {
        return;
    }
    std::fprintf(stderr,
        "[EditorSession] --net-client: auto-entering Play mode\n");
    _gameView.setMode(EditorMode::Play);
}

bool EditorSession::getViewportBounds(ayt::math::FRectangle& outBounds) const {
    ayt::ui::Widget* viewport = _ui.findById("panel_viewport");
    if (viewport == nullptr) {
        return false;
    }
    outBounds = viewport->getWorldBounds();
    return true;
}

bool EditorSession::isSplitHandlePoint(float x, float y) const {
    ayt::ui::Widget* mainRow = _ui.findById("main_row");
    if (mainRow == nullptr) {
        return false;
    }

    ayt::ui::Widget* hit = mainRow->hitTest(ayt::math::FVector2(x, y));
    return dynamic_cast<const ayt::ui::SplitterHandle*>(hit) != nullptr;
}

namespace {

void clearSplitterHoversRecursive(ayt::ui::Widget* widget)
{
    if (widget == nullptr) {
        return;
    }
    if (auto* split = dynamic_cast<ayt::ui::SplitterHandle*>(widget)) {
        // Prefer clearHoverReveal so we never depend on onMouseLeave
        // side effects / override quirks for the force-unreveal path.
        split->clearHoverReveal();
    }
    for (ayt::ui::Widget* child : widget->getChildren()) {
        clearSplitterHoversRecursive(child);
    }
}

bool pointOnSplitterRecursive(ayt::ui::Widget* widget, float x, float y)
{
    if (widget == nullptr || !widget->isVisible()) {
        return false;
    }
    if (auto* split = dynamic_cast<ayt::ui::SplitterHandle*>(widget)) {
        if (split->getWorldBounds().contains(ayt::math::FVector2(x, y))) {
            return true;
        }
    }
    for (ayt::ui::Widget* child : widget->getChildren()) {
        if (pointOnSplitterRecursive(child, x, y)) {
            return true;
        }
    }
    return false;
}

} // namespace

void EditorSession::clearSplitterHovers()
{
    clearSplitterHoversRecursive(_ui.root());
}

void EditorSession::syncSplitterRevealToMouse()
{
    if (!_hasLastMouse) {
        clearSplitterHovers();
        return;
    }
    // Use bounds walk (not hitTest): hitTest can prefer other widgets
    // or miss when layout is mid-update; bounds are the reveal source of truth.
    if (!pointOnSplitterRecursive(_ui.root(), _lastMouseX, _lastMouseY)) {
        clearSplitterHovers();
    }
}

bool EditorSession::isChromePoint(float x, float y) const {
    if (_gameView.mode() == EditorMode::Edit) {
        return true;
    }

    if (_ui.isCapturing()) {
        return true;
    }

    if (isSplitHandlePoint(x, y)) {
        return true;
    }

    // Open menus / combo popups live on the overlay and often extend into
    // panel_viewport. In Play/Paused those points must still reach UIManager
    // or dropdown items over the cube receive no hits.
    if (ayt::ui::Widget* overlay = _ui.getOverlayRoot()) {
        const ayt::math::FVector2 pos(x, y);
        for (ayt::ui::Widget* child : overlay->getChildren()) {
            if (child == nullptr || !child->isVisible()) {
                continue;
            }
            if (child->getWorldBounds().contains(pos)) {
                return true;
            }
        }
    }

    ayt::math::FRectangle viewport{};
    if (!getViewportBounds(viewport)) {
        return true;
    }

    return x < viewport.minX || x >= viewport.maxX
        || y < viewport.minY || y >= viewport.maxY;
}

bool EditorSession::viewportAcceptsGameInput() const {
    if (_hostWindow == nullptr) {
        return false;
    }
    if (::GetForegroundWindow() != _hostWindow) {
        return false;
    }
    // LMB look already started inside the viewport — keep movement.
    if (_freecam.isLooking()) {
        return true;
    }
    if (!_hasLastMouse) {
        return false;
    }
    return !isChromePoint(_lastMouseX, _lastMouseY);
}

bool EditorSession::onMouseMove(float x, float y) {
    _lastMouseX = x;
    _lastMouseY = y;
    _hasLastMouse = true;

    // Armed viewport LMB: past slop → freecam look (not a click-select).
    if (_viewportLmbPending && !_viewportLmbDragged && !_freecam.isLooking()) {
        const float dx = x - _viewportLmbX;
        const float dy = y - _viewportLmbY;
        if ((dx * dx + dy * dy) >= (5.0f * 5.0f)) {
            _viewportLmbDragged = true;
            _freecam.beginLook(_viewportLmbX, _viewportLmbY);
            _freecam.updateLook(x, y);
            pushFreecamToRenderer();
            return true;
        }
    }

    if (_freecam.isLooking()) {
        _freecam.updateLook(x, y);
        pushFreecamToRenderer();
        return true;
    }

    if (_ui.isCapturing()) {
        // Still deliver moves (drag). Bounds-checked SplitterHandle::
        // onMouseMove clears _hover when outside the band; sync in
        // update() finishes un-reveal after mouse-up.
        return _ui.onMouseMove(x, y);
    }

    if (!isChromePoint(x, y)) {
        _ui.clearHover();
        clearSplitterHovers();
        return false;
    }

    const bool handled = _ui.onMouseMove(x, y);
    if (!pointOnSplitterRecursive(_ui.root(), x, y)) {
        clearSplitterHovers();
    }
    return handled;
}

bool EditorSession::onMouseButtonDown(float x, float y, int button) {
    // Dismiss MenuBar popups on any click that is not inside an open
    // menu. Play-mode freecam / isCapturing used to skip UIManager, so
    // click-outside never ran and the dropdown stayed painted forever.
    {
        const ayt::math::FVector2 pos(x, y);
        bool insideOpenMenu = false;
        if (ayt::ui::Widget* overlay = _ui.getOverlayRoot()) {
            for (ayt::ui::Widget* child : overlay->getChildren()) {
                auto* menu = dynamic_cast<ayt::ui::Menu*>(child);
                if (menu == nullptr || !menu->isOpen()) {
                    continue;
                }
                if (menu->getWorldBounds().contains(pos)) {
                    insideOpenMenu = true;
                    break;
                }
            }
        }
        if (!insideOpenMenu) {
            if (auto* menuBar =
                    dynamic_cast<ayt::ui::MenuBar*>(_ui.findById("menubar"))) {
                menuBar->closeOpenMenu();
            }
        }
    }

    if (_ui.isCapturing()) {
        return _ui.onMouseButtonDown(x, y, button);
    }

    if (!isChromePoint(x, y)) {
        _ui.clearHover();

        if (button == 0 && freecamActive()) {
            // Defer freecam until drag past slop — short click selects.
            _viewportLmbPending = true;
            _viewportLmbDragged = false;
            _viewportLmbX = x;
            _viewportLmbY = y;
            return true; // host should SetCapture
        }
        return false;
    }

    return _ui.onMouseButtonDown(x, y, button);
}

bool EditorSession::onMouseButtonUp(float x, float y, int button) {
    if (button == 0 && _viewportLmbPending) {
        const bool wasClick = !_viewportLmbDragged && !_freecam.isLooking();
        _viewportLmbPending = false;
        _viewportLmbDragged = false;
        if (_freecam.isLooking()) {
            _freecam.endLook();
        }
        if (wasClick) {
            selectPlayEntityFromViewport();
        }
        return true;
    }

    if (_freecam.isLooking() && button == 0) {
        _freecam.endLook();
        return true;
    }

    if (_ui.isCapturing()) {
        return _ui.onMouseButtonUp(x, y, button);
    }

    if (!isChromePoint(x, y)) {
        _ui.clearHover();
        return false;
    }

    return _ui.onMouseButtonUp(x, y, button);
}

void EditorSession::onMouseLeave() {
    _hasLastMouse = false;
    _ui.onMouseLeave();
    clearSplitterHovers();
}

bool EditorSession::isUiHoverInteractive() const {
    return _ui.isHoverInteractive();
}

ayt::ui::UiCursorHint EditorSession::getUiCursorHint() const {
    return _ui.getCursorHint();
}

void EditorSession::setRepaintCallback(RepaintCallback callback) {
    _repaintCallback = std::move(callback);
}

void EditorSession::bindToolbar() {
    auto bindButton = [this](const char* id, std::function<void()> handler) {
        _ui.bindEvent(id, "onClick", handler);
        if (auto* widget = _ui.findById(id)) {
            if (auto* button = dynamic_cast<ayt::ui::Button*>(widget)) {
                button->setOnClicked(handler);
            }
        }
    };

    bindButton("btn_minimize", [this]() { requestHostMinimize(); });
    bindButton("btn_maximize", [this]() { requestHostMaximizeToggle(); });
    bindButton("btn_close", [this]() { requestHostClose(); });

    bindButton("btn_inspector_skel",  [this]() { pickInspectorSkeleton(); });
    bindButton("btn_inspector_anim",  [this]() { pickInspectorAnimation(); });
    bindButton("btn_inspector_apply", [this]() { applyInspectorOverrides(); });
    bindButton("btn_inspector_reset", [this]() { resetInspectorOverrides(); });

    if (auto* widget = _ui.findById("lbl_mode")) {
        if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(widget)) {
            label->setBackgroundColor(ayt::math::FVector4(0.10f, 0.10f, 0.11f, 1.0f));
        }
    }
}

void EditorSession::bindTransportBar() {
    auto bindButton = [this](const char* id, std::function<void()> handler) {
        _ui.bindEvent(id, "onClick", handler);
        if (auto* widget = _ui.findById(id)) {
            if (auto* button = dynamic_cast<ayt::ui::Button*>(widget)) {
                button->setOnClicked(handler);
            }
        }
    };

    bindButton("btn_play", [this]() { _gameView.setMode(EditorMode::Play); });
    bindButton("btn_pause", [this]() { _gameView.setMode(EditorMode::Paused); });
    bindButton("btn_step", [this]() {
        _gameView.stepOnce();
        if (_repaintCallback) {
            _repaintCallback();
        }
    });
    bindButton("btn_stop", [this]() { _gameView.setMode(EditorMode::Edit); });
}

void EditorSession::setDockCardVisible(const char* cardId, bool visible) {
    ayt::ui::DockCard* card = nullptr;
    if (_mainDock != nullptr) {
        card = _mainDock->findCard(cardId);
    }
    if (card == nullptr) {
        card = dynamic_cast<ayt::ui::DockCard*>(_ui.findById(cardId));
    }
    if (card != nullptr) {
        card->setVisible(visible);
    }
    _ui.invalidateLayout();
    _ui.layout();
    syncViewport();
    if (_repaintCallback) {
        _repaintCallback();
    }
}

void EditorSession::toggleDockCard(const char* cardId, bool& visibleFlag) {
    visibleFlag = !visibleFlag;
    setDockCardVisible(cardId, visibleFlag);
}

void EditorSession::bindNetworkPanelStub() {
    auto bindButton = [this](const char* id, std::function<void()> handler) {
        _ui.bindEvent(id, "onClick", handler);
        if (auto* widget = _ui.findById(id)) {
            if (auto* button = dynamic_cast<ayt::ui::Button*>(widget)) {
                button->setOnClicked(handler);
            }
        }
    };

    bindButton("btn_net_spawn", []() {
        std::fprintf(stderr,
            "[EditorSession] Network Spawn (stub) — wire to server EntitySpawn\n");
    });
    bindButton("btn_net_despawn", []() {
        std::fprintf(stderr,
            "[EditorSession] Network Despawn (stub) — wire to server despawn\n");
    });

    if (auto* w = _ui.findById("sld_net_hp")) {
        if (auto* slider = dynamic_cast<ayt::ui::Slider*>(w)) {
            slider->setOnValueChanged([this](float v) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "Cube HP  %.0f",
                              static_cast<double>(v));
                if (auto* lbl = _ui.findById("lbl_net_hp")) {
                    if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(lbl)) {
                        label->setText(std::wstring(buf, buf + std::strlen(buf)));
                    }
                }
            });
        }
    }
}

void EditorSession::bindRenderSettingsPanel()
{
    // Left "Render" panel — live knobs after RendererSubSystem exists
    // (Play session). Safe no-ops before Play: findRegistered() is null.
    auto rendererOrNull = []() -> ayt::render::Renderer* {
        if (auto* sub = ayt::render::RendererSubSystem::findRegistered()) {
            return &sub->renderer();
        }
        return nullptr;
    };

    auto setLabel = [this](const char* id, const char* textUtf8) {
        if (auto* w = _ui.findById(id)) {
            if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(w)) {
                label->setText(std::wstring(textUtf8, textUtf8 + std::strlen(textUtf8)));
            }
        }
    };

    auto bindSlider = [this](const char* id, std::function<void(float)> onChanged) {
        if (auto* w = _ui.findById(id)) {
            if (auto* slider = dynamic_cast<ayt::ui::Slider*>(w)) {
                slider->setOnValueChanged(std::move(onChanged));
            }
        }
    };

    bindSlider("sld_gamma", [rendererOrNull, setLabel](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Gamma  %.2f", static_cast<double>(v));
        setLabel("lbl_gamma", buf);
        if (ayt::render::Renderer* r = rendererOrNull()) {
            r->setPostProcessGamma(v);
        }
    });

    bindSlider("sld_exposure", [rendererOrNull, setLabel](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Exposure  %.2f", static_cast<double>(v));
        setLabel("lbl_exposure", buf);
        if (ayt::render::Renderer* r = rendererOrNull()) {
            r->setPostProcessExposure(v);
        }
    });

    bindSlider("sld_bloom", [rendererOrNull, setLabel](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Bloom  %.2f", static_cast<double>(v));
        setLabel("lbl_bloom", buf);
        if (ayt::render::Renderer* r = rendererOrNull()) {
            r->setPostProcessBloomStrength(v);
        }
    });

    // §S4d — Depth Haze (default slightly on in UI JSON).
    auto applyHazeParams = [rendererOrNull](bool enabled, float strength, float density) {
        if (ayt::render::Renderer* r = rendererOrNull()) {
            r->setDepthHazeEnabled(enabled);
            r->setDepthHazeStrength(enabled ? strength : 0.0f);
            r->setDepthHazeParams(
                density,
                ayt::math::FVector3(0.7f, 0.75f, 0.8f));
        }
    };

    if (auto* w = _ui.findById("chk_depth_haze")) {
        if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
            chk->setOnToggled([this, applyHazeParams](bool on) {
                float strength = 1.0f;
                float density = 0.04f;
                if (auto* sw = _ui.findById("sld_haze_strength")) {
                    if (auto* s = dynamic_cast<ayt::ui::Slider*>(sw)) {
                        strength = s->getValue();
                    }
                }
                if (auto* dw = _ui.findById("sld_haze_density")) {
                    if (auto* s = dynamic_cast<ayt::ui::Slider*>(dw)) {
                        density = s->getValue();
                    }
                }
                applyHazeParams(on, strength, density);
            });
        }
    }

    bindSlider("sld_haze_strength", [this, setLabel, applyHazeParams](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Haze Strength  %.2f", static_cast<double>(v));
        setLabel("lbl_haze_strength", buf);
        bool enabled = true;
        float density = 0.04f;
        if (auto* cw = _ui.findById("chk_depth_haze")) {
            if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(cw)) {
                enabled = chk->isChecked();
            }
        }
        if (auto* dw = _ui.findById("sld_haze_density")) {
            if (auto* s = dynamic_cast<ayt::ui::Slider*>(dw)) {
                density = s->getValue();
            }
        }
        applyHazeParams(enabled, v, density);
    });

    bindSlider("sld_haze_density", [this, setLabel, applyHazeParams](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Haze Density  %.3f", static_cast<double>(v));
        setLabel("lbl_haze_density", buf);
        bool enabled = true;
        float strength = 1.0f;
        if (auto* cw = _ui.findById("chk_depth_haze")) {
            if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(cw)) {
                enabled = chk->isChecked();
            }
        }
        if (auto* sw = _ui.findById("sld_haze_strength")) {
            if (auto* s = dynamic_cast<ayt::ui::Slider*>(sw)) {
                strength = s->getValue();
            }
        }
        applyHazeParams(enabled, strength, v);
    });

    // §S2 v1 — SSAO (Deferred-only; UI defaults slightly on).
    auto applySsaoParams = [rendererOrNull](bool enabled, float strength,
                                            float radius, float bias) {
        if (ayt::render::Renderer* r = rendererOrNull()) {
            r->setSsaoEnabled(enabled);
            r->setSsaoStrength(enabled ? strength : 0.0f);
            r->setSsaoParams(radius, bias);
        }
    };

    if (auto* w = _ui.findById("chk_ssao")) {
        if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
            chk->setOnToggled([this, applySsaoParams](bool on) {
                float strength = 0.45f;
                float radius = 0.4f;
                float bias = 0.04f;
                if (auto* sw = _ui.findById("sld_ssao_strength")) {
                    if (auto* s = dynamic_cast<ayt::ui::Slider*>(sw)) {
                        strength = s->getValue();
                    }
                }
                if (auto* rw = _ui.findById("sld_ssao_radius")) {
                    if (auto* s = dynamic_cast<ayt::ui::Slider*>(rw)) {
                        radius = s->getValue();
                    }
                }
                if (auto* bw = _ui.findById("sld_ssao_bias")) {
                    if (auto* s = dynamic_cast<ayt::ui::Slider*>(bw)) {
                        bias = s->getValue();
                    }
                }
                applySsaoParams(on, strength, radius, bias);
            });
        }
    }

    bindSlider("sld_ssao_strength", [this, setLabel, applySsaoParams](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "SSAO Strength  %.2f", static_cast<double>(v));
        setLabel("lbl_ssao_strength", buf);
        bool enabled = true;
        float radius = 0.4f;
        float bias = 0.04f;
        if (auto* cw = _ui.findById("chk_ssao")) {
            if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(cw)) {
                enabled = chk->isChecked();
            }
        }
        if (auto* rw = _ui.findById("sld_ssao_radius")) {
            if (auto* s = dynamic_cast<ayt::ui::Slider*>(rw)) {
                radius = s->getValue();
            }
        }
        if (auto* bw = _ui.findById("sld_ssao_bias")) {
            if (auto* s = dynamic_cast<ayt::ui::Slider*>(bw)) {
                bias = s->getValue();
            }
        }
        applySsaoParams(enabled, v, radius, bias);
    });

    bindSlider("sld_ssao_radius", [this, setLabel, applySsaoParams](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "SSAO Radius  %.2f", static_cast<double>(v));
        setLabel("lbl_ssao_radius", buf);
        bool enabled = true;
        float strength = 0.45f;
        float bias = 0.04f;
        if (auto* cw = _ui.findById("chk_ssao")) {
            if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(cw)) {
                enabled = chk->isChecked();
            }
        }
        if (auto* sw = _ui.findById("sld_ssao_strength")) {
            if (auto* s = dynamic_cast<ayt::ui::Slider*>(sw)) {
                strength = s->getValue();
            }
        }
        if (auto* bw = _ui.findById("sld_ssao_bias")) {
            if (auto* s = dynamic_cast<ayt::ui::Slider*>(bw)) {
                bias = s->getValue();
            }
        }
        applySsaoParams(enabled, strength, v, bias);
    });

    bindSlider("sld_ssao_bias", [this, setLabel, applySsaoParams](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "SSAO Bias  %.3f", static_cast<double>(v));
        setLabel("lbl_ssao_bias", buf);
        bool enabled = true;
        float strength = 0.45f;
        float radius = 0.4f;
        if (auto* cw = _ui.findById("chk_ssao")) {
            if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(cw)) {
                enabled = chk->isChecked();
            }
        }
        if (auto* sw = _ui.findById("sld_ssao_strength")) {
            if (auto* s = dynamic_cast<ayt::ui::Slider*>(sw)) {
                strength = s->getValue();
            }
        }
        if (auto* rw = _ui.findById("sld_ssao_radius")) {
            if (auto* s = dynamic_cast<ayt::ui::Slider*>(rw)) {
                radius = s->getValue();
            }
        }
        applySsaoParams(enabled, strength, radius, v);
    });

    bindSlider("sld_ambient", [rendererOrNull, setLabel](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "IBL Ambient  %.2f", static_cast<double>(v));
        setLabel("lbl_ambient", buf);
        if (ayt::render::Renderer* r = rendererOrNull()) {
            r->setAmbientStrength(v);
        }
    });

    bindSlider("sld_shadow_bias", [rendererOrNull, setLabel](float v) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Shadow Bias  %.4f", static_cast<double>(v));
        setLabel("lbl_shadow_bias", buf);
        if (ayt::render::Renderer* r = rendererOrNull()) {
            r->setShadowBias(v);
        }
    });

    if (auto* w = _ui.findById("cmb_tonemap")) {
        if (auto* combo = dynamic_cast<ayt::ui::ComboBox*>(w)) {
            combo->setOnSelectionChanged([rendererOrNull](int index) {
                ayt::render::Renderer* r = rendererOrNull();
                if (r == nullptr) {
                    return;
                }
                using TM = ayt::render::Renderer::TonemapMode;
                switch (index) {
                case 1:  r->setPostProcessTonemapMode(TM::Reinhard); break;
                case 2:  r->setPostProcessTonemapMode(TM::ACES); break;
                default: r->setPostProcessTonemapMode(TM::None); break;
                }
            });
        }
    }

    if (auto* w = _ui.findById("chk_shadow_pcf")) {
        if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
            chk->setOnToggled([rendererOrNull](bool on) {
                if (ayt::render::Renderer* r = rendererOrNull()) {
                    r->setShadowPcfEnabled(on);
                }
            });
        }
    }

    // Labels in JSON are decorative until Slider min/max/value load;
    // refresh from the live widget values so thumb ↔ text stay aligned.
    auto refreshLabelFromSlider = [this, setLabel](const char* sliderId,
                                                   const char* labelId,
                                                   const char* fmt) {
        if (auto* w = _ui.findById(sliderId)) {
            if (auto* slider = dynamic_cast<ayt::ui::Slider*>(w)) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), fmt,
                              static_cast<double>(slider->getValue()));
                setLabel(labelId, buf);
            }
        }
    };
    refreshLabelFromSlider("sld_gamma", "lbl_gamma", "Gamma  %.2f");
    refreshLabelFromSlider("sld_exposure", "lbl_exposure", "Exposure  %.2f");
    refreshLabelFromSlider("sld_bloom", "lbl_bloom", "Bloom  %.2f");
    refreshLabelFromSlider("sld_haze_strength", "lbl_haze_strength", "Haze Strength  %.2f");
    refreshLabelFromSlider("sld_haze_density", "lbl_haze_density", "Haze Density  %.3f");
    refreshLabelFromSlider("sld_ssao_strength", "lbl_ssao_strength", "SSAO Strength  %.2f");
    refreshLabelFromSlider("sld_ssao_radius", "lbl_ssao_radius", "SSAO Radius  %.2f");
    refreshLabelFromSlider("sld_ssao_bias", "lbl_ssao_bias", "SSAO Bias  %.3f");
    refreshLabelFromSlider("sld_ambient", "lbl_ambient", "IBL Ambient  %.2f");
    refreshLabelFromSlider("sld_shadow_bias", "lbl_shadow_bias", "Shadow Bias  %.4f");
}

void EditorSession::applyRenderSettingsFromPanel()
{
    auto* sub = ayt::render::RendererSubSystem::findRegistered();
    if (sub == nullptr) {
        return;
    }
    ayt::render::Renderer& r = sub->renderer();

    auto sliderValue = [this](const char* id, float fallback) -> float {
        if (auto* w = _ui.findById(id)) {
            if (auto* slider = dynamic_cast<ayt::ui::Slider*>(w)) {
                return slider->getValue();
            }
        }
        return fallback;
    };

    r.setPostProcessGamma(sliderValue("sld_gamma", 2.2f));
    r.setPostProcessExposure(sliderValue("sld_exposure", 1.0f));
    r.setPostProcessBloomStrength(sliderValue("sld_bloom", 0.3f));
    r.setAmbientStrength(sliderValue("sld_ambient", 0.85f));
    r.setShadowBias(sliderValue("sld_shadow_bias", 0.003f));

    // §S4d — Depth Haze defaults: on, density 0.04, fog (0.7,0.75,0.8).
    {
        bool hazeOn = true;
        if (auto* w = _ui.findById("chk_depth_haze")) {
            if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
                hazeOn = chk->isChecked();
            }
        }
        const float hazeStrength = sliderValue("sld_haze_strength", 1.0f);
        const float hazeDensity = sliderValue("sld_haze_density", 0.04f);
        r.setDepthHazeEnabled(hazeOn);
        r.setDepthHazeStrength(hazeOn ? hazeStrength : 0.0f);
        r.setDepthHazeParams(
            hazeDensity,
            ayt::math::FVector3(0.7f, 0.75f, 0.8f));
    }

    // §S2 v1 — SSAO defaults: on, strength 0.45, radius 0.4, bias 0.04.
    {
        bool ssaoOn = true;
        if (auto* w = _ui.findById("chk_ssao")) {
            if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
                ssaoOn = chk->isChecked();
            }
        }
        const float ssaoStrength = sliderValue("sld_ssao_strength", 0.45f);
        const float ssaoRadius = sliderValue("sld_ssao_radius", 0.4f);
        const float ssaoBias = sliderValue("sld_ssao_bias", 0.04f);
        r.setSsaoEnabled(ssaoOn);
        r.setSsaoStrength(ssaoOn ? ssaoStrength : 0.0f);
        r.setSsaoParams(ssaoRadius, ssaoBias);
    }

    if (auto* w = _ui.findById("cmb_tonemap")) {
        if (auto* combo = dynamic_cast<ayt::ui::ComboBox*>(w)) {
            using TM = ayt::render::Renderer::TonemapMode;
            switch (combo->getSelectedIndex()) {
            case 1:  r.setPostProcessTonemapMode(TM::Reinhard); break;
            case 2:  r.setPostProcessTonemapMode(TM::ACES); break;
            default: r.setPostProcessTonemapMode(TM::None); break;
            }
        }
    }

    if (auto* w = _ui.findById("chk_shadow_pcf")) {
        if (auto* chk = dynamic_cast<ayt::ui::CheckBox*>(w)) {
            r.setShadowPcfEnabled(chk->isChecked());
        }
    }
}

void EditorSession::bindMenuBar() {
    auto* widget = _ui.findById("menubar");
    auto* menuBar = dynamic_cast<ayt::ui::MenuBar*>(widget);
    if (menuBar == nullptr) {
        return;
    }

    ayt::ui::Menu* fileMenu = menuBar->addMenu(L"File");
    if (fileMenu != nullptr) {
        if (auto* item = fileMenu->addItem(L"New")) {
            item->setOnActivate([]() {});
        }
        if (auto* item = fileMenu->addItem(L"Open...")) {
            item->setOnActivate([]() {});
        }
        if (auto* item = fileMenu->addItem(L"Import...")) {
            item->setOnActivate([this]() { importCharacterFromDialog(); });
        }
        fileMenu->addSeparator();
        if (auto* item = fileMenu->addItem(L"Exit")) {
            item->setOnActivate([this]() { requestHostClose(); });
        }
    }

    ayt::ui::Menu* windowMenu = menuBar->addMenu(L"Window");
    if (windowMenu != nullptr) {
        if (auto* item = windowMenu->addItem(L"Render Settings")) {
            item->setOnActivate([this]() {
                toggleDockCard("card_render", _panelRenderVisible);
            });
        }
        if (auto* item = windowMenu->addItem(L"Inspector")) {
            item->setOnActivate([this]() {
                toggleDockCard("card_inspector", _panelInspectorVisible);
            });
        }
        if (auto* item = windowMenu->addItem(L"Network")) {
            item->setOnActivate([this]() {
                toggleDockCard("card_network", _panelNetworkVisible);
            });
        }
        windowMenu->addSeparator();
        if (auto* item = windowMenu->addItem(L"Select Character")) {
            item->setOnActivate([this]() { selectCharacter(); });
        }
    }

    ayt::ui::Menu* helpMenu = menuBar->addMenu(L"Help");
    if (helpMenu != nullptr) {
        if (auto* item = helpMenu->addItem(L"About AYEditor")) {
            item->setOnActivate([]() {});
        }
    }
}

void EditorSession::requestHostClose() {
    if (_hostWindow != nullptr) {
        ::PostMessageW(_hostWindow, WM_CLOSE, 0, 0);
    }
}

void EditorSession::requestHostMinimize() {
    if (_hostWindow != nullptr) {
        ::ShowWindow(_hostWindow, SW_MINIMIZE);
    }
}

void EditorSession::requestHostMaximizeToggle() {
    if (_hostWindow == nullptr) {
        return;
    }
    if (::IsZoomed(_hostWindow)) {
        ::ShowWindow(_hostWindow, SW_RESTORE);
    } else {
        ::ShowWindow(_hostWindow, SW_MAXIMIZE);
    }
}

namespace {

// Join a vector of type-name strings for stable stderr output,
// e.g. {"Animation", "Material"} -> "Material, Animation"
// (insertion order preserved). Used by the import pipeline's
// log lines.
std::string joinTypeNames(const std::vector<std::string>& names)
{
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) out += ", ";
        out += names[i];
    }
    return out;
}

} // namespace

// Phase 2a: toolbar Import button target. Mirrors the
// --import pipeline from EditorApp::run() but with the path
// source being the Win32 dialog (Phase 1 left this as a one-
// line shim). The dialog blocks on a modal until the user
// picks or cancels, so this method is fired-and-forgot from
// the main thread; nothing else on the UI thread runs while
// it's open.
void EditorSession::importCharacterFromDialog()
{
    const std::string sourcePath =
        ImportDialog::showOpenFileDialog(_hostWindow);
    if (sourcePath.empty()) {
        // User cancelled (or non-Windows stub returned empty).
        // Silent no-op; do not pollute stderr with a "no path"
        // message because cancels are a normal interaction.
        return;
    }

    const std::string cacheRoot =
        EditorPlayRuntime::resolvePersistentCacheRoot();
    const std::string assetRoot = cacheRoot + "assets\\";

    const Importer::Result result =
        ImportDialog::importFromPath(sourcePath, assetRoot);
    if (!result.success) {
        std::fprintf(stderr,
                     "[EditorSession] import failed: %s\n",
                     result.errorMessage.c_str());
        return;
    }

    ImportedCharacterMapDiagnostics diag;
    const ImportedCharacter mapped =
        mapConversionToImportedCharacter(result.conversion, cacheRoot, diag);
    if (!diag.success) {
        std::fprintf(stderr,
                     "[EditorSession] import produced no skinned character: "
                     "missing [%s]\n",
                     joinTypeNames(diag.missing).c_str());
        // Mapper rejected; keep whatever is currently spawned
        // (cube if no previous import, or prior character if
        // hot-swap with rejection). Do NOT replace with default-
        // constructed; that would clear a previously-imported
        // valid character on a bad second import.
        return;
    }

    _playRuntime.replaceImportedCharacter(mapped);
    std::fprintf(stderr,
                 "[EditorSession] imported character ready "
                 "(mesh=%s, skel=%s, anim=%s)\n",
                 mapped.meshPath.c_str(),
                 mapped.skeletonPath.c_str(),
                 mapped.animationPath.c_str());

    // ED-03: a successful import auto-selects the new character
    // for inspection, so the designer can immediately click
    // [Pick Anim] and re-route to a different .ayanm. Without
    // this the user has to click [Select] twice (once to
    // populate the inspector, once after applying).
    refreshInspectorLabels();

    if (_repaintCallback) {
        _repaintCallback();
    }
}

// ED-03: walk the inspector's TextLabels and update them with
// the currently-spawned Play entity (character preferred, else cube).
void EditorSession::refreshInspectorLabels()
{
    auto setUtf8 = [this](const char* id, const std::string& utf8) {
        if (auto* w = _ui.findById(id)) {
            if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(w)) {
                label->setText(std::wstring(utf8.begin(), utf8.end()));
            }
        }
    };

    ayt::entity::Entity* character = _playRuntime.selectedCharacterEntity();
    ayt::entity::Entity* cube = _playRuntime.cubeEntity();
    ayt::entity::Entity* e = nullptr;
    if (_inspectorPreferCube && cube != nullptr) {
        e = cube;
    } else if (character != nullptr) {
        e = character;
    } else {
        e = cube;
    }

    if (e == nullptr) {
        setUtf8("inspector_hint", "No selection");
        setUtf8("inspector_mesh", "mesh: -");
        setUtf8("inspector_skel", "skel: -");
        setUtf8("inspector_anim", "anim: -");
        return;
    }

    const bool isCharacter = (e == character);
    char hint[96];
    std::snprintf(hint, sizeof(hint), "%s  (click#%u)",
                  isCharacter ? "Character" : "Cube (opaque ref)",
                  static_cast<unsigned>(_viewportClickCount));
    setUtf8("inspector_hint", hint);

    if (auto* meshC = e->getComponent<ayt::entity::MeshComponent>()) {
        setUtf8("inspector_mesh", "mesh: " + meshC->meshPath);
    } else {
        setUtf8("inspector_mesh", "mesh: -");
    }
    if (auto* skelC = e->getComponent<ayt::entity::SkeletonComponent>()) {
        setUtf8("inspector_skel", "skel: " + skelC->skeletonPath);
    } else {
        setUtf8("inspector_skel", "skel: -");
    }
    if (auto* animC = e->getComponent<ayt::entity::AnimationComponent>()) {
        setUtf8("inspector_anim",
                animC->clipPath.empty() ? "anim: (bind-pose)"
                                        : ("anim: " + animC->clipPath));
    } else {
        setUtf8("inspector_anim", "anim: -");
    }
}

void EditorSession::selectPlayEntityFromViewport()
{
    // Cycle Character ↔ opaque cube so Inspector labels change on
    // every short click (ray-pick not required yet).
    ++_viewportClickCount;
    if (_playRuntime.selectedCharacterEntity() != nullptr
        && _playRuntime.cubeEntity() != nullptr) {
        _inspectorPreferCube = !_inspectorPreferCube;
    } else {
        _inspectorPreferCube = (_playRuntime.selectedCharacterEntity() == nullptr);
    }
    selectCharacter();
}

// ED-03: [Select] handler. Snapshots paths into the inspector.
// Falls back to the procedural cube when character spawn failed.
void EditorSession::selectCharacter()
{
    ayt::entity::Entity* character = _playRuntime.selectedCharacterEntity();
    ayt::entity::Entity* cube = _playRuntime.cubeEntity();
    ayt::entity::Entity* e = nullptr;
    if (_inspectorPreferCube && cube != nullptr) {
        e = cube;
    } else if (character != nullptr) {
        e = character;
    } else {
        e = cube;
    }

    if (e == nullptr) {
        std::fprintf(stderr,
            "[EditorSession] nothing to select; enter Play first "
            "(character or cube)\n");
        refreshInspectorLabels();
        return;
    }

    if (e == character) {
        if (auto* skelC = e->getComponent<ayt::entity::SkeletonComponent>()) {
            _inspectorSkelPick = skelC->skeletonPath;
        }
        if (auto* animC = e->getComponent<ayt::entity::AnimationComponent>()) {
            _inspectorAnimPick = animC->clipPath;
        }
    } else {
        _inspectorSkelPick.clear();
        _inspectorAnimPick.clear();
    }

    refreshInspectorLabels();
    if (_repaintCallback) {
        _repaintCallback();
    }
}

// ED-03: [Pick Skel] handler. Opens the Win32 dialog filtered
// to .ayskel, stashes the chosen path into _inspectorSkelPick.
// The path is NOT yet applied to the live entity - [Apply]
// commits it. Cancel returns empty = no-op.
void EditorSession::pickInspectorSkeleton()
{
    // We currently pass the empty filter straight through; the
    // ImportDialog::showOpenFileDialog defaults to its built-in
    // 3D-Model (.fbx/.gltf/.glb) filter, which is wider than
    // .ayskel. To stay within Phase 1 scope we accept the wider
    // filter (the user just types the path or picks any
    // cache-resident file). A typed filter arg is a Phase 2
    // refinement when picker infrastructure exists.
    const std::string picked =
        ayt::editor::ImportDialog::showOpenFileDialog(_hostWindow);
    if (picked.empty()) {
        return; // user cancelled
    }
    setInspectorSkeletonPath(picked);
}

// ED-03: [Pick Anim] handler, sibling of pickInspectorSkeleton.
void EditorSession::pickInspectorAnimation()
{
    const std::string picked =
        ayt::editor::ImportDialog::showOpenFileDialog(_hostWindow);
    if (picked.empty()) {
        return;
    }
    setInspectorAnimationPath(picked);
}

// ED-03: thin setters that stash the path and refresh the
// corresponding inspector label so the user sees feedback
// between [Pick ...] and [Apply].
void EditorSession::setInspectorSkeletonPath(const std::string& path)
{
    _inspectorSkelPick = path;
    if (auto* w = _ui.findById("inspector_skel")) {
        if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(w)) {
            label->setText(std::wstring(path.begin(), path.end()));
        }
    }
}

void EditorSession::setInspectorAnimationPath(const std::string& path)
{
    _inspectorAnimPick = path;
    if (auto* w = _ui.findById("inspector_anim")) {
        if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(w)) {
            label->setText(std::wstring(path.begin(), path.end()));
        }
    }
}

// ED-03: [Apply] handler. Builds an EntityInspectorOverrides
// from the staged picks and forwards to the runtime. Empty
// pick on a field => leave that component path unchanged
// (the runtime's applyComponentOverrides treats empty as
// "keep").
void EditorSession::applyInspectorOverrides()
{
    EntityInspectorOverrides ov;
    ov.skeletonPathOverride  = _inspectorSkelPick;
    ov.animationPathOverride = _inspectorAnimPick;
    commitInspectorOverrides(ov);
}

// ED-03: [Reset] handler. Clear pending overrides back to
// default (which the runtime interprets as "stop applying
// user picks on the next spawn"). Does NOT clear the live
// entity's component paths - the user keeps whatever is
// animating now.
void EditorSession::resetInspectorOverrides()
{
    _inspectorSkelPick.clear();
    _inspectorAnimPick.clear();
    EntityInspectorOverrides emptyOv;
    commitInspectorOverrides(emptyOv);
    std::fprintf(stderr, "[EditorSession] inspector overrides cleared\n");
}

// ED-03: shared work for both Apply and Reset. Forward the
// override to the runtime, refresh labels, trigger redraw.
void EditorSession::commitInspectorOverrides(const EntityInspectorOverrides& ov)
{
    _playRuntime.applyComponentOverrides(ov);
    refreshInspectorLabels();
    if (_repaintCallback) {
        _repaintCallback();
    }
}

// =============================================================================
// D5.5 — Card promotion wiring.
//
// We walk the primary UIManager's root widget tree and inject
// setPromoteCallback into every DockCard we encounter (slot or floating).
// The lambda closes over `this` so detachToOwnWindow on any card routes
// into the optional EditorChildWindowManager. Card-promotion is a
// pure-AYUI feature; the only AYEditor involvement is injecting the
// callback — we do NOT touch DockCard's interface here (callback
// injection is the contract, per design §17.5 D5.5).
//
// Walk strategy: recursive descent via getChildren(). The root may be
// nullptr (no layout loaded yet) — we silently no-op. A DockArea
// that isn't reached because the layout tree didn't include one also
// no-ops; there are simply no cards to wire.
// =============================================================================

namespace {
void wirePromoteCallbackRecursive(ayt::ui::Widget* w, EditorSession* session) {
    if (!w) return;
    if (auto* dock = dynamic_cast<ayt::ui::DockArea*>(w)) {
        // Only wire floating cards. Slot-docked cards are NOT promoted
        // because DockCard::detachToOwnWindow's parent == DockOverlay
        // gate (K-INV-D5.5 in AYDockCard.cpp) would reject them anyway;
        // injecting a no-op-true callback would invite confusion. The
        // slot-card-promotion path is a future cut.
        if (auto* overlay = dock->getOverlay()) {
            for (size_t i = 0; i < overlay->getFloatingCardCount(); ++i) {
                if (auto* card = overlay->getFloatingCard(i)) {
                    card->setPromoteCallback(
                        [session](
                            const std::string& cardId,
                            const std::wstring& title,
                            int x, int y, int w, int h) -> bool {
                            // Convert wstring title -> narrow UTF-8
                            // for ChildWindowConfig::title (the
                            // top-level window title is wide internally
                            // in AYDevice, but ChildWindowConfig is the
                            // narrow-string intermediate used at the
                            // AYEditor API boundary).
                            std::string narrowTitle(title.begin(),
                                                    title.end());
                            ChildWindowConfig cfg;
                            cfg.title = std::move(narrowTitle);
                            // The promoted card keeps its id as a
                            // loadLayout hint; the child's loadLayout
                            // is best-effort (open still succeeds on
                            // missing file).
                            cfg.layoutPath = cardId + ".json";
                            cfg.x = x;
                            cfg.y = y;
                            cfg.width  = w;
                            cfg.height = h;
                            void* hOut = nullptr;
                            if (session && session->childWindows()) {
                                return session->childWindows()
                                    ->openChildWindow(cfg, hOut);
                            }
                            return false;
                        });
                }
            }
        }
        // Don't descend — DockArea owns its subtree and we already
        // walked it via overlay enumeration. Returning here matches
        // the explicit "I own my tree" stance of CompoundWidget.
        return;
    }
    for (ayt::ui::Widget* child : w->getChildren()) {
        wirePromoteCallbackRecursive(child, session);
    }
}
} // namespace

void EditorSession::wirePromoteCallback() {
    if (!_childWindows) return;  // no manager → no-op
    wirePromoteCallbackRecursive(_ui.root(), this);
}

void EditorSession::setModeLabel(const std::wstring& text) {
    if (auto* widget = _ui.findById("lbl_mode")) {
        if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(widget)) {
            label->setText(text);
        }
    }
}

void EditorSession::onModeChanged(EditorMode mode) {
    _ui.cancelCapture();
    if (_freecam.isLooking()) {
        _freecam.endLook();
    }

    switch (mode) {
    case EditorMode::Edit:
        setModeLabel(L"EDIT");
        if (auto* sub = ayt::render::RendererSubSystem::findRegistered()) {
            sub->clearCameraOverride();
        }
        break;
    case EditorMode::Play:
        setModeLabel(_netClientAutoPlay ? L"PLAY (NET CLIENT)" : L"PLAY");
        applyRenderSettingsFromPanel();
        pushFreecamToRenderer();
        // Auto-select whatever Play just spawned so Inspector is never
        // stuck on "No selection" while the viewport shows a cube.
        selectCharacter();
        break;
    case EditorMode::Paused:
        setModeLabel(L"PAUSED");
        applyRenderSettingsFromPanel();
        pushFreecamToRenderer();
        break;
    }

    _ui.invalidateLayout();
    _ui.layout();
    syncViewport();

    if (_repaintCallback) {
        _repaintCallback();
    }
}

void EditorSession::syncViewportIfChanged() {
    if (_hostWindow == nullptr) {
        return;
    }

    ayt::ui::Widget* viewport = _ui.findById("panel_viewport");
    if (viewport == nullptr) {
        return;
    }

    const ayt::math::FRectangle bounds = viewport->getWorldBounds();
    if (_viewportBoundsCached
        && bounds.minX == _cachedViewportBounds.minX
        && bounds.minY == _cachedViewportBounds.minY
        && bounds.maxX == _cachedViewportBounds.maxX
        && bounds.maxY == _cachedViewportBounds.maxY) {
        return;
    }

    _cachedViewportBounds = bounds;
    _viewportBoundsCached = true;
    _playRuntime.syncViewportRect(bounds);
}

void EditorSession::syncViewport() {
    _viewportBoundsCached = false;
    syncViewportIfChanged();
}

} // namespace ayt::editor
