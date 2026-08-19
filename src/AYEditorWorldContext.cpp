#include "AYEditor/EditorWorldContext.h"

#include "AYScene.h"
#include "AYScene/SceneManager.h"

namespace ayt::editor {

void EditorWorldContext::setSceneManager(
    ayt::scene::SceneManager* scenes) noexcept
{
    _scenes = scenes;
}

void EditorWorldContext::setFallbackWorld(ayt::entity::World* world) noexcept
{
    _fallbackWorld = world;
}

ayt::scene::SceneManager* EditorWorldContext::sceneManager() const noexcept
{
    return _scenes;
}

ayt::scene::Scene* EditorWorldContext::scene(EditorWorldSlot slot) const noexcept
{
    if (_scenes == nullptr) {
        return nullptr;
    }
    return slot == EditorWorldSlot::Edit ? _scenes->edit() : _scenes->play();
}

ayt::entity::World* EditorWorldContext::world(
    EditorWorldSlot slot,
    bool preferSceneWorld) const noexcept
{
    if (slot == EditorWorldSlot::Edit) {
        if (auto* editScene = scene(EditorWorldSlot::Edit)) {
            return &editScene->world();
        }
        return nullptr;
    }

    if (preferSceneWorld) {
        if (auto* playScene = scene(EditorWorldSlot::Play)) {
            return &playScene->world();
        }
    }
    return _fallbackWorld;
}

} // namespace ayt::editor
