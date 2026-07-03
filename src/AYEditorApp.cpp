#include "AYEditorApp.h"

#include "AYEditorSession.h"
#include "AYGameLoop.h"
#include "AYDeviceManager.h"
#include "GdiRenderBackend.h"

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

constexpr UINT kTimerId = 1;
constexpr UINT kTimerMs = 16;

struct EditorHostState {
    EditorSession* session = nullptr;
    GdiRenderBackend* backend = nullptr;
    int clientWidth = 0;
    int clientHeight = 0;
};

bool fileExists(const std::string& path)
{
    struct stat st;
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
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

void paintEditorUi(HDC targetDc, EditorHostState* state)
{
    if (state == nullptr || state->session == nullptr || state->backend == nullptr
        || targetDc == nullptr) {
        return;
    }

    const int width  = state->clientWidth;
    const int height = state->clientHeight;
    if (width <= 0 || height <= 0) {
        return;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = width;
    bmi.bmiHeader.biHeight      = -height;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC memDc = CreateCompatibleDC(targetDc);
    HBITMAP memBitmap = CreateDIBSection(memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (memBitmap == nullptr || bits == nullptr) {
        DeleteDC(memDc);
        return;
    }

    HGDIOBJ oldBitmap = SelectObject(memDc, memBitmap);

    HBRUSH bgBrush = CreateSolidBrush(RGB(26, 26, 28));
    RECT fillRect{0, 0, width, height};
    FillRect(memDc, &fillRect, bgBrush);
    DeleteObject(bgBrush);

    state->backend->setDrawTarget(memDc, width, height);
    state->session->render();
    BitBlt(targetDc, 0, 0, width, height, memDc, 0, 0, SRCCOPY);

    SelectObject(memDc, oldBitmap);
    DeleteObject(memBitmap);
    DeleteDC(memDc);
}

void requestEditorRepaint(HWND hwnd)
{
    if (hwnd != nullptr) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

std::intptr_t handleHostMessage(HWND hwnd, EditorHostState* state, unsigned msg,
                                std::uintptr_t wParam, std::intptr_t lParam, bool& handled)
{
    handled = false;
    if (hwnd == nullptr || state == nullptr) {
        return 0;
    }

    switch (msg) {
    case WM_SIZE:
        state->clientWidth  = LOWORD(lParam);
        state->clientHeight = HIWORD(lParam);
        if (state->session != nullptr) {
            state->session->setClientSize(static_cast<float>(state->clientWidth),
                                        static_cast<float>(state->clientHeight));
        }
        requestEditorRepaint(hwnd);
        handled = true;
        return 0;
    case WM_TIMER:
        if (wParam == kTimerId && state->session != nullptr) {
            state->session->update(0.016f);
            requestEditorRepaint(hwnd);
        }
        handled = true;
        return 0;
    case WM_ERASEBKGND:
        handled = true;
        return 1;
    case WM_MOUSEMOVE:
        if (state->session != nullptr) {
            const float x = static_cast<float>(GET_X_LPARAM(lParam));
            const float y = static_cast<float>(GET_Y_LPARAM(lParam));
            if (state->session->onMouseMove(x, y)) {
                requestEditorRepaint(hwnd);
            }
        }
        handled = true;
        return 0;
    case WM_LBUTTONDOWN:
        if (state->session != nullptr) {
            const float x = static_cast<float>(GET_X_LPARAM(lParam));
            const float y = static_cast<float>(GET_Y_LPARAM(lParam));
            if (state->session->onMouseButtonDown(x, y, 0)) {
                SetCapture(hwnd);
                requestEditorRepaint(hwnd);
            }
        }
        handled = true;
        return 0;
    case WM_LBUTTONUP:
        if (state->session != nullptr) {
            const float x = static_cast<float>(GET_X_LPARAM(lParam));
            const float y = static_cast<float>(GET_Y_LPARAM(lParam));
            if (state->session->onMouseButtonUp(x, y, 0)) {
                requestEditorRepaint(hwnd);
            }
            ReleaseCapture();
        }
        handled = true;
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        if (hdc) {
            paintEditorUi(hdc, state);
            EndPaint(hwnd, &ps);
        }
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

void EditorApp::onInit() {}

void EditorApp::onPreShutdown() {}

void EditorApp::onShutdown() {}

void EditorApp::run()
{
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

    GdiRenderBackend backend(hwnd);
    EditorSession session;
    hostState.backend = &backend;
    hostState.session = &session;

    const std::string layoutPath = resolveLayoutPath();
    EditorSessionDesc sessionDesc{};
    sessionDesc.uiBackend = &backend;
    sessionDesc.layoutPath = layoutPath;
    sessionDesc.hostWindow = hwnd;
    sessionDesc.windowManager = &window;

    if (!session.initialize(sessionDesc)) {
        std::fprintf(stderr, "[EditorApp] failed to load layout: %s\n", layoutPath.c_str());
        devices.shutdown();
        return;
    }

    session.setClientSize(static_cast<float>(hostState.clientWidth),
                          static_cast<float>(hostState.clientHeight));
    session.setRepaintCallback([hwnd]() { requestEditorRepaint(hwnd); });

    bool running = true;
    window.setWindowCloseCallback([&running]() { running = false; });
    window.setWindowMessageCallback(
        [hwnd, &hostState](unsigned msg, std::uintptr_t wParam, std::intptr_t lParam,
                           bool& handled) -> std::intptr_t {
            return handleHostMessage(hwnd, &hostState, msg, wParam, lParam, handled);
        });

    SetTimer(hwnd, kTimerId, kTimerMs, nullptr);
    requestEditorRepaint(hwnd);

    while (running && window.isWindowValid()) {
        devices.pollEvents();
    }

    KillTimer(hwnd, kTimerId);
    onPreShutdown();
    session.shutdown();
    devices.shutdown();
    ayt::game::GameLoop::instance().shutdown();
    onShutdown();
}

} // namespace ayt::editor
