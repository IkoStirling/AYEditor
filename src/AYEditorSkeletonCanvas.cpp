#include "AYEditorSkeletonCanvas.h"

#include <AYUI/IRenderBackend.h>

#include <algorithm>
#include <cmath>

namespace ayt::editor {
namespace {

void drawFallbackLine(ayt::ui::IRenderBackend& renderer,
                      float x0, float y0, float x1, float y1,
                      const ayt::math::FVector4& color)
{
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const int steps = std::max(1, static_cast<int>(
        std::ceil(std::max(std::fabs(dx), std::fabs(dy)) / 3.0f)));
    for (int step = 0; step <= steps; ++step) {
        const float t = static_cast<float>(step) / steps;
        const float x = x0 + dx * t;
        const float y = y0 + dy * t;
        renderer.drawRect({x - 1.0f, y - 1.0f, x + 1.0f, y + 1.0f}, color);
    }
}

} // namespace

EditorSkeletonCanvas::EditorSkeletonCanvas(
    std::shared_ptr<EditorSkeletonDocument> document)
    : _document(std::move(document))
{
    setId("skeleton_editor_canvas");
}

void EditorSkeletonCanvas::frameSkeleton()
{
    _yaw = 0.55f;
    _pitch = -0.18f;
    _zoom = 1.0f;
    markDirty();
}

void EditorSkeletonCanvas::rebuildProjection()
{
    if (_document == nullptr) {
        _worldPoints.clear();
        _projected.clear();
        _targetWorldPoints.clear();
        _targetProjected.clear();
        _projectionValid = false;
        return;
    }
    const auto bounds = getWorldBounds();
    const std::uint64_t poseRevision = _document->core().poseRevision();
    if (_projectionValid
        && _projectedPoseRevision == poseRevision
        && _projectedYaw == _yaw
        && _projectedPitch == _pitch
        && _projectedZoom == _zoom
        && _projectedBounds.minX == bounds.minX
        && _projectedBounds.minY == bounds.minY
        && _projectedBounds.maxX == bounds.maxX
        && _projectedBounds.maxY == bounds.maxY) {
        return;
    }

    _projectionValid = true;
    _projectedPoseRevision = poseRevision;
    _projectedYaw = _yaw;
    _projectedPitch = _pitch;
    _projectedZoom = _zoom;
    _projectedBounds = bounds;
    _worldPoints.clear();
    _projected.clear();
    _targetWorldPoints.clear();
    _targetProjected.clear();
    const auto& world = _document->core().poseWorldMatrices();
    if (world.empty()) return;
    const float cy = std::cos(_yaw), sy = std::sin(_yaw);
    const float cp = std::cos(_pitch), sp = std::sin(_pitch);
    const auto project = [&](const std::vector<ayt::math::Float4x4>& matrices,
                             const ayt::math::FRectangle& viewport,
                             std::vector<ayt::math::FVector3>& worldPoints,
                             std::vector<ProjectedPoint>& projected) {
        if (matrices.empty()) return;
        worldPoints.reserve(matrices.size());
        ayt::math::FVector3 minimum =
            matrices.front().transformPoint({0, 0, 0});
        ayt::math::FVector3 maximum = minimum;
        for (const auto& matrix : matrices) {
            const ayt::math::FVector3 point =
                matrix.transformPoint({0, 0, 0});
            worldPoints.push_back(point);
            minimum.x = std::min(minimum.x, point.x);
            minimum.y = std::min(minimum.y, point.y);
            minimum.z = std::min(minimum.z, point.z);
            maximum.x = std::max(maximum.x, point.x);
            maximum.y = std::max(maximum.y, point.y);
            maximum.z = std::max(maximum.z, point.z);
        }
        const ayt::math::FVector3 center = (minimum + maximum) * 0.5f;
        const float extent = std::max({maximum.x - minimum.x,
            maximum.y - minimum.y, maximum.z - minimum.z, 0.01f});
        const float width = std::max(1.0f, viewport.maxX - viewport.minX);
        const float height = std::max(1.0f, viewport.maxY - viewport.minY);
        const float scale = std::min(width, height) * 0.68f / extent * _zoom;
        projected.reserve(worldPoints.size());
        for (ayt::math::FVector3 point : worldPoints) {
            point -= center;
            const float x1 = cy * point.x + sy * point.z;
            const float z1 = -sy * point.x + cy * point.z;
            const float y2 = cp * point.y - sp * z1;
            const float z2 = sp * point.y + cp * z1;
            projected.push_back({
                (viewport.minX + viewport.maxX) * 0.5f + x1 * scale,
                (viewport.minY + viewport.maxY) * 0.5f - y2 * scale,
                z2});
        }
    };
    const auto& targetWorld =
        _document->core().targetPoseWorldMatrices();
    if (targetWorld.empty()) {
        project(world, bounds, _worldPoints, _projected);
    } else {
        const float midpoint = (bounds.minX + bounds.maxX) * 0.5f;
        project(world, {bounds.minX, bounds.minY, midpoint, bounds.maxY},
            _worldPoints, _projected);
        project(targetWorld,
            {midpoint, bounds.minY, bounds.maxX, bounds.maxY},
            _targetWorldPoints, _targetProjected);
    }
}

int EditorSkeletonCanvas::hitBone(ayt::math::FVector2 point) const noexcept
{
    int best = -1;
    float bestDistance = 12.0f * 12.0f;
    for (std::size_t index = 0; index < _projected.size(); ++index) {
        const float dx = _projected[index].x - point.x;
        const float dy = _projected[index].y - point.y;
        const float distance = dx * dx + dy * dy;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = static_cast<int>(index);
        }
    }
    return best;
}

bool EditorSkeletonCanvas::onMouseButtonDown(const ayt::ui::UIMouseEvent& event)
{
    _lastPointer = event.mousePos;
    if (event.mouseButton == 1 || event.mouseButton == 2) {
        _rotating = true;
        return true;
    }
    if (event.mouseButton != 0) return false;
    rebuildProjection();
    const int selected = hitBone(event.mousePos);
    if (selected < 0 || _document == nullptr) return false;
    (void)_document->core().selectBone(selected);
    if (_onBoneSelected) _onBoneSelected(selected);
    markDirty();
    return true;
}

bool EditorSkeletonCanvas::onMouseMove(const ayt::ui::UIMouseEvent& event)
{
    if (!_rotating) return false;
    _yaw += (event.mousePos.x - _lastPointer.x) * 0.012f;
    _pitch = std::clamp(_pitch
        + (event.mousePos.y - _lastPointer.y) * 0.012f, -1.5f, 1.5f);
    _lastPointer = event.mousePos;
    markDirty();
    return true;
}

bool EditorSkeletonCanvas::onMouseButtonUp(const ayt::ui::UIMouseEvent& event)
{
    if (!_rotating || (event.mouseButton != 1 && event.mouseButton != 2)) {
        return false;
    }
    _rotating = false;
    return true;
}

bool EditorSkeletonCanvas::onMouseWheel(
    const ayt::ui::UIMouseWheelEvent& event)
{
    _zoom = std::clamp(_zoom * (event.deltaY > 0.0f ? 1.12f : 0.89f),
                       0.2f, 8.0f);
    markDirty();
    return true;
}

void EditorSkeletonCanvas::onMouseLeave()
{
    _rotating = false;
    ayt::ui::Widget::onMouseLeave();
}

ayt::ui::UiCursorHint EditorSkeletonCanvas::getCursorHint() const
{
    return _rotating ? ayt::ui::UiCursorHint::Move
                     : ayt::ui::UiCursorHint::Default;
}

void EditorSkeletonCanvas::onRender(ayt::ui::IRenderBackend& renderer)
{
    const auto bounds = getWorldBounds();
    renderer.drawRect(bounds, {0.055f, 0.065f, 0.085f, 1.0f});
    rebuildProjection();
    if (_document == nullptr) return;
    const auto& bones = _document->core().bones();
    const auto drawSkeleton = [&renderer](
        const std::vector<ayt::anim::editor::SkeletonBoneView>& views,
        const std::vector<ProjectedPoint>& projected,
        const ayt::math::FVector4& color) {
        const auto path = renderer.createPath();
        if (path.id >= 0) {
            for (std::size_t index = 0; index < views.size()
                 && index < projected.size(); ++index) {
                const int parent = views[index].parentIndex;
                if (parent < 0
                    || parent >= static_cast<int>(projected.size())) continue;
                const ayt::math::FVector2 segment[] = {
                    {projected[static_cast<std::size_t>(parent)].x,
                     projected[static_cast<std::size_t>(parent)].y},
                    {projected[index].x, projected[index].y},
                };
                renderer.addPathContour(path, segment, 2, false);
            }
            renderer.setPathStrokeColor(path, color);
            renderer.setPathStrokeWidth(path, 2.0f);
            renderer.setPathStrokeStyle(path, ayt::ui::PathStrokeCap::Round,
                                        ayt::ui::PathStrokeJoin::Round);
            renderer.drawPath(path, ayt::ui::PathFillMode::Stroke);
            renderer.releasePath(path);
        } else {
            for (std::size_t index = 0; index < views.size()
                 && index < projected.size(); ++index) {
                const int parent = views[index].parentIndex;
                if (parent < 0
                    || parent >= static_cast<int>(projected.size())) continue;
                const auto& a = projected[static_cast<std::size_t>(parent)];
                const auto& b = projected[index];
                drawFallbackLine(renderer, a.x, a.y, b.x, b.y, color);
            }
        }
    };
    drawSkeleton(bones, _projected,
        {0.34f, 0.72f, 0.96f, 1.0f});
    if (!_targetProjected.empty()) {
        drawSkeleton(_document->core().targetBones(), _targetProjected,
            {0.38f, 0.88f, 0.58f, 1.0f});
    }
    const int selected = _document->core().selectedBone();
    for (std::size_t index = 0; index < _projected.size(); ++index) {
        const auto& point = _projected[index];
        const float radius = static_cast<int>(index) == selected ? 5.0f : 3.0f;
        const ayt::math::FVector4 color = static_cast<int>(index) == selected
            ? ayt::math::FVector4{1.0f, 0.68f, 0.20f, 1.0f}
            : ayt::math::FVector4{0.82f, 0.88f, 0.96f, 1.0f};
        renderer.drawRect({point.x - radius, point.y - radius,
                           point.x + radius, point.y + radius}, color);
    }
    for (const auto& point : _targetProjected) {
        constexpr float radius = 3.0f;
        renderer.drawRect({point.x - radius, point.y - radius,
                           point.x + radius, point.y + radius},
            {0.76f, 0.96f, 0.82f, 1.0f});
    }
    if (!_targetProjected.empty()) {
        const float midpoint = (bounds.minX + bounds.maxX) * 0.5f;
        renderer.drawRect({midpoint - 0.5f, bounds.minY + 4.0f,
                           midpoint + 0.5f, bounds.maxY - 4.0f},
            {0.18f, 0.22f, 0.28f, 1.0f});
        renderer.drawText({bounds.minX + 8.0f, bounds.minY + 26.0f,
                           midpoint - 8.0f, bounds.minY + 46.0f},
            L"SOURCE", 11,
            ayt::math::FVector4{0.34f, 0.72f, 0.96f, 1.0f});
        renderer.drawText({midpoint + 8.0f, bounds.minY + 26.0f,
                           bounds.maxX - 8.0f, bounds.minY + 46.0f},
            L"TARGET", 11,
            ayt::math::FVector4{0.38f, 0.88f, 0.58f, 1.0f});
    }
    renderer.drawText({bounds.minX + 8.0f, bounds.minY + 6.0f,
                       bounds.maxX - 8.0f, bounds.minY + 26.0f},
        L"LMB select  |  RMB/MMB rotate  |  Wheel zoom", 11,
        ayt::math::FVector4{0.55f, 0.60f, 0.68f, 1.0f});
}

} // namespace ayt::editor
