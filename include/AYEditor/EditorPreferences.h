#pragma once

#include "AYMath/MathTypes.h"

#include <cstdint>
#include <string>

namespace ayt::editor {

enum class EditorTool : uint8_t {
    Select = 0,
    Move,
    Rotate,
    Scale,
};

enum class EditorDensity : uint8_t {
    Compact = 0,
    Comfortable,
};

// User-owned editor state. Scene contents deliberately do not live here:
// preferences may be replaced/reset without touching an open document.
struct EditorPreferences {
    static constexpr int kCurrentSchemaVersion = 3;

    int schemaVersion = kCurrentSchemaVersion;

    int windowWidth = 1280;
    int windowHeight = 720;
    bool windowMaximized = false;

    std::string themeName = "aliyat-editor-dark";
    EditorDensity density = EditorDensity::Compact;
    float uiScale = 1.0f;

    std::string dockTree;
    bool panelRenderVisible = true;
    bool panelInspectorVisible = true;
    bool panelNetworkVisible = false;
    bool panelOutlinerVisible = true;
    bool panelConsoleVisible = true;
    bool panelAssetsVisible = true;

    bool viewportOrientationAxisVisible = true;
    bool cameraPoseValid = false;
    ayt::math::FVector3 cameraEye{4.0f, 3.0f, 5.0f};
    float cameraYawRadians = 0.0f;
    float cameraPitchRadians = 0.0f;
    float cameraMoveSpeed = 6.0f;
    EditorTool activeTool = EditorTool::Select;
    bool localTransformSpace = false;
    bool orthographicView = false;
    bool wireframeView = false;

    float gamma = 2.2f;
    float exposure = 1.0f;
    bool bloomEnabled = true;
    float bloomStrength = 0.3f;
    bool depthHazeEnabled = true;
    float depthHazeStrength = 1.0f;
    float depthHazeDensity = 0.04f;
    bool ssaoEnabled = true;
    float ssaoStrength = 0.45f;
    float ssaoRadius = 0.4f;
    float ssaoBias = 0.04f;
    float ambientStrength = 0.85f;
    float shadowBias = 0.003f;
    int tonemapMode = 2;
    bool fxaaEnabled = false;
    bool smaaEnabled = true;
    bool colorGradingEnabled = false;
    int colorGradingPreset = 1;
    float colorGradingStrength = 0.75f;
    bool shadowsEnabled = true;
    bool shadowPcfEnabled = true;
};

inline bool operator==(const EditorPreferences& a,
                       const EditorPreferences& b) noexcept
{
    return a.schemaVersion == b.schemaVersion
        && a.windowWidth == b.windowWidth
        && a.windowHeight == b.windowHeight
        && a.windowMaximized == b.windowMaximized
        && a.themeName == b.themeName
        && a.density == b.density
        && a.uiScale == b.uiScale
        && a.dockTree == b.dockTree
        && a.panelRenderVisible == b.panelRenderVisible
        && a.panelInspectorVisible == b.panelInspectorVisible
        && a.panelNetworkVisible == b.panelNetworkVisible
        && a.panelOutlinerVisible == b.panelOutlinerVisible
        && a.panelConsoleVisible == b.panelConsoleVisible
        && a.panelAssetsVisible == b.panelAssetsVisible
        && a.viewportOrientationAxisVisible == b.viewportOrientationAxisVisible
        && a.cameraPoseValid == b.cameraPoseValid
        && a.cameraEye.x == b.cameraEye.x
        && a.cameraEye.y == b.cameraEye.y
        && a.cameraEye.z == b.cameraEye.z
        && a.cameraYawRadians == b.cameraYawRadians
        && a.cameraPitchRadians == b.cameraPitchRadians
        && a.cameraMoveSpeed == b.cameraMoveSpeed
        && a.activeTool == b.activeTool
        && a.localTransformSpace == b.localTransformSpace
        && a.orthographicView == b.orthographicView
        && a.wireframeView == b.wireframeView
        && a.gamma == b.gamma
        && a.exposure == b.exposure
        && a.bloomEnabled == b.bloomEnabled
        && a.bloomStrength == b.bloomStrength
        && a.depthHazeEnabled == b.depthHazeEnabled
        && a.depthHazeStrength == b.depthHazeStrength
        && a.depthHazeDensity == b.depthHazeDensity
        && a.ssaoEnabled == b.ssaoEnabled
        && a.ssaoStrength == b.ssaoStrength
        && a.ssaoRadius == b.ssaoRadius
        && a.ssaoBias == b.ssaoBias
        && a.ambientStrength == b.ambientStrength
        && a.shadowBias == b.shadowBias
        && a.tonemapMode == b.tonemapMode
        && a.fxaaEnabled == b.fxaaEnabled
        && a.smaaEnabled == b.smaaEnabled
        && a.colorGradingEnabled == b.colorGradingEnabled
        && a.colorGradingPreset == b.colorGradingPreset
        && a.colorGradingStrength == b.colorGradingStrength
        && a.shadowsEnabled == b.shadowsEnabled
        && a.shadowPcfEnabled == b.shadowPcfEnabled;
}

inline bool operator!=(const EditorPreferences& a,
                       const EditorPreferences& b) noexcept
{
    return !(a == b);
}

} // namespace ayt::editor
