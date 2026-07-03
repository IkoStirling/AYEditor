#include "AYEditorPlayRuntime.h"

#include "AYWindowManager.h"
#include "AYEntity.h"
#include "AYEntityModule.h"
#include "AYGameLoop.h"
#include "AYRendererSubSystem.h"

#include "assetsImpl/AYMaterial.h"
#include "assetsImpl/AYMesh.h"
#include "assetsImpl/AYTexture.h"

#include "AYFile.h"

#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifndef UNICODE
#  define UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <Windows.h>

#ifndef AY_SHADER_SHADERC_HINT
#  define AY_SHADER_SHADERC_HINT ""
#endif

namespace ayt::editor {

namespace {

const char* kSimpleLitPhoskia = R"(
material SimpleLit {
    texture2d albedoMap
    uniform vec3 lightDir
    uniform vec3 lightColor
    property baseColor = vec4(1.0, 1.0, 1.0, 1.0)

    vertex {
        in pos : position
        in nrm : normal
        in uv  : texcoord
        out worldNormal : normal = (modelMatrix * vec4(nrm, 0.0)).xyz
        out uvOut : texcoord = uv
        return modelViewProjection * vec4(pos, 1.0)
    }
    fragment {
        in worldNormal : normal
        in uvOut : texcoord
        let albedo = sample(albedoMap, uvOut) * baseColor
        let ndotl = max(dot(normalize(worldNormal), normalize(lightDir)), 0.05)
        return vec4(albedo.rgb * lightColor * ndotl, albedo.a)
    }
}
)";

bool fileExists(const std::string& path)
{
    struct stat st;
    return !path.empty() && ::stat(path.c_str(), &st) == 0;
}

bool writeBytes(const std::string& path, const void* data, size_t size)
{
    ayt::io::File file(path, ayt::io::File::Mode::BinaryWrite);
    if (!file.isOpen()) {
        return false;
    }
    return file.write(data, size) == size;
}

bool writeText(const std::string& path, const std::string& text)
{
    ayt::io::File file(path, ayt::io::File::Mode::Write);
    if (!file.isOpen()) {
        return false;
    }
    return file.write(text.data(), text.size()) == text.size();
}

bool ensureAssetDirectory(const std::string& path)
{
    if (CreateDirectoryA(path.c_str(), nullptr) != 0) {
        return true;
    }
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

} // namespace

EditorPlayRuntime::EditorPlayRuntime() = default;

EditorPlayRuntime::~EditorPlayRuntime() {
    shutdownEngine();
    destroyViewportWindow();
}

std::string EditorPlayRuntime::resolvePersistentCacheRoot()
{
    char modulePath[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return "ayeditor_cache\\";
    }

    std::string root(modulePath, modulePath + len);
    const size_t slash = root.find_last_of("\\/");
    if (slash != std::string::npos) {
        root.resize(slash + 1);
    }
    root += "ayeditor_cache\\";
    return root;
}

void EditorPlayRuntime::setHostWindow(HWND hostWindow) {
    _hostWindow = hostWindow;
}

void EditorPlayRuntime::setWindowManager(ayt::device::WindowManager* windowManager) {
    _windowManager = windowManager;
}

void EditorPlayRuntime::setClientSize(uint32_t width, uint32_t height) {
    _clientWidth  = width > 0 ? width : _clientWidth;
    _clientHeight = height > 0 ? height : _clientHeight;
}

bool EditorPlayRuntime::ensureAssets() {
    if (_assetsReady) {
        return true;
    }

    _cacheRoot = resolvePersistentCacheRoot();
    _assetRoot = _cacheRoot + "assets\\";
    _meshPath     = _assetRoot + "cube.aymesh";
    _materialPath = _assetRoot + "cube.aymat";

    if (!ensureAssetDirectory(_cacheRoot) || !ensureAssetDirectory(_assetRoot)) {
        return false;
    }

    const std::string shaderPath  = _assetRoot + "simple_lit.phoskia";
    const std::string texturePath = _assetRoot + "albedo.aytex";

    if (!fileExists(shaderPath) && !writeText(shaderPath, kSimpleLitPhoskia)) {
        return false;
    }

    if (!fileExists(texturePath)) {
        ayt::resource::Texture texture;
        texture.createCheckerboard(64, 64, 8);
        std::vector<ayt::resource::UInt8> texBinary;
        if (!texture.saveToBinary(texBinary)) {
            return false;
        }
        if (!writeBytes(texturePath, texBinary.data(), texBinary.size())) {
            return false;
        }
    }

    if (!fileExists(_materialPath)) {
        ayt::resource::Material material;
        material.setShader("simple_lit.phoskia");
        const ayt::resource::Float32 baseColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        material.setFloat4("baseColor", baseColor);
        material.setTexture("albedoMap", "albedo.aytex");
        std::vector<ayt::resource::UInt8> matBinary;
        if (!material.saveToBinary(matBinary)) {
            return false;
        }
        if (!writeBytes(_materialPath, matBinary.data(), matBinary.size())) {
            return false;
        }
    }

    if (!fileExists(_meshPath)) {
        ayt::resource::Mesh mesh;
        mesh.createCube(1.0f);
        std::vector<ayt::resource::UInt8> meshBinary;
        if (!mesh.saveToBinary(meshBinary)) {
            return false;
        }
        if (!writeBytes(_meshPath, meshBinary.data(), meshBinary.size())) {
            return false;
        }
    }

    const std::string shaderDumpDir  = _cacheRoot + "shader_dump\\";
    const std::string shaderCacheDir = _cacheRoot + "shaders\\";
    ensureAssetDirectory(shaderDumpDir);
    ensureAssetDirectory(shaderCacheDir);
    ayt::render::RendererSubSystem::setBootstrapShaderDumpDirectory(shaderDumpDir);
    ayt::render::RendererSubSystem::setBootstrapShaderCacheDirectory(shaderCacheDir);

    std::fprintf(stderr, "[EditorPlayRuntime] assets ready in %s\n", _assetRoot.c_str());
    _assetsReady = true;
    return true;
}

bool EditorPlayRuntime::ensureViewportWindow() {
    if (_viewportWindow != nullptr) {
        return true;
    }

    if (_hostWindow == nullptr || _windowManager == nullptr) {
        return false;
    }

    ayt::device::ChildWindowDesc desc{};
    desc.parentHandle = _hostWindow;
    desc.x = 0;
    desc.y = 0;
    desc.width = 100;
    desc.height = 100;

    void* childHandle = nullptr;
    if (!_windowManager->createChildWindow(desc, childHandle) || childHandle == nullptr) {
        return false;
    }

    _viewportWindow = static_cast<HWND>(childHandle);
    ShowWindow(_viewportWindow, SW_HIDE);
    return true;
}

void EditorPlayRuntime::destroyViewportWindow() {
    if (_viewportWindow != nullptr) {
        if (_windowManager != nullptr) {
            _windowManager->destroyChildWindow(_viewportWindow);
        } else {
            DestroyWindow(_viewportWindow);
        }
        _viewportWindow = nullptr;
    }
    _viewportVisible = false;
}

void EditorPlayRuntime::syncRendererBootstrap() {
    if (_viewportWindow == nullptr) {
        return;
    }

    const int w = static_cast<int>(_viewportBounds.maxX - _viewportBounds.minX);
    const int h = static_cast<int>(_viewportBounds.maxY - _viewportBounds.minY);
    if (w < 32 || h < 32) {
        return;
    }

    const uint32_t uw = static_cast<uint32_t>(w);
    const uint32_t uh = static_cast<uint32_t>(h);

    ayt::render::RendererSubSystem::setBootstrapWindow(_viewportWindow, uw, uh);
    ayt::render::RendererSubSystem::setBootstrapViewport(0, 0,
        static_cast<uint16_t>(uw), static_cast<uint16_t>(uh));

    if (_engineInitialized) {
        if (auto* renderer = ayt::render::RendererSubSystem::findRegistered()) {
            renderer->setClientSize(uw, uh);
            renderer->setViewportRect(0, 0, static_cast<uint16_t>(uw), static_cast<uint16_t>(uh));
        }
    }
}

bool EditorPlayRuntime::ensureEngineInitialized() {
    if (_engineInitialized) {
        syncRendererBootstrap();
        return true;
    }

    if (_hostWindow == nullptr) {
        std::fprintf(stderr, "[EditorPlayRuntime] host window unavailable\n");
        return false;
    }

    if (!ensureViewportWindow()) {
        std::fprintf(stderr, "[EditorPlayRuntime] viewport window unavailable\n");
        return false;
    }

    if (!ensureAssets()) {
        std::fprintf(stderr, "[EditorPlayRuntime] asset bake failed\n");
        return false;
    }

    syncRendererBootstrap();

    ayt::game::GameLoop& loop = ayt::game::GameLoop::instance();
    loop.setTargetFPS(60.0f);
    loop.setRenderThreadEnabled(false);

    ayt::entity::bootstrapModule();

    if (!loop.isPlaySessionActive() && !loop.preparePlaySession()) {
        std::fprintf(stderr, "[EditorPlayRuntime] preparePlaySession failed\n");
        return false;
    }

    if (auto* rendererSub = ayt::render::RendererSubSystem::findRegistered()) {
        rendererSub->renderer().resetDebugOverlayStats();
    }

    registerUpdateListener();
    _engineInitialized = true;
    return true;
}

void EditorPlayRuntime::registerUpdateListener() {
    if (_updateListenerId != 0) {
        return;
    }

    _updateListenerId = ayt::game::GameLoop::instance().onUpdate([this](float /*deltaTime*/) {
        spawnCubeIfNeeded();
        if (_cubeEntity == nullptr) {
            return;
        }

        auto* transform = _cubeEntity->getComponent<ayt::entity::Transform>();
        if (transform == nullptr) {
            return;
        }

        const float elapsed = ayt::game::GameLoop::instance().getElapsedTime();
        transform->rotation = ayt::math::FQuaternion::fromEulerAngles(
            ayt::math::FVector3(elapsed * 0.5f, elapsed * 0.8f, 0.0f));
    });
}

void EditorPlayRuntime::unregisterUpdateListener() {
    if (_updateListenerId == 0) {
        return;
    }

    ayt::game::GameLoop::instance().offUpdate(_updateListenerId);
    _updateListenerId = 0;
}

void EditorPlayRuntime::syncViewportRect(const ayt::math::FRectangle& bounds) {
    _viewportBounds = bounds;

    if (!ensureViewportWindow()) {
        return;
    }

    const int x = static_cast<int>(bounds.minX);
    const int y = static_cast<int>(bounds.minY);
    const int w = static_cast<int>(bounds.maxX - bounds.minX);
    const int h = static_cast<int>(bounds.maxY - bounds.minY);
    if (w < 32 || h < 32) {
        return;
    }

    MoveWindow(_viewportWindow, x, y, w, h, TRUE);
    syncRendererBootstrap();

    if (_viewportVisible) {
        SetWindowPos(_viewportWindow, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void EditorPlayRuntime::spawnCubeIfNeeded() {
    if (_cubeEntity != nullptr) {
        return;
    }

    _cubeEntity = ayt::entity::World::instance().createEntity();
    if (_cubeEntity == nullptr) {
        return;
    }

    _cubeEntity->addComponent<ayt::entity::Transform>();
    auto* mesh = _cubeEntity->addComponent<ayt::entity::MeshComponent>();
    mesh->meshPath     = _meshPath;
    mesh->materialPath = _materialPath;
}

void EditorPlayRuntime::clearCube() {
    if (_cubeEntity != nullptr) {
        ayt::entity::World::instance().destroyEntity(_cubeEntity);
        _cubeEntity = nullptr;
    }
}

bool EditorPlayRuntime::startPlay() {
    if (!ensureEngineInitialized()) {
        return false;
    }

    if (_viewportWindow != nullptr) {
        ShowWindow(_viewportWindow, SW_SHOW);
        SetWindowPos(_viewportWindow, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        _viewportVisible = true;
    }

    if (auto* rendererSub = ayt::render::RendererSubSystem::findRegistered()) {
        rendererSub->renderer().resetDebugOverlayStats();
    }

    ayt::game::GameLoop::instance().resume();
    spawnCubeIfNeeded();
    _simulationActive = true;
    return true;
}

void EditorPlayRuntime::enterEdit() {
    ayt::game::GameLoop::instance().pause();
    clearCube();
    _simulationActive = false;

    if (_viewportWindow != nullptr) {
        ShowWindow(_viewportWindow, SW_HIDE);
        _viewportVisible = false;
    }
}

void EditorPlayRuntime::shutdownEngine() {
    enterEdit();

    if (_engineInitialized) {
        unregisterUpdateListener();
        ayt::game::GameLoop::instance().endPlaySession();
        _engineInitialized = false;
    }
}

void EditorPlayRuntime::tick() {
    if (!_simulationActive) {
        return;
    }

    ayt::game::GameLoop::instance().tickOnce();
}

} // namespace ayt::editor
