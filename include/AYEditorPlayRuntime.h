#pragma once

#include "AYInspectorOverrides.h"
#include "aymath/MathTypes.h"

#include <cstdint>
#include <memory>
#include <string>

struct HWND__;
using HWND = HWND__*;

namespace ayt::entity {
class Entity;
}

namespace ayt::editor {

// Phase 1 ED-02: when set (via setImportedCharacter), the Play mode
// spawns the imported character instead of the procedural cube.
struct ImportedCharacter {
    std::string meshPath;
    std::string materialPath;
    std::string skeletonPath;
    std::string animationPath;
    bool isValid() const {
        return !meshPath.empty() && !materialPath.empty()
            && !skeletonPath.empty() && !animationPath.empty();
    }
};

// Single-window composite presentation: bgfx on main AYDevice window.
class EditorPlayRuntime {
public:
    EditorPlayRuntime();
    ~EditorPlayRuntime();

    EditorPlayRuntime(const EditorPlayRuntime&) = delete;
    EditorPlayRuntime& operator=(const EditorPlayRuntime&) = delete;

    void setHostWindow(HWND hostWindow);
    void setClientSize(uint32_t width, uint32_t height);
    void setImportedCharacter(const ImportedCharacter& character);

    // Phase 2a: hot-swap. Replaces whatever entity is currently
    // spawned (cube, previous character, or nothing) with the new
    // imported character, falling back to the cube if the new
    // character is invalid. The GameLoop keeps ticking throughout;
    // we do NOT call enterEdit()/startPlay() because that path
    // pauses and re-runs resetDebugOverlayStats() - observable
    // jank. Safe at any time (Edit or Play mode).
    void replaceImportedCharacter(const ImportedCharacter& character);

    // ED-03: Inspector override application. If a character is
    // currently spawned, the runtime mutates SkeletonComponent::
    // skeletonPath and AnimationComponent::clipPath in place to
    // mirror the non-empty fields of `overrides`. If no character
    // is spawned yet, the override is stashed in `_pendingOverrides`
    // and re-applied on the next spawn (set by the Inspector
    // before the user clicks Play). Validation log: missing files
    // on disk produce a one-line stderr notice but do NOT clear
    // the field - the entity keeps its previous path so the user
    // can recover by picking a different file.
    void applyComponentOverrides(const EntityInspectorOverrides& overrides);

    // ED-03: read access to the pending overrides buffer. Tests
    // use this to confirm Reset semantics.
    const EntityInspectorOverrides& pendingOverrides() const
    {
        return _pendingOverrides;
    }

    bool ensurePresentationReady();
    bool startPlay();
    void enterEdit();
    void shutdownEngine();
    void tick();

    bool isPresentationReady() const { return _presentationReady; }
    bool isEngineInitialized() const { return _engineInitialized; }
    bool isSimulationActive() const { return _simulationActive; }
    bool isPlaying() const { return _simulationActive; }

    void syncViewportRect(const ayt::math::FRectangle& bounds);
    const ayt::math::FRectangle& viewportBounds() const { return _viewportBounds; }

    // ED-02: spawn the configured character (if any) into the ECS
    // world. Returns true when an entity was created, false when no
    // character was configured or asset load failed. Exposed publicly
    // so unit tests can drive the spawn directly without bringing
    // up the full renderer + GameLoop session.
    bool trySpawnImportedCharacter();
    void clearCharacter() noexcept;
    // Phase 2a: cube teardown helper promoted to public so the
    // toolbar Import button can fully reset Play state for hot-
    // swap. Safe to call with no cube spawned (no-op).
    void clearCube() noexcept;
    ayt::entity::Entity* selectedCharacterEntity() const { return _characterEntity; }

    // G2: cache-root resolution promoted to public-static so
    // EditorApp::run() (which lives outside this class) can derive
    // the same root the runtime uses for ensureAssets(). Single
    // source of truth - no risk of cache-root drift between
    // EditorApp and EditorPlayRuntime. Returns
    // "<exeDir>/ayeditor_cache\\" on Windows (single trailing '\\'),
    // or the literal relative path on GetModuleFileNameA failure.
    static std::string resolvePersistentCacheRoot();

private:
    bool ensureAssets();
    bool ensureEngineInitialized();
    void syncRendererBootstrap();
    void applyEditorRenderPipeline();
    void spawnCubeIfNeeded();
    void spawnGroundIfNeeded();
    void spawnGlassIfNeeded();
    void clearGround() noexcept;
    void clearGlass() noexcept;
    void ensureGlassMaterialAlpha();
    void registerUpdateListener();
    void unregisterUpdateListener();
    static void configureShaderToolchainOnce();

    // INT-01 (2026-07-15): Logia end-to-end smoke — spawn a fresh
    // Entity with the EditorPlayerController ScriptComponent, bind
    // <assetRoot>/Scripts/PlayerController.logia via the registered
    // ScriptSubSystem so the per-tick `entity.onUpdate(dt)` path
    // flows Lua → AYReflect → C++ field mutation. Matches the
    // canonical `examples/player_controller.logia` shape (self.position
    // + self.speed + self.jump_force).
    void spawnPlayerControllerIfNeeded();
    bool bindPlayerScript();
    void clearPlayerController() noexcept;
    bool seedPlayerControllerLogia();


    HWND _hostWindow = nullptr;
    uint32_t _clientWidth = 1280;
    uint32_t _clientHeight = 720;
    ayt::math::FRectangle _viewportBounds{};

    bool _presentationReady = false;
    bool _engineInitialized = false;
    bool _engineShutdown = false;
    bool _simulationActive = false;
    bool _assetsReady = false;
    bool _pipelineConfigured = false;

    std::string _cacheRoot;
    std::string _assetRoot;
    std::string _meshPath;
    std::string _materialPath;
    std::string _groundMeshPath;
    std::string _groundMaterialPath;
    std::string _glassMeshPath;
    std::string _glassMaterialPath;

    ImportedCharacter _importedCharacter;
    ayt::entity::Entity* _cubeEntity = nullptr;
    ayt::entity::Entity* _groundEntity = nullptr;
    ayt::entity::Entity* _glassEntity = nullptr;
    ayt::entity::Entity* _characterEntity = nullptr;
    ayt::entity::Entity* _playerEntity = nullptr;
    bool _playerScriptBound = false;
    uint64_t _updateListenerId = 0;

    // B7+ multi-light storage lives in the .cpp (unique_ptr to a
    // complete type defined there) so this header does not pull
    // AYRenderScene into every Editor TU and does not inflate
    // sizeof(EditorPlayRuntime) — a stale AYEditorApp.obj with the
    // old size immediately AVs in initialize() string assigns.
    // Cleared via setSceneLights(nullptr) in shutdownEngine.
    struct SceneLightsStorage;
    std::unique_ptr<SceneLightsStorage> _sceneLightsStorage;
    // §Skybox0 — host-owned equirect SkySource (Deferred only).
    // TextureHandle lives on Renderer; SkySource POD must outlive
    // render() while setSkySource(&sky) is active.
    struct SkySourceStorage;
    std::unique_ptr<SkySourceStorage> _skySourceStorage;

    // ED-03: pending Inspector overrides. Populated by
    // applyComponentOverrides when no character is currently
    // spawned; consumed by trySpawnImportedCharacter right after
    // spawnCharacterFromPaths returns. Survives replaceImported
    // Character so the user's per-character clip/skel picks
    // persist across hot-swap.
    EntityInspectorOverrides _pendingOverrides;
};

} // namespace ayt::editor
