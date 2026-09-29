#include "AYEditorSpriteAnimationCanvas.h"

#include <AYUI/IRenderBackend.h>

#include <algorithm>
#include <cmath>

namespace ayt::editor {
namespace {

constexpr float kPadding = 12.0f;

bool contains(const ayt::math::FRectangle& rectangle,
              ayt::math::FVector2 point) noexcept
{
    return point.x >= rectangle.minX && point.x <= rectangle.maxX
        && point.y >= rectangle.minY && point.y <= rectangle.maxY;
}

} // namespace

EditorSpriteAnimationCanvas::EditorSpriteAnimationCanvas(bool previewOnly)
    : _previewOnly(previewOnly)
{
    setId(previewOnly ? "sprite_animation_preview"
                      : "sprite_animation_sheet");
}

void EditorSpriteAnimationCanvas::setModel(
    ayt::ay2d::editor::SpriteAnimationAuthoringModel* model) noexcept
{
    _model = model;
    if (_model != nullptr) {
        _model->setSheetSize(_imageWidth, _imageHeight);
    }
    markDirty();
}

void EditorSpriteAnimationCanvas::setImage(
    ayt::ui::ImageTextureHandle texture,
    uint32_t width, uint32_t height)
{
    _texture = texture;
    _imageWidth = width;
    _imageHeight = height;
    if (_model != nullptr) _model->setSheetSize(width, height);
    frameImage();
}

void EditorSpriteAnimationCanvas::clearImage()
{
    _texture = {};
    _imageWidth = 0u;
    _imageHeight = 0u;
    _hoverValid = false;
    _selecting = false;
    _panning = false;
    if (_model != nullptr) _model->setSheetSize(0u, 0u);
    frameImage();
}

void EditorSpriteAnimationCanvas::frameImage()
{
    _zoom = 1.0f;
    _pan = {0.0f, 0.0f};
    markDirty();
}

ayt::math::FRectangle EditorSpriteAnimationCanvas::imageRect() const noexcept
{
    const ayt::math::FRectangle bounds = getWorldBounds();
    if (_imageWidth == 0u || _imageHeight == 0u) return bounds;
    const float fit = std::min(
        std::max(1.0f, bounds.width() - kPadding * 2.0f)
            / static_cast<float>(_imageWidth),
        std::max(1.0f, bounds.height() - kPadding * 2.0f)
            / static_cast<float>(_imageHeight));
    const float width = static_cast<float>(_imageWidth) * fit * _zoom;
    const float height = static_cast<float>(_imageHeight) * fit * _zoom;
    const float centerX = (bounds.minX + bounds.maxX) * 0.5f + _pan.x;
    const float centerY = (bounds.minY + bounds.maxY) * 0.5f + _pan.y;
    return {centerX - width * 0.5f, centerY - height * 0.5f,
            centerX + width * 0.5f, centerY + height * 0.5f};
}

ayt::math::FRectangle EditorSpriteAnimationCanvas::cellRect(
    uint32_t cell) const noexcept
{
    const ayt::math::FRectangle image = imageRect();
    if (_model == nullptr || _model->cellCount() == 0u) return image;
    const auto uv = _model->cellRect(cell);
    return {image.minX + uv.minU * image.width(),
            image.minY + uv.minV * image.height(),
            image.minX + uv.maxU * image.width(),
            image.minY + uv.maxV * image.height()};
}

bool EditorSpriteAnimationCanvas::sourcePoint(
    ayt::math::FVector2 point, float& x, float& y, bool clamp) const noexcept
{
    if (!_texture.isValid() || _imageWidth == 0u || _imageHeight == 0u) {
        return false;
    }
    const ayt::math::FRectangle image = imageRect();
    if (!clamp && !contains(image, point)) return false;
    x = (point.x - image.minX) * static_cast<float>(_imageWidth)
        / image.width();
    y = (point.y - image.minY) * static_cast<float>(_imageHeight)
        / image.height();
    x = std::clamp(x, 0.0f, static_cast<float>(_imageWidth) - 0.001f);
    y = std::clamp(y, 0.0f, static_cast<float>(_imageHeight) - 0.001f);
    return true;
}

uint32_t EditorSpriteAnimationCanvas::hitCell(
    ayt::math::FVector2 point) const noexcept
{
    float x = 0.0f;
    float y = 0.0f;
    return _model != nullptr && sourcePoint(point, x, y, true)
        ? _model->cellAtPixel(x, y) : 0u;
}

bool EditorSpriteAnimationCanvas::onMouseMove(
    const ayt::ui::UIMouseEvent& event)
{
    if (_previewOnly) return false;
    if (_panning) {
        _pan.x += event.mousePos.x - _lastPointer.x;
        _pan.y += event.mousePos.y - _lastPointer.y;
        _lastPointer = event.mousePos;
        markDirty();
        return true;
    }
    if (_selecting && _model != nullptr) {
        _model->selectRange(_selectionAnchor, hitCell(event.mousePos));
        if (_onSelectionChanged) _onSelectionChanged();
        markDirty();
        return true;
    }
    float x = 0.0f;
    float y = 0.0f;
    const bool valid = sourcePoint(event.mousePos, x, y, false);
    const uint32_t cell = valid && _model != nullptr
        ? _model->cellAtPixel(x, y) : 0u;
    if (_hoverValid != valid || (valid && _hoverCell != cell)) {
        _hoverValid = valid;
        _hoverCell = cell;
        markDirty();
    }
    _lastPointer = event.mousePos;
    return valid;
}

bool EditorSpriteAnimationCanvas::onMouseButtonDown(
    const ayt::ui::UIMouseEvent& event)
{
    if (_previewOnly || _model == nullptr || !_texture.isValid()) return false;
    _lastPointer = event.mousePos;
    if (event.mouseButton == 2 && contains(imageRect(), event.mousePos)) {
        _panning = true;
        return true;
    }
    float x = 0.0f;
    float y = 0.0f;
    if (event.mouseButton != 0 || !sourcePoint(event.mousePos, x, y, false)) {
        return false;
    }
    _selectionAnchor = _model->cellAtPixel(x, y);
    _model->selectRange(_selectionAnchor, _selectionAnchor);
    _selecting = true;
    if (_onSelectionChanged) _onSelectionChanged();
    markDirty();
    return true;
}

bool EditorSpriteAnimationCanvas::onMouseButtonUp(
    const ayt::ui::UIMouseEvent& event)
{
    if (_previewOnly) return false;
    if (event.mouseButton == 0 && _selecting) {
        _selecting = false;
        if (_onSelectionChanged) _onSelectionChanged();
        markDirty();
        return true;
    }
    if (event.mouseButton == 2 && _panning) {
        _panning = false;
        markDirty();
        return true;
    }
    return false;
}

bool EditorSpriteAnimationCanvas::onMouseWheel(
    const ayt::ui::UIMouseWheelEvent& event)
{
    if (_previewOnly || !_texture.isValid()) return false;
    const ayt::math::FRectangle before = imageRect();
    const float sourceX = (event.mousePos.x - before.minX)
        * static_cast<float>(_imageWidth) / before.width();
    const float sourceY = (event.mousePos.y - before.minY)
        * static_cast<float>(_imageHeight) / before.height();
    _zoom = std::clamp(_zoom * std::pow(1.12f, -event.deltaY / 40.0f),
                       1.0f, 32.0f);
    const ayt::math::FRectangle after = imageRect();
    _pan.x += event.mousePos.x - sourceX * after.width()
        / static_cast<float>(_imageWidth) - after.minX;
    _pan.y += event.mousePos.y - sourceY * after.height()
        / static_cast<float>(_imageHeight) - after.minY;
    markDirty();
    return true;
}

void EditorSpriteAnimationCanvas::onMouseLeave()
{
    if (_panning || _selecting) return;
    if (_hoverValid) {
        _hoverValid = false;
        markDirty();
    }
}

ayt::ui::UiCursorHint EditorSpriteAnimationCanvas::getCursorHint() const
{
    if (_previewOnly) return ayt::ui::UiCursorHint::Default;
    return _panning ? ayt::ui::UiCursorHint::Move
                    : ayt::ui::UiCursorHint::Default;
}

void EditorSpriteAnimationCanvas::tick(float dt)
{
    Widget::tick(dt);
    if (getParent() == nullptr) return;
    const ayt::math::FVector2 size = getParent()->getSize();
    if (getPosition().x != 0.0f || getPosition().y != 0.0f) {
        setPosition({0.0f, 0.0f});
    }
    if (getSize().x != size.x || getSize().y != size.y) setSize(size);
}

void EditorSpriteAnimationCanvas::renderSheet(
    ayt::ui::IRenderBackend& renderer,
    const ayt::math::FRectangle& bounds)
{
    renderer.pushClip(bounds);
    const ayt::math::FRectangle image = imageRect();
    renderer.drawRect(image, _texture.handle, {0.0f, 0.0f, 1.0f, 1.0f});
    const auto& draft = _model->draft();
    const float cellWidth = image.width() / static_cast<float>(draft.columns);
    const float cellHeight = image.height() / static_cast<float>(draft.rows);

    const uint32_t first = draft.firstFrame;
    const uint32_t last = _model->selectedLastFrame();
    const uint32_t firstRow = first / draft.columns;
    const uint32_t lastRow = last / draft.columns;
    for (uint32_t row = firstRow; row <= lastRow; ++row) {
        const uint32_t rowFirst = row == firstRow
            ? first : row * draft.columns;
        const uint32_t rowLast = row == lastRow
            ? last : row * draft.columns + draft.columns - 1u;
        const auto left = cellRect(rowFirst);
        const auto right = cellRect(rowLast);
        renderer.drawRect({left.minX, left.minY, right.maxX, right.maxY},
                          {0.96f, 0.61f, 0.12f, 0.20f});
    }

    if (cellWidth >= 5.0f) {
        for (uint32_t column = 0u; column <= draft.columns; ++column) {
            const float x = image.minX + cellWidth * column;
            renderer.drawRect({x - 0.5f, image.minY,
                               x + 0.5f, image.maxY},
                              {0.20f, 0.66f, 0.94f, 0.58f});
        }
    }
    if (cellHeight >= 5.0f) {
        for (uint32_t row = 0u; row <= draft.rows; ++row) {
            const float y = image.minY + cellHeight * row;
            renderer.drawRect({image.minX, y - 0.5f,
                               image.maxX, y + 0.5f},
                              {0.20f, 0.66f, 0.94f, 0.58f});
        }
    }
    renderer.drawBorderRect(cellRect(_model->currentCell()),
                            {0.30f, 0.80f, 1.0f, 1.0f}, 2.0f);
    if (_hoverValid) {
        renderer.drawRect(cellRect(_hoverCell),
                          {1.0f, 1.0f, 1.0f, 0.12f});
        renderer.drawBorderRect(cellRect(_hoverCell),
                                {1.0f, 1.0f, 1.0f, 0.92f}, 1.0f);
    }
    renderer.popClip();
}

void EditorSpriteAnimationCanvas::renderPreview(
    ayt::ui::IRenderBackend& renderer,
    const ayt::math::FRectangle& bounds)
{
    const auto& draft = _model->draft();
    const float frameWidth = static_cast<float>(_imageWidth)
        / static_cast<float>(draft.columns);
    const float frameHeight = static_cast<float>(_imageHeight)
        / static_cast<float>(draft.rows);
    const float fit = std::min(
        std::max(1.0f, bounds.width() - kPadding * 2.0f) / frameWidth,
        std::max(1.0f, bounds.height() - kPadding * 2.0f) / frameHeight);
    const float width = frameWidth * fit;
    const float height = frameHeight * fit;
    const float centerX = (bounds.minX + bounds.maxX) * 0.5f;
    const float centerY = (bounds.minY + bounds.maxY) * 0.5f;
    const ayt::math::FRectangle destination{
        centerX - width * 0.5f, centerY - height * 0.5f,
        centerX + width * 0.5f, centerY + height * 0.5f};
    const auto uv = _model->cellRect(_model->currentCell());
    renderer.drawRect(destination, _texture.handle,
                      {uv.minU, uv.minV, uv.maxU, uv.maxV});
    renderer.drawBorderRect(destination,
                            {0.30f, 0.80f, 1.0f, 0.90f}, 1.0f);
}

void EditorSpriteAnimationCanvas::onRender(
    ayt::ui::IRenderBackend& renderer)
{
    const ayt::math::FRectangle bounds = getWorldBounds();
    renderer.drawRect(bounds, {0.025f, 0.030f, 0.040f, 1.0f});
    renderer.drawBorderRect(bounds, {0.18f, 0.22f, 0.28f, 1.0f}, 1.0f);
    if (_model == nullptr || !_texture.isValid()
        || _imageWidth == 0u || _imageHeight == 0u) {
        ayt::ui::IRenderBackend::TextStyle style;
        style.color = {0.50f, 0.55f, 0.64f, 1.0f};
        style.align = ayt::ui::IRenderBackend::TextStyle::Align::Center;
        style.valign = ayt::ui::IRenderBackend::TextStyle::VAlign::Middle;
        renderer.drawText(bounds,
            _previewOnly ? L"No animation preview" : L"Select a Sprite with a source image",
            12, style);
        return;
    }
    if (_previewOnly) renderPreview(renderer, bounds);
    else renderSheet(renderer, bounds);
}

} // namespace ayt::editor
