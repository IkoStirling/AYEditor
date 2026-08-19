#include "AYEditor/EditorPlayRuntime.h"

#include "AYEntity/CharacterEntity.h"  // ED-02 spawnCharacterFromPaths / destroyCharacter
#include "AYEntity.h"
#include "AYEntity/EntityModule.h"
#include "AYGameLoop.h"
#include "AYNetwork/NetworkModule.h"
#include <AYNetwork/INetwork.h>
#include "AYRenderer/RendererSubSystem.h"
#include "AYRenderer/RenderScene.h"
#include "AYScript/ScriptSubSystem.h"
#include "AYShader/ShadercDriver.h"
#include "AYRenderer/ShadowConfig.h"

#include <AYNetwork/Replication/EntityReplicationAdapter.h>

#include <AYEntity/components/AnimationComponent.h>  // ED-03 override target
#include <AYEntity/components/HealthComponent.h>
#include <AYEntity/components/MeshComponent.h>
#include <AYEntity/components/NetworkComponent.h>
#include <AYEntity/components/ScriptComponent.h>
#include <AYEntity/components/SkeletonComponent.h>   // ED-03 override target
#include <AYEntity/components/TransformComponent.h>

#include "AYEditor/EditorPlayerController.h"  // INT-01 sample script host

#include "AYResource/assetsImpl/Material.h"
#include "AYResource/assetsImpl/Mesh.h"
#include "AYResource/assetsImpl/Texture.h"
#include "AYResource/AssetPath.h"

#include "AYIO/Env.h"
#include "AYIO/File.h"
#include "AYMath/MathTransform.h"
#include "AYMath/MathTypes.h"
#include "AYMath/MathDefs.h"

#include "AYScene.h"           // v0.4 PR-1: sm->play()->world() in resolvePlayWorld
#include "AYScene/SceneManager.h"    // v0.4 PR-1: sm->beginPlay/endPlay in startPlay/enterEdit
#include "AYApplication.h"     // v0.4 PR-1: currentEngineHost() in resolvePlayWorld

#include <AYScript/logia/CompilerError.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

constexpr uint32_t kEditorCubeNetId = 1;

uint16_t resolveEditorPlayNetworkPort()
{
    if (const std::string env = ayt::io::env::get("AY_NETWORK_PORT").value_or(""); !env.empty()) {
        const int parsed = std::atoi(env.c_str());
        if (parsed > 0 && parsed <= 65535) {
            return static_cast<uint16_t>(parsed);
        }
    }
    return 7777;
}

bool editorRegisterReplication(ayt::entity::Entity* entity, uint32_t netId,
                               ayt::entity::IComponent* dataComponent,
                               const char* componentTypeName)
{
    auto* net = ayt::net::findRegisteredNetworkSubSystem();
    if (net == nullptr || entity == nullptr || dataComponent == nullptr
        || componentTypeName == nullptr) {
        return false;
    }
    if (std::strcmp(componentTypeName, "HealthComponent") == 0) {
        return ayt::net::EntityReplicationAdapter::registerEntityComponent<
            ayt::entity::HealthComponent>(*net->getReplicationManager(), entity, netId);
    }
    return false;
}

void editorUnregisterReplication(uint32_t netId)
{
    auto* net = ayt::net::findRegisteredNetworkSubSystem();
    if (net == nullptr || netId == 0) {
        return;
    }
    ayt::net::EntityReplicationAdapter::unregisterEntityComponent(
        *net->getReplicationManager(), netId);
}

void installNetworkComponentBindingsOnce()
{
    static bool installed = false;
    if (installed) {
        return;
    }
    installed = true;
    ayt::entity::NetworkComponent::setReplicationCallbacks(
        editorRegisterReplication, editorUnregisterReplication);
}

void startEditorNetworkListenServer()
{
    auto* net = ayt::net::findRegisteredNetworkSubSystem();
    if (net == nullptr) {
        return;
    }
    if (net->getMode() != ayt::net::ConnectionMode::Disconnected) {
        return;
    }
    const uint16_t port = resolveEditorPlayNetworkPort();
    net->listen(port);
    std::fprintf(stderr,
        "[EditorPlayRuntime] Network listen-server on port %u (AY_NETWORK_PORT to override)\n",
        static_cast<unsigned>(port));
}

void stopEditorNetworkListenServer()
{
    auto* net = ayt::net::findRegisteredNetworkSubSystem();
    if (net == nullptr) {
        return;
    }
    if (net->getMode() == ayt::net::ConnectionMode::Disconnected) {
        return;
    }
    net->disconnect();
    std::fprintf(stderr, "[EditorPlayRuntime] Network disconnected\n");
}

void wireCubeNetworkReplication(ayt::entity::Entity* cubeEntity)
{
    if (cubeEntity == nullptr) {
        return;
    }
    installNetworkComponentBindingsOnce();
    if (ayt::net::findRegisteredNetworkSubSystem() == nullptr) {
        return;
    }

    auto* netComp = cubeEntity->getComponent<ayt::entity::NetworkComponent>();
    if (netComp == nullptr) {
        netComp = cubeEntity->addComponent<ayt::entity::NetworkComponent>();
    }
    if (netComp == nullptr || netComp->isReplicationBound()) {
        return;
    }

    netComp->setNetId(kEditorCubeNetId);
    netComp->setOwner(true);

    auto* health = cubeEntity->getComponent<ayt::entity::HealthComponent>();
    if (health == nullptr) {
        health = cubeEntity->addComponent<ayt::entity::HealthComponent>();
    }
    if (health == nullptr) {
        return;
    }
    health->setHp(100);
    netComp->setReplicatedComponent(health, "HealthComponent");
    if (!netComp->bindReplication()) {
        std::fprintf(stderr,
            "[EditorPlayRuntime] cube NetworkComponent bindReplication failed\n");
    } else {
        std::fprintf(stderr,
            "[EditorPlayRuntime] cube replication bound netId=%u (EntitySpawn broadcast)\n",
            kEditorCubeNetId);
    }
}

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

struct ayt::editor::EditorPlayRuntime::SceneLightsStorage {
    ayt::render::SceneLights lights;
};

struct ayt::editor::EditorPlayRuntime::SkySourceStorage {
    ayt::render::SkySource sky;
};

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

    const std::string shadercPath =
        resolveExistingPath(AY_SHADER_SHADERC_HINT, nullptr, 0);
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

void EditorPlayRuntime::startEditorNetworkClient()
{
    auto* net = ayt::net::findRegisteredNetworkSubSystem();
    if (net == nullptr) {
        return;
    }
    if (net->getMode() != ayt::net::ConnectionMode::Disconnected) {
        return;
    }
    const uint16_t port = resolveEditorPlayNetworkPort();
    net->connect(_netConnectHost.c_str(), port);
    std::fprintf(stderr,
        "[EditorPlayRuntime] Network client connecting to %s:%u "
        "(AY_NETWORK_PORT / --net-host to override)\n",
        _netConnectHost.c_str(), static_cast<unsigned>(port));
}

void EditorPlayRuntime::installServerReplicationLateJoinHandler()
{
    if (_serverLateJoinHandlerInstalled || _netPlayRole != NetPlayRole::Server) {
        return;
    }
    auto* net = ayt::net::findRegisteredNetworkSubSystem();
    if (net == nullptr) {
        return;
    }
    _serverLateJoinHandlerInstalled = true;
    net->onConnectionChange([this](ayt::net::NetConnection* conn, bool connected,
                                   ayt::net::DisconnectReason /*reason*/) {
        if (!connected || _netPlayRole != NetPlayRole::Server) {
            return;
        }
        rebroadcastServerReplicationSpawns(conn);
        // Retry next frame in case GNS is not yet send-ready on this conn.
        _pendingLateJoinConn = conn;
    });
}

void EditorPlayRuntime::installClientReplicationConnectHandler()
{
    if (_clientConnectHandlerInstalled || _netPlayRole != NetPlayRole::Client) {
        return;
    }
    auto* net = ayt::net::findRegisteredNetworkSubSystem();
    if (net == nullptr) {
        return;
    }
    _clientConnectHandlerInstalled = true;
    net->onConnectionChange([this](ayt::net::NetConnection* /*conn*/, bool connected,
                                   ayt::net::DisconnectReason /*reason*/) {
        if (!connected || _netPlayRole != NetPlayRole::Client) {
            return;
        }
        std::fprintf(stderr,
            "[EditorPlayRuntime] net client connected — polling spawn announcements\n");
        pollClientNetworkReplication();
    });
}

void EditorPlayRuntime::rebroadcastServerReplicationSpawns(ayt::net::NetConnection* lateJoiner)
{
    if (_cubeEntity == nullptr) {
        std::fprintf(stderr,
            "[EditorPlayRuntime] rebroadcast skipped: no cube entity (enter Play on server first)\n");
        return;
    }
    auto* netComp = _cubeEntity->getComponent<ayt::entity::NetworkComponent>();
    auto* health = _cubeEntity->getComponent<ayt::entity::HealthComponent>();
    if (netComp == nullptr || health == nullptr) {
        std::fprintf(stderr,
            "[EditorPlayRuntime] rebroadcast skipped: cube missing Network/Health component\n");
        return;
    }
    if (!netComp->isReplicationBound()) {
        std::fprintf(stderr,
            "[EditorPlayRuntime] rebroadcast skipped: cube replication not bound "
            "(bindReplication failed at Play start?)\n");
        return;
    }

    auto* net = ayt::net::findRegisteredNetworkSubSystem();
    if (net == nullptr) {
        return;
    }
    ayt::net::ReplicationManager* mgr = net->getReplicationManager();
    if (mgr == nullptr) {
        return;
    }

    const uint32_t netId = netComp->getNetId();
    if (mgr->rebroadcastEntitySpawn(netId, lateJoiner)) {
        std::fprintf(stderr,
            "[EditorPlayRuntime] rebroadcast EntitySpawn netId=%u for late joiner\n",
            netId);
    } else {
        std::fprintf(stderr,
            "[EditorPlayRuntime] rebroadcast EntitySpawn failed netId=%u "
            "(no registered replication object?)\n",
            netId);
    }
}

void EditorPlayRuntime::clearClientReplicatedEntities() noexcept
{
    if (ayt::entity::World::instance().isInitialized()) {
        for (const auto& entry : _clientReplicatedEntities) {
            if (entry.second != nullptr) {
                ayt::entity::World::instance().destroyEntity(entry.second);
            }
        }
    }
    _clientReplicatedEntities.clear();
    _clientReplicatedLastHp.clear();
    _cubeEntity = nullptr;
}

ayt::entity::Entity* EditorPlayRuntime::spawnVisualCubeEntity(uint32_t /*netId*/)
{
    // v0.4 PR-1 (G4): spawn 走 resolvePlayWorld()。Server 路径走
    // sm->play()->world()；client 路径走 World::instance()（决策 4a）。
    ayt::entity::Entity* entity = resolvePlayWorld()->createEntity();
    if (entity == nullptr) {
        return nullptr;
    }

    auto* cubeXf = entity->addComponent<ayt::entity::Transform>();
    cubeXf->position = ayt::math::FVector3(0.0f, 0.85f, 0.0f);
    auto* mesh = entity->addComponent<ayt::entity::MeshComponent>();
    mesh->meshPath      = _meshPath;
    mesh->materialPath  = _materialPath;
    mesh->castShadow    = true;
    mesh->receiveShadow = false;
    return entity;
}

bool EditorPlayRuntime::trySpawnClientReplicatedEntity(uint32_t netId, uint16_t typeHash)
{
    (void)typeHash;
    if (_clientReplicatedEntities.find(netId) != _clientReplicatedEntities.end()) {
        return false;
    }

    installNetworkComponentBindingsOnce();
    auto* net = ayt::net::findRegisteredNetworkSubSystem();
    if (net == nullptr) {
        return false;
    }

    ayt::entity::Entity* entity = spawnVisualCubeEntity(netId);
    if (entity == nullptr) {
        return false;
    }

    auto* netComp = entity->addComponent<ayt::entity::NetworkComponent>();
    netComp->setNetId(netId);
    ayt::entity::HealthComponent* health =
        entity->addComponent<ayt::entity::HealthComponent>();
    if (health == nullptr) {
        ayt::entity::World::instance().destroyEntity(entity);
        return false;
    }

    if (!ayt::net::EntityReplicationAdapter::registerEntityComponent<
            ayt::entity::HealthComponent>(*net->getReplicationManager(), entity, netId)) {
        std::fprintf(stderr,
            "[EditorPlayRuntime] client registerEntityComponent failed netId=%u\n", netId);
        ayt::entity::World::instance().destroyEntity(entity);
        return false;
    }

    _clientReplicatedEntities[netId] = entity;
    _clientReplicatedLastHp[netId]   = health->getHp();
    if (netId == kEditorCubeNetId) {
        _cubeEntity = entity;
    }

    std::fprintf(stderr,
        "[EditorPlayRuntime] client spawned replicated cube netId=%u hp=%d\n",
        netId, health->getHp());
    return true;
}

void EditorPlayRuntime::pollClientNetworkReplication()
{
    if (_netPlayRole != NetPlayRole::Client || !_simulationActive) {
        return;
    }

    auto* net = ayt::net::findRegisteredNetworkSubSystem();
    if (net == nullptr || !net->isConnected()) {
        return;
    }

    ayt::net::ReplicationManager* mgr = net->getReplicationManager();
    if (mgr == nullptr) {
        return;
    }

    for (uint32_t netId = 1; netId <= 32; ++netId) {
        if (_clientReplicatedEntities.find(netId) != _clientReplicatedEntities.end()) {
            continue;
        }
        uint16_t typeHash = 0;
        if (!mgr->peekSpawnAnnouncement(netId, typeHash)) {
            continue;
        }
        trySpawnClientReplicatedEntity(netId, typeHash);
    }

    if (!_clientLoggedWaitingSpawn && _clientReplicatedEntities.empty()
        && mgr->spawnAnnouncementCount() == 0) {
        _clientLoggedWaitingSpawn = true;
        std::fprintf(stderr,
            "[EditorPlayRuntime] net client connected — waiting for EntitySpawn "
            "from server (enter Play on host first, then --net-client)\n");
    }

    std::vector<uint32_t> despawned;
    for (const auto& entry : _clientReplicatedEntities) {
        const uint32_t netId = entry.first;
        ayt::entity::Entity* entity = entry.second;
        if (mgr->findObject(netId) != nullptr) {
            if (entity != nullptr) {
                if (auto* health = entity->getComponent<ayt::entity::HealthComponent>()) {
                    const int32_t hp = health->getHp();
                    auto hpIt = _clientReplicatedLastHp.find(netId);
                    if (hpIt == _clientReplicatedLastHp.end() || hpIt->second != hp) {
                        _clientReplicatedLastHp[netId] = hp;
                        std::fprintf(stderr,
                            "[EditorPlayRuntime] client netId=%u hp=%d\n", netId, hp);
                    }
                }
            }
            continue;
        }
        despawned.push_back(netId);
    }

    for (uint32_t netId : despawned) {
        auto it = _clientReplicatedEntities.find(netId);
        if (it == _clientReplicatedEntities.end()) {
            continue;
        }
        if (it->second != nullptr && ayt::entity::World::instance().isInitialized()) {
            ayt::entity::World::instance().destroyEntity(it->second);
        }
        if (_cubeEntity == it->second) {
            _cubeEntity = nullptr;
        }
        _clientReplicatedEntities.erase(it);
        _clientReplicatedLastHp.erase(netId);
        std::fprintf(stderr,
            "[EditorPlayRuntime] client despawned replicated entity netId=%u\n",
            netId);
    }
}

bool EditorPlayRuntime::ensureAssets() {
    _cacheRoot = resolvePersistentCacheRoot();
    _assetRoot = _cacheRoot + "assets\\";
    // So mesh→material / material→texture virtual deps resolve under
    // assets/, not under the referring file's directory (meshes/).
    ayt::resource::setAssetRoot(_assetRoot);
    _meshPath             = _assetRoot + "cube.aymesh";
    _materialPath         = _assetRoot + "cube_shadow.aymat";
    _groundMeshPath       = _assetRoot + "cube.aymesh";
    _groundMaterialPath   = _assetRoot + "ground_shadow.aymat";
    _glassMeshPath        = _assetRoot + "cube.aymesh";
    _glassMaterialPath    = _assetRoot + "glass_shadow.aymat";

    if (!ensureAssetDirectory(_cacheRoot) || !ensureAssetDirectory(_assetRoot)) {
        return false;
    }

    const std::string shaderPath       = _assetRoot + "simple_lit.phoskia";
    const std::string shadowShaderPath = _assetRoot + "simple_lit_shadow.phoskia";

    // Always refresh Phoskia sources so shader fixes land without wiping cache.
    if (!writeText(shaderPath, kSimpleLitPhoskia) ||
        !writeText(shadowShaderPath, ayt::render::kSimpleLitShadowPhoskiaSource)) {
        return false;
    }

    // Dump hand-authored bgfx .sc (Editor isolation path) for inspection.
    const std::string scDir = _assetRoot + "bgfx_sc\\";
    ensureAssetDirectory(scDir);
    writeText(scDir + "simple_lit_shadow_varying.def.sc",
              ayt::render::kSimpleLitShadowVaryingSc);
    writeText(scDir + "simple_lit_shadow_vs.sc",
              ayt::render::kSimpleLitShadowVertexSc);
    writeText(scDir + "simple_lit_shadow_fs.sc",
              ayt::render::kSimpleLitShadowFragmentSc);

    if (auto* rendererSub = ayt::render::RendererSubSystem::findRegistered()) {
        const uint32_t reloaded =
            rendererSub->renderer().reloadMaterialsForShaderFile(shadowShaderPath);
        if (reloaded > 0) {
            std::fprintf(stderr,
                         "[EditorPlayRuntime] reloaded %u material(s) for '%s'\n",
                         reloaded,
                         shadowShaderPath.c_str());
        }
    }

    // Refresh glass .aymat every ensureAssets so Alpha acceptance sticks
    // even when the rest of the cache was already marked ready.
    {
        const ayt::resource::Float32 glassColor[4] = {0.25f, 0.90f, 1.0f, 0.45f};
        ayt::resource::Material material;
        material.setShader("simple_lit_shadow.phoskia");
        material.setFloat4("baseColor", glassColor);
        material.setTexture("albedoMap", "cube_albedo.aytex");
        std::vector<ayt::resource::UInt8> matBinary;
        if (material.saveToBinary(matBinary)) {
            writeBytes(_glassMaterialPath, matBinary.data(), matBinary.size());
        }
        ensureGlassMaterialAlpha();
    }

    // Sky equirect: seed every ensureAssets so Deferred Play finds it
    // even when the rest of the cache was already marked ready (cwd
    // relative lookup from the exe dir fails otherwise).
    if (!seedSkyBoxPng()) {
        std::fprintf(stderr,
            "[EditorPlayRuntime] skyBox.png seed skipped "
            "(source not found under AYRenderer/demo or AliyatRenderer)\n");
    }

    if (_assetsReady) {
        return true;
    }

    const std::string texturePath      = _assetRoot + "albedo.aytex";

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

    auto bakeMaterial = [&](const std::string& path,
                            const char* shaderRel,
                            const ayt::resource::Float32* baseColor,
                            const char* albedoRelPath) -> bool {
        ayt::resource::Material material;
        material.setShader(shaderRel);
        material.setFloat4("baseColor", baseColor);
        material.setTexture("albedoMap", albedoRelPath);
        std::vector<ayt::resource::UInt8> matBinary;
        if (!material.saveToBinary(matBinary)) {
            return false;
        }
        return writeBytes(path, matBinary.data(), matBinary.size());
    };

    // Solid albedos (avoid checkerboard black cells swallowing lit/shadow).
    const std::string cubeTexPath   = _assetRoot + "cube_albedo.aytex";
    const std::string groundTexPath = _assetRoot + "ground_albedo.aytex";
    {
        ayt::resource::Texture cubeTex;
        cubeTex.createSolidColor(4, 4, 255, 140, 40);
        std::vector<ayt::resource::UInt8> texBinary;
        if (!cubeTex.saveToBinary(texBinary) ||
            !writeBytes(cubeTexPath, texBinary.data(), texBinary.size())) {
            return false;
        }
    }
    {
        ayt::resource::Texture groundTex;
        groundTex.createSolidColor(4, 4, 150, 155, 140);
        std::vector<ayt::resource::UInt8> texBinary;
        if (!groundTex.saveToBinary(texBinary) ||
            !writeBytes(groundTexPath, texBinary.data(), texBinary.size())) {
            return false;
        }
    }

    const ayt::resource::Float32 cubeColor[4]   = {1.0f, 0.45f, 0.12f, 1.0f};
    const ayt::resource::Float32 groundColor[4] = {0.55f, 0.58f, 0.62f, 1.0f};
    // Cyan glass: a=0.35 so TransparentPass alpha blend is visible over
    // the opaque cube / ground (shader returns albedo.a).
    const ayt::resource::Float32 glassColor[4]  = {0.25f, 0.90f, 1.0f, 0.45f};
    // Both receive shadows; cube also casts via ShadowPass (all meshes).
    // baseColor carries chroma so a missing/wrong albedo sampler still
    // shows tint instead of R32F-depth grayscale.
    if (!bakeMaterial(_materialPath, "simple_lit_shadow.phoskia", cubeColor,
                      "cube_albedo.aytex") ||
        !bakeMaterial(_groundMaterialPath, "simple_lit_shadow.phoskia", groundColor,
                      "ground_albedo.aytex") ||
        !bakeMaterial(_glassMaterialPath, "simple_lit_shadow.phoskia", glassColor,
                      "cube_albedo.aytex")) {
        return false;
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

bool EditorPlayRuntime::seedSkyBoxPng() {
    const std::string destPath = _assetRoot + "skyBox.png";
    if (fileExists(destPath)) {
        return true;
    }

    // Prefer exe-relative walks: Demo cwd is often the build tree, not
    // the repo root, so plain "AYRuntime\\..." relative paths miss.
    std::vector<std::string> candidates;
    char modulePath[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        std::string exeDir(modulePath, modulePath + len);
        const size_t slash = exeDir.find_last_of("\\/");
        if (slash != std::string::npos) {
            exeDir.resize(slash + 1);
        }
        // From .../out/build/x64-Debug/AYRuntime/AYEditor/ walk up to
        // Projects/ then into AYRenderer demo + AliyatRenderer textures.
        std::string walk = exeDir;
        for (int up = 0; up < 8; ++up) {
            candidates.push_back(walk + "AYRuntime\\AYRenderer\\demo\\assets\\skyBox.png");
            candidates.push_back(walk + "AYRenderer\\demo\\assets\\skyBox.png");
            candidates.push_back(walk + "AliyatRenderer\\assets\\core\\textures\\skyBox.png");
            candidates.push_back(walk + "assets\\core\\textures\\skyBox.png");
            if (walk.size() < 2) {
                break;
            }
            const size_t cut = walk.find_last_of("\\/", walk.size() - 2);
            if (cut == std::string::npos) {
                break;
            }
            walk.resize(cut + 1);
        }
    }

    static const char* kCwdCandidates[] = {
        "AYRuntime\\AYRenderer\\demo\\assets\\skyBox.png",
        "..\\..\\..\\..\\AYRuntime\\AYRenderer\\demo\\assets\\skyBox.png",
        "..\\..\\..\\..\\AliyatRenderer\\assets\\core\\textures\\skyBox.png",
        "AliyatRenderer\\assets\\core\\textures\\skyBox.png",
    };
    for (const char* c : kCwdCandidates) {
        candidates.emplace_back(c);
    }

    std::string sourcePath;
    for (const std::string& c : candidates) {
        if (fileExists(c)) {
            sourcePath = c;
            break;
        }
    }
    if (sourcePath.empty()) {
        return false;
    }

    ayt::io::File src(sourcePath, ayt::io::File::Mode::BinaryRead);
    if (!src.isOpen()) {
        return false;
    }
    std::string bytes;
    bytes.resize(static_cast<size_t>(src.size()));
    if (bytes.empty()) {
        return false;
    }
    if (src.read(bytes.data(), bytes.size()) != static_cast<int64_t>(bytes.size())) {
        return false;
    }
    const bool ok = writeBytes(destPath, bytes.data(), bytes.size());
    if (ok) {
        std::fprintf(stderr,
            "[EditorPlayRuntime] skyBox.png seeded from %s → %s\n",
            sourcePath.c_str(), destPath.c_str());
    }
    return ok;
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
    installNetworkComponentBindingsOnce();
    ayt::net::registerNetworkSubSystem();
    ayt::render::RendererSubSystem::registerSubSystem();
    if (_netPlayRole == NetPlayRole::Server) {
        installServerReplicationLateJoinHandler();
    } else {
        installClientReplicationConnectHandler();
    }

    if (!loop.isPlaySessionActive() && !loop.prepareHostedSession()) {
        std::fprintf(stderr, "[EditorPlayRuntime] prepareHostedSession failed\n");
        return false;
    }

    // EditorApp drives presentation through renderCompositeFrame(), which
    // combines the Game View packet with editor chrome in one bgfx frame.
    loop.setPresentationOwnership(ayt::game::PresentationOwnership::ExternalHost);

    loop.pause();
    applyEditorRenderPipeline();
    _presentationReady = true;
    return true;
}

void EditorPlayRuntime::applyEditorRenderPipeline()
{
    if (_pipelineConfigured) {
        return;
    }
    auto* rendererSub = ayt::render::RendererSubSystem::findRegistered();
    if (rendererSub == nullptr || !rendererSub->isReady()) {
        return;
    }
    // B6 Editor host: Deferred is the default (Skybox + GBuffer + multi-light
    // + Transparent). Forward is opt-out via AY_DEFERRED=0 for hosts that
    // need the simpler Shadow→FO→Transparent path.
    const std::string deferredEnv = ayt::io::env::get("AY_DEFERRED").value_or("");
    const bool useDeferred =
        deferredEnv.empty()
        || deferredEnv != "0";

    if (useDeferred) {
        rendererSub->renderer().configurePipeline(
            ayt::render::RenderPipelineDesc::makeDeferred());
        // §P5.5 A — multi-light DataSource (host-owned). LightingPass
        // borrows the pointer each frame. Light[0] is the key that
        // shares ShadowPass (must match setDirectionalLight / shadow
        // matrix lightDirection). Fill/rim stay unshadowed.
        if (!_sceneLightsStorage) {
            _sceneLightsStorage = std::make_unique<SceneLightsStorage>();
        }
        ayt::render::SceneLights& lights = _sceneLightsStorage->lights;
        lights.count = 0;

        // Key — warm sun; matches ShadowPass / setDirectionalLight.
        const ayt::math::FVector3 keyDir(0.35f, -0.85f, -0.40f);
        const ayt::math::FVector3 keyColor(1.35f, 1.28f, 1.15f);
        lights.add(ayt::render::Light::directional(keyDir, keyColor));

        // Dim cool fill (directional) — keep umbra readable.
        lights.add(ayt::render::Light::directional(
            ayt::math::FVector3(-0.60f, -0.30f, 0.50f),
            ayt::math::FVector3(0.12f, 0.22f, 0.40f)));

        // §P5.5 B — warm local point (slot 2). Not shadowed.
        lights.add(ayt::render::Light::point(
            ayt::math::FVector3(1.2f, 1.6f, 0.4f),
            /*range=*/5.0f,
            /*intensity=*/2.2f,
            ayt::math::FVector3(1.0f, 0.55f, 0.25f)));

        // §P5.5 B — soft spot from above-right onto the cube/ground.
        lights.add(ayt::render::Light::spot(
            ayt::math::FVector3(-0.8f, 2.4f, 1.2f),
            ayt::math::FVector3(0.25f, -1.0f, -0.35f),
            /*range=*/7.0f,
            /*intensity=*/2.8f,
            /*coneCosInner=*/0.92f,
            /*coneCosOuter=*/0.75f,
            ayt::math::FVector3(0.55f, 0.70f, 1.0f)));

        rendererSub->renderer().setSceneLights(&lights);
        rendererSub->renderer().setDirectionalLight(keyDir, keyColor);

        // Sky + IBL: keep equirect backdrop; also upload a small
        // procedural cube for LightingPass ambientCube (envCube).
        if (!_skySourceStorage) {
            _skySourceStorage = std::make_unique<SkySourceStorage>();
        }
        (void)seedSkyBoxPng();
        const std::string skyPath = _assetRoot + "skyBox.png";
        ayt::render::TextureHandle equirectTex{};
        if (fileExists(skyPath)) {
            equirectTex = rendererSub->renderer().loadTexture(skyPath);
        }

        // 16³ RGBA8 cube — sky-ish faces for IBL diffuse (not HDR).
        constexpr uint32_t kCubeSize = 16;
        std::vector<uint8_t> cubeFaces(kCubeSize * kCubeSize * 4u * 6u);
        auto fillFace = [&](uint32_t face, uint8_t r, uint8_t g, uint8_t b) {
            uint8_t* dst = cubeFaces.data()
                + face * kCubeSize * kCubeSize * 4u;
            for (uint32_t i = 0; i < kCubeSize * kCubeSize; ++i) {
                dst[i * 4 + 0] = r;
                dst[i * 4 + 1] = g;
                dst[i * 4 + 2] = b;
                dst[i * 4 + 3] = 255;
            }
        };
        // bgfx face order: +X,-X,+Y,-Y,+Z,-Z
        fillFace(0, 180, 140, 110); // +X warm
        fillFace(1,  90, 120, 170); // -X cool
        fillFace(2, 160, 190, 230); // +Y sky
        fillFace(3,  70,  75,  70); // -Y ground
        fillFace(4, 140, 160, 190); // +Z
        fillFace(5, 140, 160, 190); // -Z
        const ayt::render::TextureHandle cubeTex =
            rendererSub->renderer().createCubeTextureFromRgba8(
                kCubeSize, cubeFaces.data(), "editor_ibl_ambient_cube_v1");

        // Sky backdrop = equirect panorama; IBL ambient = procedural
        // cube via setSkySourceCube (kind stays Equirect so SkyboxPass
        // does not replace the panorama with solid cube faces).
        if (equirectTex.isValid()) {
            _skySourceStorage->sky.kind = ayt::render::SkySourceKind::Equirect;
            _skySourceStorage->sky.equirect = equirectTex;
            _skySourceStorage->sky.cubeMap = cubeTex; // introspection only
            rendererSub->renderer().setSkySource(&_skySourceStorage->sky);
            if (cubeTex.isValid()) {
                rendererSub->renderer().setSkySourceCube(cubeTex);
                std::fprintf(stderr,
                    "[EditorPlayRuntime] Skybox equirect + IBL envCube ready "
                    "(equirect=%s, cube=16^3 procedural)\n",
                    skyPath.c_str());
            } else {
                rendererSub->renderer().setSkySourceCube({});
                std::fprintf(stderr,
                    "[EditorPlayRuntime] Skybox equirect loaded (no IBL cube): %s\n",
                    skyPath.c_str());
            }
        } else if (cubeTex.isValid()) {
            // No panorama asset — fall back to CubeMap backdrop.
            _skySourceStorage->sky.kind = ayt::render::SkySourceKind::CubeMap;
            _skySourceStorage->sky.cubeMap = cubeTex;
            _skySourceStorage->sky.equirect = {};
            rendererSub->renderer().setSkySource(&_skySourceStorage->sky);
            rendererSub->renderer().setSkySourceCube(cubeTex);
            std::fprintf(stderr,
                "[EditorPlayRuntime] Skybox CubeMap fallback + IBL "
                "(no equirect asset)\n");
        } else {
            rendererSub->renderer().setSkySource(nullptr);
            rendererSub->renderer().setSkySourceCube({});
            std::fprintf(stderr,
                "[EditorPlayRuntime] Skybox/IBL skipped (no equirect, no cube)\n");
        }
    } else {
        rendererSub->renderer().configurePipeline(
            ayt::render::RenderPipelineDesc::makeForwardWithShadows());
        rendererSub->renderer().setSceneLights(nullptr);
        rendererSub->renderer().setSkySource(nullptr);
        rendererSub->renderer().setSkySourceCube({});
        _sceneLightsStorage.reset();
        _skySourceStorage.reset();
    }
    // PostProcess: ACES tonemap + display gamma (ripple removed).
    rendererSub->renderer().setPostProcessGamma(2.2f);
    rendererSub->renderer().setPostProcessExposure(1.0f);
    rendererSub->renderer().setPostProcessTonemapMode(
        ayt::render::Renderer::TonemapMode::ACES);
    // §P5.5 D acceptance — IBL diffuse slightly raised for visible env tint.
    rendererSub->renderer().setAmbientStrength(0.85f);
    ensureGlassMaterialAlpha();
    _pipelineConfigured = true;
    if (useDeferred) {
        const unsigned n = _sceneLightsStorage
                               ? static_cast<unsigned>(_sceneLightsStorage->lights.count)
                               : 0u;
        std::fprintf(stderr,
            "[EditorPlayRuntime] render pipeline: Deferred "
            "(Shadow → Skybox → GBuffer → Lighting → Transparent → PP → UI) "
            "default; AY_DEFERRED=%s; P5.5 sceneLights=%u "
            "(keyDir+fillDir+point+spot; key-only shadow; IBL cube)\n",
            deferredEnv.empty() ? "(unset)" : deferredEnv.c_str(), n);
    } else {
        std::fprintf(stderr,
            "[EditorPlayRuntime] render pipeline: Forward "
            "(Shadow → FO → Transparent → PP → UI) via AY_DEFERRED=0; "
            "unset AY_DEFERRED (or =1) for Deferred + skybox + multi-light\n");
    }
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
        applyEditorRenderPipeline();
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
        if (_netPlayRole == NetPlayRole::Client) {
            pollClientNetworkReplication();
        } else {
            if (_pendingLateJoinConn != nullptr) {
                ayt::net::NetConnection* conn = _pendingLateJoinConn;
                _pendingLateJoinConn = nullptr;
                rebroadcastServerReplicationSpawns(conn);
            }
            if (_simulationActive) {
                spawnCubeIfNeeded();
            }
        }

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

    _cubeEntity = spawnVisualCubeEntity(kEditorCubeNetId);
    if (_cubeEntity == nullptr) {
        return;
    }

    auto* cubeXf = _cubeEntity->getComponent<ayt::entity::Transform>();
    const ayt::math::Float4x4 world =
        ayt::math::Transform::getMatrix(cubeXf->position, cubeXf->rotation, cubeXf->scale);
    std::fprintf(stderr,
        "[EditorPlayRuntime] spawn cube pos=(%.2f, %.2f, %.2f) mat=%s "
        "ayMathTranslation=(%.3f, %.3f, %.3f)\n",
        cubeXf->position.x, cubeXf->position.y, cubeXf->position.z,
        _materialPath.c_str(),
        world.row[0].w, world.row[1].w, world.row[2].w);
}

void EditorPlayRuntime::spawnGroundIfNeeded() {
    if (_groundEntity != nullptr) {
        return;
    }

    // v0.4 PR-1 (G4): spawn 走 resolvePlayWorld()。
    _groundEntity = resolvePlayWorld()->createEntity();
    if (_groundEntity == nullptr) {
        return;
    }

    auto* transform = _groundEntity->addComponent<ayt::entity::Transform>();
    // Thin shadow receiver; top face at y=0.
    transform->position = ayt::math::FVector3(0.0f, -0.05f, 0.0f);
    transform->scale    = ayt::math::FVector3(8.0f, 0.1f, 8.0f);

    auto* mesh = _groundEntity->addComponent<ayt::entity::MeshComponent>();
    mesh->meshPath        = _groundMeshPath;
    mesh->materialPath    = _groundMaterialPath;
    mesh->castShadow      = false;
    mesh->receiveShadow   = true;
}

void EditorPlayRuntime::spawnGlassIfNeeded() {
    if (_glassEntity != nullptr) {
        return;
    }

    // v0.4 PR-1 (G4): spawn 走 resolvePlayWorld()。
    _glassEntity = resolvePlayWorld()->createEntity();
    if (_glassEntity == nullptr) {
        return;
    }

    auto* transform = _glassEntity->addComponent<ayt::entity::Transform>();
    // In front of the orange cube toward the default camera (4,3,5).
    transform->position = ayt::math::FVector3(1.4f, 1.0f, 1.2f);
    transform->scale    = ayt::math::FVector3(1.1f, 1.1f, 1.1f);

    auto* mesh = _glassEntity->addComponent<ayt::entity::MeshComponent>();
    mesh->meshPath        = _glassMeshPath;
    mesh->materialPath    = _glassMaterialPath;
    mesh->castShadow      = false;
    mesh->receiveShadow   = true;
    mesh->alphaBlend      = true;

    ensureGlassMaterialAlpha();
    std::fprintf(stderr,
        "[EditorPlayRuntime] spawn glass (Transparent Alpha) pos=(%.2f, %.2f, %.2f) mat=%s\n",
        transform->position.x, transform->position.y, transform->position.z,
        _glassMaterialPath.c_str());
}

void EditorPlayRuntime::ensureGlassMaterialAlpha()
{
    if (_glassMaterialPath.empty()) {
        return;
    }
    auto* rendererSub = ayt::render::RendererSubSystem::findRegistered();
    if (rendererSub == nullptr || !rendererSub->isReady()) {
        return;
    }
    const ayt::render::MaterialHandle mat =
        rendererSub->renderer().loadMaterial(_glassMaterialPath);
    if (!mat.isValid()) {
        return;
    }
    rendererSub->renderer().setMaterialBlendMode(
        mat, ayt::render::BlendMode::Alpha);
}

namespace {

// World::shutdown() deletes entities but leaves EditorPlayRuntime's raw
// pointers dangling. Only destroy while the world still owns them.
// v0.4 PR-1: 接收 World& 参数（由 EditorPlayRuntime::resolvePlayWorld()
// 提供），不再 hardcode World::instance()，收口 LM-1。
void releaseOwnedEntity(ayt::entity::Entity*& ptr,
                       ayt::entity::World& world) noexcept
{
    if (ptr == nullptr) {
        return;
    }
    if (world.isInitialized()) {
        world.destroyEntity(ptr);
    }
    ptr = nullptr;
}

} // namespace

ayt::entity::World* EditorPlayRuntime::resolvePlayWorld() noexcept
{
    // v0.4 PR-1 (design §6, G4): 单一 helper 解析 Play World 源。
    // 优先 host->scenes()->play()->world()（beginPlay 后的 Play Scene World）；
    // fallback 到 World::instance()（SM 不可达 / beginPlay 未调 / net-client）。
    //
    // net-client 路径不调 beginPlay（G9 + 决策 4a），直接走 singleton —
    // 避免 log 噪音（LM-X2）。Server/未指定角色走 SM 路径。
    if (_netPlayRole != NetPlayRole::Client) {
        auto* host = ayt::app::currentEngineHost();
        auto* sm = (host != nullptr) ? host->scenes() : nullptr;
        if (sm != nullptr) {
            if (auto* play = sm->play()) {
                return &play->world();
            }
        }
        std::fprintf(stderr,
            "[EditorPlayRuntime] resolvePlayWorld: SM/play unavailable, "
            "falling back to World::instance()\n");
    }
    return &ayt::entity::World::instance();
}

void EditorPlayRuntime::clearCube() noexcept {
    releaseOwnedEntity(_cubeEntity, *resolvePlayWorld());
}

void EditorPlayRuntime::clearGround() noexcept {
    releaseOwnedEntity(_groundEntity, *resolvePlayWorld());
}

void EditorPlayRuntime::clearGlass() noexcept {
    releaseOwnedEntity(_glassEntity, *resolvePlayWorld());
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
    const bool hasCharacter = trySpawnImportedCharacter();
    spawnCubeIfNeeded();
    if (hasCharacter && _cubeEntity != nullptr) {
        if (auto* xf = _cubeEntity->getComponent<ayt::entity::Transform>()) {
            xf->position = ayt::math::FVector3(-2.5f, 0.85f, 0.0f);
        }
    }
    spawnGroundIfNeeded();
    if (_simulationActive) {
        wireCubeNetworkReplication(_cubeEntity);
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
        std::fprintf(stderr,
            "[EditorPlayRuntime] no valid imported character "
            "(mesh='%s' skel='%s') — cube fallback\n",
            _importedCharacter.meshPath.c_str(),
            _importedCharacter.skeletonPath.c_str());
        return false;
    }

    auto spawnOne = [&](const std::string& meshPath) -> ayt::entity::Entity* {
        return ayt::entity::spawnCharacterFromPaths(
            meshPath,
            _importedCharacter.materialPath,
            _importedCharacter.skeletonPath,
            _importedCharacter.animationPath);
    };

    _characterEntity = spawnOne(_importedCharacter.meshPath);
    if (_characterEntity == nullptr) {
        std::fprintf(stderr,
            "[EditorPlayRuntime] spawnCharacterFromPaths returned "
            "nullptr (asset load failed) — falling back to cube\n");
        return false;
    }

    // Default 1.0 — Assimp FBX often already lands near meters. MMD cm
    // assets: set AY_EDITOR_CHARACTER_SCALE=0.01. The previous 0.01
    // default made meter-scale meshes an invisible speck next to the cube.
    float characterScale = 1.0f;
    if (const std::string envScale = ayt::io::env::get("AY_EDITOR_CHARACTER_SCALE").value_or(""); !envScale.empty()) {
        const float parsed = static_cast<float>(std::atof(envScale.c_str()));
        if (parsed > 0.0f) {
            characterScale = parsed;
        }
    }
    // Many FBX/PMX sources are Z-up. Engine + freecam are Y-up, so without
    // this the character lies flat on XZ and looks like a washed-out top-down
    // silhouette. Entity rotation keeps skinning in model space correct.
    const ayt::math::FQuaternion zUpToYUp =
        ayt::math::FQuaternion::fromAxisAngle(
            ayt::math::FVector3(1.0f, 0.0f, 0.0f),
            -MATH_PI * 0.5f);
    auto applySpawnXform = [characterScale, &zUpToYUp](ayt::entity::Entity* e) {
        if (e == nullptr) return;
        if (auto* xf = e->getComponent<ayt::entity::Transform>()) {
            xf->scale = ayt::math::FVector3(
                characterScale, characterScale, characterScale);
            xf->rotation = zUpToYUp;
        }
    };
    applySpawnXform(_characterEntity);

    for (const std::string& extraMesh : _importedCharacter.additionalMeshPaths) {
        ayt::entity::Entity* part = spawnOne(extraMesh);
        if (part == nullptr) {
            std::fprintf(stderr,
                "[EditorPlayRuntime] extra mesh spawn failed: %s\n",
                extraMesh.c_str());
            continue;
        }
        applySpawnXform(part);
        _additionalCharacterEntities.push_back(part);
    }

    std::fprintf(stderr,
        "[EditorPlayRuntime] spawned character meshes=%zu scale=%.4f "
        "(anim=%s)\n",
        1u + _additionalCharacterEntities.size(),
        characterScale,
        _importedCharacter.animationPath.empty() ? "bind-pose"
                                                 : "clip");

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
                // Clip present → switch back onto the skinned draw path.
                if (auto* meshComp =
                        _characterEntity->getComponent<ayt::entity::MeshComponent>()) {
                    meshComp->skinned = true;
                    // Force AnimationSystem to (re)bind the player next tick.
                    if (auto* skelComp =
                            _characterEntity->getComponent<ayt::entity::SkeletonComponent>()) {
                        skelComp->loaded = false;
                    }
                }
            }
        }
    }
}

void EditorPlayRuntime::clearCharacter() noexcept {
    // v0.4 PR-1 (G4): isInitialized 检查走 resolvePlayWorld()。
    if (resolvePlayWorld()->isInitialized()) {
        for (ayt::entity::Entity* part : _additionalCharacterEntities) {
            ayt::entity::destroyCharacter(part);
        }
        if (_characterEntity != nullptr) {
            ayt::entity::destroyCharacter(_characterEntity);
        }
    }
    _additionalCharacterEntities.clear();
    _characterEntity = nullptr;
}

bool EditorPlayRuntime::startPlay()
{
    if (!ensureEngineInitialized()) {
        return false;
    }

    // v0.4 PR-1 (design §6, G1): SceneManager beginPlay 兜底。
    //   btn_play → applyMode(Play) → 此函数 → 编辑场景 → tmp 克隆 →
    //   setCurrent(play)。保证后续 spawn 走 sm->play()->world()，
    //   收口 LM-1（World::instance() vs Scene 独立 World 边界）。
    //   net-client 路径跳过（client 不持久化；决策 4a + G9）：
    //   client 只 consume 服务端 EntitySpawn，本地不写 tmp 克隆。
    if (_netPlayRole != NetPlayRole::Client) {
        auto* host = ayt::app::currentEngineHost();
        auto* sm = (host != nullptr) ? host->scenes() : nullptr;
        if (sm == nullptr || !sm->canBeginPlay()) {
            std::fprintf(stderr,
                "[EditorPlayRuntime] startPlay: no SceneManager or cannot "
                "begin play (host=%p sm=%p)\n", (void*)host, (void*)sm);
            return false;
        }
        if (!sm->beginPlay()) {
            std::fprintf(stderr,
                "[EditorPlayRuntime] startPlay: sm->beginPlay() failed\n");
            return false;
        }
        // P0: beginPlay setCurrent(play) → World::instance() redirects to the
        // Play Scene World. Re-run bootstrap so Animation/Render/2D systems
        // land on that World (first bootstrap during ensureEngineInitialized
        // targeted the process fallback).
        ayt::entity::bootstrapModule();
    }

    if (auto* rendererSub = ayt::render::RendererSubSystem::findRegistered()) {
        rendererSub->renderer().resetDebugOverlayStats();
    }

    ayt::game::GameLoop::instance().resume();

    if (_netPlayRole == NetPlayRole::Client) {
        startEditorNetworkClient();
        spawnGroundIfNeeded();
        _simulationActive = true;
        return true;
    }

    startEditorNetworkListenServer();

    // Spawn-policy contract (G3):
    //   1. trySpawnImportedCharacter() when EditorApp populated
    //      sessionDesc.importedCharacter with Mesh+Skeleton.
    //   2. Always keep the procedural opaque cube as a lit reference
    //      (offset aside when a character is present) so a failed /
    //      invisible character cannot be mistaken for "cube removed".
    //   3. Glass (Transparent) + ground still spawn every Play.
    const bool hasCharacter = trySpawnImportedCharacter();
    spawnCubeIfNeeded();
    if (hasCharacter && _cubeEntity != nullptr) {
        if (auto* xf = _cubeEntity->getComponent<ayt::entity::Transform>()) {
            // Keep the opaque cube visible beside the character so the
            // Deferred opaque path can be verified independently of skinning.
            xf->position = ayt::math::FVector3(-2.5f, 0.85f, 0.0f);
        }
    }
    spawnGroundIfNeeded();
    spawnGlassIfNeeded();
    wireCubeNetworkReplication(_cubeEntity);

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
    // v0.4 PR-1 (design §6, G5): SceneManager endPlay 兜底。
    //   **idempotent** — 多次调安全 (btn_stop + applyMode(Edit) +
    //   shutdownEngine 三处都走此函数；SM 内部 _play==nullptr 时 no-op，
    //   AYScene/SceneManager.h:99-103)。清 tmp 克隆 + 销毁 _play Scene →
    //   Scene 析构 → World 析构 → 无残留 (INV-3)。
    auto* host = ayt::app::currentEngineHost();
    auto* sm = (host != nullptr) ? host->scenes() : nullptr;
    if (sm != nullptr) {
        sm->endPlay();
    }

    ayt::game::GameLoop::instance().pause();
    stopEditorNetworkListenServer();
    if (_netPlayRole == NetPlayRole::Client) {
        clearClientReplicatedEntities();
        clearGround();
        _simulationActive = false;
        return;
    }
    clearCharacter();
    clearCube();
    clearGround();
    clearGlass();
    clearPlayerController();
    _simulationActive = false;
}

void EditorPlayRuntime::shutdownEngine()
{
    if (_engineShutdown) {
        return;
    }
    _engineShutdown = true;

    // Drop borrowed SceneLights / SkySource pointers before tearing
    // down Play / destroying this object — Renderer::Impl keeps raw
    // pointers.
    if (auto* rendererSub = ayt::render::RendererSubSystem::findRegistered()) {
        rendererSub->renderer().setSceneLights(nullptr);
        rendererSub->renderer().setSkySource(nullptr);
        rendererSub->renderer().setSkySourceCube({});
    }
    _sceneLightsStorage.reset();
    _skySourceStorage.reset();

    enterEdit();

    if (_engineInitialized) {
        unregisterUpdateListener();
        _engineInitialized = false;
    }

    if (_presentationReady) {
        ayt::game::GameLoop::instance().endHostedSession();
        _presentationReady = false;
        _pipelineConfigured = false;
    }
}

void EditorPlayRuntime::tick() {
    if (!_simulationActive) {
        return;
    }

    ayt::game::GameLoop::instance().tickOnce();
}

void EditorPlayRuntime::tick(const ayt::game::HostedFrameContext& hostFrame) {
    if (!_simulationActive) {
        return;
    }

    ayt::game::GameLoop::instance().tickHostedFrame(hostFrame);
}

// INT-01 (2026-07-15): PlayerController ScriptComponent spawn +
// <assetRoot>/Scripts/PlayerController.logia binding. Mirrors
// spawnCubeIfNeeded()/clearCube()'s idempotency contract.
void EditorPlayRuntime::spawnPlayerControllerIfNeeded() {
    if (_playerEntity != nullptr) {
        return;
    }
    // v0.4 PR-1 (G4): spawn 走 resolvePlayWorld()。
    _playerEntity = resolvePlayWorld()->createEntity();
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
    // v0.4 PR-1 (G4): 走 resolvePlayWorld()。
    releaseOwnedEntity(_playerEntity, *resolvePlayWorld());
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
