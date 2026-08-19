#pragma once

#include <cstdint>

namespace ayt::entity { class World; }
namespace ayt::scene { class Scene; class SceneManager; }

namespace ayt::editor {

// Logical worlds exposed to editor tools. This is deliberately separate from
// EditorMode: Paused reads the Play slot, while presentation-only editor modes
// may be added without changing scene ownership.
enum class EditorWorldSlot : uint8_t {
    Edit,
    Play,
};

// Non-owning world-resolution boundary shared by EditorSession and its hosted
// runtime. SceneManager owns/tracks scene lifetime; the process/client World is
// supplied explicitly as the fallback used when no Play Scene is present.
class EditorWorldContext {
public:
    void setSceneManager(ayt::scene::SceneManager* scenes) noexcept;
    void setFallbackWorld(ayt::entity::World* world) noexcept;

    [[nodiscard]] ayt::scene::SceneManager* sceneManager() const noexcept;
    [[nodiscard]] ayt::scene::Scene* scene(EditorWorldSlot slot) const noexcept;

    // Edit never falls back: editor tools must not accidentally mutate the
    // process World when the Edit Scene is unavailable. Play may fall back for
    // standalone runtime tests and the network-client path.
    [[nodiscard]] ayt::entity::World* world(
        EditorWorldSlot slot,
        bool preferSceneWorld = true) const noexcept;

private:
    ayt::scene::SceneManager* _scenes = nullptr;
    ayt::entity::World* _fallbackWorld = nullptr;
};

} // namespace ayt::editor
