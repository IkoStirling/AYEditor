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
#include "AYEditor/EditorDockViewHost.h"
#include "AYEditor/EditorWorkspace.h"
#include "AYUI/UIManager.h"
#include "AYDevice/WindowManager.h"
#include "AYDevice/WindowTypes.h"
#include "AYUI/DockArea.h"
#include "AYUI/DockCard.h"
#include "AYUI/DockOverlay.h"
#include "AYUI/DockTabGroup.h"
#include "AYUI/MockRenderer.h"
#include "demo/GdiRenderBackend.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

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

class ChildTextureHostServices final : public IEditorHostServices {
public:
    explicit ChildTextureHostServices(EditorWorkspace& workspace)
        : _workspace(workspace) {}

    EditorWorkspace& workspace() noexcept override { return _workspace; }
    const std::string& projectRoot() const noexcept override {
        return _projectRoot;
    }
    EditorAuthoringImage loadAuthoringImage(
        const std::string&, std::string* error = nullptr) override {
        if (error != nullptr) error->clear();
        return image;
    }
    void requestRepaint() override {}
    void setStatusText(const std::wstring&) override {}

    EditorAuthoringImage image;

private:
    EditorWorkspace& _workspace;
    std::string _projectRoot;
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

TEST_CASE(test_os_close_is_deferred_and_primary_survives) {
    WindowManager wm;
    WindowCreateInfo info{};
    info.title = "D5 Editor Primary";
    info.width = 800;
    info.height = 600;
    info.hidden = true;
    CHECK(wm.createWindow(info));
    const HWND primaryHwnd = static_cast<HWND>(wm.getWindowHandle());

    MockRenderer backend;
    UIManager primary;
    primary.initialize(&backend);
    primary.setClientSize(800.0f, 600.0f);

    EditorChildWindowManager mgr(wm, primary);
    ChildWindowConfig cfg;
    cfg.title = "Deferred close child";
    cfg.width = 320;
    cfg.height = 240;
    bool beforeCloseCalled = false;
    bool beforeCloseSawLiveRoot = false;
    cfg.beforeClose = [&](UIManager& childUi) {
        beforeCloseCalled = true;
        beforeCloseSawLiveRoot = childUi.root() != nullptr;
    };

    EditorChildWindowManager::Handle h = nullptr;
    CHECK(mgr.openChildWindow(cfg, h));
    const HWND childHwnd = static_cast<HWND>(h);

    ::SendMessageW(childHwnd, WM_CLOSE, 0, 0);
    CHECK(mgr.count() == 1);
    CHECK(mgr.entries()[0].closeRequested);
    CHECK(::IsWindow(childHwnd));
    CHECK(::IsWindow(primaryHwnd));

    mgr.tickAll(0.0f);
    CHECK(mgr.count() == 0);
    CHECK(beforeCloseCalled);
    CHECK(beforeCloseSawLiveRoot);
    CHECK_FALSE(::IsWindow(childHwnd));
    CHECK(::IsWindow(primaryHwnd));

    primary.shutdown();
    wm.destroyWindow();
}

TEST_CASE(test_owned_tool_window_can_veto_close_and_update_title) {
    WindowManager wm;
    WindowCreateInfo info{};
    info.title = "D5 Editor Primary";
    info.width = 800;
    info.height = 600;
    info.hidden = true;
    CHECK(wm.createWindow(info));
    const HWND primaryHwnd = static_cast<HWND>(wm.getWindowHandle());

    MockRenderer backend;
    UIManager primary;
    primary.initialize(&backend);
    primary.setClientSize(800.0f, 600.0f);

    EditorChildWindowManager mgr(wm, primary);
    ChildWindowConfig cfg;
    cfg.title = "Owned tool";
    cfg.width = 480;
    cfg.height = 320;
    bool allowClose = false;
    int closeChecks = 0;
    cfg.beforeCloseRequested = [&](UIManager&) {
        ++closeChecks;
        return allowClose;
    };

    EditorChildWindowManager::Handle handle = nullptr;
    CHECK(mgr.openChildWindow(cfg, handle));
    const HWND childHwnd = static_cast<HWND>(handle);
    CHECK(::GetWindow(childHwnd, GW_OWNER) == primaryHwnd);
    CHECK(mgr.uiForHandle(handle) != nullptr);
    CHECK(mgr.setChildWindowTitle(handle, "AYUI Designer - Dirty *"));
    wchar_t title[128] = {};
    CHECK(::GetWindowTextW(childHwnd, title, 128) > 0);
    CHECK(std::wstring(title) == L"AYUI Designer - Dirty *");

    ::SendMessageW(childHwnd, WM_CLOSE, 0, 0);
    CHECK(closeChecks == 1);
    CHECK(mgr.count() == 1u);
    CHECK_FALSE(mgr.entries().front().closeRequested);
    CHECK(::IsWindow(childHwnd));

    allowClose = true;
    ::SendMessageW(childHwnd, WM_CLOSE, 0, 0);
    CHECK(closeChecks == 2);
    CHECK(mgr.entries().front().closeRequested);
    mgr.tickAll(0.0f);
    CHECK(mgr.count() == 0u);
    CHECK_FALSE(::IsWindow(childHwnd));
    CHECK(::IsWindow(primaryHwnd));

    primary.shutdown();
    wm.destroyWindow();
}

TEST_CASE(test_child_dock_host_copies_authoring_texture_to_local_backend) {
    WindowManager wm;
    WindowCreateInfo info{};
    info.title = "Authoring texture primary";
    info.width = 800;
    info.height = 600;
    info.hidden = true;
    CHECK(wm.createWindow(info));

    MockRenderer primaryBackend;
    UIManager primary;
    primary.initialize(&primaryBackend);
    primary.setClientSize(800.0f, 600.0f);

    EditorChildWindowManager manager(wm, primary);
    ChildWindowConfig config;
    config.title = "Texture child";
    config.width = 320;
    config.height = 240;
    EditorChildWindowManager::Handle handle = nullptr;
    CHECK(manager.openChildWindow(config, handle));

    UIManager* childUi = manager.uiForHandle(handle);
    auto* childBackend = childUi != nullptr
        ? dynamic_cast<GdiRenderBackend*>(childUi->backend()) : nullptr;
    CHECK(childUi != nullptr);
    CHECK(childBackend != nullptr);

    EditorWorkspace workspace;
    ChildTextureHostServices outerHost(workspace);
    outerHost.image.texture.handle = reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(0x1234u));
    outerHost.image.texture.width = 2;
    outerHost.image.texture.height = 2;
    outerHost.image.texture.format = TextureFormat::RGBA8;
    outerHost.image.width = 2u;
    outerHost.image.height = 2u;
    outerHost.image.bgraPixels =
        std::make_shared<const std::vector<std::uint8_t>>(
            std::vector<std::uint8_t>{
                0u, 0u, 255u, 255u, 0u, 255u, 0u, 255u,
                255u, 0u, 0u, 255u, 255u, 255u, 255u, 255u});

    DockArea dock;
    void* localHandle = nullptr;
    {
        EditorDockViewHost host(workspace, dock, outerHost, childUi);
        std::string error;
        const EditorAuthoringImage first =
            host.loadAuthoringImage("sheet.png", &error);
        CHECK(error.empty());
        CHECK(static_cast<bool>(first));
        CHECK(first.texture.handle != outerHost.image.texture.handle);
        CHECK(childBackend != nullptr
              && childBackend->ownsUiTexture(first.texture.handle));
        localHandle = first.texture.handle;

        const EditorAuthoringImage second =
            host.loadAuthoringImage("sheet.png", &error);
        CHECK(second.texture.handle == localHandle);
        host.shutdown();
        CHECK(childBackend != nullptr
              && !childBackend->ownsUiTexture(localHandle));
    }

    manager.closeChildWindow(handle);
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

    // The promoted DockCard's own X closes only its host. Dispatch is
    // deferred until tickAll so neither the UI callback nor its HWND
    // destroys itself while still on the message stack.
    const HWND primaryHwnd = static_cast<HWND>(wm.getWindowHandle());
    const HWND childHwnd = static_cast<HWND>(entry.handle);
    const int closeX = static_cast<int>(profiler->getSize().x) - 5;
    ::SendMessageW(childHwnd, WM_LBUTTONDOWN, MK_LBUTTON,
                   MAKELPARAM(closeX, 5));
    ::SendMessageW(childHwnd, WM_LBUTTONUP, 0, MAKELPARAM(closeX, 5));
    CHECK(mgr.count() == 1);
    CHECK(mgr.entries()[0].closeRequested);
    CHECK(::IsWindow(primaryHwnd));

    mgr.tickAll(0.0f);
    CHECK(mgr.count() == 0);
    CHECK_FALSE(::IsWindow(childHwnd));
    CHECK(::IsWindow(primaryHwnd));

    primary.shutdown();
    wm.destroyWindow();
}

TEST_CASE(test_detached_persistent_card_close_parks_and_reopens_same_instance) {
    WindowManager wm;
    WindowCreateInfo info{};
    info.title = "D5.5 Persistent Panel Primary";
    info.width = 800;
    info.height = 600;
    info.hidden = true;
    CHECK(wm.createWindow(info));
    const HWND primaryHwnd = static_cast<HWND>(wm.getWindowHandle());

    MockRenderer backend;
    UIManager primary;
    primary.initialize(&backend);
    primary.setClientSize(800.0f, 600.0f);

    auto* dock = new DockArea();
    dock->setId("shell");
    dock->setSize({800.0f, 600.0f});
    primary.root()->addChild(dock);

    auto ownedCard = std::make_unique<DockCard>();
    DockCard* panel = ownedCard.get();
    panel->setId("render-settings");
    panel->setTitle(L"Render Settings");
    auto* content = new Widget();
    content->setId("persistent-content");
    panel->setContent(content);
    dock->addCard(DockArea::Slot::Right, std::move(ownedCard));
    primary.layout();

    dock->setOnCardCloseRequested([dock](DockCard* requested) {
        if (requested == nullptr || requested->getId() != "render-settings") {
            return false;
        }
        return dock->setCardVisible("render-settings", false,
                                    DockArea::Slot::Right);
    });

    CHECK(dock->floatCard("render-settings", {420.0f, 80.0f}));
    CHECK(dock->getOverlay()->getFloatingCardCount() == 1);

    EditorChildWindowManager mgr(wm, primary);
    mgr.setRedockTarget(dock);
    panel->setPromoteCallback(
        [&mgr](DockCard* card, const std::wstring& title,
               int x, int y, int w, int h) -> bool {
            return mgr.promoteCard(card, title, x, y, w, h);
        });

    CHECK(panel->detachToOwnWindow());
    CHECK(mgr.count() == 1);
    CHECK(panel->getContent() == content);
    const HWND childHwnd = static_cast<HWND>(mgr.entries()[0].handle);

    ::SendMessageW(childHwnd, WM_CLOSE, 0, 0);
    CHECK(mgr.count() == 1);
    CHECK(mgr.entries()[0].closeRequested);
    mgr.tickAll(0.0f);

    CHECK(mgr.count() == 0);
    CHECK_FALSE(::IsWindow(childHwnd));
    CHECK(::IsWindow(primaryHwnd));
    CHECK(dock->findCard("render-settings") == panel);
    CHECK(panel->getContent() == content);
    CHECK_FALSE(panel->isVisible());

    // Mirrors the Window-menu action: reveal the parked live instance.
    CHECK(dock->setCardVisible("render-settings", true,
                               DockArea::Slot::Right));
    CHECK(panel->isVisible());
    CHECK(panel->getContent() == content);
    CHECK(dynamic_cast<DockTabGroup*>(panel->getParent()) != nullptr);

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
