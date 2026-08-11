#include "GdiRenderBackend.h"
#include "aymath/MathDefs.h"

// PR-Dock-TearOff: GDI backend for EditorChildWindowManager promoted
// windows. Keep in lockstep with AYUI/demo/GalleryChildBackend.cpp.
#if defined(_WIN32)

namespace ayt::editor {

using namespace ayt::math;

namespace {

UINT horizontalAlign(ayt::ui::IRenderBackend::TextStyle::Align align) {
    switch (align) {
    case ayt::ui::IRenderBackend::TextStyle::Align::Center: return DT_CENTER;
    case ayt::ui::IRenderBackend::TextStyle::Align::Right:  return DT_RIGHT;
    default: return DT_LEFT;
    }
}

} // namespace

GdiRenderBackend::GdiRenderBackend(HWND hwnd)
    : _hwnd(hwnd) {
}

GdiRenderBackend::~GdiRenderBackend() {
    releaseBackbuffer();
    if (_font != nullptr) {
        DeleteObject(_font);
        _font = nullptr;
    }
}

void GdiRenderBackend::releaseBackbuffer() {
    if (_memDc != nullptr) {
        if (_oldBitmap != nullptr) {
            SelectObject(_memDc, _oldBitmap);
            _oldBitmap = nullptr;
        }
        DeleteDC(_memDc);
        _memDc = nullptr;
    }
    if (_bitmap != nullptr) {
        DeleteObject(_bitmap);
        _bitmap = nullptr;
    }
    _bbWidth = 0;
    _bbHeight = 0;
    _hdc = nullptr;
}

void GdiRenderBackend::ensureBackbuffer(int width, int height) {
    if (width < 1 || height < 1 || _windowDc == nullptr) {
        return;
    }
    if (_memDc != nullptr && _bitmap != nullptr
        && _bbWidth == width && _bbHeight == height) {
        _hdc = _memDc;
        return;
    }
    releaseBackbuffer();
    _memDc = CreateCompatibleDC(_windowDc);
    if (_memDc == nullptr) {
        return;
    }
    _bitmap = CreateCompatibleBitmap(_windowDc, width, height);
    if (_bitmap == nullptr) {
        DeleteDC(_memDc);
        _memDc = nullptr;
        return;
    }
    _oldBitmap = static_cast<HBITMAP>(SelectObject(_memDc, _bitmap));
    _bbWidth = width;
    _bbHeight = height;
    _hdc = _memDc;
}

void GdiRenderBackend::setDrawTarget(HDC hdc, int width, int height) {
    _windowDc = hdc;
    _width = width;
    _height = height;
    ensureBackbuffer(width, height);
}

void GdiRenderBackend::beginFrame() {
}

void GdiRenderBackend::endFrame() {
    if (_windowDc != nullptr && _memDc != nullptr && _width > 0 && _height > 0) {
        BitBlt(_windowDc, 0, 0, _width, _height, _memDc, 0, 0, SRCCOPY);
    }
}

void GdiRenderBackend::beginCanvas(const math::FRectangle& viewport) {
    if (!_hdc) {
        return;
    }
    drawRect(viewport, math::FVector4(0.10f, 0.10f, 0.11f, 1.0f));
}

void GdiRenderBackend::endCanvas() {
}

COLORREF GdiRenderBackend::toColorRef(const math::FVector4& color) {
    const int r = static_cast<int>(color.x * 255.0f);
    const int g = static_cast<int>(color.y * 255.0f);
    const int b = static_cast<int>(color.z * 255.0f);
    return RGB(r, g, b);
}

RECT GdiRenderBackend::toRect(const math::FRectangle& bounds) const {
    RECT rect{};
    rect.left = static_cast<LONG>(bounds.minX);
    rect.top = static_cast<LONG>(bounds.minY);
    rect.right = static_cast<LONG>(bounds.maxX);
    rect.bottom = static_cast<LONG>(bounds.maxY);
    return rect;
}

HFONT GdiRenderBackend::fontForSize(int fontSize) {
    if (fontSize < 1) {
        fontSize = 12;
    }
    if (_font != nullptr && _fontSize == fontSize) {
        return _font;
    }
    if (_font != nullptr) {
        DeleteObject(_font);
        _font = nullptr;
    }
    _font = CreateFontW(
        fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    _fontSize = fontSize;
    return _font;
}

void GdiRenderBackend::drawRect(const math::FRectangle& bounds, const math::FVector4& color) {
    if (!_hdc) {
        return;
    }

    HBRUSH brush = CreateSolidBrush(toColorRef(color));
    RECT rect = toRect(bounds);
    FillRect(_hdc, &rect, brush);
    DeleteObject(brush);
}

void GdiRenderBackend::drawRect(const math::FRectangle& bounds, void* textureHandle,
                                const math::FRectangle& uv) {
    AYUNREFERENCED_PARAM(textureHandle);
    AYUNREFERENCED_PARAM(uv);
    drawRect(bounds, math::FVector4(0.25f, 0.25f, 0.28f, 1.0f));
}

void GdiRenderBackend::drawText(const math::FRectangle& bounds, const std::wstring& text,
                                int fontSize, const math::FVector4& color) {
    if (!_hdc || text.empty()) {
        return;
    }

    SetBkMode(_hdc, TRANSPARENT);
    SetTextColor(_hdc, toColorRef(color));

    HFONT font = fontForSize(fontSize);
    HGDIOBJ oldFont = font ? SelectObject(_hdc, font) : nullptr;
    RECT rect = toRect(bounds);
    const UINT align = (text.size() == 1)
        ? DT_CENTER
        : horizontalAlign(ayt::ui::IRenderBackend::TextStyle::Align::Left);
    DrawTextW(_hdc, text.c_str(), static_cast<int>(text.size()), &rect,
              DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | align);
    if (oldFont != nullptr) {
        SelectObject(_hdc, oldFont);
    }
}

void GdiRenderBackend::drawWithAlpha(const math::FRectangle& bounds, void* textureHandle,
                                     float alpha) {
    AYUNREFERENCED_PARAM(alpha);
    drawRect(bounds, textureHandle, math::FRectangle(0.0f, 0.0f, 1.0f, 1.0f));
}

} // namespace ayt::editor

#endif // _WIN32
