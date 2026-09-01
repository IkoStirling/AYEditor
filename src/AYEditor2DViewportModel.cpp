#include "AYEditor/Editor2DViewportModel.h"

#include <algorithm>
#include <cmath>

namespace ayt::editor
{

void Editor2DViewportModel::setViewport(uint32_t widthPx,
                                        uint32_t heightPx) noexcept
{
    _widthPx = std::max(1u, widthPx);
    _heightPx = std::max(1u, heightPx);
}

void Editor2DViewportModel::setCamera(ayt::math::FVector2 center,
                                      float verticalWorldSize) noexcept
{
    _center = center;
    _viewHeight = std::max(0.001f, verticalWorldSize);
}

void Editor2DViewportModel::setGrid(float tileWidth, float tileHeight,
                                    ayt::math::FVector2 origin) noexcept
{
    _gridSize.x = std::max(0.001f, tileWidth);
    _gridSize.y = std::max(0.001f, tileHeight);
    _gridOrigin = origin;
}

ayt::math::FVector2 Editor2DViewportModel::screenToWorld(
    ayt::math::FVector2 screen) const noexcept
{
    const float unitsPerPixel = _viewHeight / static_cast<float>(_heightPx);
    return ayt::math::FVector2{
        _center.x + (screen.x - static_cast<float>(_widthPx) * 0.5f)
                    * unitsPerPixel,
        _center.y - (screen.y - static_cast<float>(_heightPx) * 0.5f)
                    * unitsPerPixel};
}

ayt::math::FVector2 Editor2DViewportModel::worldToScreen(
    ayt::math::FVector2 world) const noexcept
{
    const float pixelsPerUnit = static_cast<float>(_heightPx) / _viewHeight;
    return ayt::math::FVector2{
        static_cast<float>(_widthPx) * 0.5f
            + (world.x - _center.x) * pixelsPerUnit,
        static_cast<float>(_heightPx) * 0.5f
            - (world.y - _center.y) * pixelsPerUnit};
}

EditorTileCell Editor2DViewportModel::worldToCell(
    ayt::math::FVector2 world) const noexcept
{
    return EditorTileCell{
        static_cast<int32_t>(std::floor((world.x - _gridOrigin.x) / _gridSize.x)),
        static_cast<int32_t>(std::floor((world.y - _gridOrigin.y) / _gridSize.y))};
}

ayt::math::FVector2 Editor2DViewportModel::snapToGrid(
    ayt::math::FVector2 world) const noexcept
{
    const EditorTileCell cell = worldToCell(world);
    return ayt::math::FVector2{
        _gridOrigin.x + static_cast<float>(cell.x) * _gridSize.x,
        _gridOrigin.y + static_cast<float>(cell.y) * _gridSize.y};
}

void Editor2DViewportModel::panPixels(float dx, float dy) noexcept
{
    const float unitsPerPixel = _viewHeight / static_cast<float>(_heightPx);
    _center.x -= dx * unitsPerPixel;
    _center.y += dy * unitsPerPixel;
}

void Editor2DViewportModel::zoomAt(
    float factor, ayt::math::FVector2 screenAnchor) noexcept
{
    if (factor <= 0.0f) return;
    const ayt::math::FVector2 before = screenToWorld(screenAnchor);
    _viewHeight = std::clamp(_viewHeight / factor, 0.001f, 10000000.0f);
    const ayt::math::FVector2 after = screenToWorld(screenAnchor);
    _center += before - after;
}

} // namespace ayt::editor
