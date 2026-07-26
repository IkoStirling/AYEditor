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

#include "EditorChildWindowManager.h"
#include "AYUIManager.h"
#include "AYWindowManager.h"
#include "AYWindowTypes.h"
#include "AYMockRenderer.h"

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
    mgr.tickAll(0.016f, nullptr);
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

TEST_SUITE_END
