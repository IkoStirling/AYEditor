#pragma once

// GDI per-HWND render backend for Editor child windows (DockCard promote).
// Draws into an offscreen bitmap and BitBlts once per frame to avoid
// front-buffer flicker (keep in lockstep with AYUI demo GalleryChildBackend).

#include "AYMath/MathDefs.h"
#include "AYUI/IRenderBackend.h"
#include "AYMath/MathTypes.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <Windows.h>
#include <memory>
#include <unordered_map>
#include <vector>

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

    // IRenderBackend clips are part of the widget-container contract.  GDI
    // does not inherit them from the interface (the defaults are no-ops), so
    // keep a SaveDC/RestoreDC stack for nested ScrollView/ListView clips.
    void pushClip(const math::FRectangle& bounds) override;
    void popClip() override;
    void pushTransform(const math::Float4x4& transform) override;
    void popTransform() override;

    // Designer image-preview textures. Handles are local to this child-window
    // backend and stay valid until releaseUiTexture or backend destruction.
    void* createUiTexture(int width, int height, const void* bgraPixels);
    void releaseUiTexture(void* textureHandle);
    [[nodiscard]] bool ownsUiTexture(void* textureHandle) const noexcept {
        return _textures.find(textureHandle) != _textures.end();
    }

    PathHandle createPath() override;
    void releasePath(PathHandle path) override;
    void addPathContour(PathHandle path, const math::FVector2* points,
                        int count, bool closed,
                        ayt::ui::PathWinding winding) override;
    void setPathFillColor(PathHandle path, const math::FVector4& color) override;
    void setPathStrokeColor(PathHandle path, const math::FVector4& color) override;
    void setPathStrokeWidth(PathHandle path, float width) override;
    void setPathStrokeStyle(PathHandle path, ayt::ui::PathStrokeCap cap,
                            ayt::ui::PathStrokeJoin join,
                            float miterLimit) override;
    void drawPath(PathHandle path,
                  ayt::ui::PathFillMode mode) override;

private:
    static COLORREF toColorRef(const math::FVector4& color);
    RECT toRect(const math::FRectangle& bounds) const;
    void ensureBackbuffer(int width, int height);
    void releaseBackbuffer();
    HFONT fontForSize(int fontSize);

    struct PathContour {
        std::vector<POINT> points;
        bool closed = false;
    };
    struct PathState {
        std::vector<PathContour> contours;
        COLORREF fillColor = RGB(255, 255, 255);
        COLORREF strokeColor = RGB(255, 255, 255);
        float strokeWidth = 1.0f;
        ayt::ui::PathStrokeCap cap = ayt::ui::PathStrokeCap::Butt;
        ayt::ui::PathStrokeJoin join = ayt::ui::PathStrokeJoin::Miter;
        float miterLimit = 4.0f;
    };
    struct TextureState {
        HBITMAP bitmap = nullptr;
        int width = 0;
        int height = 0;
    };

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
    int _nextPathId = 1;
    enum class SavedStateKind { Clip, Transform };
    struct SavedDcState {
        int state = 0;
        SavedStateKind kind = SavedStateKind::Clip;
    };
    std::vector<SavedDcState> _savedStates;
    std::unordered_map<int, PathState> _paths;
    std::unordered_map<void*, std::unique_ptr<TextureState>> _textures;
};

} // namespace ayt::editor
