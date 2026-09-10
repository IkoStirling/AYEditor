#pragma once

#include "AYEditor/EditorCommandStack.h"
#include "AYEditor/EditorPreferences.h"
#include "AYMath/MathTypes.h"

#include <cstdint>

namespace ayt::editor {

// Stable handle ids are also forwarded to AYRenderer for hover/active
// highlighting. Keep None at zero so a default-initialized state is inert.
enum class EditorGizmoHandle : uint8_t {
    None = 0,
    AxisX,
    AxisY,
    AxisZ,
    PlaneXY,
    PlaneYZ,
    PlaneZX,
    RingX,
    RingY,
    RingZ,
    ScaleX,
    ScaleY,
    ScaleZ,
    ScaleUniform,
};

// Pure editor-side interaction state. Rendering consumes only the resulting
// pose/mode/handle; all ray tests and drag constraints remain testable without
// a GPU backend.
class EditorTransformGizmo {
public:
    static constexpr float kWorldScalePerCameraDistance = 0.18f;
    // Projection quality is the length/area that remains after a handle is
    // projected onto the view plane. Two thresholds provide hysteresis so a
    // handle cannot flicker while the camera crosses the cutoff.
    static constexpr float kProjectionDisableThreshold = 0.25f;
    static constexpr float kProjectionEnableThreshold = 0.32f;
    static constexpr uint16_t kAutoDisabledHandleMask = 0xffffu;

    static constexpr uint16_t handleBit(EditorGizmoHandle handle) noexcept
    {
        const uint8_t index = static_cast<uint8_t>(handle);
        return index > 0u && index < 16u
            ? static_cast<uint16_t>(1u << index) : 0u;
    }

    static constexpr bool handleDisabled(
        uint16_t mask, EditorGizmoHandle handle) noexcept
    {
        return (mask & handleBit(handle)) != 0u;
    }

    static float worldScale(const ayt::math::FVector3& pivot,
                            const ayt::math::FVector3& cameraEye) noexcept;

    // Axes facing the camera have almost no screen-space travel; planes and
    // rings seen edge-on have almost no projected area. Such handles remain
    // visible as muted orientation cues but are excluded from interaction.
    static uint16_t disabledHandleMask(
        const EditorTransformState& transform,
        bool localSpace,
        const ayt::math::FVector3& cameraEye,
        uint16_t previouslyDisabled = 0u) noexcept;

    EditorGizmoHandle hitTest(
        EditorTool tool,
        const EditorTransformState& transform,
        bool localSpace,
        const ayt::math::FVector3& cameraEye,
        const ayt::math::FVector3& rayDirection) const noexcept;

    // Universal mode exposes translation axes/planes, rotation rings and
    // scale handles at the same time. The returned handle determines the
    // operation, so callers do not need a separate active-tool state.
    EditorGizmoHandle hitTestUniversal(
        const EditorTransformState& transform,
        bool localSpace,
        const ayt::math::FVector3& cameraEye,
        const ayt::math::FVector3& rayDirection,
        uint16_t disabledHandles = kAutoDisabledHandleMask,
        float worldScaleOverride = 0.0f) const noexcept;

    bool begin(EditorTool tool,
               EditorGizmoHandle handle,
               const EditorTransformState& transform,
               bool localSpace,
               const ayt::math::FVector3& cameraEye,
               const ayt::math::FVector3& rayDirection,
               float mouseY,
               float worldScaleOverride = 0.0f) noexcept;

    bool beginUniversal(EditorGizmoHandle handle,
                        const EditorTransformState& transform,
                        bool localSpace,
                        const ayt::math::FVector3& cameraEye,
                        const ayt::math::FVector3& rayDirection,
                        float mouseY,
                        uint16_t disabledHandles =
                            kAutoDisabledHandleMask,
                        float worldScaleOverride = 0.0f) noexcept;

    bool update(const ayt::math::FVector3& cameraEye,
                const ayt::math::FVector3& rayDirection,
                float mouseX,
                float mouseY,
                EditorTransformState& outTransform) const noexcept;

    void reset() noexcept;
    bool active() const noexcept { return _handle != EditorGizmoHandle::None; }
    EditorGizmoHandle activeHandle() const noexcept { return _handle; }
    const EditorTransformState& before() const noexcept { return _before; }

private:
    static ayt::math::FVector3 basisAxis(
        EditorTool tool,
        const EditorTransformState& transform,
        bool localSpace,
        int axis) noexcept;

    EditorTool _tool = EditorTool::Select;
    EditorGizmoHandle _handle = EditorGizmoHandle::None;
    EditorTransformState _before{};
    ayt::math::FVector3 _pivot{};
    ayt::math::FVector3 _axes[3]{};
    ayt::math::FVector3 _dragPlaneNormal{};
    ayt::math::FVector3 _startHit{};
    ayt::math::FVector3 _startVector{};
    float _worldScale = 1.0f;
    float _startMouseY = 0.0f;
};

} // namespace ayt::editor
