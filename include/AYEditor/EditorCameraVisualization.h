#pragma once
#include <AYEditor/EditorPreferences.h>
#include <AYEditor/EditorSceneCamera.h>
#include <AYMath/MathTypes.h>
#include <array>
#include <memory>
#include <vector>

namespace ayt::entity { class World; class Entity; struct PerspectiveCameraComponent; }
namespace ayt::render { class Renderer; class RenderScene; }
namespace ayt::editor {
struct EditorCameraVisual {
    ayt::entity::Entity* entity = nullptr; // frame-local borrow, never retained by renderer
    ayt::entity::PerspectiveCameraComponent* camera = nullptr;
    ayt::math::Float4x4 pose = ayt::math::Float4x4::identity();
    ayt::math::Float4x4 iconWorld = ayt::math::Float4x4::identity();
    float scale = 1;
    bool selected = false;
    bool runtimeActive = false;
    std::array<ayt::math::FVector3,8> frustum{}; // near/far corners, lens local space
};
/// Editor-only camera poses. Ignore entity scale; include each lens's local TR.
/// Empty outside 3D Edit or when Hidden. Display scale targets 48 logical pixels.
std::vector<EditorCameraVisual> buildEditorCameraVisuals(
    ayt::entity::World& world, const EditorSceneCameraFrame& view, float viewportHeight,
    float aspect, uint32_t selectedEntity, EditorCameraDisplayMode mode, bool edit3D);
/// Same pose and bounds as display; world-space hit distance, independent of entity scale.
bool hitEditorCameraVisual(const EditorCameraVisual& visual,
    const ayt::math::FVector3& origin, const ayt::math::FVector3& direction, float& distance);

// Internal editor submission adapter; uploaded geometry is owned by this adapter.
class EditorCameraVisualRenderer {
public:
    EditorCameraVisualRenderer();
    ~EditorCameraVisualRenderer();
    void append(ayt::render::Renderer& renderer, ayt::render::RenderScene& scene,
                const std::string& engineAssetsRoot, const std::vector<EditorCameraVisual>& visuals,
                EditorCameraDisplayMode mode);
    void release(ayt::render::Renderer& renderer);
    bool ready(EditorCameraDisplayMode mode) const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}
