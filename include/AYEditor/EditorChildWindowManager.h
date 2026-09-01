#pragma once

// D5 — child window manager for the editor shell.
//
// Owns a vector of {HWND, UIManager, layoutPath} entries — one per
// top-level OS window spawned via WindowManager::createTopLevelWindow.
// tickAll() walks every entry under pushActive scope; closeChildWindow
// tears it down. The manager keeps the primary EditorSession
// UIManager reference for g_activeUIManager restoration on teardown
// (K-INV-D5-6 shutdown order).
//
// PR-Dock-TearOff (2026-08-08): child UIManagers now render through a
// per-window GdiRenderBackend (Win32) and receive typed input events
// (mouse/key/wheel/char) forwarded from TopLevelWindowCallbacks. The
// old null-backend path (K-INV-D5-4) remains the non-Win32 fallback.

#include "AYUI/UIManager.h"
#include "AYDevice/WindowManager.h"
#include "AYDevice/WindowTypes.h"
#include "AYDevice/InputTypes.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ayt::ui {
class DockCard;
class DockArea;
} // namespace ayt::ui

namespace ayt::editor {

struct ChildWindowConfig {
    std::string title;
    std::string layoutPath;
    int  x = 100;
    int  y = 100;
    int  width  = 800;
    int  height = 600;
    // PR-Dock-TearOff live-card migration: when non-null, the child
    // window hosts the LIVE card (reparented into the child root) and
    // layoutPath is ignored. null = classic config-file path.
    ayt::ui::DockCard* card = nullptr;
    // Optional hooks for UI Layout Editor.
    // before*: return true to consume (skip UIManager).
    std::function<bool(ayt::ui::UIManager& ui, float x, float y,
                       int button, bool pressed)> beforeMouseButton;
    std::function<bool(ayt::ui::UIManager& ui, float x, float y)> beforeMouseMove;
    std::function<bool(ayt::ui::UIManager& ui, float x, float y, float deltaY)>
        beforeMouseWheel;
    std::function<bool(ayt::ui::UIManager& ui, ayt::device::KeyCode kc,
                       bool pressed)> beforeKey;
    std::function<void(ayt::ui::UIManager& ui, bool focused)> onFocusChanged;
    // Runs at the deferred close safe point while the child UI tree and
    // backend are still alive. Tool sessions use this to detach callbacks
    // and raw widget pointers before UIManager::shutdown destroys them.
    std::function<void(ayt::ui::UIManager& ui)> beforeClose;
    // Optional cursor override (Layout Editor resize/move hints).
    // Return Default to fall back to UIManager::getCursorHint().
    std::function<ayt::ui::UiCursorHint(ayt::ui::UIManager& ui, float x, float y)>
        resolveCursorHint;
};

// PR-Dock-TearOff: client→screen coordinate conversion for promote
// frames (AYUI world space = primary client coords → OS screen space).
// Win32 does ClientToScreen against the primary HWND; other platforms
// are identity (createTopLevelWindow is a stub there). Exposed (not
// anonymous-namespace) so the cross-platform test can pin the
// identity contract without an HWND.
void clientToScreenCoords(ayt::device::WindowManager& wm, int& x, int& y);

// Static helper. Parses a JSON config file with shape:
//   { "windows": [ { "title": "...", "layoutPath": "...", "x": 100,
//                     "y": 100, "width": 800, "height": 600 }, ... ] }
//
// Returns the empty vector on any parse error. Logs the first error
// line for visibility — does NOT propagate it (matches the existing
// EditorSession::initialize silent-fallback policy for layout
// failures; the host can detect "no children opened" and continue).
std::vector<ChildWindowConfig> parseChildWindowConfig(const std::string& path);

class EditorChildWindowManager {
public:
    using Handle = void*;  // opaque HWND alias (avoid <Windows.h> in headers)

    EditorChildWindowManager(ayt::device::WindowManager& wm,
                             ayt::ui::UIManager& primary);
    ~EditorChildWindowManager();

    EditorChildWindowManager(const EditorChildWindowManager&) = delete;
    EditorChildWindowManager& operator=(const EditorChildWindowManager&) = delete;

    bool openChildWindow(const ChildWindowConfig& cfg, Handle& outHandle);
    void closeChildWindow(Handle h);

    // PR-Dock-TearOff: promote a live DockCard into a new top-level
    // window. The frame arrives in PRIMARY-window client coordinates
    // (AYUI world space); Win32 converts to screen coords here
    // (ClientToScreen) — AYUI stays cross-platform, AYDevice's
    // TopLevelWindowDesc keeps OS screen space. Non-Win32: coordinates
    // pass through unchanged and createTopLevelWindow's stub returns
    // false.
    bool promoteCard(ayt::ui::DockCard* card, const std::wstring& title,
                     int x, int y, int w, int h);

    // Primary DockArea bridge for promoted-card return. Dragging a child
    // title over the primary client previews and commits a live redock;
    // closing a promoted host returns the card through DockArea's normal
    // close-request policy so persistent Window-menu panels remain reopenable.
    void setRedockTarget(ayt::ui::DockArea* dock) { _dock = dock; }
    void setChromeIconRoot(std::string root) { _iconRootPath = std::move(root); }
    bool hasActiveDrag() const;

    // Tick every open child: pushActive scope → update → GDI render
    // into the per-window backend (Win32; GetDC per frame).
    void tickAll(float dt);

    // Route a device key event to the matching child's UIManager.
    // Returns false if no entry with `h` exists. Caller must already
    // have filtered the key event by HWND (typically the editor
    // forwards Win32 WM_* messages filtered by the originating HWND).
    bool routeKey(Handle h, ::ayt::device::KeyCode kc);

    size_t count() const { return _entries.size(); }

    // Internal — exposed only for tests to verify push/pop ordering.
    struct Entry {
        Handle                            handle = nullptr;
        std::shared_ptr<ayt::ui::UIManager> ui;
        std::string                       layoutPath;
        // PR-Dock-TearOff: per-window render backend (null on
        // non-Win32 → K-INV-D5-4 null-backend no-op path).
        std::unique_ptr<ayt::ui::IRenderBackend> backend;
        // Live promoted card (null for config-file children).
        ayt::ui::DockCard*                card = nullptr;
        std::function<void(ayt::ui::UIManager& ui)> beforeClose;
        bool                              needsDraw = false;
        // Window/card close callbacks run inside Win32/UI dispatch. They
        // only mark the entry; tickAll tears it down after dispatch returns.
        bool                              closeRequested = false;
        bool                              dragMoveActive = false;
        int                               dragGrabX = 0;
        int                               dragGrabY = 0;
        int                               dragStartScreenX = 0;
        int                               dragStartScreenY = 0;
        int                               dragLastScreenX = 0;
        int                               dragLastScreenY = 0;
        int                               dragTravel = 0;
    };
    const std::vector<Entry>& entries() const { return _entries; }

private:
    void requestCloseChildWindow(Handle h);
    void closeChildWindowNow(Handle h, bool returnPromotedCard);
    void teardownEntry(Entry& entry, bool returnPromotedCard);
    void drainCloseRequests();

    Entry* findEntryByHandle(Handle h);
    Entry* findEntryByUi(const ayt::ui::UIManager* ui);
    Entry* findEntryByCard(const ayt::ui::DockCard* card);
    bool screenToPrimaryWorld(int screenX, int screenY,
                              ayt::math::FVector2& out) const;
    bool screenPointOverPrimaryWindow(int screenX, int screenY) const;
    void beginDragMove(Handle h, float clientX, float clientY);
    void updateDragMove(Handle h, float clientX, float clientY);
    void endDragMove(Handle h);
    void updateRedockHover();
    bool tryRedock(const std::shared_ptr<ayt::ui::UIManager>& ui);
    void configurePromotedCard(ayt::ui::DockCard* card, Handle h);
    static void resetPromotedCardChrome(ayt::ui::DockCard* card);

    ayt::device::WindowManager& _wm;
    ayt::ui::UIManager&         _primary;
    ayt::ui::DockArea*          _dock = nullptr;
    std::string                 _iconRootPath;
    std::vector<Entry>          _entries;
};

} // namespace ayt::editor
