#include "AYEditorAnimationDopeSheet.h"

#include <AYUI/IRenderBackend.h>
#include <AYUI/UnicodeText.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace ayt::editor {
namespace {

constexpr float kLabelWidth = 168.0f;
constexpr float kHeaderHeight = 22.0f;
constexpr float kRowHeight = 24.0f;
constexpr float kKeyRadius = 5.0f;

} // namespace

EditorAnimationDopeSheet::EditorAnimationDopeSheet(
    std::shared_ptr<EditorAnimationDocument> document)
    : _document(std::move(document))
{
    setId("animation_dope_sheet");
}

void EditorAnimationDopeSheet::frameAll()
{
    _viewValid = false;
    markDirty();
}

void EditorAnimationDopeSheet::setSelection(
    std::string trackId, std::string keyId)
{
    if (_selectedTrackId == trackId && _selectedKeyId == keyId) return;
    _selectedTrackId = std::move(trackId);
    _selectedKeyId = std::move(keyId);
    markDirty();
}

ayt::math::FRectangle EditorAnimationDopeSheet::plotBounds() const noexcept
{
    const auto bounds = getWorldBounds();
    return {std::min(bounds.maxX, bounds.minX + kLabelWidth),
            std::min(bounds.maxY, bounds.minY + kHeaderHeight),
            bounds.maxX, bounds.maxY};
}

double EditorAnimationDopeSheet::secondsAt(float x) const noexcept
{
    const auto plot = plotBounds();
    return _viewStart + std::clamp(static_cast<double>((x - plot.minX)
        / std::max(1.0f, plot.maxX - plot.minX)), 0.0, 1.0)
        * _viewDuration;
}

float EditorAnimationDopeSheet::worldX(double seconds) const noexcept
{
    const auto plot = plotBounds();
    return plot.minX + static_cast<float>((seconds - _viewStart)
        / std::max(1.0e-9, _viewDuration)) * (plot.maxX - plot.minX);
}

EditorAnimationDopeSheet::KeyHit EditorAnimationDopeSheet::hitKey(
    ayt::math::FVector2 point) const
{
    if (_document == nullptr || !plotBounds().contains(point)) return {};
    const auto tracks = _document->timelineTracks();
    const auto keys = _document->timelineKeyframes();
    for (std::size_t row = 0u; row < tracks.size(); ++row) {
        if (tracks[row].kind != EditorTimelineTrackKind::Animation) continue;
        const float y = getWorldBounds().minY + kHeaderHeight
            + (static_cast<float>(row) + 0.5f) * kRowHeight;
        for (const auto& key : keys) {
            if (key.trackId != tracks[row].id) continue;
            const ayt::math::FVector2 center{worldX(key.timeSeconds), y};
            const float dx = point.x - center.x;
            const float dy = point.y - center.y;
            if (dx * dx + dy * dy <= 100.0f) {
                return {key.trackId, key.id};
            }
        }
    }
    return {};
}

bool EditorAnimationDopeSheet::onMouseButtonDown(
    const ayt::ui::UIMouseEvent& event)
{
    if (!getWorldBounds().contains(event.mousePos)) return false;
    _lastPointer = event.mousePos;
    if (event.mouseButton == 1 || event.mouseButton == 2) {
        _panning = true;
        return true;
    }
    if (event.mouseButton != 0 || !plotBounds().contains(event.mousePos)) {
        return false;
    }
    const KeyHit hit = hitKey(event.mousePos);
    if (hit.keyId.empty()) {
        if (_document != nullptr) {
            (void)_document->setTimelinePositionSeconds(secondsAt(event.mousePos.x));
        }
        markDirty();
        return true;
    }
    _selectedTrackId = hit.trackId;
    _selectedKeyId = hit.keyId;
    _dragKeyId = hit.keyId;
    _gestureChanged = false;
    if (_document != nullptr
        && _document->beginAnimationEditGesture("Move dope sheet key")) {
        _draggingKey = true;
        if (_onSelectionChanged) {
            _onSelectionChanged(_selectedTrackId, _selectedKeyId);
        }
        markDirty();
        return true;
    }
    _dragKeyId.clear();
    return false;
}

bool EditorAnimationDopeSheet::onMouseMove(
    const ayt::ui::UIMouseEvent& event)
{
    if (_panning) {
        const auto plot = plotBounds();
        _viewStart -= (event.mousePos.x - _lastPointer.x)
            / std::max(1.0f, plot.maxX - plot.minX) * _viewDuration;
        _lastPointer = event.mousePos;
        markDirty();
        return true;
    }
    if (!_draggingKey || _document == nullptr) {
        return getWorldBounds().contains(event.mousePos);
    }
    std::vector<float> values;
    if (!_document->animationKeyframeValues(_dragKeyId, values)) return true;
    EditorAnimationCurveTrack track;
    if (!_document->animationCurveTrack(_selectedTrackId, track)) return true;
    const double frame = track.ticksPerSecond > 0.0
        ? 1.0 / track.ticksPerSecond : 0.0;
    double seconds = secondsAt(event.mousePos.x);
    if (frame > 0.0) seconds = std::round(seconds / frame) * frame;
    std::string updated = _dragKeyId;
    if (_document->updateAnimationKeyframe(updated, seconds, values)) {
        _dragKeyId = updated;
        _selectedKeyId = updated;
        _gestureChanged = true;
        (void)_document->setTimelinePositionSeconds(seconds);
        if (_onSelectionChanged) {
            _onSelectionChanged(_selectedTrackId, _selectedKeyId);
        }
        if (_onEdited) _onEdited();
        markDirty();
    }
    return true;
}

bool EditorAnimationDopeSheet::onMouseButtonUp(
    const ayt::ui::UIMouseEvent& event)
{
    if ((event.mouseButton == 1 || event.mouseButton == 2) && _panning) {
        _panning = false;
        return true;
    }
    if (event.mouseButton != 0 || !_draggingKey) return false;
    finishDrag(false);
    return true;
}

bool EditorAnimationDopeSheet::onMouseWheel(
    const ayt::ui::UIMouseWheelEvent& event)
{
    if (!plotBounds().contains(event.mousePos)) return false;
    const double anchor = secondsAt(event.mousePos.x);
    const auto plot = plotBounds();
    const double normalized = std::clamp(static_cast<double>(
        (event.mousePos.x - plot.minX)
        / std::max(1.0f, plot.maxX - plot.minX)), 0.0, 1.0);
    _viewDuration = std::clamp(_viewDuration
        * std::pow(1.1, event.deltaY / 40.0f), 0.01, 3600.0);
    _viewStart = anchor - normalized * _viewDuration;
    _viewValid = true;
    markDirty();
    return true;
}

void EditorAnimationDopeSheet::finishDrag(bool cancel)
{
    if (_document != nullptr && _document->animationEditGestureActive()) {
        if (cancel) (void)_document->cancelAnimationEditGesture();
        else (void)_document->commitAnimationEditGesture();
    }
    const bool changed = _gestureChanged;
    _draggingKey = false;
    _dragKeyId.clear();
    _gestureChanged = false;
    if (changed && _onEdited) _onEdited();
    markDirty();
}

void EditorAnimationDopeSheet::onCaptureCancelled()
{
    _panning = false;
    finishDrag(true);
}

ayt::ui::UiCursorHint EditorAnimationDopeSheet::getCursorHint() const
{
    return _panning || _draggingKey ? ayt::ui::UiCursorHint::Move
                                    : ayt::ui::UiCursorHint::SizeHorizontal;
}

void EditorAnimationDopeSheet::onRender(ayt::ui::IRenderBackend& renderer)
{
    const auto bounds = getWorldBounds();
    renderer.pushClip(bounds);
    renderer.drawRect(bounds, {0.043f, 0.052f, 0.069f, 1.0f});
    if (_document == nullptr) {
        renderer.popClip();
        return;
    }
    if (!_viewValid) {
        _viewStart = 0.0;
        _viewDuration = std::max(0.05, _document->timelineDurationSeconds());
        _viewValid = true;
    }
    const auto plot = plotBounds();
    renderer.drawRect({bounds.minX, bounds.minY, bounds.maxX,
                       bounds.minY + kHeaderHeight},
                      {0.065f, 0.079f, 0.105f, 1.0f});
    renderer.drawText({bounds.minX + 8.0f, bounds.minY,
                       bounds.minX + kLabelWidth, bounds.minY + kHeaderHeight},
        L"DOPE SHEET", 11, ayt::math::FVector4{0.66f, 0.72f, 0.82f, 1.0f});
    for (int tick = 0; tick <= 10; ++tick) {
        const double seconds = _viewStart + _viewDuration * tick / 10.0;
        const float x = worldX(seconds);
        renderer.drawRect({x, bounds.minY, x + 1.0f, bounds.maxY},
                          {0.11f, 0.13f, 0.17f, 1.0f});
        std::wostringstream label;
        label << std::fixed << std::setprecision(2) << seconds;
        renderer.drawText({x + 3.0f, bounds.minY, x + 58.0f,
                           bounds.minY + kHeaderHeight}, label.str(), 9,
                          ayt::math::FVector4{0.47f, 0.53f, 0.62f, 1.0f});
    }
    const auto tracks = _document->timelineTracks();
    const auto keys = _document->timelineKeyframes();
    for (std::size_t row = 0u; row < tracks.size(); ++row) {
        const float y = bounds.minY + kHeaderHeight
            + static_cast<float>(row) * kRowHeight;
        if (y >= bounds.maxY) break;
        const bool selected = tracks[row].id == _selectedTrackId;
        if (selected) {
            renderer.drawRect({bounds.minX, y, bounds.maxX,
                               std::min(bounds.maxY, y + kRowHeight)},
                              {0.075f, 0.16f, 0.25f, 0.85f});
        }
        renderer.drawRect({bounds.minX, y + kRowHeight - 1.0f, bounds.maxX,
                           y + kRowHeight}, {0.10f, 0.12f, 0.16f, 1.0f});
        renderer.drawText({bounds.minX + 8.0f, y,
                           bounds.minX + kLabelWidth - 5.0f,
                           std::min(bounds.maxY, y + kRowHeight)},
            ayt::ui::decodeUtf8Text(tracks[row].name), 10,
            selected ? ayt::math::FVector4{0.83f, 0.91f, 1.0f, 1.0f}
                     : ayt::math::FVector4{0.59f, 0.65f, 0.74f, 1.0f});
        for (const auto& key : keys) {
            if (key.trackId != tracks[row].id) continue;
            const float x = worldX(key.timeSeconds);
            if (x < plot.minX - kKeyRadius || x > plot.maxX + kKeyRadius) continue;
            const float centerY = y + kRowHeight * 0.5f;
            const bool keySelected = key.id == _selectedKeyId;
            renderer.drawRoundedRect(
                {x - kKeyRadius, centerY - kKeyRadius,
                 x + kKeyRadius, centerY + kKeyRadius},
                keySelected
                    ? ayt::math::FVector4{1.0f, 0.75f, 0.28f, 1.0f}
                    : tracks[row].kind == EditorTimelineTrackKind::Event
                        ? ayt::math::FVector4{0.85f, 0.42f, 0.76f, 1.0f}
                        : ayt::math::FVector4{0.31f, 0.68f, 1.0f, 1.0f},
                2.0f);
        }
    }
    const float playhead = worldX(_document->timelinePositionSeconds());
    if (playhead >= plot.minX && playhead <= plot.maxX) {
        renderer.drawRect({playhead, bounds.minY, playhead + 1.0f, bounds.maxY},
                          {1.0f, 0.33f, 0.29f, 0.95f});
    }
    renderer.popClip();
}

} // namespace ayt::editor
