#include "AYEditor/EditorStartupSplash.h"
#include "AYEditor/EditorProductPaths.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <Windows.h>
#  include <objbase.h>
#  include <wincodec.h>
#endif

namespace ayt::editor {

#if defined(_WIN32)

struct EditorStartupSplash::Impl {
    static constexpr UINT kUpdateMessage = WM_APP + 0x271;
    static constexpr UINT kCloseMessage = WM_APP + 0x272;
    static constexpr wchar_t kWindowClass[] =
        L"Aliyat.Editor.StartupSplash.v1";
    static constexpr int kWidth = 560;
    static constexpr int kHeight = 260;
    static constexpr int kLogoSize = 88;

    struct PaintState {
        float progress = 0.0f;
        std::wstring stage = L"Starting editor...";
    };

    mutable std::mutex mutex;
    std::condition_variable readyCondition;
    std::thread messageThread;
    HWND window = nullptr;
    bool ready = false;
    PaintState paintState;
    std::filesystem::path logoPath;
    HBITMAP logoBitmap = nullptr;
    UINT logoWidth = 0;
    UINT logoHeight = 0;

    Impl()
        : logoPath(EditorProductPaths::detect().engineAsset(
              std::filesystem::path("AYLogo") / "splash"
              / "splash-symbol_88px.png"))
    {
    }

    ~Impl()
    {
        releaseLogoBitmap();
    }

    void releaseLogoBitmap()
    {
        if (logoBitmap != nullptr) {
            ::DeleteObject(logoBitmap);
            logoBitmap = nullptr;
        }
        logoWidth = 0;
        logoHeight = 0;
    }

    bool loadLogoBitmap()
    {
        releaseLogoBitmap();
        if (logoPath.empty()) return false;

        IWICImagingFactory* factory = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;
        const auto releaseInterfaces = [&]() {
            if (converter != nullptr) converter->Release();
            if (frame != nullptr) frame->Release();
            if (decoder != nullptr) decoder->Release();
            if (factory != nullptr) factory->Release();
        };

        HRESULT result = ::CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));
        if (SUCCEEDED(result)) {
            result = factory->CreateDecoderFromFilename(
                logoPath.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnLoad, &decoder);
        }
        if (SUCCEEDED(result)) result = decoder->GetFrame(0, &frame);
        if (SUCCEEDED(result)) result = frame->GetSize(&logoWidth, &logoHeight);
        if (SUCCEEDED(result)
            && (logoWidth == 0 || logoHeight == 0
                || logoWidth > 4096 || logoHeight > 4096)) {
            result = E_INVALIDARG;
        }
        if (SUCCEEDED(result)) result = factory->CreateFormatConverter(&converter);
        if (SUCCEEDED(result)) {
            // GDI AlphaBlend expects premultiplied BGRA for AC_SRC_ALPHA.
            result = converter->Initialize(
                frame, GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0,
                WICBitmapPaletteTypeCustom);
        }

        void* pixels = nullptr;
        if (SUCCEEDED(result)) {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = static_cast<LONG>(logoWidth);
            // A negative height creates a top-down DIB matching WIC rows.
            info.bmiHeader.biHeight = -static_cast<LONG>(logoHeight);
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            logoBitmap = ::CreateDIBSection(
                nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
            if (logoBitmap == nullptr || pixels == nullptr) {
                result = E_OUTOFMEMORY;
            }
        }
        if (SUCCEEDED(result)) {
            const UINT stride = logoWidth * 4u;
            result = converter->CopyPixels(
                nullptr, stride, stride * logoHeight,
                static_cast<BYTE*>(pixels));
        }

        releaseInterfaces();
        if (FAILED(result)) {
            releaseLogoBitmap();
            std::fwprintf(stderr,
                L"[EditorStartupSplash] logo unavailable: %ls (0x%08lx)\n",
                logoPath.c_str(), static_cast<unsigned long>(result));
            return false;
        }
        return true;
    }

    bool drawLogo(HDC target) const
    {
        if (target == nullptr || logoBitmap == nullptr
            || logoWidth == 0 || logoHeight == 0) {
            return false;
        }
        HDC source = ::CreateCompatibleDC(target);
        if (source == nullptr) return false;
        HGDIOBJ oldBitmap = ::SelectObject(source, logoBitmap);
        const BLENDFUNCTION blend{
            AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        const BOOL drawn = ::AlphaBlend(
            target, 52, 40, kLogoSize, kLogoSize,
            source, 0, 0,
            static_cast<int>(logoWidth), static_cast<int>(logoHeight), blend);
        ::SelectObject(source, oldBitmap);
        ::DeleteDC(source);
        return drawn != FALSE;
    }

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message,
                                       WPARAM wParam, LPARAM lParam)
    {
        Impl* self = reinterpret_cast<Impl*>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<Impl*>(create->lpCreateParams);
            ::SetWindowLongPtrW(
                hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }

        // B-9 (ayeditor audit 2026-09-14): a window can receive messages
        // before WM_NCCREATE finishes — for example, if the OS dispatches a
        // pending WM_PAINT or WM_DESTROY while the CREATESTRUCT has not yet
        // been stored. Guard every handler that dereferences `self` and fall
        // back to a safe return so a null self cannot crash the message
        // thread. Paint and destroy were the two paths that already locked
        // `self->mutex`; both now also gate on self != nullptr.
        switch (message) {
        case kUpdateMessage:
            if (self == nullptr) return 0;
            ::InvalidateRect(hwnd, nullptr, FALSE);
            ::UpdateWindow(hwnd);
            return 0;

        case kCloseMessage:
            ::DestroyWindow(hwnd);
            return 0;

        case WM_ERASEBKGND:
            return TRUE;

        case WM_PAINT:
            if (self != nullptr) self->paint(hwnd);
            return 0;

        case WM_NCHITTEST:
            // A borderless startup window can still be moved out of the way.
            return HTCAPTION;

        case WM_DESTROY:
            if (self != nullptr) {
                std::lock_guard<std::mutex> lock(self->mutex);
                self->window = nullptr;
            }
            ::PostQuitMessage(0);
            return 0;

        default:
            return ::DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }

    PaintState snapshot() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return paintState;
    }

    static HFONT createFont(int pixelHeight, int weight)
    {
        return ::CreateFontW(
            -pixelHeight, 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }

    void paint(HWND hwnd)
    {
        PAINTSTRUCT ps{};
        HDC target = ::BeginPaint(hwnd, &ps);
        if (target == nullptr) return;

        RECT client{};
        ::GetClientRect(hwnd, &client);
        const int width = std::max(1L, client.right - client.left);
        const int height = std::max(1L, client.bottom - client.top);

        HDC buffer = ::CreateCompatibleDC(target);
        HBITMAP bitmap = ::CreateCompatibleBitmap(target, width, height);
        HGDIOBJ oldBitmap = ::SelectObject(buffer, bitmap);

        const PaintState state = snapshot();
        HBRUSH background = ::CreateSolidBrush(RGB(20, 22, 27));
        HBRUSH panel = ::CreateSolidBrush(RGB(25, 28, 35));
        HBRUSH accent = ::CreateSolidBrush(RGB(49, 126, 218));
        HBRUSH track = ::CreateSolidBrush(RGB(47, 51, 61));
        HBRUSH border = ::CreateSolidBrush(RGB(61, 66, 78));
        ::FillRect(buffer, &client, background);

        RECT accentEdge{0, 0, 6, height};
        ::FillRect(buffer, &accentEdge, accent);
        RECT body{28, 22, width - 28, height - 22};
        ::FillRect(buffer, &body, panel);
        ::FrameRect(buffer, &body, border);

        ::SetBkMode(buffer, TRANSPARENT);
        HFONT eyebrowFont = createFont(12, FW_SEMIBOLD);
        HFONT titleFont = createFont(31, FW_SEMIBOLD);
        HFONT statusFont = createFont(14, FW_NORMAL);
        HFONT percentFont = createFont(12, FW_SEMIBOLD);

        const bool hasLogo = drawLogo(buffer);
        const LONG brandTextLeft = hasLogo ? 154 : 52;
        RECT eyebrow{brandTextLeft, 42, width - 52, 64};
        HGDIOBJ oldFont = ::SelectObject(buffer, eyebrowFont);
        ::SetTextColor(buffer, RGB(105, 164, 235));
        ::DrawTextW(buffer, L"ALIYAT ENGINE", -1, &eyebrow,
                    DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        RECT title{brandTextLeft - 2, 66, width - 50, 112};
        ::SelectObject(buffer, titleFont);
        ::SetTextColor(buffer, RGB(235, 238, 244));
        ::DrawTextW(buffer, L"AY Editor", -1, &title,
                    DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        RECT status{52, 131, width - 114, 158};
        ::SelectObject(buffer, statusFont);
        ::SetTextColor(buffer, RGB(171, 177, 190));
        ::DrawTextW(buffer, state.stage.c_str(), -1, &status,
                    DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        const int percent = static_cast<int>(
            state.progress * 100.0f + 0.5f);
        wchar_t percentText[16]{};
        std::swprintf(percentText, sizeof(percentText) / sizeof(wchar_t),
                      L"%d%%", percent);
        RECT percentRect{width - 112, 131, width - 52, 158};
        ::SelectObject(buffer, percentFont);
        ::SetTextColor(buffer, RGB(210, 215, 225));
        ::DrawTextW(buffer, percentText, -1, &percentRect,
                    DT_RIGHT | DT_SINGLELINE | DT_VCENTER);

        RECT progressTrack{52, 174, width - 52, 184};
        ::FillRect(buffer, &progressTrack, track);
        RECT progressFill = progressTrack;
        progressFill.right = progressFill.left
            + static_cast<LONG>((progressTrack.right - progressTrack.left)
                                * state.progress);
        if (progressFill.right > progressFill.left) {
            ::FillRect(buffer, &progressFill, accent);
        }

        RECT footer{52, 199, width - 52, 222};
        ::SelectObject(buffer, eyebrowFont);
        ::SetTextColor(buffer, RGB(104, 110, 124));
        ::DrawTextW(buffer, L"Preparing the editor workspace", -1, &footer,
                    DT_LEFT | DT_SINGLELINE | DT_VCENTER);

        ::BitBlt(target, 0, 0, width, height, buffer, 0, 0, SRCCOPY);

        ::SelectObject(buffer, oldFont);
        ::SelectObject(buffer, oldBitmap);
        ::DeleteObject(eyebrowFont);
        ::DeleteObject(titleFont);
        ::DeleteObject(statusFont);
        ::DeleteObject(percentFont);
        ::DeleteObject(background);
        ::DeleteObject(panel);
        ::DeleteObject(accent);
        ::DeleteObject(track);
        ::DeleteObject(border);
        ::DeleteObject(bitmap);
        ::DeleteDC(buffer);
        ::EndPaint(hwnd, &ps);
    }

    void runMessageThread()
    {
        const HRESULT comResult = ::CoInitializeEx(
            nullptr, COINIT_APARTMENTTHREADED);
        const bool uninitializeCom = SUCCEEDED(comResult);
        if (SUCCEEDED(comResult)) {
            (void)loadLogoBitmap();
        } else if (!logoPath.empty()) {
            std::fwprintf(stderr,
                L"[EditorStartupSplash] WIC unavailable for logo: 0x%08lx\n",
                static_cast<unsigned long>(comResult));
        }
        struct ThreadCleanup {
            Impl* impl = nullptr;
            bool uninitializeCom = false;
            ~ThreadCleanup()
            {
                if (impl != nullptr) impl->releaseLogoBitmap();
                if (uninitializeCom) ::CoUninitialize();
            }
        } cleanup{this, uninitializeCom};

        const HINSTANCE instance = ::GetModuleHandleW(nullptr);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.hInstance = instance;
        // This target does not globally define UNICODE, so IDC_ARROW expands
        // to MAKEINTRESOURCEA even though the explicit API is LoadCursorW.
        windowClass.hCursor =
            ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        windowClass.hbrBackground = nullptr;
        windowClass.lpfnWndProc = &Impl::windowProc;
        windowClass.lpszClassName = kWindowClass;
        const ATOM registered = ::RegisterClassExW(&windowClass);
        if (registered == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            signalReady(nullptr);
            return;
        }

        POINT origin{0, 0};
        const HMONITOR monitor = ::MonitorFromPoint(
            origin, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        ::GetMonitorInfoW(monitor, &monitorInfo);
        const RECT work = monitorInfo.rcWork;
        const int x = work.left + ((work.right - work.left) - kWidth) / 2;
        const int y = work.top + ((work.bottom - work.top) - kHeight) / 2;

        HWND hwnd = ::CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            kWindowClass, L"AY Editor is starting", WS_POPUP,
            x, y, kWidth, kHeight, nullptr, nullptr, instance, this);
        if (hwnd == nullptr) {
            signalReady(nullptr);
            return;
        }

        ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        ::UpdateWindow(hwnd);
        signalReady(hwnd);

        MSG message{};
        while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }

    void signalReady(HWND hwnd)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            window = hwnd;
            ready = true;
        }
        readyCondition.notify_all();
    }
};

#else

struct EditorStartupSplash::Impl {};

#endif

EditorStartupSplash::EditorStartupSplash()
    : _impl(std::make_unique<Impl>())
{
}

EditorStartupSplash::~EditorStartupSplash()
{
    close();
}

bool EditorStartupSplash::show()
{
#if defined(_WIN32)
    if (_impl->messageThread.joinable()) return isVisible();
    {
        // close() joins the previous thread but deliberately remains
        // idempotent. Reset the startup latch here so a host that reuses the
        // object cannot observe stale readiness before the new HWND exists.
        std::lock_guard<std::mutex> lock(_impl->mutex);
        _impl->ready = false;
        _impl->window = nullptr;
    }
    _impl->messageThread = std::thread(
        [impl = _impl.get()]() { impl->runMessageThread(); });
    std::unique_lock<std::mutex> lock(_impl->mutex);
    _impl->readyCondition.wait(lock, [this]() { return _impl->ready; });
    return _impl->window != nullptr;
#else
    return false;
#endif
}

void EditorStartupSplash::update(float progress, std::wstring_view stage)
{
#if defined(_WIN32)
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        _impl->paintState.progress = std::clamp(progress, 0.0f, 1.0f);
        _impl->paintState.stage.assign(stage.begin(), stage.end());
        hwnd = _impl->window;
    }
    if (hwnd != nullptr) {
        ::PostMessageW(hwnd, Impl::kUpdateMessage, 0, 0);
    }
#else
    (void)progress;
    (void)stage;
#endif
}

void EditorStartupSplash::close()
{
#if defined(_WIN32)
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        hwnd = _impl->window;
    }
    if (hwnd != nullptr) {
        ::PostMessageW(hwnd, Impl::kCloseMessage, 0, 0);
    }
    if (_impl->messageThread.joinable()) {
        _impl->messageThread.join();
    }
#endif
}

bool EditorStartupSplash::isVisible() const noexcept
{
#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(_impl->mutex);
    return _impl->window != nullptr;
#else
    return false;
#endif
}

} // namespace ayt::editor
