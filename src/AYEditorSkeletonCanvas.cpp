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
    _orbit.reset();
    _projectionCache.invalidate();
    markDirty();
}

void EditorSkeletonCanvas::rebuildProjection()
{
    if (_document == nullptr) {
        _worldPoints.clear();
        _projected.clear();
        _targetWorldPoints.clear();
        _targetProjected.clear();
        _projectionCache.invalidate();
        return;
    }
    const auto bounds = getWorldBounds();
    const std::uint64_t poseRevision = _document->core().poseRevision();
    if (!_projectionCache.consume({0u, poseRevision, bounds,
        _orbit.yaw, _orbit.pitch, _orbit.zoom})) return;
    _worldPoints.clear();
    _projected.clear();
    _targetWorldPoints.clear();
    _targetProjected.clear();
    const auto& world = _document->core().poseWorldMatrices();
    if (world.empty()) return;
    const auto project = [&](const std::vector<ayt::math::Float4x4>& matrices,
                             const ayt::math::FRectangle& viewport,
                             std::vector<ayt::math::FVector3>& worldPoints,
                             std::vector<ProjectedPoint>& projected) {
        if (matrices.empty()) return;
        worldPoints.reserve(matrices.size());
        ayt::ui::authoring::PreviewBounds points;
        for (const auto& matrix : matrices) {
            const auto point = matrix.transformPoint({0, 0, 0});
            worldPoints.push_back(point);
            points.include(point);
        }
        const ayt::ui::authoring::PreviewProjection projection(points, viewport, _orbit, 0.68f);
        projected.reserve(worldPoints.size());
        for (const auto& point : worldPoints) projected.push_back(projection(point));
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
    if (event.mouseButton == 1 || event.mouseButton == 2) {
        _orbit.begin(event.mousePos);
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
    if (!_orbit.move(event.mousePos)) return false;
    markDirty();
    return true;
}

bool EditorSkeletonCanvas::onMouseButtonUp(const ayt::ui::UIMouseEvent& event)
{
    if (event.mouseButton != 1 && event.mouseButton != 2) return false;
    return _orbit.end();
}

bool EditorSkeletonCanvas::onMouseWheel(
    const ayt::ui::UIMouseWheelEvent& event)
{
    _orbit.wheel(event.deltaY);
    markDirty();
    return true;
}

void EditorSkeletonCanvas::onMouseLeave()
{
    _orbit.cancel();
    ayt::ui::Widget::onMouseLeave();
}

ayt::ui::UiCursorHint EditorSkeletonCanvas::getCursorHint() const
{
    return _orbit.rotating() ? ayt::ui::UiCursorHint::Move
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
