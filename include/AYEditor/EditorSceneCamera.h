#pragma once

#include "AYEditor/Editor2DViewportModel.h"
#include "AYEditor/EditorFreecam.h"

#include <AYMath/MathTypes.h>

#include <cstdint>
#include <string_view>

namespace ayt::editor {

enum class SceneViewMode : std::uint8_t {
    ThreeD = 0,
    TwoD,
};

enum class ProjectionMode : std::uint8_t {
    Perspective = 0,
    Orthographic,
};

enum class TwoDPlane : std::uint8_t {
    XY = 0,
};

struct EditorSceneCameraState {
    SceneViewMode mode = SceneViewMode::ThreeD;
    ProjectionMode threeDProjection = ProjectionMode::Perspective;
    TwoDPlane twoDPlane = TwoDPlane::XY;
    ayt::math::FVector2 twoDCenter{0.0f, 0.0f};
    float twoDViewHeight = 600.0f;
    float threeDOrthoHeight = 10.0f;
    ayt::math::FVector3 threeDEye{4.0f, 3.0f, 5.0f};
    float threeDYawRadians = 0.0f;
    float threeDPitchRadians = 0.0f;
    float threeDMoveSpeed = 6.0f;
};

struct EditorSceneCameraFrame {
    ayt::math::Float4x4 view = ayt::math::Float4x4::identity();
    ayt::math::Float4x4 projection = ayt::math::Float4x4::identity();
    ayt::math::FVector3 position{};
    bool orthographic = false;
};

struct EditorSceneContentProfile {
    bool hasOrthographicCamera = false;
    bool hasTwoDContent = false;
    bool hasThreeDContent = false;
};

// Applies project/content/profile precedence after the caller has checked for
// a per-scene workspace state.
SceneViewMode chooseInitialSceneViewMode(
    std::string_view projectDefaultSceneView,
    const EditorSceneContentProfile& content,
    std::string_view engineProfile) noexcept;

// Editor-only camera. It never writes Scene camera components. The 3D freecam
// and the XY 2D pose live side by side, so switching modes preserves both.
class EditorSceneCamera {
public:
    EditorSceneCamera();

    void setViewport(std::uint32_t widthPx, std::uint32_t heightPx) noexcept;
    std::uint32_t viewportWidth() const noexcept { return _viewportWidth; }
    std::uint32_t viewportHeight() const noexcept { return _viewportHeight; }
    float viewportAspect() const noexcept;

    void setMode(SceneViewMode mode) noexcept;
    SceneViewMode mode() const noexcept { return _mode; }
    bool isTwoD() const noexcept { return _mode == SceneViewMode::TwoD; }

    void setThreeDProjection(ProjectionMode mode) noexcept;
    ProjectionMode threeDProjection() const noexcept {
        return _threeDProjection;
    }
    ProjectionMode projection() const noexcept {
        return isTwoD() ? ProjectionMode::Orthographic : _threeDProjection;
    }

    EditorFreecam& threeD() noexcept { return _threeD; }
    const EditorFreecam& threeD() const noexcept { return _threeD; }

    void setTwoDPose(ayt::math::FVector2 center,
                     float verticalWorldSize) noexcept;
    ayt::math::FVector2 twoDCenter() const noexcept {
        return _twoD.center();
    }
    float twoDViewHeight() const noexcept {
        return _twoD.verticalWorldSize();
    }
    float threeDOrthoHeight() const noexcept { return _threeDOrthoHeight; }

    void beginTwoDPan(ayt::math::FVector2 logicalPoint) noexcept;
    bool updateTwoDPan(ayt::math::FVector2 logicalPoint) noexcept;
    void endTwoDPan() noexcept { _twoDPanning = false; }
    bool isTwoDPanning() const noexcept { return _twoDPanning; }

    void zoomAt(float factor, ayt::math::FVector2 logicalAnchor) noexcept;
    ayt::math::FVector2 twoDWorldAt(
        ayt::math::FVector2 logicalPoint) const noexcept;
    float adaptiveGridSpacing(float targetPixels = 48.0f) const noexcept;

    EditorSceneCameraFrame frame() const noexcept;
    bool ray(ayt::math::FVector2 logicalPoint,
             ayt::math::FVector3& outOrigin,
             ayt::math::FVector3& outDirection) const noexcept;

    EditorSceneCameraState state() const noexcept;
    void restoreState(const EditorSceneCameraState& state) noexcept;

private:
    EditorFreecam _threeD;
    Editor2DViewportModel _twoD;
    SceneViewMode _mode = SceneViewMode::ThreeD;
    ProjectionMode _threeDProjection = ProjectionMode::Perspective;
    TwoDPlane _twoDPlane = TwoDPlane::XY;
    std::uint32_t _viewportWidth = 1u;
    std::uint32_t _viewportHeight = 1u;
    float _threeDOrthoHeight = 10.0f;
    bool _twoDPanning = false;
    ayt::math::FVector2 _lastTwoDPanPoint{};
};

} // namespace ayt::editor
