#pragma once

#include <AYMath/MathTypes.h>

#include <cstdint>

namespace ayt::editor
{

struct EditorTileCell {
    int32_t x = 0;
    int32_t y = 0;
};

// UI-independent orthographic viewport math. Screen coordinates are top-left
// origin; world coordinates are Y-up. AYUI only binds inputs/visuals to it.
class Editor2DViewportModel {
public:
    void setViewport(uint32_t widthPx, uint32_t heightPx) noexcept;
    void setCamera(ayt::math::FVector2 center, float verticalWorldSize) noexcept;
    void setGrid(float tileWidth, float tileHeight,
                 ayt::math::FVector2 origin = {0.0f, 0.0f}) noexcept;

    [[nodiscard]] ayt::math::FVector2 screenToWorld(
        ayt::math::FVector2 screen) const noexcept;
    [[nodiscard]] ayt::math::FVector2 worldToScreen(
        ayt::math::FVector2 world) const noexcept;
    [[nodiscard]] EditorTileCell worldToCell(
        ayt::math::FVector2 world) const noexcept;
    [[nodiscard]] ayt::math::FVector2 snapToGrid(
        ayt::math::FVector2 world) const noexcept;

    void panPixels(float dx, float dy) noexcept;
    void zoomAt(float factor, ayt::math::FVector2 screenAnchor) noexcept;

    [[nodiscard]] ayt::math::FVector2 center() const noexcept { return _center; }
    [[nodiscard]] float verticalWorldSize() const noexcept { return _viewHeight; }

private:
    uint32_t _widthPx = 1;
    uint32_t _heightPx = 1;
    ayt::math::FVector2 _center{0.0f, 0.0f};
    float _viewHeight = 720.0f;
    ayt::math::FVector2 _gridSize{32.0f, 32.0f};
    ayt::math::FVector2 _gridOrigin{0.0f, 0.0f};
};

} // namespace ayt::editor
