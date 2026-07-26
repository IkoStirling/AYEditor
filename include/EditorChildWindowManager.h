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
// Renderer: child UIManagers are initialized with nullptr. Real bgfx
// routing per HWND is v2 (D5.6). For now, the child's render path
// no-ops via the populateFrame / flushFrame guard at AYUIManager.cpp
// (K-INV-D5-4 null backend).

#include "AYUIManager.h"
#include "AYWindowManager.h"
#include "AYWindowTypes.h"
#include "AYInputTypes.h"

#include <memory>
#include <string>
#include <vector>

namespace ayt::editor {

struct ChildWindowConfig {
    std::string title;
    std::string layoutPath;
    int  x = 100;
    int  y = 100;
    int  width  = 800;
    int  height = 600;
};

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

    // Tick every open child. backend may be null (D5 v1 — no real
    // bgfx routing per HWND; the child's render path early-returns
    // because UIManager::_backend == nullptr).
    void tickAll(float dt, ayt::ui::IRenderBackend* backend);

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
        bool                              needsDraw = false;
    };
    const std::vector<Entry>& entries() const { return _entries; }

private:
    ayt::device::WindowManager& _wm;
    ayt::ui::UIManager&         _primary;
    std::vector<Entry>          _entries;
};

} // namespace ayt::editor
