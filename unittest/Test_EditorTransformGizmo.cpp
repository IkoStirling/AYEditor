#include "AYTest.h"

#include "AYEditor/EditorTransformGizmo.h"

#include <cmath>

using namespace ayt::editor;

namespace {

ayt::math::FVector3 rayTo(const ayt::math::FVector3& eye,
                          const ayt::math::FVector3& point)
{
    return (point - eye).normalize();
}

bool nearlyEqual(float a, float b, float tolerance = 1.0e-4f)
{
    return std::fabs(a - b) <= tolerance;
}

} // namespace

TEST_SUITE(AYEditor_TransformGizmo)

TEST_CASE(gizmo_handle_ids_are_stable_for_renderer_highlighting)
{
    CHECK(static_cast<uint8_t>(EditorGizmoHandle::None) == 0u);
    CHECK(static_cast<uint8_t>(EditorGizmoHandle::AxisX) == 1u);
    CHECK(static_cast<uint8_t>(EditorGizmoHandle::PlaneXY) == 4u);
    CHECK(static_cast<uint8_t>(EditorGizmoHandle::RingX) == 7u);
    CHECK(static_cast<uint8_t>(EditorGizmoHandle::ScaleX) == 10u);
    CHECK(static_cast<uint8_t>(EditorGizmoHandle::ScaleUniform) == 13u);
}

TEST_CASE(gizmo_hit_test_separates_axis_plane_ring_and_uniform_handles)
{
    EditorTransformGizmo gizmo;
    EditorTransformState transform;
    transform.rotation = ayt::math::FQuaternion::identity();
    const ayt::math::FVector3 eye(0.0f, 0.0f, 5.0f);
    const float scale = EditorTransformGizmo::worldScale(
        transform.position, eye);

    CHECK(gizmo.hitTest(EditorTool::Move, transform, false, eye,
                        rayTo(eye, {scale * 0.72f, 0.0f, 0.0f}))
          == EditorGizmoHandle::AxisX);
    CHECK(gizmo.hitTest(EditorTool::Move, transform, false, eye,
                        rayTo(eye, {scale * 0.35f, scale * 0.35f, 0.0f}))
          == EditorGizmoHandle::PlaneXY);

    CHECK(gizmo.hitTest(
              EditorTool::Rotate, transform, false, eye,
              rayTo(eye, {scale * 0.82f, 0.0f, 0.0f}))
          == EditorGizmoHandle::RingZ);
    CHECK(gizmo.hitTest(EditorTool::Scale, transform, false, eye,
                        rayTo(eye, transform.position))
          == EditorGizmoHandle::ScaleUniform);
}

TEST_CASE(gizmo_universal_hit_test_exposes_all_transform_operations)
{
    EditorTransformGizmo gizmo;
    EditorTransformState transform;
    transform.rotation = ayt::math::FQuaternion::identity();
    const ayt::math::FVector3 eye(0.0f, 0.0f, 5.0f);
    const float scale = EditorTransformGizmo::worldScale(
        transform.position, eye);

    CHECK(gizmo.hitTestUniversal(
              transform, false, eye,
              rayTo(eye, {scale * 0.90f, 0.0f, 0.0f}))
          == EditorGizmoHandle::AxisX);
    CHECK(gizmo.hitTestUniversal(
              transform, false, eye,
              rayTo(eye, {scale * 0.25f, scale * 0.25f, 0.0f}))
          == EditorGizmoHandle::PlaneXY);
    CHECK(gizmo.hitTestUniversal(
              transform, false, eye,
              rayTo(eye, {scale * 0.35f, 0.0f, 0.0f}))
          == EditorGizmoHandle::ScaleX);
    CHECK(gizmo.hitTestUniversal(
              transform, false, eye,
              rayTo(eye, {scale * 0.20f, 0.0f, 0.0f}))
          == EditorGizmoHandle::ScaleX);
    CHECK(gizmo.hitTestUniversal(
              transform, false, eye,
              rayTo(eye, {scale * 0.728f, scale * 0.728f, 0.0f}))
          == EditorGizmoHandle::RingZ);
    CHECK(gizmo.hitTestUniversal(
              transform, false, eye, rayTo(eye, transform.position))
          == EditorGizmoHandle::ScaleUniform);

    // The visible gap between the scale cube and translation arrow is also a
    // semantic gap: neither collinear handle may claim it through a rounded
    // pick-volume end cap.
    CHECK(gizmo.hitTestUniversal(
              transform, false, eye,
              rayTo(eye, {scale * 0.435f, 0.0f, 0.0f}))
          == EditorGizmoHandle::None);
}

TEST_CASE(gizmo_disables_projection_degenerate_handles)
{
    EditorTransformState transform;
    transform.rotation = ayt::math::FQuaternion::identity();
    const ayt::math::FVector3 eye(0.0f, 0.0f, 5.0f);
    const uint16_t mask = EditorTransformGizmo::disabledHandleMask(
        transform, false, eye);

    CHECK(EditorTransformGizmo::handleDisabled(
        mask, EditorGizmoHandle::AxisZ));
    CHECK(EditorTransformGizmo::handleDisabled(
        mask, EditorGizmoHandle::ScaleZ));
    CHECK(EditorTransformGizmo::handleDisabled(
        mask, EditorGizmoHandle::PlaneYZ));
    CHECK(EditorTransformGizmo::handleDisabled(
        mask, EditorGizmoHandle::PlaneZX));
    CHECK(EditorTransformGizmo::handleDisabled(
        mask, EditorGizmoHandle::RingX));
    CHECK(EditorTransformGizmo::handleDisabled(
        mask, EditorGizmoHandle::RingY));

    CHECK_FALSE(EditorTransformGizmo::handleDisabled(
        mask, EditorGizmoHandle::AxisX));
    CHECK_FALSE(EditorTransformGizmo::handleDisabled(
        mask, EditorGizmoHandle::PlaneXY));
    CHECK_FALSE(EditorTransformGizmo::handleDisabled(
        mask, EditorGizmoHandle::RingZ));
    CHECK_FALSE(EditorTransformGizmo::handleDisabled(
        mask, EditorGizmoHandle::ScaleUniform));
}

TEST_CASE(gizmo_projection_disable_uses_hysteresis)
{
    EditorTransformState transform;
    transform.rotation = ayt::math::FQuaternion::identity();
    constexpr float projectedQuality = 0.28f;
    const float alignment = std::sqrt(
        1.0f - projectedQuality * projectedQuality);
    const ayt::math::FVector3 eye(alignment * 5.0f, 0.0f,
                                  projectedQuality * 5.0f);
    const uint16_t fresh = EditorTransformGizmo::disabledHandleMask(
        transform, false, eye, 0u);
    CHECK_FALSE(EditorTransformGizmo::handleDisabled(
        fresh, EditorGizmoHandle::AxisX));

    const uint16_t prior = EditorTransformGizmo::handleBit(
        EditorGizmoHandle::AxisX);
    const uint16_t held = EditorTransformGizmo::disabledHandleMask(
        transform, false, eye, prior);
    CHECK(EditorTransformGizmo::handleDisabled(
        held, EditorGizmoHandle::AxisX));
}

TEST_CASE(gizmo_disabled_handle_cannot_win_hit_or_begin_drag)
{
    EditorTransformGizmo gizmo;
    EditorTransformState transform;
    transform.rotation = ayt::math::FQuaternion::identity();
    const ayt::math::FVector3 eye(0.0f, 0.0f, 5.0f);
    const float scale = EditorTransformGizmo::worldScale(
        transform.position, eye);
    const uint16_t mask = EditorTransformGizmo::handleBit(
        EditorGizmoHandle::AxisX);
    const ayt::math::FVector3 ray = rayTo(
        eye, {scale * 0.90f, 0.0f, 0.0f});

    CHECK(gizmo.hitTestUniversal(
              transform, false, eye, ray, mask)
          != EditorGizmoHandle::AxisX);
    CHECK_FALSE(gizmo.beginUniversal(
        EditorGizmoHandle::AxisX, transform, false, eye, ray, 0.0f, mask));
}

TEST_CASE(gizmo_universal_handle_selects_operation_without_tool_mode)
{
    EditorTransformGizmo gizmo;
    EditorTransformState transform;
    transform.rotation = ayt::math::FQuaternion::identity();
    const ayt::math::FVector3 eye(0.0f, 0.0f, 5.0f);
    const float scale = EditorTransformGizmo::worldScale(
        transform.position, eye);

    CHECK(gizmo.beginUniversal(
        EditorGizmoHandle::AxisX, transform, false, eye,
        rayTo(eye, {scale * 0.90f, 0.0f, 0.0f}), 0.0f));
    EditorTransformState moved;
    CHECK(gizmo.update(
        eye, rayTo(eye, {scale * 1.20f, 0.0f, 0.0f}),
        0.0f, 0.0f, moved));
    CHECK(moved.position.x > 0.0f);

    gizmo.reset();
    CHECK(gizmo.beginUniversal(
        EditorGizmoHandle::ScaleUniform, transform, false, eye,
        rayTo(eye, transform.position), 100.0f));
    EditorTransformState scaled;
    CHECK(gizmo.update(
        eye, rayTo(eye, transform.position), 0.0f, 80.0f, scaled));
    CHECK(scaled.scale.x > transform.scale.x);
}

TEST_CASE(gizmo_axis_and_plane_drag_apply_constrained_translation)
{
    EditorTransformGizmo gizmo;
    EditorTransformState transform;
    transform.rotation = ayt::math::FQuaternion::identity();
    const ayt::math::FVector3 eye(0.0f, 0.0f, 5.0f);
    const float scale = EditorTransformGizmo::worldScale(
        transform.position, eye);

    const auto axisStart = ayt::math::FVector3(scale * 0.65f, 0.0f, 0.0f);
    CHECK(gizmo.begin(EditorTool::Move, EditorGizmoHandle::AxisX,
                      transform, false, eye, rayTo(eye, axisStart), 0.0f));
    EditorTransformState moved;
    CHECK(gizmo.update(eye,
                       rayTo(eye, {scale * 1.15f, 0.0f, 0.0f}),
                       20.0f, 0.0f, moved));
    CHECK(nearlyEqual(moved.position.x, scale * 0.5f));
    CHECK(nearlyEqual(moved.position.y, 0.0f));
    CHECK(nearlyEqual(moved.position.z, 0.0f));

    gizmo.reset();
    const ayt::math::FVector3 planeStart(
        scale * 0.3f, scale * 0.3f, 0.0f);
    CHECK(gizmo.begin(EditorTool::Move, EditorGizmoHandle::PlaneXY,
                      transform, false, eye, rayTo(eye, planeStart), 0.0f));
    CHECK(gizmo.update(
        eye, rayTo(eye, {scale * 0.5f, scale * 0.6f, 0.0f}),
        20.0f, 20.0f, moved));
    CHECK(nearlyEqual(moved.position.x, scale * 0.2f));
    CHECK(nearlyEqual(moved.position.y, scale * 0.3f));
    CHECK(nearlyEqual(moved.position.z, 0.0f));
}

TEST_CASE(gizmo_ring_drag_rotates_and_scale_axis_changes_one_component)
{
    EditorTransformGizmo gizmo;
    EditorTransformState transform;
    transform.rotation = ayt::math::FQuaternion::identity();
    const ayt::math::FVector3 eye(0.0f, 0.0f, 5.0f);
    const float scale = EditorTransformGizmo::worldScale(
        transform.position, eye);

    CHECK(gizmo.begin(EditorTool::Rotate, EditorGizmoHandle::RingZ,
                      transform, false, eye,
                      rayTo(eye, {scale * 0.82f, 0.0f, 0.0f}),
                      0.0f));
    EditorTransformState rotated;
    CHECK(gizmo.update(eye,
                       rayTo(eye, {0.0f, scale * 0.82f, 0.0f}),
                       0.0f, 0.0f, rotated));
    const ayt::math::FVector3 rotatedX =
        rotated.rotation * ayt::math::FVector3(1.0f, 0.0f, 0.0f);
    CHECK(nearlyEqual(rotatedX.x, 0.0f));
    CHECK(nearlyEqual(rotatedX.y, 1.0f));
    CHECK(nearlyEqual(rotatedX.z, 0.0f));

    gizmo.reset();
    CHECK(gizmo.begin(EditorTool::Scale, EditorGizmoHandle::ScaleX,
                      transform, false, eye,
                      rayTo(eye, {scale * 0.65f, 0.0f, 0.0f}),
                      0.0f));
    EditorTransformState scaled;
    CHECK(gizmo.update(eye,
                       rayTo(eye, {scale * 1.15f, 0.0f, 0.0f}),
                       0.0f, 0.0f, scaled));
    CHECK(nearlyEqual(scaled.scale.x, 1.5f));
    CHECK(nearlyEqual(scaled.scale.y, 1.0f));
    CHECK(nearlyEqual(scaled.scale.z, 1.0f));
}

TEST_CASE(gizmo_local_move_uses_rotated_basis_and_uniform_scale_is_proportional)
{
    EditorTransformGizmo gizmo;
    EditorTransformState transform;
    constexpr float halfPi = 1.57079632679489661923f;
    transform.rotation = ayt::math::FQuaternion::fromAxisAngle(
        {0.0f, 0.0f, 1.0f}, halfPi);
    transform.scale = {1.0f, 2.0f, 4.0f};
    const ayt::math::FVector3 eye(0.0f, 0.0f, 5.0f);
    const float scale = EditorTransformGizmo::worldScale(
        transform.position, eye);

    // Local +X points along world +Y after a 90-degree Z rotation.
    CHECK(gizmo.begin(
        EditorTool::Move, EditorGizmoHandle::AxisX, transform, true, eye,
        rayTo(eye, {0.0f, scale * 0.65f, 0.0f}), 0.0f));
    EditorTransformState moved;
    CHECK(gizmo.update(
        eye, rayTo(eye, {0.0f, scale * 1.15f, 0.0f}), 0.0f, 0.0f, moved));
    CHECK(nearlyEqual(moved.position.x, 0.0f));
    CHECK(nearlyEqual(moved.position.y, scale * 0.5f));

    gizmo.reset();
    CHECK(gizmo.begin(
        EditorTool::Scale, EditorGizmoHandle::ScaleUniform, transform, false,
        eye, rayTo(eye, transform.position), 100.0f));
    EditorTransformState uniformlyScaled;
    CHECK(gizmo.update(
        eye, rayTo(eye, transform.position), 0.0f, 60.0f,
        uniformlyScaled));
    const float factor = std::exp(0.4f);
    CHECK(nearlyEqual(uniformlyScaled.scale.x, factor));
    CHECK(nearlyEqual(uniformlyScaled.scale.y, 2.0f * factor));
    CHECK(nearlyEqual(uniformlyScaled.scale.z, 4.0f * factor));
}

TEST_CASE(gizmo_continuous_drag_remains_finite_and_uses_drag_origin)
{
    EditorTransformGizmo gizmo;
    EditorTransformState transform;
    transform.rotation = ayt::math::FQuaternion::identity();
    const ayt::math::FVector3 eye(0.0f, 0.0f, 5.0f);
    const float scale = EditorTransformGizmo::worldScale(
        transform.position, eye);
    CHECK(gizmo.beginUniversal(
        EditorGizmoHandle::AxisX, transform, false, eye,
        rayTo(eye, {scale * 0.65f, 0.0f, 0.0f}), 0.0f));

    EditorTransformState current;
    bool allUpdatesAccepted = true;
    bool allValuesFinite = true;
    for (int sample = 0; sample < 10000; ++sample) {
        const float fraction = static_cast<float>(sample) / 9999.0f;
        const ayt::math::FVector3 point(
            scale * (0.65f + fraction * 0.75f),
            std::sin(fraction * 50.0f) * scale * 0.002f, 0.0f);
        allUpdatesAccepted = gizmo.update(
            eye, rayTo(eye, point), fraction * 800.0f, 0.0f, current);
        if (!allUpdatesAccepted) break;
        allValuesFinite = std::isfinite(current.position.x)
            && std::isfinite(current.position.y)
            && std::isfinite(current.position.z)
            && std::isfinite(current.rotation.x)
            && std::isfinite(current.rotation.y)
            && std::isfinite(current.rotation.z)
            && std::isfinite(current.rotation.w)
            && std::isfinite(current.scale.x)
            && std::isfinite(current.scale.y)
            && std::isfinite(current.scale.z);
        if (!allValuesFinite) break;
    }
    CHECK(allUpdatesAccepted);
    CHECK(allValuesFinite);

    EditorTransformState repeated;
    CHECK(gizmo.update(eye,
        rayTo(eye, {scale * 1.40f, 0.0f, 0.0f}),
        800.0f, 0.0f, repeated));
    CHECK(nearlyEqual(repeated.position.x, scale * 0.75f));
    CHECK(nearlyEqual(repeated.position.y, 0.0f));
    CHECK(nearlyEqual(repeated.position.z, 0.0f));
}

TEST_SUITE_END
