#include "AYEditor/EditorFreecam.h"

#include <cmath>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <Windows.h>

namespace ayt::editor {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kHalfPi = kPi * 0.5f;
constexpr float kPitchLimit = kHalfPi - 0.05f;

bool keyDown(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

} // namespace

EditorFreecam::EditorFreecam()
{
    resetToDefaultView();
}

void EditorFreecam::resetToDefaultView()
{
    // Historical Editor eye (4,3,5) looking at origin.
    _eye = ayt::math::FVector3(4.0f, 3.0f, 5.0f);
    const float dx = -_eye.x;
    const float dy = -_eye.y;
    const float dz = -_eye.z;
    const float horiz = std::sqrt(dx * dx + dz * dz);
    _yawRad = std::atan2(dx, dz);
    _pitchRad = std::atan2(dy, horiz);
    clampPitch();
    _looking = false;
}

void EditorFreecam::clampPitch()
{
    if (_pitchRad > kPitchLimit) {
        _pitchRad = kPitchLimit;
    } else if (_pitchRad < -kPitchLimit) {
        _pitchRad = -kPitchLimit;
    }
}

ayt::math::FVector3 EditorFreecam::forward() const
{
    const float cp = std::cos(_pitchRad);
    const float sp = std::sin(_pitchRad);
    const float cy = std::cos(_yawRad);
    const float sy = std::sin(_yawRad);
    // yaw 0 → -Z (toward -Z when looking from +Z); matches atan2(dx,dz) from eye→origin.
    return ayt::math::FVector3(sy * cp, sp, cy * cp);
}

ayt::math::FVector3 EditorFreecam::right() const
{
    const ayt::math::FVector3 f = forward();
    const ayt::math::FVector3 worldUp(0.0f, 1.0f, 0.0f);
    // Screen-right for this yaw/pitch basis (matches user A/D expectation).
    ayt::math::FVector3 r = worldUp.cross(f);
    // If looking straight up/down, cross can degenerate — fall back.
    if (r.lengthSq() < 1.0e-8f) {
        return ayt::math::FVector3(1.0f, 0.0f, 0.0f);
    }
    return r.normalize();
}

void EditorFreecam::beginLook(float mouseX, float mouseY)
{
    _looking = true;
    _lastMouseX = mouseX;
    _lastMouseY = mouseY;
}

void EditorFreecam::updateLook(float mouseX, float mouseY)
{
    if (!_looking) {
        return;
    }
    const float dx = mouseX - _lastMouseX;
    const float dy = mouseY - _lastMouseY;
    _lastMouseX = mouseX;
    _lastMouseY = mouseY;
    _yawRad += dx * _lookSensitivity;
    _pitchRad -= dy * _lookSensitivity; // mouse up → look up
    clampPitch();
}

void EditorFreecam::endLook()
{
    _looking = false;
}

void EditorFreecam::updateMovement(float dtSeconds)
{
    if (dtSeconds <= 0.0f) {
        return;
    }
    // Skip typed text focus: if a text field had focus we'd still move —
    // acceptable for v1 Editor freecam (no TextInput in shell yet).
    const float boost = keyDown(VK_SHIFT) ? 2.5f : 1.0f;
    const float speed = _moveSpeed * boost * dtSeconds;

    ayt::math::FVector3 f = forward();
    // Flatten forward for ground-plane WASD (keep pitch for look only).
    ayt::math::FVector3 flat(f.x, 0.0f, f.z);
    if (flat.lengthSq() > 1.0e-8f) {
        flat = flat.normalize();
    } else {
        flat = ayt::math::FVector3(0.0f, 0.0f, -1.0f);
    }
    const ayt::math::FVector3 r = right();
    const ayt::math::FVector3 worldUp(0.0f, 1.0f, 0.0f);

    ayt::math::FVector3 delta(0.0f, 0.0f, 0.0f);
    if (keyDown('W')) {
        delta = delta + flat;
    }
    if (keyDown('S')) {
        delta = delta - flat;
    }
    if (keyDown('D')) {
        delta = delta + r;
    }
    if (keyDown('A')) {
        delta = delta - r;
    }
    if (keyDown('E') || keyDown(VK_SPACE)) {
        delta = delta + worldUp;
    }
    if (keyDown('Q') || keyDown(VK_CONTROL)) {
        delta = delta - worldUp;
    }

    if (delta.lengthSq() > 1.0e-8f) {
        delta = delta.normalize() * speed;
        _eye = _eye + delta;
    }
}

} // namespace ayt::editor
