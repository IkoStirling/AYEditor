#include "AYEditor/EditorTransformGizmo.h"

#include <algorithm>
#include <cmath>

namespace ayt::editor {
namespace {

constexpr float kEpsilon = 1.0e-6f;
constexpr float kAxisStart = 0.14f;
constexpr float kAxisEnd = 1.02f;
constexpr float kAxisPickRadius = 0.085f;
constexpr float kPlaneMin = 0.24f;
constexpr float kPlaneMax = 0.48f;
constexpr float kRingRadius = 0.82f;
constexpr float kRingPickRadius = 0.075f;
// Universal uses separated radial bands so scale cubes cannot visually or
// semantically merge into translation arrows. Rotation lives outside both.
constexpr float kUniversalScaleStart = 0.13f;
constexpr float kUniversalScaleEnd = 0.40f;
constexpr float kUniversalMoveStart = 0.47f;
constexpr float kUniversalMoveEnd = 0.97f;
constexpr float kUniversalAxisPickRadius = 0.105f;
constexpr float kUniversalPlaneMin = 0.16f;
constexpr float kUniversalPlaneMax = 0.34f;
constexpr float kUniversalRingRadius = 1.03f;
constexpr float kUniversalRingPickRadius = 0.085f;
constexpr float kUniversalCenterPickRadius = 0.13f;

bool finiteVector(const ayt::math::FVector3& value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y)
        && std::isfinite(value.z);
}

ayt::math::FVector3 normalizedRay(
    const ayt::math::FVector3& direction) noexcept
{
    if (!finiteVector(direction) || direction.lengthSq() <= kEpsilon) {
        return {};
    }
    return direction.normalize();
}

bool intersectRayPlane(const ayt::math::FVector3& origin,
                       const ayt::math::FVector3& direction,
                       const ayt::math::FVector3& point,
                       const ayt::math::FVector3& normal,
                       ayt::math::FVector3& outHit,
                       float* outDistance = nullptr) noexcept
{
    const float denominator = direction.dot(normal);
    if (std::fabs(denominator) <= kEpsilon) return false;
    const float distance = (point - origin).dot(normal) / denominator;
    if (!std::isfinite(distance) || distance < 0.0f) return false;
    outHit = origin + direction * distance;
    if (outDistance != nullptr) *outDistance = distance;
    return finiteVector(outHit);
}

bool raySegmentDistance(const ayt::math::FVector3& origin,
                        const ayt::math::FVector3& direction,
                        const ayt::math::FVector3& segmentOrigin,
                        const ayt::math::FVector3& segmentAxis,
                        float segmentStart,
                         float segmentEnd,
                         float& outDistance,
                         float& outRayDistance,
                         float* outUnclampedSegmentT = nullptr) noexcept
{
    const ayt::math::FVector3 w = origin - segmentOrigin;
    const float b = direction.dot(segmentAxis);
    const float d = direction.dot(w);
    const float e = segmentAxis.dot(w);
    const float denominator = 1.0f - b * b;
    float segmentT = segmentStart;
    if (std::fabs(denominator) > kEpsilon) {
        segmentT = (e - b * d) / denominator;
    }
    if (outUnclampedSegmentT != nullptr) {
        *outUnclampedSegmentT = segmentT;
    }
    segmentT = std::clamp(segmentT, segmentStart, segmentEnd);
    const ayt::math::FVector3 segmentPoint =
        segmentOrigin + segmentAxis * segmentT;
    outRayDistance = std::max(0.0f, (segmentPoint - origin).dot(direction));
    const ayt::math::FVector3 rayPoint =
        origin + direction * outRayDistance;
    outDistance = (rayPoint - segmentPoint).length();
    return std::isfinite(outDistance) && std::isfinite(outRayDistance);
}

int axisIndex(EditorGizmoHandle handle) noexcept
{
    switch (handle) {
    case EditorGizmoHandle::AxisX:
    case EditorGizmoHandle::RingX:
    case EditorGizmoHandle::ScaleX: return 0;
    case EditorGizmoHandle::AxisY:
    case EditorGizmoHandle::RingY:
    case EditorGizmoHandle::ScaleY: return 1;
    case EditorGizmoHandle::AxisZ:
    case EditorGizmoHandle::RingZ:
    case EditorGizmoHandle::ScaleZ: return 2;
    default: return -1;
    }
}

bool planeAxes(EditorGizmoHandle handle, int& first, int& second) noexcept
{
    switch (handle) {
    case EditorGizmoHandle::PlaneXY: first = 0; second = 1; return true;
    case EditorGizmoHandle::PlaneYZ: first = 1; second = 2; return true;
    case EditorGizmoHandle::PlaneZX: first = 2; second = 0; return true;
    default: return false;
    }
}

float safeScaleComponent(float before, float factor) noexcept
{
    const float value = before * std::max(factor, 0.01f);
    if (std::fabs(value) >= 0.001f) return value;
    return std::copysign(0.001f, before == 0.0f ? 1.0f : before);
}

EditorTool toolForHandle(EditorGizmoHandle handle) noexcept
{
    switch (handle) {
    case EditorGizmoHandle::AxisX:
    case EditorGizmoHandle::AxisY:
    case EditorGizmoHandle::AxisZ:
    case EditorGizmoHandle::PlaneXY:
    case EditorGizmoHandle::PlaneYZ:
    case EditorGizmoHandle::PlaneZX:
        return EditorTool::Move;
    case EditorGizmoHandle::RingX:
    case EditorGizmoHandle::RingY:
    case EditorGizmoHandle::RingZ:
        return EditorTool::Rotate;
    case EditorGizmoHandle::ScaleX:
    case EditorGizmoHandle::ScaleY:
    case EditorGizmoHandle::ScaleZ:
    case EditorGizmoHandle::ScaleUniform:
        return EditorTool::Scale;
    default:
        return EditorTool::Select;
    }
}

float axisProjectionQuality(const ayt::math::FVector3& axis,
                            const ayt::math::FVector3& view) noexcept
{
    if (!finiteVector(axis) || axis.lengthSq() <= kEpsilon) return 0.0f;
    const float alignment = std::clamp(
        std::fabs(axis.normalize().dot(view)), 0.0f, 1.0f);
    return std::sqrt(std::max(0.0f, 1.0f - alignment * alignment));
}

float planeProjectionQuality(const ayt::math::FVector3& normal,
                             const ayt::math::FVector3& view) noexcept
{
    if (!finiteVector(normal) || normal.lengthSq() <= kEpsilon) return 0.0f;
    return std::clamp(
        std::fabs(normal.normalize().dot(view)), 0.0f, 1.0f);
}

} // namespace

float EditorTransformGizmo::worldScale(
    const ayt::math::FVector3& pivot,
    const ayt::math::FVector3& cameraEye) noexcept
{
    const float distance = (pivot - cameraEye).length();
    if (!std::isfinite(distance)) return 1.0f;
    return std::clamp(distance * kWorldScalePerCameraDistance, 0.12f, 1000.0f);
}

ayt::math::FVector3 EditorTransformGizmo::basisAxis(
    EditorTool tool,
    const EditorTransformState& transform,
    bool localSpace,
    int axis) noexcept
{
    ayt::math::FVector3 result;
    if (axis == 0) result = {1.0f, 0.0f, 0.0f};
    else if (axis == 1) result = {0.0f, 1.0f, 0.0f};
    else result = {0.0f, 0.0f, 1.0f};

    // Component scale cannot represent arbitrary world-space non-uniform
    // scaling without introducing shear, so Scale intentionally follows the
    // object's local basis even while translation/rotation are in World mode.
    if (localSpace || tool == EditorTool::Scale) {
        result = transform.rotation * result;
    }
    return result.lengthSq() > kEpsilon ? result.normalize() : result;
}

uint16_t EditorTransformGizmo::disabledHandleMask(
    const EditorTransformState& transform,
    bool localSpace,
    const ayt::math::FVector3& cameraEye,
    uint16_t previouslyDisabled) noexcept
{
    ayt::math::FVector3 view = cameraEye - transform.position;
    if (!finiteVector(transform.position) || !finiteVector(cameraEye)
        || view.lengthSq() <= kEpsilon) {
        // Directional handles are undefined when the eye is at the pivot.
        // Keep uniform scale usable because it is screen-space driven.
        return 0x1ffeu;
    }
    view = view.normalize();

    ayt::math::FVector3 transformAxes[3] = {
        basisAxis(EditorTool::Move, transform, localSpace, 0),
        basisAxis(EditorTool::Move, transform, localSpace, 1),
        basisAxis(EditorTool::Move, transform, localSpace, 2),
    };
    ayt::math::FVector3 scaleAxes[3] = {
        basisAxis(EditorTool::Scale, transform, localSpace, 0),
        basisAxis(EditorTool::Scale, transform, localSpace, 1),
        basisAxis(EditorTool::Scale, transform, localSpace, 2),
    };

    uint16_t disabled = 0u;
    auto classify = [&](EditorGizmoHandle handle, float quality) {
        const bool wasDisabled = handleDisabled(previouslyDisabled, handle);
        const float threshold = wasDisabled
            ? kProjectionEnableThreshold : kProjectionDisableThreshold;
        if (!std::isfinite(quality) || quality < threshold) {
            disabled |= handleBit(handle);
        }
    };

    for (int axis = 0; axis < 3; ++axis) {
        classify(static_cast<EditorGizmoHandle>(
                     static_cast<uint8_t>(EditorGizmoHandle::AxisX) + axis),
                 axisProjectionQuality(transformAxes[axis], view));
        classify(static_cast<EditorGizmoHandle>(
                     static_cast<uint8_t>(EditorGizmoHandle::ScaleX) + axis),
                 axisProjectionQuality(scaleAxes[axis], view));
        // A rotation ring occupies the plane whose normal is its axis.
        classify(static_cast<EditorGizmoHandle>(
                     static_cast<uint8_t>(EditorGizmoHandle::RingX) + axis),
                 planeProjectionQuality(transformAxes[axis], view));
    }

    constexpr EditorGizmoHandle planes[3] = {
        EditorGizmoHandle::PlaneXY,
        EditorGizmoHandle::PlaneYZ,
        EditorGizmoHandle::PlaneZX,
    };
    for (EditorGizmoHandle handle : planes) {
        int first = 0;
        int second = 1;
        planeAxes(handle, first, second);
        classify(handle, planeProjectionQuality(
            transformAxes[first].cross(transformAxes[second]), view));
    }
    return disabled;
}

EditorGizmoHandle EditorTransformGizmo::hitTest(
    EditorTool tool,
    const EditorTransformState& transform,
    bool localSpace,
    const ayt::math::FVector3& cameraEye,
    const ayt::math::FVector3& rayDirection) const noexcept
{
    if (tool == EditorTool::Select || !finiteVector(transform.position)) {
        return EditorGizmoHandle::None;
    }
    const ayt::math::FVector3 ray = normalizedRay(rayDirection);
    if (ray.lengthSq() <= kEpsilon) return EditorGizmoHandle::None;

    const float scale = worldScale(transform.position, cameraEye);
    ayt::math::FVector3 axes[3] = {
        basisAxis(tool, transform, localSpace, 0),
        basisAxis(tool, transform, localSpace, 1),
        basisAxis(tool, transform, localSpace, 2),
    };

    if (tool == EditorTool::Move || tool == EditorTool::Scale) {
        if (tool == EditorTool::Scale) {
            const float centerDistance =
                (transform.position - cameraEye).dot(ray);
            const ayt::math::FVector3 closest =
                cameraEye + ray * std::max(centerDistance, 0.0f);
            if (centerDistance >= 0.0f
                && (closest - transform.position).length()
                    <= scale * 0.14f) {
                return EditorGizmoHandle::ScaleUniform;
            }
        }

        EditorGizmoHandle best = EditorGizmoHandle::None;
        float bestDistance = scale * kAxisPickRadius;
        float bestRayDistance = 1.0e30f;
        for (int axis = 0; axis < 3; ++axis) {
            float distance = 0.0f;
            float rayDistance = 0.0f;
            if (raySegmentDistance(cameraEye, ray, transform.position,
                                   axes[axis], scale * kAxisStart,
                                   scale * kAxisEnd, distance, rayDistance)
                && distance <= bestDistance
                && rayDistance <= bestRayDistance) {
                bestDistance = distance;
                bestRayDistance = rayDistance;
                if (tool == EditorTool::Move) {
                    best = static_cast<EditorGizmoHandle>(
                        static_cast<uint8_t>(EditorGizmoHandle::AxisX) + axis);
                } else {
                    best = static_cast<EditorGizmoHandle>(
                        static_cast<uint8_t>(EditorGizmoHandle::ScaleX) + axis);
                }
            }
        }
        if (best != EditorGizmoHandle::None) return best;

        if (tool == EditorTool::Move) {
            constexpr EditorGizmoHandle planes[3] = {
                EditorGizmoHandle::PlaneXY,
                EditorGizmoHandle::PlaneYZ,
                EditorGizmoHandle::PlaneZX,
            };
            for (EditorGizmoHandle handle : planes) {
                int first = 0;
                int second = 1;
                planeAxes(handle, first, second);
                const ayt::math::FVector3 normal =
                    axes[first].cross(axes[second]).normalize();
                ayt::math::FVector3 hit;
                if (!intersectRayPlane(cameraEye, ray, transform.position,
                                       normal, hit)) {
                    continue;
                }
                const ayt::math::FVector3 offset = hit - transform.position;
                const float u = offset.dot(axes[first]) / scale;
                const float v = offset.dot(axes[second]) / scale;
                if (u >= kPlaneMin && u <= kPlaneMax
                    && v >= kPlaneMin && v <= kPlaneMax) {
                    return handle;
                }
            }
        }
        return EditorGizmoHandle::None;
    }

    if (tool == EditorTool::Rotate) {
        EditorGizmoHandle best = EditorGizmoHandle::None;
        float bestError = scale * kRingPickRadius;
        float bestRayDistance = 1.0e30f;
        for (int axis = 0; axis < 3; ++axis) {
            ayt::math::FVector3 hit;
            float rayDistance = 0.0f;
            if (!intersectRayPlane(cameraEye, ray, transform.position,
                                   axes[axis], hit, &rayDistance)) {
                continue;
            }
            const float error = std::fabs(
                (hit - transform.position).length() - scale * kRingRadius);
            if (error <= bestError && rayDistance <= bestRayDistance) {
                bestError = error;
                bestRayDistance = rayDistance;
                best = static_cast<EditorGizmoHandle>(
                    static_cast<uint8_t>(EditorGizmoHandle::RingX) + axis);
            }
        }
        return best;
    }

    return EditorGizmoHandle::None;
}

EditorGizmoHandle EditorTransformGizmo::hitTestUniversal(
    const EditorTransformState& transform,
    bool localSpace,
    const ayt::math::FVector3& cameraEye,
    const ayt::math::FVector3& rayDirection,
    uint16_t disabledHandles,
    float worldScaleOverride) const noexcept
{
    if (!finiteVector(transform.position)) {
        return EditorGizmoHandle::None;
    }
    const ayt::math::FVector3 ray = normalizedRay(rayDirection);
    if (ray.lengthSq() <= kEpsilon) return EditorGizmoHandle::None;
    if (disabledHandles == kAutoDisabledHandleMask) {
        disabledHandles = disabledHandleMask(
            transform, localSpace, cameraEye);
    }

    const float scale = std::isfinite(worldScaleOverride)
            && worldScaleOverride > 0.0f
        ? std::clamp(worldScaleOverride, 0.12f, 1000.0f)
        : worldScale(transform.position, cameraEye);
    ayt::math::FVector3 transformAxes[3] = {
        basisAxis(EditorTool::Move, transform, localSpace, 0),
        basisAxis(EditorTool::Move, transform, localSpace, 1),
        basisAxis(EditorTool::Move, transform, localSpace, 2),
    };
    // Component scale remains local even when translation and rotation use
    // world axes. This keeps non-uniform scaling representable as TRS.
    ayt::math::FVector3 scaleAxes[3] = {
        basisAxis(EditorTool::Scale, transform, localSpace, 0),
        basisAxis(EditorTool::Scale, transform, localSpace, 1),
        basisAxis(EditorTool::Scale, transform, localSpace, 2),
    };

    const float centerDistance = (transform.position - cameraEye).dot(ray);
    const ayt::math::FVector3 centerClosest =
        cameraEye + ray * std::max(centerDistance, 0.0f);
    if (centerDistance >= 0.0f
        && (centerClosest - transform.position).length()
            <= scale * kUniversalCenterPickRadius) {
        return EditorGizmoHandle::ScaleUniform;
    }

    // Scale cubes sit on the inner portion of each axis. Test them before
    // the longer translation arrows so projected overlap remains stable.
    EditorGizmoHandle bestScale = EditorGizmoHandle::None;
    float bestScaleDistance = scale * kUniversalAxisPickRadius;
    float bestScaleRayDistance = 1.0e30f;
    for (int axis = 0; axis < 3; ++axis) {
        const EditorGizmoHandle handle = static_cast<EditorGizmoHandle>(
            static_cast<uint8_t>(EditorGizmoHandle::ScaleX) + axis);
        if (handleDisabled(disabledHandles, handle)) continue;
        float distance = 0.0f;
        float rayDistance = 0.0f;
        float segmentT = 0.0f;
        if (raySegmentDistance(cameraEye, ray, transform.position,
                               scaleAxes[axis],
                               scale * kUniversalScaleStart,
                               scale * kUniversalScaleEnd,
                               distance, rayDistance, &segmentT)
            && segmentT >= scale * kUniversalScaleStart
            && segmentT <= scale * kUniversalScaleEnd
            && distance <= bestScaleDistance
            && rayDistance <= bestScaleRayDistance) {
            bestScaleDistance = distance;
            bestScaleRayDistance = rayDistance;
            bestScale = handle;
        }
    }
    if (bestScale != EditorGizmoHandle::None) return bestScale;

    constexpr EditorGizmoHandle planes[3] = {
        EditorGizmoHandle::PlaneXY,
        EditorGizmoHandle::PlaneYZ,
        EditorGizmoHandle::PlaneZX,
    };
    for (EditorGizmoHandle handle : planes) {
        if (handleDisabled(disabledHandles, handle)) continue;
        int first = 0;
        int second = 1;
        planeAxes(handle, first, second);
        const ayt::math::FVector3 normal =
            transformAxes[first].cross(transformAxes[second]).normalize();
        ayt::math::FVector3 hit;
        if (!intersectRayPlane(cameraEye, ray, transform.position,
                               normal, hit)) {
            continue;
        }
        const ayt::math::FVector3 offset = hit - transform.position;
        const float u = offset.dot(transformAxes[first]) / scale;
        const float v = offset.dot(transformAxes[second]) / scale;
        if (u >= kUniversalPlaneMin && u <= kUniversalPlaneMax
            && v >= kUniversalPlaneMin && v <= kUniversalPlaneMax) {
            return handle;
        }
    }

    EditorGizmoHandle bestAxis = EditorGizmoHandle::None;
    float bestAxisDistance = scale * kUniversalAxisPickRadius;
    float bestAxisRayDistance = 1.0e30f;
    for (int axis = 0; axis < 3; ++axis) {
        const EditorGizmoHandle handle = static_cast<EditorGizmoHandle>(
            static_cast<uint8_t>(EditorGizmoHandle::AxisX) + axis);
        if (handleDisabled(disabledHandles, handle)) continue;
        float distance = 0.0f;
        float rayDistance = 0.0f;
        float segmentT = 0.0f;
        if (raySegmentDistance(cameraEye, ray, transform.position,
                               transformAxes[axis],
                               scale * kUniversalMoveStart,
                               scale * kUniversalMoveEnd,
                               distance, rayDistance, &segmentT)
            && segmentT >= scale * kUniversalMoveStart
            && segmentT <= scale * kUniversalMoveEnd
            && distance <= bestAxisDistance
            && rayDistance <= bestAxisRayDistance) {
            bestAxisDistance = distance;
            bestAxisRayDistance = rayDistance;
            bestAxis = handle;
        }
    }
    if (bestAxis != EditorGizmoHandle::None) return bestAxis;

    EditorGizmoHandle bestRing = EditorGizmoHandle::None;
    float bestRingError = scale * kUniversalRingPickRadius;
    float bestRingRayDistance = 1.0e30f;
    for (int axis = 0; axis < 3; ++axis) {
        const EditorGizmoHandle handle = static_cast<EditorGizmoHandle>(
            static_cast<uint8_t>(EditorGizmoHandle::RingX) + axis);
        if (handleDisabled(disabledHandles, handle)) continue;
        ayt::math::FVector3 hit;
        float rayDistance = 0.0f;
        if (!intersectRayPlane(cameraEye, ray, transform.position,
                               transformAxes[axis], hit, &rayDistance)) {
            continue;
        }
        const float error = std::fabs(
            (hit - transform.position).length()
            - scale * kUniversalRingRadius);
        if (error <= bestRingError && rayDistance <= bestRingRayDistance) {
            bestRingError = error;
            bestRingRayDistance = rayDistance;
            bestRing = handle;
        }
    }
    return bestRing;
}

bool EditorTransformGizmo::begin(
    EditorTool tool,
    EditorGizmoHandle handle,
    const EditorTransformState& transform,
    bool localSpace,
    const ayt::math::FVector3& cameraEye,
    const ayt::math::FVector3& rayDirection,
    float mouseY,
    float worldScaleOverride) noexcept
{
    reset();
    if (handle == EditorGizmoHandle::None || tool == EditorTool::Select) {
        return false;
    }
    const ayt::math::FVector3 ray = normalizedRay(rayDirection);
    if (ray.lengthSq() <= kEpsilon) return false;

    _tool = tool;
    _handle = handle;
    _before = transform;
    _pivot = transform.position;
    _worldScale = std::isfinite(worldScaleOverride)
            && worldScaleOverride > 0.0f
        ? std::clamp(worldScaleOverride, 0.12f, 1000.0f)
        : worldScale(_pivot, cameraEye);
    _startMouseY = mouseY;
    for (int axis = 0; axis < 3; ++axis) {
        _axes[axis] = basisAxis(tool, transform, localSpace, axis);
    }

    int first = 0;
    int second = 1;
    if (planeAxes(handle, first, second)) {
        _dragPlaneNormal = _axes[first].cross(_axes[second]).normalize();
        if (!intersectRayPlane(cameraEye, ray, _pivot, _dragPlaneNormal,
                               _startHit)) {
            reset();
            return false;
        }
        return true;
    }

    const int axis = axisIndex(handle);
    if (axis >= 0 && (tool == EditorTool::Move || tool == EditorTool::Scale)) {
        ayt::math::FVector3 view = _pivot - cameraEye;
        if (view.lengthSq() <= kEpsilon) view = ray;
        view = view.normalize();
        ayt::math::FVector3 side = _axes[axis].cross(view);
        if (side.lengthSq() <= kEpsilon) {
            side = _axes[axis].cross(ayt::math::FVector3(0.0f, 1.0f, 0.0f));
        }
        if (side.lengthSq() <= kEpsilon) {
            side = _axes[axis].cross(ayt::math::FVector3(1.0f, 0.0f, 0.0f));
        }
        _dragPlaneNormal = side.cross(_axes[axis]).normalize();
        if (!intersectRayPlane(cameraEye, ray, _pivot, _dragPlaneNormal,
                               _startHit)) {
            reset();
            return false;
        }
        return true;
    }

    if (axis >= 0 && tool == EditorTool::Rotate) {
        _dragPlaneNormal = _axes[axis];
        if (!intersectRayPlane(cameraEye, ray, _pivot, _dragPlaneNormal,
                               _startHit)) {
            reset();
            return false;
        }
        _startVector = _startHit - _pivot;
        if (_startVector.lengthSq() <= kEpsilon) {
            reset();
            return false;
        }
        _startVector = _startVector.normalize();
        return true;
    }

    if (handle == EditorGizmoHandle::ScaleUniform
        && tool == EditorTool::Scale) {
        return true;
    }

    reset();
    return false;
}

bool EditorTransformGizmo::beginUniversal(
    EditorGizmoHandle handle,
    const EditorTransformState& transform,
    bool localSpace,
    const ayt::math::FVector3& cameraEye,
    const ayt::math::FVector3& rayDirection,
    float mouseY,
    uint16_t disabledHandles,
    float worldScaleOverride) noexcept
{
    const EditorTool tool = toolForHandle(handle);
    if (tool == EditorTool::Select) return false;
    if (disabledHandles == kAutoDisabledHandleMask) {
        disabledHandles = disabledHandleMask(
            transform, localSpace, cameraEye);
    }
    if (handleDisabled(disabledHandles, handle)) return false;
    return begin(tool, handle, transform, localSpace,
                 cameraEye, rayDirection, mouseY, worldScaleOverride);
}

bool EditorTransformGizmo::update(
    const ayt::math::FVector3& cameraEye,
    const ayt::math::FVector3& rayDirection,
    float mouseX,
    float mouseY,
    EditorTransformState& outTransform) const noexcept
{
    if (!active()) return false;
    const ayt::math::FVector3 ray = normalizedRay(rayDirection);
    if (ray.lengthSq() <= kEpsilon) return false;
    outTransform = _before;

    if (_handle == EditorGizmoHandle::ScaleUniform) {
        const float factor = std::exp((_startMouseY - mouseY) * 0.01f);
        outTransform.scale.x = safeScaleComponent(_before.scale.x, factor);
        outTransform.scale.y = safeScaleComponent(_before.scale.y, factor);
        outTransform.scale.z = safeScaleComponent(_before.scale.z, factor);
        return true;
    }

    ayt::math::FVector3 hit;
    if (!intersectRayPlane(cameraEye, ray, _pivot, _dragPlaneNormal, hit)) {
        return false;
    }

    int first = 0;
    int second = 1;
    if (planeAxes(_handle, first, second)) {
        const ayt::math::FVector3 delta = hit - _startHit;
        outTransform.position = _before.position
            + _axes[first] * delta.dot(_axes[first])
            + _axes[second] * delta.dot(_axes[second]);
        return true;
    }

    const int axis = axisIndex(_handle);
    if (axis < 0) return false;

    if (_tool == EditorTool::Move) {
        const float delta = (hit - _startHit).dot(_axes[axis]);
        outTransform.position = _before.position + _axes[axis] * delta;
        return true;
    }

    if (_tool == EditorTool::Rotate) {
        ayt::math::FVector3 current = hit - _pivot;
        if (current.lengthSq() <= kEpsilon) return false;
        current = current.normalize();
        const float angle = std::atan2(
            _axes[axis].dot(_startVector.cross(current)),
            std::clamp(_startVector.dot(current), -1.0f, 1.0f));
        const ayt::math::FQuaternion delta =
            ayt::math::FQuaternion::fromAxisAngle(_axes[axis], angle);
        outTransform.rotation = (delta * _before.rotation).normalize();
        return true;
    }

    if (_tool == EditorTool::Scale) {
        const float delta = (hit - _startHit).dot(_axes[axis]);
        const float factor = 1.0f + delta / std::max(_worldScale, 0.001f);
        if (axis == 0) {
            outTransform.scale.x = safeScaleComponent(_before.scale.x, factor);
        } else if (axis == 1) {
            outTransform.scale.y = safeScaleComponent(_before.scale.y, factor);
        } else {
            outTransform.scale.z = safeScaleComponent(_before.scale.z, factor);
        }
        return true;
    }

    return false;
}

void EditorTransformGizmo::reset() noexcept
{
    _tool = EditorTool::Select;
    _handle = EditorGizmoHandle::None;
    _before = {};
    _pivot = {};
    _axes[0] = {};
    _axes[1] = {};
    _axes[2] = {};
    _dragPlaneNormal = {};
    _startHit = {};
    _startVector = {};
    _worldScale = 1.0f;
    _startMouseY = 0.0f;
}

} // namespace ayt::editor
