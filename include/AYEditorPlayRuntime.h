#pragma once

#include "AYMathTypes.h"

#include <cstdint>
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
    ayt::entity::Entity* selectedCharacterEntity() const { return _characterEntity; }

private:
    bool ensureAssets();
    bool ensureEngineInitialized();
    void syncRendererBootstrap();
    void spawnCubeIfNeeded();
    void clearCube();
    void registerUpdateListener();
    void unregisterUpdateListener();
    static void configureShaderToolchainOnce();

    static std::string resolvePersistentCacheRoot();

    HWND _hostWindow = nullptr;
    uint32_t _clientWidth = 1280;
    uint32_t _clientHeight = 720;
    ayt::math::FRectangle _viewportBounds{};

    bool _presentationReady = false;
    bool _engineInitialized = false;
    bool _simulationActive = false;
    bool _assetsReady = false;

    std::string _cacheRoot;
    std::string _assetRoot;
    std::string _meshPath;
    std::string _materialPath;

    ImportedCharacter _importedCharacter;
    ayt::entity::Entity* _cubeEntity = nullptr;
    ayt::entity::Entity* _characterEntity = nullptr;
    uint64_t _updateListenerId = 0;
};

} // namespace ayt::editor
