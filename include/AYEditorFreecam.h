#pragma once

#include "AYMath/MathTypes.h"

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

    // Poll WASD/QE via Win32 GetAsyncKeyState (Editor eats WM_* from
    // KeyboardDevice while chrome handles messages).
    void updateMovement(float dtSeconds);

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

    bool _looking = false;
    float _lastMouseX = 0.0f;
    float _lastMouseY = 0.0f;
};

} // namespace ayt::editor
