#pragma once

// GDI per-HWND render backend for Editor child windows (DockCard promote).
// Draws into an offscreen bitmap and BitBlts once per frame to avoid
// front-buffer flicker (keep in lockstep with AYUI demo GalleryChildBackend).

#include "aymath/MathDefs.h"
#include "IAYRenderBackend.h"
#include "aymath/MathTypes.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <Windows.h>

namespace ayt::editor {

using namespace ayt::math;

class GdiRenderBackend : public ayt::ui::IRenderBackend {
public:
    explicit GdiRenderBackend(HWND hwnd);
    ~GdiRenderBackend() override;

    void setDrawTarget(HDC hdc, int width, int height);

    void beginFrame() override;
    void endFrame() override;
    void beginCanvas(const math::FRectangle& viewport) override;
    void endCanvas() override;

    void drawRect(const math::FRectangle& bounds, const math::FVector4& color) override;
    void drawRect(const math::FRectangle& bounds, void* textureHandle,
                  const math::FRectangle& uv) override;
    void drawText(const math::FRectangle& bounds, const std::wstring& text, int fontSize,
                  const math::FVector4& color) override;
    void drawWithAlpha(const math::FRectangle& bounds, void* textureHandle, float alpha) override;

private:
    static COLORREF toColorRef(const math::FVector4& color);
    RECT toRect(const math::FRectangle& bounds) const;
    void ensureBackbuffer(int width, int height);
    void releaseBackbuffer();
    HFONT fontForSize(int fontSize);

    HWND _hwnd = nullptr;
    HDC _windowDc = nullptr;
    HDC _hdc = nullptr;
    HDC _memDc = nullptr;
    HBITMAP _bitmap = nullptr;
    HBITMAP _oldBitmap = nullptr;
    HFONT _font = nullptr;
    int _fontSize = 0;
    int _width = 0;
    int _height = 0;
    int _bbWidth = 0;
    int _bbHeight = 0;
};

} // namespace ayt::editor
