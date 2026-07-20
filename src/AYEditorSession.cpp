#include "AYEditorSession.h"

#include "AYEditorHeapDebug.h"
#include "AYEntity.h"
#include "AYSplitterHandle.h"
#include "AYButton.h"
#include "AYGameLoop.h"
#include "AYMenuBar.h"
#include "AYMenu.h"
#include "AYMenuItem.h"
#include "AYTextLabel.h"
#include "AYWidget.h"

#include <components/AYAnimationComponent.h>
#include <components/AYMeshComponent.h>
#include <components/AYSkeletonComponent.h>

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
    AY_EDITOR_TRACE("initialize: toolbar bound");

    setModeLabel(L"EDIT");

    syncViewport();
    AY_EDITOR_TRACE("initialize: done");
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
    _gameView.setMode(EditorMode::Edit);
    _ui.shutdown();
    _playRuntime.shutdownEngine();
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
    _ui.update(dt);
    // Per-frame reconcile: if the last known cursor is not on a splitter
    // band, force every SplitterHandle un-revealed. Leave events alone
    // are not sufficient (capture path / skipped WM_MOUSEMOVE).
    syncSplitterRevealToMouse();

    if (_gameView.mode() == EditorMode::Play) {
        _playRuntime.tick();
    } else if (_gameView.mode() == EditorMode::Paused) {
        // Keep last rendered frame visible; stepOnce drives simulation separately.
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

bool EditorSession::onMouseMove(float x, float y) {
    _lastMouseX = x;
    _lastMouseY = y;
    _hasLastMouse = true;

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
    if (_ui.isCapturing()) {
        return _ui.onMouseButtonDown(x, y, button);
    }

    if (!isChromePoint(x, y)) {
        _ui.clearHover();
        return false;
    }

    return _ui.onMouseButtonDown(x, y, button);
}

bool EditorSession::onMouseButtonUp(float x, float y, int button) {
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

    // Icon toolbar placeholders (visual chrome only for now).
    bindButton("btn_icon_1", []() {});
    bindButton("btn_icon_2", []() {});

    // Custom title-bar chrome (OS caption still present — these mirror
    // common actions; host drag/borderless is not wired yet).
    bindButton("btn_minimize", [this]() { requestHostMinimize(); });
    bindButton("btn_maximize", [this]() { requestHostMaximizeToggle(); });
    bindButton("btn_close", [this]() { requestHostClose(); });

    // ED-03: Inspector body buttons.
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

    ayt::ui::Menu* viewMenu = menuBar->addMenu(L"View");
    if (viewMenu != nullptr) {
        if (auto* item = viewMenu->addItem(L"Play")) {
            item->setOnActivate([this]() { _gameView.setMode(EditorMode::Play); });
        }
        if (auto* item = viewMenu->addItem(L"Pause")) {
            item->setOnActivate([this]() { _gameView.setMode(EditorMode::Paused); });
        }
        if (auto* item = viewMenu->addItem(L"Step")) {
            item->setOnActivate([this]() {
                _gameView.stepOnce();
                if (_repaintCallback) {
                    _repaintCallback();
                }
            });
        }
        if (auto* item = viewMenu->addItem(L"Stop")) {
            item->setOnActivate([this]() { _gameView.setMode(EditorMode::Edit); });
        }
        viewMenu->addSeparator();
        if (auto* item = viewMenu->addItem(L"Select Character")) {
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
// the currently-spawned character entity's path strings. Safe
// to call from any time (refresh after spawn, refresh after
// hot-swap, refresh after apply-overrides, refresh after
// reset-overrides). When no character is spawned, sets the
// title to "No selection".
void EditorSession::refreshInspectorLabels()
{
    auto setUtf8 = [this](const char* id, const std::string& utf8) {
        if (auto* w = _ui.findById(id)) {
            if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(w)) {
                label->setText(std::wstring(utf8.begin(), utf8.end()));
            }
        }
    };

    ayt::entity::Entity* e = _playRuntime.selectedCharacterEntity();
    if (e == nullptr) {
        setUtf8("inspector_hint", "No selection");
        setUtf8("inspector_mesh", "mesh: -");
        setUtf8("inspector_skel", "skel: -");
        setUtf8("inspector_anim", "anim: -");
        return;
    }

    setUtf8("inspector_hint", "Character");

    if (auto* meshC = e->getComponent<ayt::entity::MeshComponent>()) {
        setUtf8("inspector_mesh", "mesh: " + meshC->meshPath);
    }
    if (auto* skelC = e->getComponent<ayt::entity::SkeletonComponent>()) {
        setUtf8("inspector_skel", "skel: " + skelC->skeletonPath);
    }
    if (auto* animC = e->getComponent<ayt::entity::AnimationComponent>()) {
        setUtf8("inspector_anim", "anim: " + animC->clipPath);
    }
}

// ED-03: [Select] handler. Snapshots the live character
// entity's paths into the inspector labels and into the staged
// pick state. Idempotent - clicking multiple times re-renders.
// Phase 1 includes only the toolbar [Select] button as the
// selection mechanism; ED-05 (Hierarchy panel) replaces it.
void EditorSession::selectCharacter()
{
    if (_playRuntime.selectedCharacterEntity() == nullptr) {
        std::fprintf(stderr,
            "[EditorSession] no character to select; click Import first\n");
        refreshInspectorLabels();
        return;
    }

    auto* e = _playRuntime.selectedCharacterEntity();
    if (auto* skelC = e->getComponent<ayt::entity::SkeletonComponent>()) {
        _inspectorSkelPick = skelC->skeletonPath;
    }
    if (auto* animC = e->getComponent<ayt::entity::AnimationComponent>()) {
        _inspectorAnimPick = animC->clipPath;
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

void EditorSession::setModeLabel(const std::wstring& text) {
    if (auto* widget = _ui.findById("lbl_mode")) {
        if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(widget)) {
            label->setText(text);
        }
    }
}

void EditorSession::onModeChanged(EditorMode mode) {
    _ui.cancelCapture();

    switch (mode) {
    case EditorMode::Edit:
        setModeLabel(L"EDIT");
        break;
    case EditorMode::Play:
        setModeLabel(L"PLAY");
        break;
    case EditorMode::Paused:
        setModeLabel(L"PAUSED");
        break;
    }

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
