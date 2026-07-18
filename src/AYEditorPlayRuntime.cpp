#include "AYEditorPlayRuntime.h"

#include "AYCharacterEntity.h"  // ED-02 spawnCharacterFromPaths / destroyCharacter
#include "AYEntity.h"
#include "AYEntityModule.h"
#include "AYGameLoop.h"
#include "AYRendererSubSystem.h"
#include "AYScriptSubSystem.h"
#include "AYShadercDriver.h"

#include <components/AYAnimationComponent.h>  // ED-03 override target
#include <components/AYMeshComponent.h>
#include <components/AYScriptComponent.h>
#include <components/AYSkeletonComponent.h>   // ED-03 override target

#include "EditorPlayerController.h"  // INT-01 sample script host

#include "assetsImpl/AYMaterial.h"
#include "assetsImpl/AYMesh.h"
#include "assetsImpl/AYTexture.h"

#include "ayio/File.h"

#include <logia/AYCompilerError.h>

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

#ifndef AY_SHADER_BGFX_COMMON_HINT
#  define AY_SHADER_BGFX_COMMON_HINT ""
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

std::string resolveExistingPath(const char* hint, const char* const* fallbacks, size_t count)
{
    if (hint != nullptr && hint[0] != '\0' && fileExists(hint)) {
        return hint;
    }
    for (size_t i = 0; i < count; ++i) {
        if (fileExists(fallbacks[i])) {
            return fallbacks[i];
        }
    }
    return hint != nullptr ? std::string(hint) : std::string{};
}

} // namespace

EditorPlayRuntime::EditorPlayRuntime() = default;

EditorPlayRuntime::~EditorPlayRuntime()
{
    shutdownEngine();
}

void EditorPlayRuntime::configureShaderToolchainOnce()
{
    static bool configured = false;
    if (configured) {
        return;
    }

    static const char* kShadercFallbacks[] = {
        "AYRuntime/AYShader/thirdParty/bgfx-install/debug/bin/shaderc.exe",
        "../AYShader/thirdParty/bgfx-install/debug/bin/shaderc.exe",
        "../../AYShader/thirdParty/bgfx-install/debug/bin/shaderc.exe",
    };
    const std::string shadercPath =
        resolveExistingPath(AY_SHADER_SHADERC_HINT, kShadercFallbacks,
                            sizeof(kShadercFallbacks) / sizeof(kShadercFallbacks[0]));
    if (!shadercPath.empty() && fileExists(shadercPath)) {
        ayt::shader::AYShadercDriver::setDefaultExecutable(shadercPath);
    }

    configured = true;
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

void EditorPlayRuntime::setHostWindow(HWND hostWindow)
{
    _hostWindow = hostWindow;
}

void EditorPlayRuntime::setClientSize(uint32_t width, uint32_t height)
{
    _clientWidth  = width > 0 ? width : _clientWidth;
    _clientHeight = height > 0 ? height : _clientHeight;
}

// ED-02: stash the imported character paths. Applied lazily on the
// next `startPlay()` call — changing the import while a simulation
// is running is a Phase 2 / "live preview" feature. The default-ctor
// ImportedCharacter has all empty strings, which `isValid()` reports
// false; startPlay falls through to the cube path in that case.
void EditorPlayRuntime::setImportedCharacter(const ImportedCharacter& character)
{
    _importedCharacter = character;
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

    // INT-01 (2026-07-15): seed <assetRoot>/Scripts/PlayerController.logia
    // from the canonical example bundled with AYScript so a fresh
    // Editor Play session always has a Logia sample to bind. Mirror
    // the meshPath/materialPath seeding above — only copy when the
    // user hasn't supplied a path already. Hot-reload watch activates
    // once bindPlayerScript() runs (S3.7b).
    if (!seedPlayerControllerLogia()) {
        std::fprintf(stderr,
            "[EditorPlayRuntime] PlayerController.logia seed skipped "
            "(source not bundled; bind will no-op until the file exists)\n");
    }

    std::fprintf(stderr, "[EditorPlayRuntime] assets ready in %s\n", _assetRoot.c_str());
    _assetsReady = true;
    return true;
}

bool EditorPlayRuntime::seedPlayerControllerLogia() {
    const std::string destPath = _assetRoot + "Scripts\\PlayerController.logia";
    if (ayt::io::File::exists(destPath)) {
        return true;
    }
    ensureAssetDirectory(_assetRoot + "Scripts\\");

    static const char* kRepoSourceCandidates[] = {
        "AYRuntime\\AYScript\\examples\\player_controller.logia",
        "..\\AYScript\\examples\\player_controller.logia",
        "..\\..\\AYScript\\examples\\player_controller.logia",
        "..\\..\\..\\AYScript\\examples\\player_controller.logia",
    };
    std::string sourcePath;
    for (const char* c : kRepoSourceCandidates) {
        if (fileExists(c)) { sourcePath = c; break; }
    }
    if (sourcePath.empty()) {
        return false;
    }
    ayt::io::File src(sourcePath, ayt::io::File::Mode::Read);
    if (!src.isOpen()) {
        return false;
    }
    std::string contents;
    contents.resize(static_cast<size_t>(src.size()));
    if (contents.empty()) return true;  // zero-byte file is a valid seed
    if (src.read(contents.data(), contents.size()) != static_cast<int64_t>(contents.size())) {
        return false;
    }
    return writeText(destPath, contents);
}

bool EditorPlayRuntime::ensurePresentationReady()
{
    if (_presentationReady) {
        syncRendererBootstrap();
        return true;
    }

    if (_hostWindow == nullptr) {
        std::fprintf(stderr, "[EditorPlayRuntime] host window unavailable\n");
        return false;
    }

    configureShaderToolchainOnce();

    if (!ensureAssets()) {
        std::fprintf(stderr, "[EditorPlayRuntime] asset bake failed\n");
        return false;
    }

    syncRendererBootstrap();

    ayt::game::GameLoop& loop = ayt::game::GameLoop::instance();
    loop.setTargetFPS(60.0f);
    loop.setRenderThreadEnabled(false);

    ayt::entity::bootstrapModule();
    ayt::render::RendererSubSystem::registerSubSystem();

    if (!loop.isPlaySessionActive() && !loop.preparePlaySession()) {
        std::fprintf(stderr, "[EditorPlayRuntime] preparePlaySession failed\n");
        return false;
    }

    // EditorApp drives presentation via renderCompositeFrame; suppress GameLoop auto-render.
    loop.setRenderCallback([]() {});

    loop.pause();
    _presentationReady = true;
    return true;
}

void EditorPlayRuntime::syncRendererBootstrap()
{
    if (_hostWindow == nullptr) {
        return;
    }

    ayt::render::RendererSubSystem::setBootstrapWindow(_hostWindow, _clientWidth, _clientHeight);

    uint16_t vx = 0;
    uint16_t vy = 0;
    uint16_t vw = static_cast<uint16_t>(_clientWidth);
    uint16_t vh = static_cast<uint16_t>(_clientHeight);

    const int w = static_cast<int>(_viewportBounds.maxX - _viewportBounds.minX);
    const int h = static_cast<int>(_viewportBounds.maxY - _viewportBounds.minY);
    if (w >= 32 && h >= 32) {
        vx = static_cast<uint16_t>(_viewportBounds.minX);
        vy = static_cast<uint16_t>(_viewportBounds.minY);
        vw = static_cast<uint16_t>(w);
        vh = static_cast<uint16_t>(h);
    }

    ayt::render::RendererSubSystem::setBootstrapViewport(vx, vy, vw, vh);

    if (auto* renderer = ayt::render::RendererSubSystem::findRegistered()) {
        renderer->setClientSize(_clientWidth, _clientHeight);
        renderer->setViewportRect(vx, vy, vw, vh);
    }
}

bool EditorPlayRuntime::ensureEngineInitialized()
{
    if (_engineInitialized) {
        syncRendererBootstrap();
        return true;
    }

    if (!ensurePresentationReady()) {
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
        // Never spawn/rotate the fallback cube while a character (or any
        // non-cube visual) is the active Play subject — that used to leave
        // a second mesh in the viewport beside the imported character.
        if (_characterEntity != nullptr) {
            return;
        }

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

void EditorPlayRuntime::syncViewportRect(const ayt::math::FRectangle& bounds)
{
    _viewportBounds = bounds;
    syncRendererBootstrap();
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

void EditorPlayRuntime::clearCube() noexcept {
    if (_cubeEntity != nullptr) {
        ayt::entity::World::instance().destroyEntity(_cubeEntity);
        _cubeEntity = nullptr;
    }
}

// Phase 2a: hot-swap. Sequence mirrors the spawn policy documented
// in startPlay() (G3) - clear whatever entity is currently spawned,
// then attempt the character, falling back to the procedural cube
// when the new character is invalid. The GameLoop keeps ticking
// throughout (no enterEdit + startPlay jank).
void EditorPlayRuntime::replaceImportedCharacter(const ImportedCharacter& character)
{
    setImportedCharacter(character);
    clearCharacter();
    clearCube();
    if (!trySpawnImportedCharacter()) {
        spawnCubeIfNeeded();
    }
}

// ED-02: spawn the imported skinned character if the user has set
// `setImportedCharacter(...)` with valid paths. Idempotent — won't
// re-spawn if the entity already exists. Returns true when an
// entity was successfully created, false when (a) no character was
// configured, (b) an entity was already spawned, or (c) asset load
// failed. The `startPlay()` caller falls back to the cube in (a)/(c).
bool EditorPlayRuntime::trySpawnImportedCharacter() {
    if (_characterEntity != nullptr) {
        return false;
    }
    if (!_importedCharacter.isValid()) {
        return false;
    }

    _characterEntity = ayt::entity::spawnCharacterFromPaths(
        _importedCharacter.meshPath,
        _importedCharacter.materialPath,
        _importedCharacter.skeletonPath,
        _importedCharacter.animationPath);

    if (_characterEntity == nullptr) {
        std::fprintf(stderr,
            "[EditorPlayRuntime] spawnCharacterFromPaths returned "
            "nullptr (asset load failed) — falling back to cube\n");
        return false;
    }

    // ED-03: if the Inspector stashed overrides earlier
    // (before any character was spawned, or via a previous
    // round), apply them to the freshly spawned entity now.
    // Doing this here means the user can hot-swap an FBX, set
    // new clip + skel paths in the Inspector, click Play, and
    // see the animation bound to the entity on the very first
    // tick.
    if (!_pendingOverrides.isCleared()) {
        applyComponentOverrides(_pendingOverrides);
    }
    return true;
}

// ED-03: applyInspectorOverrides body. Mutates the spawned
// entity's component paths in place when present. Always
// updates _pendingOverrides so subsequent replaceImportedCharacter
// calls re-apply the user's picks. Validation: missing files on
// disk produce a one-line stderr notice but do NOT clear the
// field (the entity keeps its current path so the user can
// recover by picking a different file). Empty input struct is
// the explicit "Reset" call - it clears _pendingOverrides and
// does not touch the live entity's component paths.
void EditorPlayRuntime::applyComponentOverrides(
    const EntityInspectorOverrides& overrides)
{
    // The "Reset" button sends a default-constructed
    // EntityInspectorOverrides (all fields empty). Treat that
    // as a clear-pending; leave the live entity's existing
    // component paths alone so the user can keep animating
    // with whatever value is currently on the entity.
    if (overrides.isCleared()) {
        _pendingOverrides.clear();
        return;
    }

    // Always update the pending buffer FIRST so a future
    // replaceImportedCharacter re-applies the same picks even
    // when no character is currently spawned.
    _pendingOverrides = overrides;

    if (_characterEntity == nullptr) {
        // No live entity yet - overrides will be applied at
        // the next spawn. Stderr is silent here because the
        // Inspector caller is the only path that hits this
        // branch and it always logs the click.
        return;
    }

    if (!overrides.skeletonPathOverride.empty()) {
        if (auto* skelComp = _characterEntity->getComponent<ayt::entity::SkeletonComponent>()) {
            if (!ayt::io::File::exists(overrides.skeletonPathOverride)) {
                std::fprintf(stderr,
                    "[EditorPlayRuntime] skeleton override not found on disk: %s "
                    "(keeping previous path)\n",
                    overrides.skeletonPathOverride.c_str());
            } else {
                skelComp->skeletonPath = overrides.skeletonPathOverride;
            }
        }
    }
    if (!overrides.animationPathOverride.empty()) {
        if (auto* animComp = _characterEntity->getComponent<ayt::entity::AnimationComponent>()) {
            if (!ayt::io::File::exists(overrides.animationPathOverride)) {
                std::fprintf(stderr,
                    "[EditorPlayRuntime] animation override not found on disk: %s "
                    "(keeping previous path)\n",
                    overrides.animationPathOverride.c_str());
            } else {
                animComp->clipPath = overrides.animationPathOverride;
            }
        }
    }
}

void EditorPlayRuntime::clearCharacter() noexcept {
    if (_characterEntity != nullptr) {
        ayt::entity::destroyCharacter(_characterEntity);
        _characterEntity = nullptr;
    }
}

bool EditorPlayRuntime::startPlay()
{
    if (!ensureEngineInitialized()) {
        return false;
    }

    if (auto* rendererSub = ayt::render::RendererSubSystem::findRegistered()) {
        rendererSub->renderer().resetDebugOverlayStats();
    }

    ayt::game::GameLoop::instance().resume();
    // Spawn-policy contract (G3):
    //   1. trySpawnImportedCharacter() wins when EditorApp populated
    //      sessionDesc.importedCharacter with a valid 4-tuple
    //      (mesh + material + skeleton + animation) via the
    //      `--import <path.fbx>` flag. See mapConversionToImportedCharacter
    //      in AYImportedCharacterMapper.{h,cpp}; that helper enforces
    //      Phase 1's "Animation is required" policy so a mesh-only
    //      or mesh+skeleton-only conversion can never reach here with
    //      a half-valid character.
    //   2. The procedural cube is the fallback for every other path:
    //      - no `--import` flag
    //      - import failed (file missing / unsupported extension /
    //        FBX parse error)
    //      - import succeeded but produced no skinned character
    //        (mapper returned success=false; cube still renders)
    //      - trySpawnImportedCharacter was called but
    //        spawnCharacterFromPaths returned nullptr at runtime
    //        (resource adapter failure; cube replaces it)
    //   3. Order is intentional: a designer passing a valid
    //      `--import <character.fbx>` should always see that
    //      character; the cube is the always-visible default when
    //      nothing else resolves. Cube spawn is gated by
    //      `_cubeEntity != nullptr` so it never doubles up with the
    //      character entity.
    if (!trySpawnImportedCharacter()) {
        spawnCubeIfNeeded();
    }

    // INT-01 (2026-07-15): spawn a PlayerController ScriptComponent
    // and bind <assetRoot>/Scripts/PlayerController.logia. The hot
    // reload watcher activates on first bind (setHotReloadEnabled runs
    // INSIDE bindPlayerScript BEFORE bindAndLoadFromFile, so a
    // user-edit during Play is auto-applied via S3.7b pollAndApplyReloads).
    spawnPlayerControllerIfNeeded();
    if (!_playerScriptBound) {
        _playerScriptBound = bindPlayerScript();
    }

    _simulationActive = true;
    return true;
}

void EditorPlayRuntime::enterEdit()
{
    ayt::game::GameLoop::instance().pause();
    clearCharacter();
    clearCube();
    clearPlayerController();
    _simulationActive = false;
}

void EditorPlayRuntime::shutdownEngine()
{
    if (_engineShutdown) {
        return;
    }
    _engineShutdown = true;

    enterEdit();

    if (_engineInitialized) {
        unregisterUpdateListener();
        _engineInitialized = false;
    }

    if (_presentationReady) {
        ayt::game::GameLoop::instance().endPlaySession();
        _presentationReady = false;
    }
}

void EditorPlayRuntime::tick() {
    if (!_simulationActive) {
        return;
    }

    ayt::game::GameLoop::instance().tickOnce();
}

// INT-01 (2026-07-15): PlayerController ScriptComponent spawn +
// <assetRoot>/Scripts/PlayerController.logia binding. Mirrors
// spawnCubeIfNeeded()/clearCube()'s idempotency contract.
void EditorPlayRuntime::spawnPlayerControllerIfNeeded() {
    if (_playerEntity != nullptr) {
        return;
    }
    _playerEntity = ayt::entity::World::instance().createEntity();
    if (_playerEntity == nullptr) {
        return;
    }

    // INT-01 smoke host: Transform + PlayerController only.
    // Do NOT attach MeshComponent here — startPlay already spawned either
    // the procedural cube or an imported character. A second cube mesh on
    // this entity showed up in the viewport as a fixed duplicate beside
    // the rotating `_cubeEntity` (Logia mutates PlayerController::position,
    // not Transform, so the duplicate never moved).
    _playerEntity->addComponent<ayt::entity::Transform>();
    _playerEntity->addComponent<ayt::editor::PlayerController>();
}

void EditorPlayRuntime::clearPlayerController() noexcept {
    if (_playerEntity != nullptr) {
        ayt::entity::World::instance().destroyEntity(_playerEntity);
        _playerEntity = nullptr;
    }
    _playerScriptBound = false;
}

bool EditorPlayRuntime::bindPlayerScript() {
    // Resolve the registered ScriptSubSystem (EditorApp registers it
    // at startup; this is a defensive dynamic-cast in case ordering
    // ever shifts the bind point earlier than the registration point).
    auto* sub = ayt::game::SubSystemRegistry::instance().findSubSystem(
        "ayt.script.runtime");
    if (sub == nullptr) {
        std::fprintf(stderr,
            "[EditorPlayRuntime] ScriptSubSystem not registered yet; "
            "bindPlayerScript no-op (will retry on next startPlay)\n");
        return false;
    }
    auto* scriptSub = dynamic_cast<ayt::script::ScriptSubSystem*>(sub);
    if (scriptSub == nullptr) {
        return false;
    }

    const std::string scriptPath =
        _assetRoot + "Scripts\\PlayerController.logia";
    if (!ayt::io::File::exists(scriptPath)) {
        std::fprintf(stderr,
            "[EditorPlayRuntime] %s not on disk (skip bind; "
            "Editor Player remains inert until a user copies the "
            "file or edits the asset seed path)\n",
            scriptPath.c_str());
        return false;
    }

    auto* pc = _playerEntity
        ? _playerEntity->getComponent<ayt::editor::PlayerController>()
        : nullptr;
    if (pc == nullptr) {
        std::fprintf(stderr,
            "[EditorPlayRuntime] PlayerController component missing "
            "on _playerEntity (spawn order bug)\n");
        return false;
    }

    // INT-01 (2026-07-15): bind BEFORE enabling hot reload. The
    // implementation's `watchScriptPath` always records the path in
    // _hotReload->byPath (regardless of enabled state); `setHotReloadEnabled(true)`
    // backfills OS-level watches for any pre-recorded paths in
    // a single sweep (S3.7b AYSubscriptSubSystem.cpp:160-176).
    // Doing the bind first ensures the just-loaded .logia path is
    // observed by the FileWatcher as soon as reload is enabled —
    // a user edit during Play will fire on the next pollAndApplyReloads.
    std::vector<ayt::script::logia::CompilerError> errors;
    const bool ok = scriptSub->bindAndLoadFromFile(*pc, scriptPath, errors);
    if (!ok) {
        for (const auto& err : errors) {
            std::fprintf(stderr,
                "[EditorPlayRuntime] bindAndLoadFromFile '%s' failed: "
                "%d:%d %s\n",
                scriptPath.c_str(), err.line, err.column,
                err.message.c_str());
        }
        return false;
    }
    scriptSub->setHotReloadEnabled(true);
    return ok;
}

} // namespace ayt::editor
