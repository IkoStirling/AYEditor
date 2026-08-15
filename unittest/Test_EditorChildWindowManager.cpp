// =============================================================================
// D5 — EditorChildWindowManager tests.
//
// Three tests:
//   1. test_open_close_lifecycle (Win32) — creates a real HWND + a
//      child UIManager, verifies count() round-trips, destroy path
//      closes the HWND cleanly.
//   2. test_tick_all_push_pop_order (cross-platform) — runs tickAll
//      with no entries (skips HWND requirement) and verifies
//      g_activeUIManager is restored to the primary after the call.
//   3. test_child_window_config_parse (cross-platform) — writes a
//      temp JSON file, parses via the static helper, verifies 2
//      entries with the expected fields. RAII temp-file cleanup.
// =============================================================================

#include "AYTest.h"

#include "AYEditor/EditorChildWindowManager.h"
#include "AYUI/UIManager.h"
#include "AYDevice/WindowManager.h"
#include "AYDevice/WindowTypes.h"
#include "AYUI/DockArea.h"
#include "AYUI/DockCard.h"
#include "AYUI/DockOverlay.h"
#include "AYUI/MockRenderer.h"

#include <cstdio>
#include <fstream>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <Windows.h>
#endif

using namespace ayt::editor;
using namespace ayt::device;
using namespace ayt::ui;

namespace {

std::string makeTempPath(const char* tag) {
#if defined(_WIN32)
    char buf[MAX_PATH];
    if (GetTempPathA(MAX_PATH, buf) == 0) {
        std::fprintf(stderr, "[D5 test] GetTempPathA failed\n");
        return {};
    }
    char unique[MAX_PATH];
    if (GetTempFileNameA(buf, tag, 0, unique) == 0) {
        std::fprintf(stderr, "[D5 test] GetTempFileNameA failed\n");
        return {};
    }
    return std::string(unique);
#else
    (void)tag;
    return std::string("/tmp/d5_test_XXXXXX");
#endif
}

struct TempFile {
    std::string path;
    TempFile(const std::string& p) : path(p) {}
    ~TempFile() {
        if (!path.empty()) std::remove(path.c_str());
    }
};

} // namespace

TEST_SUITE(AYEditor_ChildWindowManager)

// -------------------------------------------------------------------------
// 1. Win32 open/close lifecycle. Skips on non-Win32 because D5 v1
//    only ships the Win32 backend for createTopLevelWindow.
// -------------------------------------------------------------------------
#if defined(_WIN32)
TEST_CASE(test_open_close_lifecycle) {
    WindowManager wm;
    WindowCreateInfo info{};
    info.title = "D5 Editor Primary";
    info.width = 800;
    info.height = 600;
    info.hidden = true;
    CHECK(wm.createWindow(info));

    MockRenderer backend;
    UIManager primary;
    primary.initialize(&backend);
    primary.setClientSize(800.0f, 600.0f);

    EditorChildWindowManager mgr(wm, primary);
    CHECK(mgr.count() == 0);

    ChildWindowConfig cfg;
    cfg.title = "Child A";
    cfg.width = 320;
    cfg.height = 240;
    cfg.layoutPath.clear();   // no layout — child's render is empty

    EditorChildWindowManager::Handle h = nullptr;
    CHECK(mgr.openChildWindow(cfg, h));
    CHECK(h != nullptr);
    CHECK(mgr.count() == 1);
    CHECK(mgr.entries()[0].ui != nullptr);
    CHECK(mgr.entries()[0].ui->getClientSize().x == 320.0f);

    mgr.closeChildWindow(h);
    CHECK(mgr.count() == 0);

    primary.shutdown();
    wm.destroyWindow();
}
#endif

// -------------------------------------------------------------------------
// 2. Cross-platform pushActive/popActive ordering during tickAll.
//    With zero entries the tick is a guaranteed no-op (verified by
//    reading g_activeUIManager before/after — K-INV-D5-1).
// -------------------------------------------------------------------------
TEST_CASE(test_tick_all_push_pop_order) {
    WindowManager wm;

    MockRenderer backend;
    UIManager primary;
    primary.initialize(&backend);
    primary.setClientSize(100.0f, 100.0f);

    EditorChildWindowManager mgr(wm, primary);

    UIManager* activeBefore = UIManager::tryGet();
    CHECK(activeBefore == &primary);
    mgr.tickAll(0.016f);
    UIManager* activeAfter = UIManager::tryGet();
    CHECK(activeAfter == &primary);

    primary.shutdown();
}

// -------------------------------------------------------------------------
// 3. JSON config parse — writes a temp file with 2 entries, parses,
//    verifies fields. RAII deletes the file on exit.
// -------------------------------------------------------------------------
TEST_CASE(test_child_window_config_parse) {
    const std::string path = makeTempPath("d5a");
    CHECK_FALSE(path.empty());
    TempFile cleanup(path);

    {
        std::ofstream out(path);
        CHECK(out.is_open());
        out << R"({
            "windows": [
                { "title": "Hierarchy", "layoutPath": "ui/hierarchy.json", "x": 100, "y": 100, "width": 600, "height": 400 },
                { "title": "Inspector", "layoutPath": "ui/inspector.json" }
            ]
        })";
    }

    auto cfgs = parseChildWindowConfig(path);
    CHECK(cfgs.size() == 2);

    CHECK(cfgs[0].title == "Hierarchy");
    CHECK(cfgs[0].layoutPath == "ui/hierarchy.json");
    CHECK(cfgs[0].x == 100);
    CHECK(cfgs[0].y == 100);
    CHECK(cfgs[0].width == 600);
    CHECK(cfgs[0].height == 400);

    CHECK(cfgs[1].title == "Inspector");
    CHECK(cfgs[1].layoutPath == "ui/inspector.json");
    CHECK(cfgs[1].x == 100);
    CHECK(cfgs[1].y == 100);
    CHECK(cfgs[1].width == 800);
    CHECK(cfgs[1].height == 600);

    CHECK(parseChildWindowConfig("").empty());
    CHECK(parseChildWindowConfig("/nonexistent/path/config.json").empty());
}

// -------------------------------------------------------------------------
// 4. D5.5 (2026-07-26): card promotion wiring — building a dock tree
//    with a floating card, injecting the promote callback via
//    EditorChildWindowManager, and calling detachToOwnWindow() on the
//    card should open a real top-level HWND and bump mgr.count() to 1.
//    Win32 only (mirrors test 1's gating).
// -------------------------------------------------------------------------
#if defined(_WIN32)
TEST_CASE(test_card_promotion_opens_top_level_hwnd) {
    WindowManager wm;
    WindowCreateInfo info{};
    info.title = "D5.5 Editor Primary";
    info.width = 800;
    info.height = 600;
    info.hidden = true;
    CHECK(wm.createWindow(info));

    MockRenderer backend;
    UIManager primary;
    primary.initialize(&backend);
    primary.setClientSize(800.0f, 600.0f);

    // Build a primary-root dock with one floating card so the card's
    // parent is a DockOverlay (the only parent DockCard::detachToOwnWindow
    // accepts for promotion).
    auto* dock = new DockArea();
    dock->setId("shell");
    primary.root()->addChild(dock);

    DockOverlay* overlay = dock->getOverlay();
    CHECK(overlay != nullptr);
    auto* profiler = new DockCard();
    profiler->setId("profiler");
    profiler->setTitle(L"Profiler");
    profiler->setPosition({800.0f, 60.0f});
    profiler->setSize({320.0f, 220.0f});
    overlay->addFloatingCard(profiler);

    EditorChildWindowManager mgr(wm, primary);

    // Inject the promote callback. Mirrors the wiring
    // EditorSession::wirePromoteCallbackRecursive does for floating
    // cards — PR-Dock-TearOff: the live card goes straight to
    // promoteCard (no JSON rebuild).
    profiler->setPromoteCallback(
        [&mgr](DockCard* card, const std::wstring& title,
               int x, int y, int w, int h) -> bool {
            return mgr.promoteCard(card, title, x, y, w, h);
        });

    // Pre-condition: zero children.
    CHECK(mgr.count() == 0);

    // Trigger the promotion.
    const bool accepted = profiler->detachToOwnWindow();
    CHECK(accepted);

    // The manager now owns one entry; the floating card has detached
    // from the overlay. The child UIManager reports the promoted
    // frame size verbatim.
    CHECK(mgr.count() == 1);
    CHECK(overlay->getFloatingCardCount() == 0);
    CHECK(mgr.entries()[0].ui != nullptr);
    CHECK(mgr.entries()[0].ui->getClientSize().x == 320.0f);
    CHECK(mgr.entries()[0].ui->getClientSize().y == 220.0f);

    // Close the promoted window to clean up the HWND before teardown.
    mgr.closeChildWindow(mgr.entries()[0].handle);
    CHECK(mgr.count() == 0);

    primary.shutdown();
    wm.destroyWindow();
}
#endif

// -------------------------------------------------------------------------
// 5. PR-Dock-TearOff live-card migration (Win32): the promote callback
//    routes through EditorChildWindowManager::promoteCard; the card's
//    LIVE widget tree ends up in the child window's UIManager root, the
//    child has a real GDI backend, and closing the child cleans up the
//    entry (the card is freed with the child root).
// -------------------------------------------------------------------------
#if defined(_WIN32)
TEST_CASE(test_promote_live_card_migration) {
    WindowManager wm;
    WindowCreateInfo info{};
    info.title = "D5.5 Editor Primary";
    info.width = 800;
    info.height = 600;
    info.hidden = true;
    CHECK(wm.createWindow(info));

    MockRenderer backend;
    UIManager primary;
    primary.initialize(&backend);
    primary.setClientSize(800.0f, 600.0f);

    auto* dock = new DockArea();
    dock->setId("shell");
    primary.root()->addChild(dock);

    DockOverlay* overlay = dock->getOverlay();
    CHECK(overlay != nullptr);
    auto* profiler = new DockCard();
    profiler->setId("profiler");
    profiler->setTitle(L"Profiler");
    profiler->setPosition({800.0f, 60.0f});
    profiler->setSize({320.0f, 220.0f});
    // Live content subtree — must survive the migration verbatim.
    auto* content = new Widget();
    content->setId("live-content");
    profiler->setContent(content);
    overlay->addFloatingCard(profiler);

    EditorChildWindowManager mgr(wm, primary);
    profiler->setPromoteCallback(
        [&mgr](DockCard* card, const std::wstring& title,
               int x, int y, int w, int h) -> bool {
            return mgr.promoteCard(card, title, x, y, w, h);
        });

    CHECK(mgr.count() == 0);
    const bool accepted = profiler->detachToOwnWindow();
    CHECK(accepted);
    CHECK(mgr.count() == 1);

    // The LIVE card migrated: it now sits in the child root with its
    // content intact; the source overlay forgot it.
    const EditorChildWindowManager::Entry& entry = mgr.entries()[0];
    CHECK(entry.card == profiler);
    CHECK(profiler->getParent() == entry.ui->root());
    CHECK(profiler->getContent() == content);
    CHECK(overlay->getFloatingCardCount() == 0);

    // Child renders through a real GDI backend.
    CHECK(entry.backend != nullptr);
    // Child client matches the promoted frame.
    CHECK(entry.ui->getClientSize().x == 320.0f);
    CHECK(entry.ui->getClientSize().y == 220.0f);
    // Card fills the client area.
    CHECK(profiler->getSize().x == 320.0f);
    CHECK(profiler->getSize().y == 220.0f);

    // tickAll renders into the window without crashing (GetDC path).
    mgr.tickAll(0.016f);

    // Closing the child window destroys the entry; the card is freed
    // with the child root's widget tree.
    mgr.closeChildWindow(entry.handle);
    CHECK(mgr.count() == 0);

    primary.shutdown();
    wm.destroyWindow();
}
#endif

// -------------------------------------------------------------------------
// 6. Cross-platform contract for clientToScreenCoords: non-Win32 is an
//    identity transform (createTopLevelWindow is a stub there). Win32
//    conversion is covered end-to-end by the migration test above.
// -------------------------------------------------------------------------
TEST_CASE(test_client_to_screen_coords_contract) {
    WindowManager wm;
    int x = 321;
    int y = 123;
    clientToScreenCoords(wm, x, y);
#if !defined(_WIN32)
    CHECK(x == 321);
    CHECK(y == 123);
#else
    // Win32: must not crash with no primary window (getWindowHandle
    // nullptr → no-op) and must produce a finite int.
    CHECK(x == 321);
    CHECK(y == 123);
#endif
}

TEST_SUITE_END
