#include "AYEditorTilemapCanvas.h"

#include <AYUI/IRenderBackend.h>

#include <algorithm>
#include <cmath>

namespace ayt::editor {
namespace {

using ayt::ay2d::editor::PaintTool;
using ayt::ay2d::editor::TileCell;
using ayt::ay2d::editor::TilemapDocument;

ayt::math::FRectangle normalizedRect(ayt::math::FVector2 a,
                                     ayt::math::FVector2 b,
                                     ayt::math::FVector2 offset)
{
    return {offset.x + std::min(a.x, b.x),
            offset.y + std::min(a.y, b.y),
            offset.x + std::max(a.x, b.x),
            offset.y + std::max(a.y, b.y)};
}

ayt::math::FRectangle insetRect(const ayt::math::FRectangle& rect,
                                float inset)
{
    return {rect.minX + inset, rect.minY + inset,
            rect.maxX - inset, rect.maxY - inset};
}

ayt::math::FVector4 unpackRgba(uint32_t rgba)
{
    return {static_cast<float>((rgba >> 24u) & 0xffu) / 255.0f,
            static_cast<float>((rgba >> 16u) & 0xffu) / 255.0f,
            static_cast<float>((rgba >> 8u) & 0xffu) / 255.0f,
            static_cast<float>(rgba & 0xffu) / 255.0f};
}

ayt::math::FVector4 placeholderColor(uint32_t tileId)
{
    if (tileId == 0u) return {0.105f, 0.115f, 0.135f, 1.0f};
    const uint32_t hash = tileId * 2654435761u;
    return {
        std::min(0.26f + static_cast<float>(hash & 0xffu) / 255.0f * 0.46f,
                 0.78f),
        std::min(0.26f
            + static_cast<float>((hash >> 8u) & 0xffu) / 255.0f * 0.46f,
                 0.78f),
        std::min(0.26f
            + static_cast<float>((hash >> 16u) & 0xffu) / 255.0f * 0.46f,
                 0.78f),
        1.0f};
}

ayt::math::FVector4 previewColor(const TilemapDocument& document,
                                 uint32_t tileId)
{
    const auto* asset = document.tileAsset(tileId);
    ayt::math::FVector4 base = asset != nullptr && asset->previewRgba != 0u
        ? unpackRgba(asset->previewRgba) : placeholderColor(tileId);
    if (asset == nullptr || asset->tintRgba == 0xffffffffu) return base;
    const ayt::math::FVector4 tint = unpackRgba(asset->tintRgba);
    const float strength = tint.w;
    base.x *= 1.0f + (tint.x - 1.0f) * strength;
    base.y *= 1.0f + (tint.y - 1.0f) * strength;
    base.z *= 1.0f + (tint.z - 1.0f) * strength;
    return base;
}

ayt::math::FVector4 renderTint(const TilemapDocument& document,
                               uint32_t tileId)
{
    const auto* asset = document.tileAsset(tileId);
    const ayt::math::FVector4 tint = asset != nullptr
        ? unpackRgba(asset->tintRgba)
        : ayt::math::FVector4{1.0f, 1.0f, 1.0f, 1.0f};
    return {1.0f + (tint.x - 1.0f) * tint.w,
            1.0f + (tint.y - 1.0f) * tint.w,
            1.0f + (tint.z - 1.0f) * tint.w, 1.0f};
}

ayt::math::FVector4 withAlpha(ayt::math::FVector4 color, float alpha)
{
    color.w = alpha;
    return color;
}

void drawShadowMask(ayt::ui::IRenderBackend& renderer,
                    const ayt::math::FRectangle& cell,
                    uint8_t mask, const ayt::math::FVector4& color)
{
    const float middleX = std::floor(
        (cell.minX + cell.maxX) * 0.5f + 0.5f);
    const float middleY = std::floor(
        (cell.minY + cell.maxY) * 0.5f + 0.5f);
    if ((mask & ayt::ay2d::editor::ShadowMask_TopLeft) != 0u) {
        renderer.drawRect(
            {cell.minX, cell.minY, middleX, middleY}, color);
    }
    if ((mask & ayt::ay2d::editor::ShadowMask_TopRight) != 0u) {
        renderer.drawRect(
            {middleX, cell.minY, cell.maxX, middleY}, color);
    }
    if ((mask & ayt::ay2d::editor::ShadowMask_BottomLeft) != 0u) {
        renderer.drawRect(
            {cell.minX, middleY, middleX, cell.maxY}, color);
    }
    if ((mask & ayt::ay2d::editor::ShadowMask_BottomRight) != 0u) {
        renderer.drawRect(
            {middleX, middleY, cell.maxX, cell.maxY}, color);
    }
}

} // namespace

EditorTilemapCanvas::EditorTilemapCanvas(
    ayt::ay2d::editor::TilemapEditorModel& model)
    : _model(model)
{
    setId("tilemap_workspace_canvas");
    setLayoutPositionManaged(false);
    setLayoutSizeManaged(false);
    setDisplayListPolicy(ayt::ui::DisplayListPolicy::Immediate);
}

void EditorTilemapCanvas::frameDocument()
{
    const TilemapDocument& document = _model.document();
    const float mapWidth = static_cast<float>(document.cols())
        * document.tileWidth();
    const float mapHeight = static_cast<float>(document.rows())
        * document.tileHeight();
    const float width = std::max(1.0f, getSize().x);
    const float height = std::max(1.0f, getSize().y);
    const float verticalForWidth = mapWidth / std::max(0.01f, width / height);
    _viewport.setCamera({mapWidth * 0.5f, mapHeight * 0.5f},
                        std::max(mapHeight, verticalForWidth) * 1.12f);
    _viewport.setGrid(static_cast<float>(document.tileWidth()),
                      static_cast<float>(document.tileHeight()));
    _cameraInitialized = true;
    notifyViewChanged();
    markDirty();
}

void EditorTilemapCanvas::setShowGrid(bool show)
{
    _showGrid = show;
    markDirty();
}

void EditorTilemapCanvas::setShowCollision(bool show)
{
    _showCollision = show;
    markDirty();
}

void EditorTilemapCanvas::setSpacePan(bool enabled)
{
    _spacePan = enabled;
    markDirty();
}

bool EditorTilemapCanvas::editingGestureActive() const noexcept
{
    return _drawing || _rectangleDrawing || _selectionDrawing
        || _selectionMoving || _panning;
}

ayt::math::FVector2 EditorTilemapCanvas::localPoint(
    ayt::math::FVector2 worldPoint) const
{
    const ayt::math::FRectangle bounds = getWorldBounds();
    return {worldPoint.x - bounds.minX, worldPoint.y - bounds.minY};
}

TileCell EditorTilemapCanvas::cellAt(
    ayt::math::FVector2 worldPoint) const
{
    return _viewport.worldToCell(
        _viewport.screenToWorld(localPoint(worldPoint)));
}

bool EditorTilemapCanvas::isInDocument(TileCell cell) const noexcept
{
    return cell.x >= 0 && cell.y >= 0
        && static_cast<uint32_t>(cell.x) < _model.document().cols()
        && static_cast<uint32_t>(cell.y) < _model.document().rows();
}

TileCell EditorTilemapCanvas::clampToDocument(TileCell cell) const noexcept
{
    const TilemapDocument& document = _model.document();
    if (document.cols() == 0u || document.rows() == 0u) return {-1, -1};
    cell.x = std::clamp(cell.x, 0, static_cast<int>(document.cols()) - 1);
    cell.y = std::clamp(cell.y, 0, static_cast<int>(document.rows()) - 1);
    return cell;
}

uint32_t EditorTilemapCanvas::visibleTileAt(TileCell cell) const noexcept
{
    const TilemapDocument& document = _model.document();
    if (!isInDocument(cell)) return document.defaultTileId();
    uint32_t result = document.defaultTileId();
    for (size_t layer = 0u; layer < document.layerCount(); ++layer) {
        if (!document.layers()[layer].visible) continue;
        const uint32_t candidate = document.tileAtLayer(
            layer, static_cast<uint32_t>(cell.x),
            static_cast<uint32_t>(cell.y));
        if (candidate != document.defaultTileId()) result = candidate;
    }
    return result;
}

void EditorTilemapCanvas::applyCell(TileCell cell)
{
    if (!isInDocument(cell) || cell == _lastPainted) return;
    _model.applyAt(static_cast<uint32_t>(cell.x),
                   static_cast<uint32_t>(cell.y));
    _lastPainted = cell;
    markDirty();
}

void EditorTilemapCanvas::applyStrokeSegment(TileCell from, TileCell to)
{
    if (!isInDocument(to)) return;
    if (!isInDocument(from)) {
        applyCell(to);
        return;
    }
    int x = from.x;
    int y = from.y;
    const int dx = std::abs(to.x - from.x);
    const int sx = from.x < to.x ? 1 : -1;
    const int dy = -std::abs(to.y - from.y);
    const int sy = from.y < to.y ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        applyCell({x, y});
        if (x == to.x && y == to.y) break;
        const int doubled = error * 2;
        if (doubled >= dy) {
            error += dy;
            x += sx;
        }
        if (doubled <= dx) {
            error += dx;
            y += sy;
        }
    }
}

void EditorTilemapCanvas::updateHover(ayt::math::FVector2 worldPoint)
{
    const TileCell next = cellAt(worldPoint);
    if (next == _hover) return;
    _hover = next;
    markDirty();
}

void EditorTilemapCanvas::notifyViewChanged()
{
    if (!_onViewChanged) return;
    const float canvasHeight = std::max(1.0f, getSize().y);
    _onViewChanged(canvasHeight / _viewport.verticalWorldSize() * 100.0f);
}

bool EditorTilemapCanvas::tileTextureVisual(
    uint32_t tileId, ayt::ui::ImageTextureHandle& texture,
    ayt::math::FRectangle& uv) const noexcept
{
    if (_atlasTextures == nullptr) return false;
    const TilemapDocument& document = _model.document();
    const auto* asset = document.tileAsset(tileId);
    if (asset == nullptr || asset->atlasId == 0u
        || asset->sourceWidth == 0u || asset->sourceHeight == 0u) {
        return false;
    }
    const auto* atlas = document.tileAtlas(asset->atlasId);
    const auto found = _atlasTextures->find(asset->atlasId);
    if (atlas == nullptr || atlas->imageWidth == 0u
        || atlas->imageHeight == 0u || found == _atlasTextures->end()
        || !found->second.isValid()) {
        return false;
    }
    const float insetX = asset->sourceWidth > 1u ? 0.5f : 0.0f;
    const float insetY = asset->sourceHeight > 1u ? 0.5f : 0.0f;
    const float atlasWidth = static_cast<float>(atlas->imageWidth);
    const float atlasHeight = static_cast<float>(atlas->imageHeight);
    uv = {(static_cast<float>(asset->sourceX) + insetX) / atlasWidth,
          (static_cast<float>(asset->sourceY) + insetY) / atlasHeight,
          (static_cast<float>(asset->sourceX + asset->sourceWidth) - insetX)
              / atlasWidth,
          (static_cast<float>(asset->sourceY + asset->sourceHeight) - insetY)
              / atlasHeight};
    texture = found->second;
    return true;
}

bool EditorTilemapCanvas::onMouseButtonDown(
    const ayt::ui::UIMouseEvent& event)
{
    _lastPointer = event.mousePos;
    updateHover(event.mousePos);
    if (event.mouseButton == 2
        || (event.mouseButton == 0 && _spacePan)) {
        _panning = true;
        _panButton = event.mouseButton;
        return true;
    }
    if (event.mouseButton == 1 && isInDocument(_hover)) {
        const uint32_t picked = visibleTileAt(_hover);
        _model.setSelectedTileId(picked);
        if (_onTilePicked) _onTilePicked(picked);
        markDirty();
        return true;
    }
    if (event.mouseButton != 0 || !isInDocument(_hover)) return false;

    _lastPainted = {-1, -1};
    if (_model.tool() == PaintTool::Selection) {
        const auto& selection = _model.selection();
        _selectionMoving = selection.contains(
            static_cast<uint32_t>(_hover.x),
            static_cast<uint32_t>(_hover.y));
        _selectionDrawing = !_selectionMoving;
        _selectionStart = _hover;
        _selectionCurrent = _hover;
        markDirty();
        return true;
    }
    if (_model.tool() == PaintTool::FloodFill) {
        _model.applyAt(static_cast<uint32_t>(_hover.x),
                       static_cast<uint32_t>(_hover.y));
        if (_onEdited) _onEdited();
        markDirty();
        return true;
    }
    _model.beginGesture();
    if (_model.tool() == PaintTool::Rectangle) {
        _rectangleDrawing = true;
        _rectangleStart = _hover;
        _rectangleCurrent = _hover;
        markDirty();
        return true;
    }
    _drawing = true;
    applyCell(_hover);
    return true;
}

bool EditorTilemapCanvas::onMouseMove(const ayt::ui::UIMouseEvent& event)
{
    if (_panning) {
        _viewport.panPixels(event.mousePos.x - _lastPointer.x,
                            event.mousePos.y - _lastPointer.y);
        _lastPointer = event.mousePos;
        updateHover(event.mousePos);
        notifyViewChanged();
        markDirty();
        return true;
    }
    updateHover(event.mousePos);
    _lastPointer = event.mousePos;
    if (_rectangleDrawing) {
        _rectangleCurrent = clampToDocument(_hover);
        markDirty();
        return true;
    }
    if (_selectionDrawing || _selectionMoving) {
        _selectionCurrent = clampToDocument(_hover);
        markDirty();
        return true;
    }
    if (_drawing) {
        applyStrokeSegment(_lastPainted, _hover);
        return true;
    }
    return isInDocument(_hover);
}

bool EditorTilemapCanvas::onMouseButtonUp(
    const ayt::ui::UIMouseEvent& event)
{
    if (_panning && event.mouseButton == _panButton) {
        _panning = false;
        _panButton = -1;
        return true;
    }
    if (_drawing && event.mouseButton == 0) {
        _drawing = false;
        const bool changed = _model.endGesture();
        _lastPainted = {-1, -1};
        if (changed && _onEdited) _onEdited();
        return true;
    }
    if (_rectangleDrawing && event.mouseButton == 0) {
        _rectangleDrawing = false;
        const TileCell end = clampToDocument(_rectangleCurrent);
        _model.applyRectangle(
            static_cast<uint32_t>(_rectangleStart.x),
            static_cast<uint32_t>(_rectangleStart.y),
            static_cast<uint32_t>(end.x),
            static_cast<uint32_t>(end.y));
        const bool changed = _model.endGesture();
        _rectangleStart = {-1, -1};
        _rectangleCurrent = {-1, -1};
        if (changed && _onEdited) _onEdited();
        markDirty();
        return true;
    }
    if (event.mouseButton == 0
        && (_selectionDrawing || _selectionMoving)) {
        const bool moving = _selectionMoving;
        _selectionDrawing = false;
        _selectionMoving = false;
        const TileCell end = clampToDocument(_selectionCurrent);
        bool changed = false;
        if (moving) {
            changed = _model.moveSelection(
                end.x - _selectionStart.x,
                end.y - _selectionStart.y);
            if (changed && _onEdited) _onEdited();
        } else {
            changed = _model.setSelection(
                static_cast<uint32_t>(_selectionStart.x),
                static_cast<uint32_t>(_selectionStart.y),
                static_cast<uint32_t>(end.x),
                static_cast<uint32_t>(end.y));
        }
        _selectionStart = {-1, -1};
        _selectionCurrent = {-1, -1};
        markDirty();
        return changed || moving;
    }
    return false;
}

bool EditorTilemapCanvas::onMouseWheel(
    const ayt::ui::UIMouseWheelEvent& event)
{
    const float steps = -event.deltaY / 40.0f;
    _viewport.zoomAt(std::pow(1.1f, steps), localPoint(event.mousePos));
    updateHover(event.mousePos);
    notifyViewChanged();
    markDirty();
    return true;
}

void EditorTilemapCanvas::onMouseLeave()
{
    if (_drawing || _panning || _rectangleDrawing
        || _selectionDrawing || _selectionMoving) return;
    _hover = {-1, -1};
    markDirty();
}

ayt::ui::UiCursorHint EditorTilemapCanvas::getCursorHint() const
{
    if (_panning || _spacePan || _selectionMoving) {
        return ayt::ui::UiCursorHint::Move;
    }
    if (_model.tool() == PaintTool::Selection && isInDocument(_hover)
        && _model.selection().contains(
            static_cast<uint32_t>(_hover.x),
            static_cast<uint32_t>(_hover.y))) {
        return ayt::ui::UiCursorHint::Move;
    }
    return ayt::ui::UiCursorHint::Default;
}

void EditorTilemapCanvas::tick(float dt)
{
    Widget::tick(dt);
    if (_animationPreviewEnabled && !_model.document().animations().empty()
        && dt > 0.0f) {
        _animationElapsedMs += static_cast<uint64_t>(dt * 1000.0f);
        markDirty();
    }
    if (getParent() == nullptr) return;
    const ayt::math::FVector2 parentSize = getParent()->getSize();
    if (getPosition().x != 0.0f || getPosition().y != 0.0f) {
        setPosition({0.0f, 0.0f});
    }
    if (getSize().x != parentSize.x || getSize().y != parentSize.y) {
        setSize(parentSize);
    }
    if (_lastCanvasSize.x == getSize().x
        && _lastCanvasSize.y == getSize().y) return;
    _lastCanvasSize = getSize();
    _viewport.setViewport(
        static_cast<uint32_t>(std::max(1.0f, getSize().x)),
        static_cast<uint32_t>(std::max(1.0f, getSize().y)));
    if (!_cameraInitialized) frameDocument();
    else notifyViewChanged();
}

void EditorTilemapCanvas::onRender(ayt::ui::IRenderBackend& renderer)
{
    const ayt::math::FRectangle bounds = getWorldBounds();
    renderer.drawGradientRect(bounds, {0.055f, 0.061f, 0.075f, 1.0f},
                              {0.078f, 0.086f, 0.102f, 1.0f});
    renderer.pushClip(bounds);

    const auto drawToolBadge = [&]() {
        std::wstring label;
        ayt::math::FVector4 accent{0.30f, 0.62f, 0.92f, 1.0f};
        switch (_model.tool()) {
        case PaintTool::Selection:
            label = L"SELECT  ·  drag to select or move";
            accent = {0.30f, 0.82f, 0.95f, 1.0f};
            break;
        case PaintTool::Pencil:
            label = L"PENCIL  ·  Tile "
                + std::to_wstring(_model.selectedTileId());
            break;
        case PaintTool::Eraser:
            label = L"ERASER  ·  clears cells";
            accent = {1.0f, 0.53f, 0.23f, 1.0f};
            break;
        case PaintTool::FloodFill:
            label = L"FILL  ·  Tile "
                + std::to_wstring(_model.selectedTileId());
            break;
        case PaintTool::Rectangle:
            label = L"RECTANGLE  ·  Tile "
                + std::to_wstring(_model.selectedTileId());
            break;
        case PaintTool::Terrain:
            label = L"TERRAIN  ·  Rule "
                + std::to_wstring(_model.selectedTerrainId());
            break;
        case PaintTool::Stamp:
            label = L"STAMP  ·  Pattern "
                + std::to_wstring(_model.selectedStampId());
            break;
        case PaintTool::Shadow:
            if (_model.selectedShadowMask()
                == ayt::ay2d::editor::ShadowMask_None) {
                label = L"SHADOW  ·  CLEAR";
            } else if (_model.selectedShadowMask()
                       == ayt::ay2d::editor::ShadowMask_All) {
                label = L"SHADOW  ·  FULL";
            } else {
                label = L"SHADOW  ·  CUSTOM";
            }
            accent = {0.70f, 0.48f, 0.94f, 1.0f};
            break;
        }
        const ayt::math::FRectangle badge{
            bounds.minX + 12.0f, bounds.minY + 12.0f,
            bounds.minX + 244.0f, bounds.minY + 42.0f};
        renderer.drawRoundedRect(badge, {0.035f, 0.041f, 0.052f, 0.94f},
                                 5.0f);
        renderer.drawBorderRect(badge, accent, 1.0f);
        ayt::ui::IRenderBackend::TextStyle style;
        style.color = accent;
        style.bold = true;
        style.align = ayt::ui::IRenderBackend::TextStyle::Align::Center;
        style.valign = ayt::ui::IRenderBackend::TextStyle::VAlign::Middle;
        renderer.drawText(badge, label, 12, style);
    };

    const TilemapDocument& document = _model.document();
    if (document.cols() == 0u || document.rows() == 0u) {
        drawToolBadge();
        renderer.popClip();
        return;
    }

    const float tileWidth = static_cast<float>(document.tileWidth());
    const float tileHeight = static_cast<float>(document.tileHeight());
    const ayt::math::FVector2 offset{bounds.minX, bounds.minY};
    const auto mapTopLeft = _viewport.worldToScreen(
        {0.0f, static_cast<float>(document.rows()) * tileHeight});
    const auto mapBottomRight = _viewport.worldToScreen(
        {static_cast<float>(document.cols()) * tileWidth, 0.0f});
    const ayt::math::FRectangle mapRect = normalizedRect(
        mapTopLeft, mapBottomRight, offset);
    renderer.drawRect(mapRect, {0.105f, 0.115f, 0.135f, 1.0f});

    const float cellWidthPx = std::abs(
        _viewport.worldToScreen({tileWidth, 0.0f}).x
        - _viewport.worldToScreen({0.0f, 0.0f}).x);
    const float cellHeightPx = std::abs(
        _viewport.worldToScreen({0.0f, tileHeight}).y
        - _viewport.worldToScreen({0.0f, 0.0f}).y);
    if (cellWidthPx < 3.0f || cellHeightPx < 3.0f) {
        renderer.drawBorderRect(mapRect, {0.30f, 0.50f, 0.76f, 1.0f},
                                1.0f);
        drawToolBadge();
        renderer.popClip();
        return;
    }

    const auto worldTopLeft = _viewport.screenToWorld({0.0f, 0.0f});
    const auto worldBottomRight = _viewport.screenToWorld(getSize());
    const int minCol = std::max(
        0, static_cast<int>(std::floor(worldTopLeft.x / tileWidth)));
    const int maxCol = std::min(static_cast<int>(document.cols()) - 1,
        static_cast<int>(std::floor(worldBottomRight.x / tileWidth)));
    const int minRow = std::max(
        0, static_cast<int>(std::floor(worldBottomRight.y / tileHeight)));
    const int maxRow = std::min(static_cast<int>(document.rows()) - 1,
        static_cast<int>(std::floor(worldTopLeft.y / tileHeight)));

    for (int row = minRow; row <= maxRow; ++row) {
        for (int col = minCol; col <= maxCol; ++col) {
            const auto a = _viewport.worldToScreen(
                {static_cast<float>(col) * tileWidth,
                 static_cast<float>(row + 1) * tileHeight});
            const auto b = _viewport.worldToScreen(
                {static_cast<float>(col + 1) * tileWidth,
                 static_cast<float>(row) * tileHeight});
            const ayt::math::FRectangle cell = normalizedRect(a, b, offset);
            const float inset = cellWidthPx >= 8.0f && cellHeightPx >= 8.0f
                ? 0.6f : 0.0f;
            renderer.drawRoundedRect(insetRect(cell, inset),
                ((row + col) & 1) == 0
                    ? ayt::math::FVector4{0.105f, 0.115f, 0.135f, 1.0f}
                    : ayt::math::FVector4{0.112f, 0.122f, 0.143f, 1.0f},
                inset > 0.0f ? 1.5f : 0.0f);

            uint32_t tileId = document.defaultTileId();
            for (size_t layer = 0u; layer < document.layerCount(); ++layer) {
                if (!document.layers()[layer].visible) continue;
                const uint32_t candidate = document.tileAtLayer(
                    layer, static_cast<uint32_t>(col),
                    static_cast<uint32_t>(row));
                if (candidate == document.defaultTileId()) continue;
                tileId = _animationPreviewEnabled
                    ? _model.animationPreviewTileId(
                        candidate, _animationElapsedMs)
                    : candidate;
                ayt::ui::ImageTextureHandle texture;
                ayt::math::FRectangle uv;
                if (tileTextureVisual(tileId, texture, uv)) {
                    renderer.addTexturedQuad(
                        insetRect(cell, inset), texture.handle, uv,
                        renderTint(document, tileId));
                } else {
                    renderer.drawRoundedRect(insetRect(cell, inset),
                        previewColor(document, tileId),
                        inset > 0.0f ? 1.5f : 0.0f);
                }
            }
            if (_showShadows) {
                drawShadowMask(renderer, cell,
                    document.shadowMaskAt(static_cast<uint32_t>(col),
                                          static_cast<uint32_t>(row)),
                    unpackRgba(document.shadowColor()));
            }
            if (_showGrid && cellWidthPx >= 9.0f && cellHeightPx >= 9.0f) {
                renderer.drawBorderRect(
                    cell, {0.035f, 0.040f, 0.050f, 0.52f}, 1.0f);
            }
            if (_showCollision && document.collisionFlagsFor(tileId) != 0u) {
                const float marker = std::min(9.0f, cell.height() * 0.30f);
                renderer.drawRoundedRect(
                    {cell.minX + 3.0f, cell.minY + 3.0f,
                     cell.minX + marker + 3.0f,
                     cell.minY + marker + 3.0f},
                    {0.96f, 0.27f, 0.25f, 0.96f}, 2.0f);
            }
            if (tileId != document.defaultTileId()
                && cellWidthPx >= 46.0f && cellHeightPx >= 34.0f) {
                ayt::ui::IRenderBackend::TextStyle style;
                style.color = {0.98f, 0.98f, 1.0f, 0.88f};
                style.align =
                    ayt::ui::IRenderBackend::TextStyle::Align::Center;
                style.valign =
                    ayt::ui::IRenderBackend::TextStyle::VAlign::Middle;
                renderer.drawText(cell, std::to_wstring(tileId), 12, style);
            }
        }
    }
    renderer.drawBorderRect(mapRect, {0.31f, 0.52f, 0.79f, 0.92f}, 1.0f);

    ayt::ay2d::editor::TileRegionSelection shownSelection =
        _model.selection();
    if (_selectionDrawing && isInDocument(_selectionStart)
        && isInDocument(_selectionCurrent)) {
        shownSelection = {
            static_cast<uint32_t>(std::min(
                _selectionStart.x, _selectionCurrent.x)),
            static_cast<uint32_t>(std::min(
                _selectionStart.y, _selectionCurrent.y)),
            static_cast<uint32_t>(std::max(
                _selectionStart.x, _selectionCurrent.x)),
            static_cast<uint32_t>(std::max(
                _selectionStart.y, _selectionCurrent.y)),
            true};
    } else if (_selectionMoving && shownSelection.valid
               && isInDocument(_selectionCurrent)) {
        const int dx = _selectionCurrent.x - _selectionStart.x;
        const int dy = _selectionCurrent.y - _selectionStart.y;
        const int firstCol = static_cast<int>(shownSelection.firstCol) + dx;
        const int firstRow = static_cast<int>(shownSelection.firstRow) + dy;
        const int lastCol = static_cast<int>(shownSelection.lastCol) + dx;
        const int lastRow = static_cast<int>(shownSelection.lastRow) + dy;
        if (firstCol >= 0 && firstRow >= 0
            && lastCol < static_cast<int>(document.cols())
            && lastRow < static_cast<int>(document.rows())) {
            shownSelection = {
                static_cast<uint32_t>(firstCol),
                static_cast<uint32_t>(firstRow),
                static_cast<uint32_t>(lastCol),
                static_cast<uint32_t>(lastRow), true};
        }
    }
    if (shownSelection.valid) {
        const auto a = _viewport.worldToScreen(
            {static_cast<float>(shownSelection.firstCol) * tileWidth,
             static_cast<float>(shownSelection.lastRow + 1u) * tileHeight});
        const auto b = _viewport.worldToScreen(
            {static_cast<float>(shownSelection.lastCol + 1u) * tileWidth,
             static_cast<float>(shownSelection.firstRow) * tileHeight});
        const ayt::math::FRectangle selectionRect = normalizedRect(
            a, b, offset);
        renderer.drawRect(selectionRect, {0.20f, 0.68f, 0.94f, 0.10f});
        renderer.drawBorderRect(
            selectionRect, {0.30f, 0.82f, 1.0f, 1.0f}, 2.0f);
    }

    if (_rectangleDrawing && isInDocument(_rectangleStart)
        && isInDocument(_rectangleCurrent)) {
        const int minX = std::min(_rectangleStart.x, _rectangleCurrent.x);
        const int maxX = std::max(_rectangleStart.x, _rectangleCurrent.x);
        const int minY = std::min(_rectangleStart.y, _rectangleCurrent.y);
        const int maxY = std::max(_rectangleStart.y, _rectangleCurrent.y);
        const auto a = _viewport.worldToScreen(
            {static_cast<float>(minX) * tileWidth,
             static_cast<float>(maxY + 1) * tileHeight});
        const auto b = _viewport.worldToScreen(
            {static_cast<float>(maxX + 1) * tileWidth,
             static_cast<float>(minY) * tileHeight});
        const ayt::math::FRectangle preview = normalizedRect(a, b, offset);
        renderer.drawRect(preview, withAlpha(
            previewColor(document, _model.selectedTileId()), 0.24f));
        renderer.drawBorderRect(preview, {0.48f, 0.76f, 1.0f, 1.0f},
                                2.0f);
    } else if (isInDocument(_hover)) {
        if (_model.tool() == PaintTool::Selection) {
            drawToolBadge();
            renderer.popClip();
            return;
        }
        if (_model.tool() == PaintTool::Shadow) {
            const auto a = _viewport.worldToScreen(
                {static_cast<float>(_hover.x) * tileWidth,
                 static_cast<float>(_hover.y + 1) * tileHeight});
            const auto b = _viewport.worldToScreen(
                {static_cast<float>(_hover.x + 1) * tileWidth,
                 static_cast<float>(_hover.y) * tileHeight});
            const ayt::math::FRectangle hover = normalizedRect(a, b, offset);
            if (_model.selectedShadowMask() == 0u) {
                renderer.drawRect(
                    hover, {0.92f, 0.27f, 0.30f, 0.20f});
            } else {
                ayt::math::FVector4 color = unpackRgba(
                    document.shadowColor());
                color.w = std::max(color.w, 0.34f);
                drawShadowMask(renderer, hover,
                    _model.selectedShadowMask(), color);
            }
            renderer.drawBorderRect(
                hover, {0.78f, 0.59f, 1.0f, 1.0f}, 2.0f);
            drawToolBadge();
            renderer.popClip();
            return;
        }
        if (_model.tool() == PaintTool::Stamp) {
            if (const ayt::ay2d::editor::TileStampDefinition* stamp =
                    document.tileStamp(
                    _model.selectedStampId())) {
                for (const ayt::ay2d::editor::TileStampCell& cell :
                     stamp->cells) {
                    const int column = _hover.x + cell.offsetCol;
                    const int row = _hover.y + cell.offsetRow;
                    if (column < 0 || row < 0
                        || column >= static_cast<int>(document.cols())
                        || row >= static_cast<int>(document.rows())) continue;
                    const auto a = _viewport.worldToScreen(
                        {static_cast<float>(column) * tileWidth,
                         static_cast<float>(row + 1) * tileHeight});
                    const auto b = _viewport.worldToScreen(
                        {static_cast<float>(column + 1) * tileWidth,
                         static_cast<float>(row) * tileHeight});
                    const ayt::math::FRectangle cellRect = normalizedRect(
                        a, b, offset);
                    renderer.drawRect(cellRect, withAlpha(
                        previewColor(document, cell.tileId), 0.24f));
                    renderer.drawBorderRect(
                        cellRect, {0.96f, 0.67f, 0.24f, 1.0f}, 1.5f);
                }
            }
            drawToolBadge();
            renderer.popClip();
            return;
        }
        const auto a = _viewport.worldToScreen(
            {static_cast<float>(_hover.x) * tileWidth,
             static_cast<float>(_hover.y + 1) * tileHeight});
        const auto b = _viewport.worldToScreen(
            {static_cast<float>(_hover.x + 1) * tileWidth,
             static_cast<float>(_hover.y) * tileHeight});
        const ayt::math::FRectangle hover = normalizedRect(a, b, offset);
        ayt::math::FVector4 color = _model.tool() == PaintTool::Eraser
            ? ayt::math::FVector4{0.92f, 0.33f, 0.30f, 1.0f}
            : previewColor(document, _model.selectedTileId());
        if (_model.tool() != PaintTool::FloodFill) {
            renderer.drawRect(hover, withAlpha(color, 0.18f));
        }
        renderer.drawBorderRect(hover, {0.56f, 0.80f, 1.0f, 1.0f}, 2.0f);
    }

    drawToolBadge();
    renderer.popClip();
}

} // namespace ayt::editor
