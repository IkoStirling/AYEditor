#include "AYEditor/EditorChildWindowManager.h"

#include "AYUI/DockArea.h"
#include "AYUI/DockCard.h"
#include "AYUI/DockTabGroup.h"
#include "AYUI/DockTrace.h"
#include "AYUI/DeviceInputBridge.h"
#include "AYUI/SvgIcon.h"

#if defined(_WIN32)
#  include "GdiRenderBackend.h"
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <Windows.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

// nlohmann/json single-header — already in AYUI's thirdparty. Reach
// across the AYUI include path so we don't pull a new dep into
// AYEditor.
#include <nlohmann/json.hpp>

namespace ayt::editor {

namespace {

ChildWindowConfig parseOneConfig(const nlohmann::json& j) {
    ChildWindowConfig cfg;
    const auto itTitle = j.find("title");
    if (itTitle != j.end() && itTitle->is_string()) {
        cfg.title = itTitle->get<std::string>();
    }
    const auto itLayout = j.find("layoutPath");
    if (itLayout != j.end() && itLayout->is_string()) {
        cfg.layoutPath = itLayout->get<std::string>();
    }
    const auto itX = j.find("x");
    if (itX != j.end() && itX->is_number_integer()) {
        cfg.x = itX->get<int>();
    }
    const auto itY = j.find("y");
    if (itY != j.end() && itY->is_number_integer()) {
        cfg.y = itY->get<int>();
    }
    const auto itW = j.find("width");
    if (itW != j.end() && itW->is_number_integer()) {
        cfg.width = itW->get<int>();
    }
    const auto itH = j.find("height");
    if (itH != j.end() && itH->is_number_integer()) {
        cfg.height = itH->get<int>();
    }
    return cfg;
}

} // namespace

std::vector<ChildWindowConfig> parseChildWindowConfig(const std::string& path) {
    std::vector<ChildWindowConfig> out;
    if (path.empty()) {
        return out;
    }
    std::ifstream in(path);
    if (!in.is_open()) {
        // Missing file is a non-fatal "no children requested" — log
        // once and return empty so the editor proceeds normally.
        std::fprintf(stderr,
            "[EditorChildWindowManager] no child config at %s\n",
            path.c_str());
        return out;
    }
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        std::fprintf(stderr,
            "[EditorChildWindowManager] parse error at %s: %s\n",
            path.c_str(), e.what());
        return out;
    }
    const auto itWindows = j.find("windows");
    if (itWindows == j.end() || !itWindows->is_array()) {
        std::fprintf(stderr,
            "[EditorChildWindowManager] no 'windows' array in %s\n",
            path.c_str());
        return out;
    }
    for (const auto& w : *itWindows) {
        if (w.is_object()) {
            out.push_back(parseOneConfig(w));
        }
    }
    return out;
}

// PR-Dock-TearOff: convert a promote frame from primary-window CLIENT
// coordinates to SCREEN coordinates. Win32: ClientToScreen against the
// primary HWND. Non-Win32: identity pass-through (createTopLevelWindow
// is a stub there anyway). Declared in the header so the
// cross-platform test can pin the identity contract.
void clientToScreenCoords(ayt::device::WindowManager& wm, int& x, int& y) {
#if defined(_WIN32)
    if (HWND primaryHwnd = static_cast<HWND>(wm.getWindowHandle())) {
        POINT pt{static_cast<LONG>(x), static_cast<LONG>(y)};
        ::ClientToScreen(primaryHwnd, &pt);
        x = static_cast<int>(pt.x);
        y = static_cast<int>(pt.y);
    }
#else
    (void)wm;
    (void)x;
    (void)y;
#endif
}

EditorChildWindowManager::EditorChildWindowManager(ayt::device::WindowManager& wm,
                                                   ayt::ui::UIManager& primary)
    : _wm(wm)
    , _primary(primary) {
}

EditorChildWindowManager::~EditorChildWindowManager() {
    // K-INV-D5-6: tear down child windows BEFORE the primary UI.
    // ~UIManager calls shutdown() which can poke g_activeUIManager
    // (only if it was active); the primary's active flag wins over
    // a potentially-null child, so destroying the manager here
    // (with primary still alive) avoids an UAF cleanup race.
    for (auto& e : _entries) {
        teardownEntry(e, false);
    }
    _entries.clear();
}

EditorChildWindowManager::Entry*
EditorChildWindowManager::findEntryByHandle(Handle h) {
    for (auto& entry : _entries) {
        if (entry.handle == h) return &entry;
    }
    return nullptr;
}

EditorChildWindowManager::Entry*
EditorChildWindowManager::findEntryByUi(const ayt::ui::UIManager* ui) {
    for (auto& entry : _entries) {
        if (entry.ui.get() == ui) return &entry;
    }
    return nullptr;
}

EditorChildWindowManager::Entry*
EditorChildWindowManager::findEntryByCard(const ayt::ui::DockCard* card) {
    for (auto& entry : _entries) {
        if (entry.card == card) return &entry;
    }
    return nullptr;
}

bool EditorChildWindowManager::hasActiveDrag() const {
    for (const auto& entry : _entries) {
        if (entry.ui != nullptr && entry.ui->isDragging()) return true;
    }
    return false;
}

void EditorChildWindowManager::resetPromotedCardChrome(
    ayt::ui::DockCard* card) {
    if (card == nullptr) return;
    card->clearMinimizeHandler();
    card->clearMaximizeHandler();
    card->setShowMinimizeButton(false);
    card->setShowMaximizeButton(false);
    card->setShowResizeGrip(false);
    card->setMaximizedVisual(false);
    card->setOnCloseRequested({});
}

void EditorChildWindowManager::configurePromotedCard(
    ayt::ui::DockCard* card, Handle h) {
    if (card == nullptr) return;

    ayt::ui::SvgDocument::Ptr minimize;
    ayt::ui::SvgDocument::Ptr maximize;
    ayt::ui::SvgDocument::Ptr restore;
    ayt::ui::SvgDocument::Ptr close;
    if (!_iconRootPath.empty()) {
        const std::filesystem::path root(_iconRootPath);
        auto load = [&root](const char* relative) {
            std::string ignored;
            return ayt::ui::SvgDocument::loadFromFile(root / relative,
                                                       &ignored);
        };
        minimize = load("outline/minus.svg");
        maximize = load("outline/maximize.svg");
        restore = load("outline/restore.svg");
        close = load("outline/x.svg");
    }
    card->setHostChromeIcons(std::move(minimize), std::move(maximize),
                             std::move(restore), std::move(close));
    card->setShowResizeGrip(true);
    card->setShowMinimizeButton(true);
    card->setShowMaximizeButton(true);
    card->setMinimizeHandler(
        [](void* user, ayt::ui::DockCard* requested) {
            auto* self = static_cast<EditorChildWindowManager*>(user);
            Entry* entry = self != nullptr
                ? self->findEntryByCard(requested) : nullptr;
            if (entry != nullptr && entry->handle != nullptr) {
                self->_wm.minimizeTopLevelWindow(entry->handle);
            }
        },
        this);
    card->setMaximizeHandler(
        [](void* user, ayt::ui::DockCard* requested) {
            auto* self = static_cast<EditorChildWindowManager*>(user);
            Entry* entry = self != nullptr
                ? self->findEntryByCard(requested) : nullptr;
            if (entry != nullptr && entry->handle != nullptr) {
                self->_wm.toggleTopLevelMaximized(entry->handle);
                requested->setMaximizedVisual(
                    self->_wm.isTopLevelMaximized(entry->handle));
            }
        },
        this);
    card->setOnCloseRequested([this, h](ayt::ui::DockCard*) {
        requestCloseChildWindow(h);
    });
}

bool EditorChildWindowManager::openChildWindow(const ChildWindowConfig& cfg,
                                               Handle& outHandle) {
    outHandle = nullptr;

    ayt::device::TopLevelWindowDesc d;
    d.title  = cfg.title;
    d.x      = cfg.x;
    d.y      = cfg.y;
    d.width  = cfg.width;
    d.height = cfg.height;
    // Live DockCard promote: no OS caption (card paints its own chrome).
    // JSON-config children keep the classic overlapped frame.
    d.borderless = (cfg.card != nullptr);
    d.resizable  = (cfg.card != nullptr);
    // Hidden until first GDI frame — avoids white flash on show.
    d.visible = false;

    Handle h = nullptr;
    if (!_wm.createTopLevelWindow(d, h)) {
        std::fprintf(stderr,
            "[EditorChildWindowManager] createTopLevelWindow failed for "
            "'%s'\n", cfg.title.c_str());
        return false;
    }

    // Build the entry first so the callbacks can capture a stable
    // shared_ptr (the vector may reallocate on push_back; std::shared_ptr
    // keeps the UIManager alive across the lifetime of the callback
    // even if closeChildWindow removes the entry under it).
    Entry e;
    e.handle     = h;
    e.ui         = std::make_shared<ayt::ui::UIManager>();
    e.layoutPath = cfg.layoutPath;
    e.beforeClose = cfg.beforeClose;
#if defined(_WIN32)
    // PR-Dock-TearOff: per-HWND GDI backend — the promoted card renders
    // into THIS window's DC (bgfx is process-singleton-bound to the
    // primary window and cannot switch HWNDs per frame).
    e.backend = std::make_unique<GdiRenderBackend>(static_cast<HWND>(h));
    e.ui->initialize(e.backend.get());
#else
    e.ui->initialize(nullptr);  // K-INV-D5-4 null backend = no render
#endif
    // initialize() claims g_activeUIManager; restore the editor primary
    // so tryGet() between frames does not stay on the child.
    ayt::ui::UIManager::makeActive(&_primary);

    e.ui->setClientSize(static_cast<float>(cfg.width),
                        static_cast<float>(cfg.height));
    if (cfg.card != nullptr) {
        // PR-Dock-TearOff live-card migration: reparent the LIVE card
        // into the child root. addChild auto-detaches from the old
        // parent (the source DockOverlay); layoutPath is ignored.
        e.card = cfg.card;
        e.card->setPosition(ayt::math::FVector2(0.0f, 0.0f));
        e.card->setSize(ayt::math::FVector2(
            static_cast<float>(cfg.width), static_cast<float>(cfg.height)));
        configurePromotedCard(e.card, h);
        e.ui->root()->addChild(e.card);
        e.ui->layout();
    } else if (!cfg.layoutPath.empty()) {
        // Best-effort — failure logs but doesn't abort open. The
        // child window still lives and shows whatever the default
        // canvas draws.
        e.ui->loadLayout(cfg.layoutPath);
    }

    ayt::device::TopLevelWindowCallbacks cbs;
    // K-INV-D5-6: capture by value. The UIManager lives in `_entries`
    // by shared_ptr; the lambda runs on the Win32 message thread,
    // NOT concurrent with our tick (single-threaded editor v1).
    cbs.onCloseRequested = [this, h]() {
        this->requestCloseChildWindow(h);
    };

    // PR-Dock-TearOff: input forwarding. Every callback grabs its own
    // ActiveScope — these fire during the Win32 message pump
    // (pollEvents), NOT inside tickAll, so each must push/pop the
    // active UIManager independently without polluting the primary's
    // slot. Coordinates arrive client-relative (AYDevice translated
    // them); buttons map down/up to the UIManager pair. Capture the
    // shared_ptr + card (NOT the Entry — it holds a non-copyable
    // unique_ptr backend).
    const std::shared_ptr<ayt::ui::UIManager> ui = e.ui;
    ayt::ui::DockCard* card = e.card;
    cbs.onResize = [ui, card](int width, int height) {
        ayt::ui::UIManager::ActiveScope guard(ui.get());
        ui->setClientSize(static_cast<float>(width),
                          static_cast<float>(height));
        if (card != nullptr) {
            card->setSize(ayt::math::FVector2(
                static_cast<float>(width), static_cast<float>(height)));
        }
        ui->root()->performLayout();
    };
    const auto beforeButton = cfg.beforeMouseButton;
    const auto beforeMove = cfg.beforeMouseMove;
    const auto beforeWheel = cfg.beforeMouseWheel;
    const auto beforeKey = cfg.beforeKey;
    const auto focusChanged = cfg.onFocusChanged;
    const auto resolveCursor = cfg.resolveCursorHint;
    auto mousePos = std::make_shared<ayt::math::FVector2>(0.0f, 0.0f);
    cbs.onMouseMove = [this, ui, beforeMove, mousePos, h](float x, float y) {
        mousePos->x = x;
        mousePos->y = y;
        ayt::ui::UIManager::ActiveScope guard(ui.get());
        if (beforeMove && beforeMove(*ui, x, y)) {
            return;
        }
        ui->onMouseMove(x, y);
        this->updateDragMove(h, x, y);
    };
    cbs.onMouseLeave = [ui]() {
        ayt::ui::UIManager::ActiveScope guard(ui.get());
        ui->onMouseLeave();
    };
    cbs.onMouseButton = [this, ui, beforeButton, h](
        float x, float y, int button, bool pressed) {
        ayt::ui::UIManager::ActiveScope guard(ui.get());
        if (beforeButton && beforeButton(*ui, x, y, button, pressed)) {
            return true; // request capture while document-dragging
        }
        if (pressed) {
            const bool handled = ui->onMouseButtonDown(x, y, button);
            if (button == 0 && ui->isDragging()) {
                this->beginDragMove(h, x, y);
            }
            return handled;
        }
        // Commit a live redock before the child UI ends its local drag.
        if (button == 0 && this->tryRedock(ui)) {
            this->endDragMove(h);
            return true;
        }
        const bool handled = ui->onMouseButtonUp(x, y, button);
        this->endDragMove(h);
        return handled;
    };
    cbs.onMouseWheel = [ui, beforeWheel](float x, float y, float deltaY) {
        ayt::ui::UIManager::ActiveScope guard(ui.get());
        if (beforeWheel && beforeWheel(*ui, x, y, deltaY)) {
            return;
        }
        ui->onMouseWheel(x, y, deltaY);
    };
    cbs.onKey = [ui, beforeKey](::ayt::device::KeyCode kc, bool pressed) {
        ayt::ui::UIManager::ActiveScope guard(ui.get());
        if (beforeKey && beforeKey(*ui, kc, pressed)) {
            return;
        }
        if (pressed) {
            ui->onDeviceKeyDown(kc);
        } else {
            ui->onDeviceKeyUp(kc);
        }
    };
    cbs.onChar = [ui](const char* utf8, int byteCount) {
        ayt::ui::UIManager::ActiveScope guard(ui.get());
        ui->onDeviceChar(utf8, byteCount);
    };
    cbs.onFocusChanged = [ui, focusChanged](bool focused) {
        ayt::ui::UIManager::ActiveScope guard(ui.get());
        if (!focused) {
            ui->onDeviceKeyUp(ayt::device::KeyCode::LeftControl);
            ui->onDeviceKeyUp(ayt::device::KeyCode::RightControl);
            ui->onDeviceKeyUp(ayt::device::KeyCode::LeftShift);
            ui->onDeviceKeyUp(ayt::device::KeyCode::RightShift);
            ui->onDeviceKeyUp(ayt::device::KeyCode::LeftAlt);
            ui->onDeviceKeyUp(ayt::device::KeyCode::RightAlt);
            ui->cancelCapture();
        }
        if (focusChanged) {
            focusChanged(*ui, focused);
        }
    };
    cbs.cursorShape = [ui, resolveCursor, mousePos]() {
        ayt::ui::UIManager::ActiveScope guard(ui.get());
        ayt::ui::UiCursorHint hint = ayt::ui::UiCursorHint::Default;
        if (resolveCursor) {
            hint = resolveCursor(*ui, mousePos->x, mousePos->y);
        }
        if (hint == ayt::ui::UiCursorHint::Default) {
            hint = ui->getCursorHint();
        }
        return ayt::ui::systemCursorFromUi(hint);
    };
#if defined(_WIN32)
    if (e.backend && e.handle != nullptr) {
        if (HDC hdc = ::GetDC(static_cast<HWND>(e.handle))) {
            ayt::ui::UIManager::ActiveScope guard(e.ui.get());
            const int cw = static_cast<int>(e.ui->getClientSize().x);
            const int ch = static_cast<int>(e.ui->getClientSize().y);
            auto* gdi = static_cast<GdiRenderBackend*>(e.backend.get());
            gdi->setDrawTarget(hdc, cw, ch);
            e.ui->render();
            ::ReleaseDC(static_cast<HWND>(e.handle), hdc);
        }
    }
#endif
    // Publish the entry before callbacks/visibility. ShowWindow can
    // synchronously dispatch messages; every callback must be able to
    // find its entry even during the first show.
    _entries.push_back(std::move(e));
    _wm.setTopLevelCallbacks(h, cbs);
    _wm.setTopLevelVisible(h, true);

    outHandle = h;
    return true;
}

bool EditorChildWindowManager::screenToPrimaryWorld(
    int screenX, int screenY, ayt::math::FVector2& out) const {
#if defined(_WIN32)
    HWND primary = static_cast<HWND>(_wm.getWindowHandle());
    if (primary == nullptr) return false;
    POINT point{static_cast<LONG>(screenX), static_cast<LONG>(screenY)};
    if (!::ScreenToClient(primary, &point)) return false;
    // Win32 client coordinates are physical pixels, while DockArea geometry
    // lives in UI logical coordinates. Feeding the physical point directly
    // shifts the external drop by the monitor DPI scale and can make a visible
    // 125%/150% drop target resolve as "none".
    out = _primary.physicalToLogical(ayt::math::FVector2(
        static_cast<float>(point.x), static_cast<float>(point.y)));
    return true;
#else
    (void)screenX;
    (void)screenY;
    (void)out;
    return false;
#endif
}

bool EditorChildWindowManager::screenPointOverPrimaryWindow(
    int screenX, int screenY) const {
#if defined(_WIN32)
    HWND primary = static_cast<HWND>(_wm.getWindowHandle());
    if (primary == nullptr) return false;
    POINT screenPoint{static_cast<LONG>(screenX), static_cast<LONG>(screenY)};
    POINT clientPoint = screenPoint;
    RECT clientRect{};
    if (!::ScreenToClient(primary, &clientPoint)
        || !::GetClientRect(primary, &clientRect)
        || !::PtInRect(&clientRect, clientPoint)) {
        return false;
    }

    HWND under = ::WindowFromPoint(screenPoint);
    if (under == primary || (under != nullptr && ::IsChild(primary, under))) {
        return true;
    }
    for (const Entry& entry : _entries) {
        if (entry.handle == nullptr) continue;
        HWND child = static_cast<HWND>(entry.handle);
        if (under == child || (under != nullptr && ::IsChild(child, under))) {
            return entry.ui != nullptr && entry.ui->isDragging();
        }
    }
    return false;
#else
    (void)screenX;
    (void)screenY;
    return false;
#endif
}

void EditorChildWindowManager::beginDragMove(Handle h, float clientX,
                                             float clientY) {
#if defined(_WIN32)
    Entry* entry = findEntryByHandle(h);
    if (entry == nullptr || entry->handle == nullptr) return;
    HWND child = static_cast<HWND>(entry->handle);
    POINT cursorPoint{static_cast<LONG>(std::lround(clientX)),
                      static_cast<LONG>(std::lround(clientY))};
    RECT windowRect{};
    if (!::ClientToScreen(child, &cursorPoint)
        || !::GetWindowRect(child, &windowRect)) {
        return;
    }
    entry->dragGrabX = static_cast<int>(cursorPoint.x - windowRect.left);
    entry->dragGrabY = static_cast<int>(cursorPoint.y - windowRect.top);
    entry->dragStartScreenX = static_cast<int>(cursorPoint.x);
    entry->dragStartScreenY = static_cast<int>(cursorPoint.y);
    entry->dragLastScreenX = entry->dragStartScreenX;
    entry->dragLastScreenY = entry->dragStartScreenY;
    entry->dragTravel = 0;
    entry->dragMoveActive = true;
#else
    (void)h;
    (void)clientX;
    (void)clientY;
#endif
}

void EditorChildWindowManager::updateDragMove(Handle h, float clientX,
                                              float clientY) {
#if defined(_WIN32)
    Entry* entry = findEntryByHandle(h);
    if (entry == nullptr || !entry->dragMoveActive
        || entry->handle == nullptr) {
        return;
    }
    if (entry->ui == nullptr || !entry->ui->isDragging()) {
        entry->dragMoveActive = false;
        return;
    }
    HWND child = static_cast<HWND>(entry->handle);
    POINT cursorPoint{static_cast<LONG>(std::lround(clientX)),
                      static_cast<LONG>(std::lround(clientY))};
    if (!::ClientToScreen(child, &cursorPoint)) return;
    entry->dragLastScreenX = static_cast<int>(cursorPoint.x);
    entry->dragLastScreenY = static_cast<int>(cursorPoint.y);
    entry->dragTravel = std::max(
        entry->dragTravel,
        std::abs(entry->dragLastScreenX - entry->dragStartScreenX)
            + std::abs(entry->dragLastScreenY - entry->dragStartScreenY));
    ::SetWindowPos(child, nullptr,
                   entry->dragLastScreenX - entry->dragGrabX,
                   entry->dragLastScreenY - entry->dragGrabY,
                   0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
#else
    (void)h;
    (void)clientX;
    (void)clientY;
#endif
}

void EditorChildWindowManager::endDragMove(Handle h) {
    if (Entry* entry = findEntryByHandle(h)) {
        entry->dragMoveActive = false;
    }
}

void EditorChildWindowManager::updateRedockHover() {
    if (_dock == nullptr) return;
    for (const Entry& entry : _entries) {
        if (entry.ui == nullptr || !entry.ui->isDragging()) continue;
        ayt::math::FVector2 world;
        if (screenPointOverPrimaryWindow(entry.dragLastScreenX,
                                         entry.dragLastScreenY)
            && screenToPrimaryWorld(entry.dragLastScreenX,
                                    entry.dragLastScreenY, world)) {
            _dock->setExternalDropPos(world);
        } else {
            _dock->clearExternalDropPos();
        }
        return;
    }
    _dock->clearExternalDropPos();
}

bool EditorChildWindowManager::tryRedock(
    const std::shared_ptr<ayt::ui::UIManager>& ui) {
    if (_dock == nullptr || ui == nullptr || !ui->isDragging()) {
        ayt::ui::dockTrace(
            "[editor-child] redock skip dock=%d ui=%d dragging=%d\n",
            _dock != nullptr ? 1 : 0, ui != nullptr ? 1 : 0,
            (ui != nullptr && ui->isDragging()) ? 1 : 0);
        return false;
    }
    Entry* entry = findEntryByUi(ui.get());
    if (entry == nullptr || entry->card == nullptr) {
        ayt::ui::dockTrace(
            "[editor-child] redock skip entry=%d card=%d\n",
            entry != nullptr ? 1 : 0,
            (entry != nullptr && entry->card != nullptr) ? 1 : 0);
        return false;
    }

#if defined(_WIN32)
    if (entry->dragTravel < 12) {
        ayt::ui::dockTrace(
            "[editor-child] redock skip moved=%d screen=(%d,%d) "
            "start=(%d,%d)\n",
            entry->dragTravel, entry->dragLastScreenX,
            entry->dragLastScreenY, entry->dragStartScreenX,
            entry->dragStartScreenY);
        return false;
    }
#endif
    if (!screenPointOverPrimaryWindow(entry->dragLastScreenX,
                                      entry->dragLastScreenY)) {
        ayt::ui::dockTrace(
            "[editor-child] redock skip cursor outside primary\n");
        return false;
    }
    ayt::math::FVector2 world;
    if (!screenToPrimaryWorld(entry->dragLastScreenX,
                              entry->dragLastScreenY, world)) {
        ayt::ui::dockTrace(
            "[editor-child] redock skip client conversion failed\n");
        return false;
    }

    const ayt::ui::DockArea::DropTarget target =
        _dock->resolveDropTarget(world);
    ayt::ui::dockTrace(
        "[editor-child] redock cursor logical=(%.1f,%.1f) kind=%d "
        "slot=%d zone=%d leaf=%s scale=%.3f\n",
        world.x, world.y, static_cast<int>(target.kind),
        static_cast<int>(target.slot), static_cast<int>(target.zone),
        target.leaf != nullptr ? target.leaf->getLeafId().c_str() : "-",
        _primary.getEffectiveScale());
    if (target.kind == ayt::ui::DockArea::DropKind::None) return false;
    if (target.kind == ayt::ui::DockArea::DropKind::Slot) {
        const ayt::math::FRectangle slotRect = _dock->getSlotRect(target.slot);
        if ((slotRect.maxX - slotRect.minX) < 8.0f
            || (slotRect.maxY - slotRect.minY) < 8.0f) {
            return false;
        }
    }

    ayt::ui::DockCard* card = entry->card;
    const std::string cardId = card->getId();
    const Handle handle = entry->handle;
    ui->cancelDrag();
    ui->root()->removeChild(card);
    resetPromotedCardChrome(card);
    entry->card = nullptr;

    {
        ayt::ui::UIManager::ActiveScope primaryGuard(&_primary);
        if (!_dock->redockAt(card, world)) {
            configurePromotedCard(card, handle);
            entry->card = card;
            ui->root()->addChild(card);
            ui->layout();
            return false;
        }
        _dock->clearExternalDropPos();
        _primary.layout();
    }
    ayt::ui::dockTrace("[editor-child] redock OK card=%s pos=(%.1f,%.1f)\n",
                       cardId.c_str(), world.x, world.y);
    closeChildWindowNow(handle, false);
    return true;
}

bool EditorChildWindowManager::promoteCard(ayt::ui::DockCard* card,
                                           const std::wstring& title,
                                           int x, int y, int w, int h) {
    if (card == nullptr) {
        return false;
    }
    // The promote frame is the card's WORLD position = primary client
    // coords. Convert to screen coords before handing to
    // createTopLevelWindow (which positions in OS screen space).
    clientToScreenCoords(_wm, x, y);
    ChildWindowConfig cfg;
    cfg.title  = std::string(title.begin(), title.end());
    cfg.card   = card;
    cfg.x = x;
    cfg.y = y;
    cfg.width  = w;
    cfg.height = h;
    Handle hOut = nullptr;
    return openChildWindow(cfg, hOut);
}

void EditorChildWindowManager::closeChildWindow(Handle h) {
    closeChildWindowNow(h, true);
}

void EditorChildWindowManager::requestCloseChildWindow(Handle h) {
    for (auto& entry : _entries) {
        if (entry.handle == h) {
            entry.closeRequested = true;
            return;
        }
    }
}

void EditorChildWindowManager::teardownEntry(
    Entry& entry, bool returnPromotedCard) {
    const Handle handle = entry.handle;
    entry.handle = nullptr;

    // Stop new platform dispatch first. TopLevelWndProc may still hold its
    // current local callback copy, so the UI is explicitly shut down while
    // both its backend and HWND remain valid.
    if (handle != nullptr) {
        _wm.setTopLevelCallbacks(handle, {});
    }

    auto beforeClose = std::move(entry.beforeClose);
    entry.beforeClose = {};
    ayt::ui::DockCard* returningCard = nullptr;
    if (entry.ui != nullptr) {
        ayt::ui::UIManager::ActiveScope guard(entry.ui.get());
        if (beforeClose) {
            beforeClose(*entry.ui);
        }
        if (entry.card != nullptr) {
            if (returnPromotedCard && _dock != nullptr) {
                entry.ui->cancelDrag();
                entry.ui->root()->removeChild(entry.card);
                resetPromotedCardChrome(entry.card);
                returningCard = entry.card;
                entry.card = nullptr;
            } else {
                // Shutdown owns and destroys the card in this branch; make
                // sure no callback can re-enter this manager from teardown.
                resetPromotedCardChrome(entry.card);
            }
        }
        entry.ui->shutdown();
    }
    if (handle != nullptr) {
        _wm.destroyTopLevelWindow(handle);
    }
    entry.card = nullptr;
    entry.ui.reset();
    entry.backend.reset();

    if (returningCard != nullptr) {
        ayt::ui::UIManager::ActiveScope primaryGuard(&_primary);
        // Re-enter DockArea before routing close so its editor policy can
        // park persistent cards. This is what keeps Window-menu reopening
        // functional after a detached host is closed.
        if (_dock != nullptr
            && _dock->adoptCard(ayt::ui::DockArea::Slot::Center,
                                returningCard)) {
            (void)_dock->requestCloseCard(returningCard);
            _dock->clearExternalDropPos();
            _primary.layout();
        } else {
            ayt::ui::destroyWidgetTree(returningCard);
        }
    }
}

void EditorChildWindowManager::closeChildWindowNow(
    Handle h, bool returnPromotedCard) {
    for (auto it = _entries.begin(); it != _entries.end(); ++it) {
        if (it->handle == h) {
            teardownEntry(*it, returnPromotedCard);
            _entries.erase(it);
            ayt::ui::UIManager::makeActive(&_primary);
            return;
        }
    }
}

void EditorChildWindowManager::drainCloseRequests() {
    for (size_t i = 0; i < _entries.size();) {
        if (!_entries[i].closeRequested) {
            ++i;
            continue;
        }
        teardownEntry(_entries[i], true);
        _entries.erase(_entries.begin() + static_cast<std::ptrdiff_t>(i));
    }
    ayt::ui::UIManager::makeActive(&_primary);
}

void EditorChildWindowManager::tickAll(float dt) {
    // Close requests originate in WindowManager/UIManager callbacks. Drain
    // only after their dispatch stack has unwound, before iterating entries.
    drainCloseRequests();
    updateRedockHover();
    for (auto& e : _entries) {
        if (!e.ui) continue;
        // D5 — pushActive swaps g_activeUIManager for the duration of
        // this iteration; on scope exit the previous active (typically
        // the editor's primary) is restored.
        ayt::ui::UIManager::ActiveScope guard(e.ui.get());
        e.ui->update(dt);
#if defined(_WIN32)
        // PR-Dock-TearOff: per-window GDI draw. Grab the window DC for
        // this frame, point the backend at it, render. GetDC/ReleaseDC
        // round-trip per frame keeps the DC lifetime tight (no stale
        // handle across resize/destroy).
        if (e.backend && e.handle != nullptr) {
            if (HWND childHwnd = static_cast<HWND>(e.handle)) {
                if (HDC hdc = ::GetDC(childHwnd)) {
                    const int w = static_cast<int>(e.ui->getClientSize().x);
                    const int h = static_cast<int>(e.ui->getClientSize().y);
                    auto* gdi = static_cast<GdiRenderBackend*>(e.backend.get());
                    gdi->setDrawTarget(hdc, w, h);
                    e.ui->render();
                    ::ReleaseDC(childHwnd, hdc);
                }
            }
        }
#else
        e.ui->render();  // nullptr backend → populateFrame/flushFrame guard
#endif
    }
    // Also cover a future widget/update callback that requests close while
    // this tick is running; erasure remains outside the range-for loop.
    drainCloseRequests();
}

bool EditorChildWindowManager::routeKey(Handle h, ::ayt::device::KeyCode kc) {
    for (auto& e : _entries) {
        if (e.handle == h && e.ui) {
            ayt::ui::UIManager::ActiveScope guard(e.ui.get());
            return e.ui->onDeviceKeyDown(kc);
        }
    }
    return false;
}

} // namespace ayt::editor
