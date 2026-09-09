#include "AYEditorTileAtlasPicker.h"

#include <AYUI/IRenderBackend.h>

#include <algorithm>
#include <cmath>

namespace ayt::editor {
namespace {

constexpr float kPadding = 6.0f;

bool contains(const ayt::math::FRectangle& rectangle,
              ayt::math::FVector2 point) noexcept
{
    return point.x >= rectangle.minX && point.x <= rectangle.maxX
        && point.y >= rectangle.minY && point.y <= rectangle.maxY;
}

} // namespace

void EditorTileAtlasPicker::setAtlas(
    const ayt::ui::ImageTextureHandle& texture,
    const ayt::ay2d::editor::TileAtlasSource& source,
    std::vector<Cell> cells)
{
    const bool changed = _texture.handle != texture.handle
        || _texture.generation != texture.generation
        || !(_source == source) || _cells != cells;
    _texture = texture;
    _source = source;
    _cells = std::move(cells);
    if (changed) frameAtlas();
    else markDirty();
}

void EditorTileAtlasPicker::clearAtlas()
{
    _texture = {};
    _source = {};
    _cells.clear();
    _selectedTileId = 0u;
    _hoverColumn = _hoverRow = _hoverCellIndex = -1;
    _selection.reset();
    _selectingRectangle = false;
    frameAtlas();
}

void EditorTileAtlasPicker::setSelectedTileId(uint32_t tileId)
{
    if (_selectedTileId == tileId) return;
    _selectedTileId = tileId;
    markDirty();
}

void EditorTileAtlasPicker::frameAtlas()
{
    _zoom = 1.0f;
    _pan = {0.0f, 0.0f};
    markDirty();
}

void EditorTileAtlasPicker::setRectangleSelectionEnabled(
    bool enabled, bool snapToGrid)
{
    _rectangleSelectionEnabled = enabled;
    _selectionSnapsToGrid = enabled && snapToGrid
        && _source.layout == ayt::ay2d::editor::TileAtlasLayout::Grid;
    _selectingRectangle = false;
    if (!enabled) _selection.reset();
    markDirty();
}

void EditorTileAtlasPicker::clearRectangleSelection()
{
    _selection.reset();
    _selectingRectangle = false;
    markDirty();
}

ayt::math::FRectangle EditorTileAtlasPicker::imageRect() const noexcept
{
    const ayt::math::FRectangle bounds = getWorldBounds();
    if (_source.imageWidth == 0u || _source.imageHeight == 0u) return bounds;
    const float fit = std::min(
        std::max(1.0f, bounds.width() - kPadding * 2.0f)
            / static_cast<float>(_source.imageWidth),
        std::max(1.0f, bounds.height() - kPadding * 2.0f)
            / static_cast<float>(_source.imageHeight));
    const float width = static_cast<float>(_source.imageWidth) * fit * _zoom;
    const float height = static_cast<float>(_source.imageHeight) * fit * _zoom;
    const float centerX = (bounds.minX + bounds.maxX) * 0.5f + _pan.x;
    const float centerY = (bounds.minY + bounds.maxY) * 0.5f + _pan.y;
    return {centerX - width * 0.5f, centerY - height * 0.5f,
            centerX + width * 0.5f, centerY + height * 0.5f};
}

ayt::math::FRectangle EditorTileAtlasPicker::cellRect(
    int column, int row) const noexcept
{
    const ayt::math::FRectangle image = imageRect();
    const float scaleX = image.width() / static_cast<float>(_source.imageWidth);
    const float scaleY = image.height() / static_cast<float>(_source.imageHeight);
    const uint32_t x = _source.marginX + static_cast<uint32_t>(column)
        * (_source.tileWidth + _source.spacingX);
    const uint32_t y = _source.marginY + static_cast<uint32_t>(row)
        * (_source.tileHeight + _source.spacingY);
    return {image.minX + static_cast<float>(x) * scaleX,
            image.minY + static_cast<float>(y) * scaleY,
            image.minX + static_cast<float>(x + _source.tileWidth) * scaleX,
            image.minY + static_cast<float>(y + _source.tileHeight) * scaleY};
}

ayt::math::FRectangle EditorTileAtlasPicker::cellRect(
    const Cell& cell) const noexcept
{
    const ayt::math::FRectangle image = imageRect();
    const float scaleX = image.width() / static_cast<float>(_source.imageWidth);
    const float scaleY = image.height() / static_cast<float>(_source.imageHeight);
    return {
        image.minX + static_cast<float>(cell.sourceX) * scaleX,
        image.minY + static_cast<float>(cell.sourceY) * scaleY,
        image.minX + static_cast<float>(cell.sourceX + cell.sourceWidth) * scaleX,
        image.minY + static_cast<float>(cell.sourceY + cell.sourceHeight) * scaleY};
}

std::optional<ayt::math::FVector2> EditorTileAtlasPicker::sourcePoint(
    ayt::math::FVector2 point, bool clampToImage) const noexcept
{
    if (_source.imageWidth == 0u || _source.imageHeight == 0u) {
        return std::nullopt;
    }
    const ayt::math::FRectangle image = imageRect();
    if (!clampToImage && !contains(image, point)) return std::nullopt;
    const float x = (point.x - image.minX)
        * static_cast<float>(_source.imageWidth) / image.width();
    const float y = (point.y - image.minY)
        * static_cast<float>(_source.imageHeight) / image.height();
    return ayt::math::FVector2{
        std::clamp(x, 0.0f,
            std::max(0.0f, static_cast<float>(_source.imageWidth) - 0.001f)),
        std::clamp(y, 0.0f,
            std::max(0.0f, static_cast<float>(_source.imageHeight) - 0.001f))};
}

EditorTileAtlasPicker::SourceRect EditorTileAtlasPicker::selectionFromPoints(
    ayt::math::FVector2 first, ayt::math::FVector2 last) const noexcept
{
    if (_selectionSnapsToGrid && _source.tileWidth != 0u
        && _source.tileHeight != 0u && _source.columns != 0u
        && _source.rows != 0u) {
        const auto columnAt = [this](float x) {
            if (x <= static_cast<float>(_source.marginX)) return uint32_t{0u};
            return std::min(_source.columns - 1u,
                static_cast<uint32_t>(x - _source.marginX)
                    / (_source.tileWidth + _source.spacingX));
        };
        const auto rowAt = [this](float y) {
            if (y <= static_cast<float>(_source.marginY)) return uint32_t{0u};
            return std::min(_source.rows - 1u,
                static_cast<uint32_t>(y - _source.marginY)
                    / (_source.tileHeight + _source.spacingY));
        };
        const uint32_t firstColumn = columnAt(std::min(first.x, last.x));
        const uint32_t lastColumn = columnAt(std::max(first.x, last.x));
        const uint32_t firstRow = rowAt(std::min(first.y, last.y));
        const uint32_t lastRow = rowAt(std::max(first.y, last.y));
        const uint32_t x = _source.marginX + firstColumn
            * (_source.tileWidth + _source.spacingX);
        const uint32_t y = _source.marginY + firstRow
            * (_source.tileHeight + _source.spacingY);
        const uint32_t right = _source.marginX + lastColumn
            * (_source.tileWidth + _source.spacingX) + _source.tileWidth;
        const uint32_t bottom = _source.marginY + lastRow
            * (_source.tileHeight + _source.spacingY) + _source.tileHeight;
        return {x, y, right - x, bottom - y};
    }
    const float minimumX = std::min(first.x, last.x);
    const float minimumY = std::min(first.y, last.y);
    const float maximumX = std::max(first.x, last.x);
    const float maximumY = std::max(first.y, last.y);
    const uint32_t x = static_cast<uint32_t>(std::floor(minimumX));
    const uint32_t y = static_cast<uint32_t>(std::floor(minimumY));
    const uint32_t right = std::min(_source.imageWidth,
        std::max(x + 1u, static_cast<uint32_t>(std::ceil(maximumX))));
    const uint32_t bottom = std::min(_source.imageHeight,
        std::max(y + 1u, static_cast<uint32_t>(std::ceil(maximumY))));
    return {x, y, right - x, bottom - y};
}

const EditorTileAtlasPicker::Cell* EditorTileAtlasPicker::importedCell(
    uint32_t sourceX, uint32_t sourceY) const noexcept
{
    const auto found = std::find_if(_cells.begin(), _cells.end(),
        [sourceX, sourceY](const Cell& cell) {
            return cell.sourceX == sourceX && cell.sourceY == sourceY;
        });
    return found == _cells.end() ? nullptr : &*found;
}

EditorTileAtlasPicker::GridHit EditorTileAtlasPicker::hit(
    ayt::math::FVector2 point) const noexcept
{
    if (!_texture.isValid() || !contains(imageRect(), point)) return {};
    const ayt::math::FRectangle image = imageRect();
    const float sourceX = (point.x - image.minX)
        * static_cast<float>(_source.imageWidth) / image.width();
    const float sourceY = (point.y - image.minY)
        * static_cast<float>(_source.imageHeight) / image.height();
    if (_source.layout == ayt::ay2d::editor::TileAtlasLayout::Regions) {
        for (size_t index = _cells.size(); index > 0u; --index) {
            const Cell& cell = _cells[index - 1u];
            if (sourceX >= cell.sourceX && sourceY >= cell.sourceY
                && sourceX < cell.sourceX + cell.sourceWidth
                && sourceY < cell.sourceY + cell.sourceHeight) {
                return {-1, -1, static_cast<int>(index - 1u), &cell};
            }
        }
        return {};
    }
    if (_source.tileWidth == 0u || _source.tileHeight == 0u
        || _source.columns == 0u || _source.rows == 0u) return {};
    if (sourceX < static_cast<float>(_source.marginX)
        || sourceY < static_cast<float>(_source.marginY)) return {};
    const uint32_t x = static_cast<uint32_t>(sourceX - _source.marginX);
    const uint32_t y = static_cast<uint32_t>(sourceY - _source.marginY);
    const uint32_t pitchX = _source.tileWidth + _source.spacingX;
    const uint32_t pitchY = _source.tileHeight + _source.spacingY;
    const uint32_t column = x / pitchX;
    const uint32_t row = y / pitchY;
    if (column >= _source.columns || row >= _source.rows
        || x % pitchX >= _source.tileWidth
        || y % pitchY >= _source.tileHeight) return {};
    const uint32_t cellX = _source.marginX + column * pitchX;
    const uint32_t cellY = _source.marginY + row * pitchY;
    return {static_cast<int>(column), static_cast<int>(row), -1,
            importedCell(cellX, cellY)};
}

void EditorTileAtlasPicker::updateHover(ayt::math::FVector2 point)
{
    const GridHit result = hit(point);
    if (_hoverColumn == result.column && _hoverRow == result.row
        && _hoverCellIndex == result.cellIndex) return;
    _hoverColumn = result.column;
    _hoverRow = result.row;
    _hoverCellIndex = result.cellIndex;
    if (_onStatus && result.valid()) {
        std::wstring text;
        if (result.column >= 0) {
            text = L"Atlas cell " + std::to_wstring(result.column)
                + L", " + std::to_wstring(result.row);
            text += result.cell != nullptr
                ? L"  ·  Tile " + std::to_wstring(result.cell->tileId)
                : L"  ·  empty / skipped";
        } else if (result.cell != nullptr) {
            text = L"Atlas region  ·  Tile "
                + std::to_wstring(result.cell->tileId) + L"  ·  "
                + std::to_wstring(result.cell->sourceWidth) + L" × "
                + std::to_wstring(result.cell->sourceHeight) + L" px";
        }
        _onStatus(text);
    }
    markDirty();
}

bool EditorTileAtlasPicker::onMouseMove(const ayt::ui::UIMouseEvent& event)
{
    if (_panning) {
        _pan.x += event.mousePos.x - _lastPointer.x;
        _pan.y += event.mousePos.y - _lastPointer.y;
        _lastPointer = event.mousePos;
        markDirty();
        return true;
    }
    if (_selectingRectangle) {
        const auto point = sourcePoint(event.mousePos, true);
        if (point) _selection = selectionFromPoints(_selectionStart, *point);
        markDirty();
        return true;
    }
    _lastPointer = event.mousePos;
    updateHover(event.mousePos);
    return _hoverColumn >= 0 || _hoverCellIndex >= 0;
}

bool EditorTileAtlasPicker::onMouseButtonDown(
    const ayt::ui::UIMouseEvent& event)
{
    _lastPointer = event.mousePos;
    if (event.mouseButton == 2 && _texture.isValid()) {
        _panning = true;
        return true;
    }
    if (event.mouseButton != 0) return false;
    if (_rectangleSelectionEnabled) {
        const auto point = sourcePoint(event.mousePos, false);
        if (!point) return false;
        _selectionStart = *point;
        _selection = selectionFromPoints(*point, *point);
        _selectingRectangle = true;
        markDirty();
        return true;
    }
    const GridHit result = hit(event.mousePos);
    if (!result.valid()) return false;
    if (result.cell == nullptr) {
        if (_onStatus) _onStatus(L"This atlas cell is empty and was skipped");
        return true;
    }
    setSelectedTileId(result.cell->tileId);
    if (_onTileSelected) _onTileSelected(result.cell->tileId);
    return true;
}

bool EditorTileAtlasPicker::onMouseButtonUp(
    const ayt::ui::UIMouseEvent& event)
{
    if (event.mouseButton == 0 && _selectingRectangle) {
        const auto point = sourcePoint(event.mousePos, true);
        if (point) _selection = selectionFromPoints(_selectionStart, *point);
        _selectingRectangle = false;
        if (_selection && _onRectangleSelected) {
            _onRectangleSelected(*_selection);
        }
        markDirty();
        return true;
    }
    if (event.mouseButton != 2 || !_panning) return false;
    _panning = false;
    markDirty();
    return true;
}

bool EditorTileAtlasPicker::onMouseWheel(
    const ayt::ui::UIMouseWheelEvent& event)
{
    if (!_texture.isValid()) return false;
    const ayt::math::FRectangle before = imageRect();
    const float sourceX = (event.mousePos.x - before.minX)
        * static_cast<float>(_source.imageWidth) / before.width();
    const float sourceY = (event.mousePos.y - before.minY)
        * static_cast<float>(_source.imageHeight) / before.height();
    _zoom = std::clamp(_zoom * std::pow(1.12f, -event.deltaY / 40.0f),
                       1.0f, 24.0f);
    const ayt::math::FRectangle after = imageRect();
    _pan.x += event.mousePos.x - sourceX * after.width()
        / static_cast<float>(_source.imageWidth) - after.minX;
    _pan.y += event.mousePos.y - sourceY * after.height()
        / static_cast<float>(_source.imageHeight) - after.minY;
    markDirty();
    return true;
}

void EditorTileAtlasPicker::onMouseLeave()
{
    if (_panning || _selectingRectangle) return;
    _hoverColumn = _hoverRow = _hoverCellIndex = -1;
    markDirty();
}

ayt::ui::UiCursorHint EditorTileAtlasPicker::getCursorHint() const
{
    return _panning ? ayt::ui::UiCursorHint::Move
        : (_rectangleSelectionEnabled ? ayt::ui::UiCursorHint::Default
                                      : ayt::ui::UiCursorHint::Hand);
}

void EditorTileAtlasPicker::tick(float dt)
{
    Widget::tick(dt);
    if (getParent() == nullptr) return;
    const ayt::math::FVector2 size = getParent()->getSize();
    if (getPosition().x != 0.0f || getPosition().y != 0.0f) {
        setPosition({0.0f, 0.0f});
    }
    if (getSize().x != size.x || getSize().y != size.y) setSize(size);
}

void EditorTileAtlasPicker::onRender(ayt::ui::IRenderBackend& renderer)
{
    const ayt::math::FRectangle bounds = getWorldBounds();
    renderer.drawRect(bounds, {0.030f, 0.035f, 0.045f, 1.0f});
    renderer.drawBorderRect(bounds, {0.20f, 0.23f, 0.28f, 1.0f}, 1.0f);
    if (!_texture.isValid() || _source.imageWidth == 0u
        || _source.imageHeight == 0u) {
        ayt::ui::IRenderBackend::TextStyle style;
        style.color = {0.48f, 0.53f, 0.61f, 1.0f};
        style.align = ayt::ui::IRenderBackend::TextStyle::Align::Center;
        style.valign = ayt::ui::IRenderBackend::TextStyle::VAlign::Middle;
        renderer.drawText(bounds, L"Import a tile sheet to browse it", 11, style);
        return;
    }
    renderer.pushClip(bounds);
    const ayt::math::FRectangle image = imageRect();
    renderer.drawRect(image, _texture.handle, {0.0f, 0.0f, 1.0f, 1.0f});
    const float cellWidth = image.width() * _source.tileWidth / _source.imageWidth;
    const float cellHeight = image.height() * _source.tileHeight / _source.imageHeight;
    if (_source.layout == ayt::ay2d::editor::TileAtlasLayout::Grid
        && cellWidth >= 5.0f && cellHeight >= 5.0f) {
        for (uint32_t row = 0u; row < _source.rows; ++row) {
            for (uint32_t column = 0u; column < _source.columns; ++column) {
                renderer.drawBorderRect(cellRect(
                    static_cast<int>(column), static_cast<int>(row)),
                    {0.28f, 0.72f, 0.96f, 0.48f}, 1.0f);
            }
        }
    } else if (_source.layout
               == ayt::ay2d::editor::TileAtlasLayout::Regions) {
        for (const Cell& cell : _cells) {
            renderer.drawBorderRect(cellRect(cell),
                {0.28f, 0.72f, 0.96f, 0.68f}, 1.0f);
        }
    }
    const auto selected = std::find_if(_cells.begin(), _cells.end(),
        [this](const Cell& cell) { return cell.tileId == _selectedTileId; });
    if (selected != _cells.end()) {
        ayt::math::FRectangle selectedRect = cellRect(*selected);
        if (_source.layout == ayt::ay2d::editor::TileAtlasLayout::Grid) {
            const int column = static_cast<int>(
                (selected->sourceX - _source.marginX)
                / (_source.tileWidth + _source.spacingX));
            const int row = static_cast<int>(
                (selected->sourceY - _source.marginY)
                / (_source.tileHeight + _source.spacingY));
            selectedRect = cellRect(column, row);
        }
        renderer.drawRect(selectedRect, {0.18f, 0.52f, 0.94f, 0.24f});
        renderer.drawBorderRect(selectedRect,
                                {0.38f, 0.78f, 1.0f, 1.0f}, 2.0f);
    }
    if (_hoverColumn >= 0 && _hoverRow >= 0) {
        renderer.drawRect(cellRect(_hoverColumn, _hoverRow),
                          {1.0f, 1.0f, 1.0f, 0.12f});
        renderer.drawBorderRect(cellRect(_hoverColumn, _hoverRow),
                                {1.0f, 1.0f, 1.0f, 0.90f}, 1.0f);
    } else if (_hoverCellIndex >= 0
               && _hoverCellIndex < static_cast<int>(_cells.size())) {
        const ayt::math::FRectangle hover = cellRect(
            _cells[static_cast<size_t>(_hoverCellIndex)]);
        renderer.drawRect(hover, {1.0f, 1.0f, 1.0f, 0.12f});
        renderer.drawBorderRect(hover, {1.0f, 1.0f, 1.0f, 0.90f}, 1.0f);
    }
    if (_selection) {
        const Cell selectionCell{0u, _selection->x, _selection->y,
            _selection->width, _selection->height};
        const ayt::math::FRectangle selection = cellRect(selectionCell);
        renderer.drawRect(selection, {0.95f, 0.65f, 0.12f, 0.18f});
        renderer.drawBorderRect(selection,
                                {1.0f, 0.72f, 0.22f, 1.0f}, 2.0f);
    }
    renderer.popClip();
}

} // namespace ayt::editor
