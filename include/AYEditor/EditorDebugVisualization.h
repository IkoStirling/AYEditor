#pragma once
#include <cstdint>
namespace ayt::editor {
/// Bit order: colliders, queries, contacts, characters, motion, joints, cameras,
/// skeletons, abilities. Settings belong to this editor session only.
struct EditorDebugVisualizationOptions {
    uint32_t categories = 0;
    uint32_t layerMask = 0xffffffffu;
    bool selectedOnly = false;
    bool depthTest = false;
    bool frozen = false;
    uint32_t trailFrames = 120;
};
}
