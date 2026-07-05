#include "AYEditorApp.h"

#include "AYEditorHeapDebug.h"
#include "AYEditorSession.h"
#include "AYEntityModule.h"
#include "AYGameLoop.h"
#include "AYDeviceManager.h"
#include "AYRendererSubSystem.h"
#include "AYUIRenderBackend.h"

#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <Windows.h>
#include <windowsx.h>

namespace ayt::editor {

namespace {

bool fileExists(const std::string& path)
{
    struct stat st;
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

void attachDebugConsole()
{
    if (AllocConsole() == 0) {
        return;
    }
    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    std::fprintf(stdout, "[EditorApp] debug console attached\n");
}

std::string resolveLayoutPath()
{
    const std::vector<std::string> candidates = {
        "assets/ui/editor_shell.ui.json",
        "AYRuntime/AYEditor/assets/ui/editor_shell.ui.json",
        "../AYRuntime/AYEditor/assets/ui/editor_shell.ui.json",
        "../../AYRuntime/AYEditor/assets/ui/editor_shell.ui.json",
    };

    for (const std::string& path : candidates) {
        if (fileExists(path)) {
            return path;
        }
    }
    return candidates.front();
}

struct EditorHostState {
    EditorSession* session = nullptr;
    int clientWidth = 0;
    int clientHeight = 0;
};

std::intptr_t handleHostMessage(HWND hwnd, EditorHostState* state, unsigned msg,
                                std::uintptr_t wParam, std::intptr_t lParam, bool& handled)
{
    handled = false;
    if (state == nullptr || state->session == nullptr) {
        return 0;
    }

    switch (msg) {
    case WM_SIZE:
        state->clientWidth  = LOWORD(lParam);
        state->clientHeight = HIWORD(lParam);
        state->session->setClientSize(static_cast<float>(state->clientWidth),
                                      static_cast<float>(state->clientHeight));
        handled = true;
        return 0;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT trackLeave{};
        trackLeave.cbSize = sizeof(trackLeave);
        trackLeave.dwFlags = TME_LEAVE;
        trackLeave.hwndTrack = hwnd;
        TrackMouseEvent(&trackLeave);

        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));
        state->session->onMouseMove(x, y);
        switch (state->session->getUiCursorHint()) {
        case ayt::ui::UiCursorHint::Hand:
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            break;
        case ayt::ui::UiCursorHint::SizeHorizontal:
            SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
            break;
        case ayt::ui::UiCursorHint::Move:
            SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
            break;
        default:
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            break;
        }
        handled = true;
        return 0;
    }
    case WM_MOUSELEAVE:
        if (GetCapture() != hwnd) {
            state->session->onMouseLeave();
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
        }
        handled = true;
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            switch (state->session->getUiCursorHint()) {
            case ayt::ui::UiCursorHint::Hand:
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                handled = true;
                return TRUE;
            case ayt::ui::UiCursorHint::SizeHorizontal:
                SetCursor(LoadCursor(nullptr, IDC_SIZEWE));
                handled = true;
                return TRUE;
            case ayt::ui::UiCursorHint::Move:
                SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                handled = true;
                return TRUE;
            default:
                break;
            }
        }
        break;
    case WM_LBUTTONDOWN: {
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));
        if (state->session->onMouseButtonDown(x, y, 0)) {
            SetCapture(hwnd);
        }
        handled = true;
        return 0;
    }
    case WM_LBUTTONUP: {
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));
        state->session->onMouseButtonUp(x, y, 0);
        ReleaseCapture();
        handled = true;
        return 0;
    }
    default:
        break;
    }

    return 0;
}

} // namespace

EditorApp::EditorApp(const ayt::app::GameDesc& desc) : _desc(desc) {}

EditorApp::EditorApp(const ayt::app::GameDesc& desc, const ayt::app::AppCommandLine& cmdLine)
    : _desc(desc), _cmdLine(cmdLine)
{
    if (_cmdLine.width > 0) {
        _desc.width = _cmdLine.width;
    }
    if (_cmdLine.height > 0) {
        _desc.height = _cmdLine.height;
    }
    if (_cmdLine.fps > 0.0f) {
        _desc.targetFPS = _cmdLine.fps;
    }
}

std::unique_ptr<EditorApp> EditorApp::create(const ayt::app::GameDesc& desc)
{
    return std::make_unique<EditorApp>(desc);
}

std::unique_ptr<EditorApp> EditorApp::create(const ayt::app::GameDesc& desc,
                                             const ayt::app::AppCommandLine& cmdLine)
{
    return std::make_unique<EditorApp>(desc, cmdLine);
}

ayt::game::GameLoop& EditorApp::getGameLoop()
{
    return ayt::game::GameLoop::instance();
}

void EditorApp::registerSubSystems()
{
    ayt::entity::bootstrapModule();
}

void EditorApp::onInit()
{
    registerSubSystems();
}

void EditorApp::onPreShutdown() {}

void EditorApp::onShutdown() {}

void EditorApp::run()
{
    attachDebugConsole();
    AY_EDITOR_HEAP_DEBUG_INIT();
    onInit();

    ayt::device::DeviceManager devices;
    ayt::device::DeviceConfig deviceConfig{};
    deviceConfig.window.title = _desc.name != nullptr ? _desc.name : "AY Editor";
    deviceConfig.window.width = static_cast<int>(_desc.width);
    deviceConfig.window.height = static_cast<int>(_desc.height);

    if (!devices.initialize(deviceConfig)) {
        std::fprintf(stderr, "[EditorApp] DeviceManager initialize failed\n");
        return;
    }

    ayt::device::WindowManager& window = devices.window();
    HWND hwnd = static_cast<HWND>(window.getWindowHandle());
    if (hwnd == nullptr) {
        std::fprintf(stderr, "[EditorApp] host window unavailable\n");
        devices.shutdown();
        return;
    }

    EditorHostState hostState{};
    hostState.clientWidth  = static_cast<int>(_desc.width);
    hostState.clientHeight = static_cast<int>(_desc.height);

    ayt::render::UIRenderBackend uiBackend;
    EditorSession session;
    hostState.session = &session;

    const std::string layoutPath = resolveLayoutPath();
    EditorSessionDesc sessionDesc{};
    sessionDesc.uiBackend = &uiBackend;
    sessionDesc.layoutPath = layoutPath;
    sessionDesc.hostWindow = hwnd;

    if (!session.initialize(sessionDesc)) {
        std::fprintf(stderr, "[EditorApp] failed to load layout: %s\n", layoutPath.c_str());
        devices.shutdown();
        return;
    }

    session.setClientSize(static_cast<float>(hostState.clientWidth),
                          static_cast<float>(hostState.clientHeight));

    if (!session.ensurePresentationReady()) {
        std::fprintf(stderr, "[EditorApp] presentation bootstrap failed\n");
        session.shutdown();
        devices.shutdown();
        return;
    }

    ayt::render::RendererSubSystem* rendererSub = ayt::render::RendererSubSystem::findRegistered();
    if (rendererSub == nullptr) {
        std::fprintf(stderr, "[EditorApp] renderer subsystem unavailable\n");
        session.shutdown();
        devices.shutdown();
        return;
    }

    if (!uiBackend.initialize(rendererSub->renderer())) {
        std::fprintf(stderr, "[EditorApp] UIRenderBackend initialize failed\n");
        uiBackend.shutdown();
        session.shutdown();
        devices.shutdown();
        return;
    }
    AY_EDITOR_HEAP_CHECK("after_full_init");

    bool running = true;
    bool loggedFirstFrameHeap = false;
    window.setWindowCloseCallback([&running]() { running = false; });
    window.setWindowMessageCallback(
        [hwnd, &hostState](unsigned msg, std::uintptr_t wParam, std::intptr_t lParam,
                           bool& handled) -> std::intptr_t {
            return handleHostMessage(hwnd, &hostState, msg, wParam, lParam, handled);
        });

    constexpr float kDeltaSeconds = 1.0f / 60.0f;
    while (running && window.isWindowValid()) {
        devices.pollEvents();
        session.update(kDeltaSeconds);
        session.syncViewportIfChanged();

        if (rendererSub != nullptr) {
            const bool renderScene = session.shouldCompositeViewport();
            rendererSub->renderCompositeFrame(
                renderScene, &uiBackend,
                [&session](bool skipViewportPanel) { session.render(skipViewportPanel); });
        }

        if (!loggedFirstFrameHeap) {
            AY_EDITOR_HEAP_CHECK("after_first_frame");
            loggedFirstFrameHeap = true;
        }

        Sleep(1);
    }

    onPreShutdown();
    hostState.session = nullptr;

    AY_EDITOR_HEAP_CHECK("before_ui_shutdown");
    session.ui().shutdown();
    if (rendererSub != nullptr) {
        rendererSub->renderer().shutdownUiRenderBackend(uiBackend);
    } else {
        uiBackend.shutdown();
    }
    session.shutdown();
    devices.shutdown();
    ayt::game::GameLoop::instance().shutdown();
    onShutdown();
}

} // namespace ayt::editor
