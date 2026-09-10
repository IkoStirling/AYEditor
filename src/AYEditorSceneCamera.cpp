#include "AYEditor/EditorSceneCamera.h"

#include <AYMath/MathUtils.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace ayt::editor {
namespace {

constexpr float kDegreesToRadians = 0.017453292519943295f;

bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept
{
    if (lhs.size() != rhs.size()) return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(lhs[i]);
        const unsigned char b = static_cast<unsigned char>(rhs[i]);
        if (static_cast<char>(std::tolower(a))
            != static_cast<char>(std::tolower(b))) {
            return false;
        }
    }
    return true;
}

bool finite(const ayt::math::FVector2& value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y);
}

} // namespace

SceneViewMode chooseInitialSceneViewMode(
    std::string_view projectDefaultSceneView,
    const EditorSceneContentProfile& content,
    std::string_view engineProfile) noexcept
{
    if (equalsIgnoreCase(projectDefaultSceneView, "2D")) {
        return SceneViewMode::TwoD;
    }
    if (equalsIgnoreCase(projectDefaultSceneView, "3D")) {
        return SceneViewMode::ThreeD;
    }
    if (content.hasOrthographicCamera
        || (content.hasTwoDContent && !content.hasThreeDContent)) {
        return SceneViewMode::TwoD;
    }
    if (content.hasThreeDContent) {
        return SceneViewMode::ThreeD;
    }
    return equalsIgnoreCase(engineProfile, "CLIENT_2D")
        ? SceneViewMode::TwoD : SceneViewMode::ThreeD;
}

EditorSceneCamera::EditorSceneCamera()
{
    _twoD.setViewport(_viewportWidth, _viewportHeight);
    _twoD.setCamera({0.0f, 0.0f}, 600.0f);
}

void EditorSceneCamera::setViewport(std::uint32_t widthPx,
                                    std::uint32_t heightPx) noexcept
{
    _viewportWidth = std::max(1u, widthPx);
    _viewportHeight = std::max(1u, heightPx);
    _twoD.setViewport(_viewportWidth, _viewportHeight);
}

float EditorSceneCamera::viewportAspect() const noexcept
{
    return static_cast<float>(_viewportWidth)
         / static_cast<float>(_viewportHeight);
}

void EditorSceneCamera::setMode(SceneViewMode mode) noexcept
{
    _mode = mode;
    _threeD.endLook();
    _twoDPanning = false;
}

void EditorSceneCamera::setThreeDProjection(ProjectionMode mode) noexcept
{
    _threeDProjection = mode;
}

void EditorSceneCamera::setTwoDPose(ayt::math::FVector2 center,
                                    float verticalWorldSize) noexcept
{
    if (!finite(center) || !std::isfinite(verticalWorldSize)
        || verticalWorldSize <= 0.0f) {
        return;
    }
    _twoD.setCamera(center, verticalWorldSize);
}

void EditorSceneCamera::beginTwoDPan(
    ayt::math::FVector2 logicalPoint) noexcept
{
    if (!finite(logicalPoint)) return;
    _twoDPanning = true;
    _lastTwoDPanPoint = logicalPoint;
}

bool EditorSceneCamera::updateTwoDPan(
    ayt::math::FVector2 logicalPoint) noexcept
{
    if (!_twoDPanning || !finite(logicalPoint)) return false;
    _twoD.panPixels(logicalPoint.x - _lastTwoDPanPoint.x,
                    logicalPoint.y - _lastTwoDPanPoint.y);
    _lastTwoDPanPoint = logicalPoint;
    return true;
}

void EditorSceneCamera::zoomAt(float factor,
                               ayt::math::FVector2 logicalAnchor) noexcept
{
    if (!std::isfinite(factor) || factor <= 0.0f
        || !finite(logicalAnchor)) {
        return;
    }
    if (isTwoD()) {
        _twoD.zoomAt(factor, logicalAnchor);
        return;
    }
    if (_threeDProjection == ProjectionMode::Orthographic) {
        _threeDOrthoHeight = std::clamp(
            _threeDOrthoHeight / factor, 0.01f, 1000000.0f);
    }
}

ayt::math::FVector2 EditorSceneCamera::twoDWorldAt(
    ayt::math::FVector2 logicalPoint) const noexcept
{
    return _twoD.screenToWorld(logicalPoint);
}

float EditorSceneCamera::adaptiveGridSpacing(float targetPixels) const noexcept
{
    const float safeTarget = std::max(8.0f, targetPixels);
    const float unitsPerPixel = _twoD.verticalWorldSize()
                              / static_cast<float>(_viewportHeight);
    const float targetWorld = std::max(1.0e-6f, unitsPerPixel * safeTarget);
    const float decade = std::pow(10.0f, std::floor(std::log10(targetWorld)));
    const float normalized = targetWorld / decade;
    const float step = normalized <= 1.0f ? 1.0f
                     : normalized <= 2.0f ? 2.0f
                     : normalized <= 5.0f ? 5.0f : 10.0f;
    return step * decade;
}

EditorSceneCameraFrame EditorSceneCamera::frame() const noexcept
{
    EditorSceneCameraFrame result;
    const float aspect = viewportAspect();
    if (isTwoD()) {
        const ayt::math::FVector2 center = _twoD.center();
        const float halfH = _twoD.verticalWorldSize() * 0.5f;
        result.view = ayt::math::translate(-center.x, -center.y, 0.0f);
        result.projection = ayt::math::lh::ortho(
            -halfH * aspect, halfH * aspect, -halfH, halfH,
            -10000.0f, 10000.0f);
        result.position = {center.x, center.y, 1.0f};
        result.orthographic = true;
        return result;
    }

    result.view = ayt::math::lh::lookAt(
        _threeD.eye(), _threeD.at(), _threeD.up());
    result.position = _threeD.eye();
    if (_threeDProjection == ProjectionMode::Orthographic) {
        const float halfH = _threeDOrthoHeight * 0.5f;
        result.projection = ayt::math::lh::ortho(
            -halfH * aspect, halfH * aspect, -halfH, halfH,
            0.1f, 1000.0f);
        result.orthographic = true;
    } else {
        result.projection = ayt::math::lh::perspective(
            _threeD.fovYDegrees() * kDegreesToRadians,
            aspect, 0.1f, 1000.0f);
    }
    return result;
}

bool EditorSceneCamera::ray(
    ayt::math::FVector2 logicalPoint,
    ayt::math::FVector3& outOrigin,
    ayt::math::FVector3& outDirection) const noexcept
{
    if (!finite(logicalPoint)) return false;
    const float ndcX = 2.0f * logicalPoint.x
                     / static_cast<float>(_viewportWidth) - 1.0f;
    const float ndcY = 1.0f - 2.0f * logicalPoint.y
                     / static_cast<float>(_viewportHeight);

    if (isTwoD()) {
        const ayt::math::FVector2 world = _twoD.screenToWorld(logicalPoint);
        outOrigin = {world.x, world.y, 1.0f};
        outDirection = {0.0f, 0.0f, -1.0f};
        return true;
    }

    const ayt::math::FVector3 forward = _threeD.forward();
    if (_threeDProjection == ProjectionMode::Orthographic) {
        const float halfH = _threeDOrthoHeight * 0.5f;
        outOrigin = _threeD.eye()
            + _threeD.right() * (ndcX * halfH * viewportAspect())
            + _threeD.up() * (ndcY * halfH);
        outDirection = forward;
        return outDirection.lengthSq() > 1.0e-8f;
    }

    const float tanHalfFov = std::tan(
        _threeD.fovYDegrees() * kDegreesToRadians * 0.5f);
    outOrigin = _threeD.eye();
    outDirection = forward
        + _threeD.right() * (ndcX * viewportAspect() * tanHalfFov)
        + _threeD.up() * (ndcY * tanHalfFov);
    if (outDirection.lengthSq() < 1.0e-8f) return false;
    outDirection = outDirection.normalize();
    return true;
}

EditorSceneCameraState EditorSceneCamera::state() const noexcept
{
    EditorSceneCameraState result;
    result.mode = _mode;
    result.threeDProjection = _threeDProjection;
    result.twoDPlane = _twoDPlane;
    result.twoDCenter = _twoD.center();
    result.twoDViewHeight = _twoD.verticalWorldSize();
    result.threeDOrthoHeight = _threeDOrthoHeight;
    result.threeDEye = _threeD.eye();
    result.threeDYawRadians = _threeD.yawRadians();
    result.threeDPitchRadians = _threeD.pitchRadians();
    result.threeDMoveSpeed = _threeD.moveSpeed();
    return result;
}

void EditorSceneCamera::restoreState(
    const EditorSceneCameraState& state) noexcept
{
    setMode(state.mode);
    setThreeDProjection(state.threeDProjection);
    _twoDPlane = state.twoDPlane;
    setTwoDPose(state.twoDCenter, state.twoDViewHeight);
    if (std::isfinite(state.threeDOrthoHeight)
        && state.threeDOrthoHeight > 0.0f) {
        _threeDOrthoHeight = std::clamp(
            state.threeDOrthoHeight, 0.01f, 1000000.0f);
    }
    _threeD.setPose(state.threeDEye,
                    state.threeDYawRadians,
                    state.threeDPitchRadians);
    if (std::isfinite(state.threeDMoveSpeed)
        && state.threeDMoveSpeed > 0.01f) {
        _threeD.setMoveSpeed(state.threeDMoveSpeed);
    }
}

} // namespace ayt::editor
