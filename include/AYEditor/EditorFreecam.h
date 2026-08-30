#pragma once

#include "AYMath/MathTypes.h"

namespace ayt::device { class KeyboardDevice; }

namespace ayt::editor {

// Editor-owned freecam (Play/Paused viewport). LMB drag looks,
// WASD/QE move. Does not use PlayerController / Logia.
class EditorFreecam {
public:
    EditorFreecam();

    // Match the historical hardcoded Editor look-at.
    void resetToDefaultView();

    void beginLook(float mouseX, float mouseY);
    void updateLook(float mouseX, float mouseY);
    void endLook();
    bool isLooking() const noexcept { return _looking; }

    // Poll canonical AYDevice state. Editor chrome routing no longer starves
    // the KeyboardDevice because native messages are decoded before routing.
    void updateMovement(float dtSeconds,
                        const ayt::device::KeyboardDevice& keyboard);

    // Viewport wheel zoom is a camera dolly: positive notches move along the
    // current view direction and negative notches move away. This deliberately
    // preserves perspective/FOV while navigating an editor scene.
    void zoom(float wheelNotches);

    // Cursor-anchored variant used by the editor viewport. The camera moves
    // along the ray under the pointer, so the world point under that pointer
    // remains at the same screen position while zooming. The direction need
    // not be normalized; invalid/degenerate rays are ignored.
    void zoomToward(float wheelNotches,
                    const ayt::math::FVector3& worldDirection);

    // Preferences restore an exact editor camera instead of reconstructing
    // it from a lossy look-at point. Invalid values leave the current pose
    // unchanged.
    void setPose(const ayt::math::FVector3& eye,
                 float yawRadians,
                 float pitchRadians);

    ayt::math::FVector3 eye() const noexcept { return _eye; }
    ayt::math::FVector3 forward() const;
    ayt::math::FVector3 right() const;
    ayt::math::FVector3 up() const noexcept {
        return ayt::math::FVector3(0.0f, 1.0f, 0.0f);
    }
    ayt::math::FVector3 at() const {
        const ayt::math::FVector3 f = forward();
        return ayt::math::FVector3(_eye.x + f.x, _eye.y + f.y, _eye.z + f.z);
    }

    float fovYDegrees() const noexcept { return _fovYDegrees; }
    float yawRadians() const noexcept { return _yawRad; }
    float pitchRadians() const noexcept { return _pitchRad; }
    float moveSpeed() const noexcept { return _moveSpeed; }
    void setMoveSpeed(float metersPerSecond) noexcept { _moveSpeed = metersPerSecond; }

private:
    void clampPitch();

    ayt::math::FVector3 _eye;
    float _yawRad = 0.0f;    // radians, 0 = -Z
    float _pitchRad = 0.0f;  // radians, + = look up
    float _fovYDegrees = 50.0f;
    float _moveSpeed = 6.0f;
    float _lookSensitivity = 0.005f;
    // Deliberately gentler than keyboard movement: one full wheel notch moves
    // 0.75 world units at the default speed, while touchpad fractions remain
    // proportional instead of snapping to a whole notch.
    float _wheelMoveScale = 0.125f;

    bool _looking = false;
    float _lastMouseX = 0.0f;
    float _lastMouseY = 0.0f;
};

} // namespace ayt::editor
