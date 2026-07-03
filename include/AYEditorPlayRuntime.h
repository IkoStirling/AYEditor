#pragma once

#include "AYMathTypes.h"

#include <cstdint>
#include <string>

struct HWND__;
using HWND = HWND__*;

namespace ayt::entity {
class Entity;
}

namespace ayt::device {
class WindowManager;
}

namespace ayt::editor {

// Host-window GDI chrome + child HWND viewport for bgfx (avoids GDI/D3D blit conflicts).
class EditorPlayRuntime {
public:
    EditorPlayRuntime();
    ~EditorPlayRuntime();

    EditorPlayRuntime(const EditorPlayRuntime&) = delete;
    EditorPlayRuntime& operator=(const EditorPlayRuntime&) = delete;

    void setHostWindow(HWND hostWindow);
    void setWindowManager(ayt::device::WindowManager* windowManager);
    void setClientSize(uint32_t width, uint32_t height);

    bool startPlay();
    void enterEdit();
    void shutdownEngine();
    void tick();

    bool isEngineInitialized() const { return _engineInitialized; }
    bool isSimulationActive() const { return _simulationActive; }
    bool isPlaying() const { return _simulationActive; }
    bool isViewportVisible() const { return _viewportVisible; }

    void syncViewportRect(const ayt::math::FRectangle& bounds);
    const ayt::math::FRectangle& viewportBounds() const { return _viewportBounds; }

private:
    bool ensureAssets();
    bool ensureViewportWindow();
    void destroyViewportWindow();
    bool ensureEngineInitialized();
    void syncRendererBootstrap();
    void spawnCubeIfNeeded();
    void clearCube();
    void registerUpdateListener();
    void unregisterUpdateListener();

    static std::string resolvePersistentCacheRoot();

    HWND _hostWindow = nullptr;
    HWND _viewportWindow = nullptr;
    ayt::device::WindowManager* _windowManager = nullptr;
    uint32_t _clientWidth = 1280;
    uint32_t _clientHeight = 720;
    ayt::math::FRectangle _viewportBounds{};

    bool _engineInitialized = false;
    bool _simulationActive = false;
    bool _viewportVisible = false;
    bool _assetsReady = false;

    std::string _cacheRoot;
    std::string _assetRoot;
    std::string _meshPath;
    std::string _materialPath;

    ayt::entity::Entity* _cubeEntity = nullptr;
    uint64_t _updateListenerId = 0;
};

} // namespace ayt::editor
