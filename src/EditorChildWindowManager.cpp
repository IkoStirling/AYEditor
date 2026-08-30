#include "AYEditor/EditorChildWindowManager.h"

#include "AYUI/DockCard.h"
#include "AYUI/DeviceInputBridge.h"

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

#include <cstdio>
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
        teardownEntry(e);
    }
    _entries.clear();
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
        e.card->setShowResizeGrip(true);
        e.card->setShowMaximizeButton(true);
        e.card->setMaximizeHandler(
            [](void* user, ayt::ui::DockCard* c) {
                auto* self = static_cast<EditorChildWindowManager*>(user);
                if (self == nullptr || c == nullptr) {
                    return;
                }
                for (const auto& ent : self->entries()) {
                    if (ent.card != c || ent.handle == nullptr) {
                        continue;
                    }
                    self->_wm.toggleTopLevelMaximized(ent.handle);
                    c->setMaximizedVisual(
                        self->_wm.isTopLevelMaximized(ent.handle));
                    break;
                }
            },
            this);
        // A promoted card no longer belongs to its source DockArea. Its
        // chrome X must close this host window, and must not retain or
        // invoke the source DockArea's close callback after reparenting.
        e.card->setOnCloseRequested([this, h](ayt::ui::DockCard*) {
            requestCloseChildWindow(h);
        });
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
    cbs.onMouseMove = [ui, beforeMove, mousePos](float x, float y) {
        mousePos->x = x;
        mousePos->y = y;
        ayt::ui::UIManager::ActiveScope guard(ui.get());
        if (beforeMove && beforeMove(*ui, x, y)) {
            return;
        }
        ui->onMouseMove(x, y);
    };
    cbs.onMouseLeave = [ui]() {
        ayt::ui::UIManager::ActiveScope guard(ui.get());
        ui->onMouseLeave();
    };
    cbs.onMouseButton = [ui, beforeButton](float x, float y, int button, bool pressed) {
        ayt::ui::UIManager::ActiveScope guard(ui.get());
        if (beforeButton && beforeButton(*ui, x, y, button, pressed)) {
            return true; // request capture while document-dragging
        }
        return pressed ? ui->onMouseButtonDown(x, y, button)
                       : ui->onMouseButtonUp(x, y, button);
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
    closeChildWindowNow(h);
}

void EditorChildWindowManager::requestCloseChildWindow(Handle h) {
    for (auto& entry : _entries) {
        if (entry.handle == h) {
            entry.closeRequested = true;
            return;
        }
    }
}

void EditorChildWindowManager::teardownEntry(Entry& entry) {
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
    if (entry.ui != nullptr) {
        ayt::ui::UIManager::ActiveScope guard(entry.ui.get());
        if (beforeClose) {
            beforeClose(*entry.ui);
        }
        if (entry.card != nullptr) {
            entry.card->clearMaximizeHandler();
            entry.card->setOnCloseRequested({});
        }
        entry.ui->shutdown();
    }
    if (handle != nullptr) {
        _wm.destroyTopLevelWindow(handle);
    }
    entry.card = nullptr;
    entry.ui.reset();
    entry.backend.reset();
}

void EditorChildWindowManager::closeChildWindowNow(Handle h) {
    for (auto it = _entries.begin(); it != _entries.end(); ++it) {
        if (it->handle == h) {
            teardownEntry(*it);
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
        teardownEntry(_entries[i]);
        _entries.erase(_entries.begin() + static_cast<std::ptrdiff_t>(i));
    }
    ayt::ui::UIManager::makeActive(&_primary);
}

void EditorChildWindowManager::tickAll(float dt) {
    // Close requests originate in WindowManager/UIManager callbacks. Drain
    // only after their dispatch stack has unwound, before iterating entries.
    drainCloseRequests();
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
