#pragma once
// Real EditorSession asset/Dock/field routing with a real GPU backend.
// Scene extraction is an explicit fixture Host, not default GameLoop assembly.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <AYEditor/EditorSession.h>
#include <AYEditor/EditorMaterialDocument.h>
#include <AYEditor/EditorSceneDocument.h>
#include <AYEditor/EditorWorkspace.h>
#include <AYApplication/IEngineHost.h>
#include <AYEntity.h>
#include <AYEntity/EntityModule.h>
#include <AYEntity/EntityRenderIntegrationModule.h>
#include <AYEntity/PerspectiveCameraSelection.h>
#include <AYEntity/components/MeshComponent.h>
#include <AYScene.h>
#include <AYRenderer.h>
#include <AYRenderer/UIRenderBackend.h>
#include <AYResource/AssetPath.h>
#include <AYResource/ResourceManager.h>
#include <AYResource/ResourceBootstrap.h>
#include <AYUI/DockArea.h>
#include <AYUI/DockCard.h>
#include <AYUI/DockTabGroup.h>
#include <AYUI/TextInput.h>
#include <AYUI/UIKeyCode.h>
#include "detail/TextureImageLoader.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

namespace
{
namespace fs = std::filesystem;
using namespace ayt;
void require(bool value, const std::string &message)
{
    if (!value)
        throw std::runtime_error(message);
}
std::string utf8(const fs::path &path)
{
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
struct Fixture
{
    static constexpr uint16_t width = 1800, height = 1000;
    fs::path output, assets;
    app::EngineHostScope hostScope{app::defaultEngineHost()};
    HWND window = nullptr;
    render::Renderer renderer;
    render::UIRenderBackend gpu;
    editor::EditorSession session;
    std::unordered_map<std::string, render::MeshHandle> meshes;
    std::unordered_map<std::string, render::MaterialHandle> materials;
    render::SceneLights lights;
    std::shared_ptr<editor::EditorMaterialDocument> material;
    editor::EditorAssetId materialId{};
    bool renderSceneGeometry = true;
    bool useEditorCamera = false;
    explicit Fixture(fs::path destination, bool d3d12, bool assetPreviews = false) : output(fs::absolute(destination))
    {
        require(!fs::exists(output), "Use a fresh output directory");
        fs::create_directories(output);
        fs::copy(fs::path(AY_MATERIAL_WORKSHOP_HINT), output / "project", fs::copy_options::recursive);
        assets = output / "project/Assets";
        resource::setAssetRoot(utf8(assets));
        require(resource::initializeLoaders(), "Register resource loaders");
        resource::ResourceManager::instance().setAutoWatchLoadedResources(false);
        auto &registry = entity::ComponentRegistry::instance();
        require(bool(entity::registerEntityCoreComponents(registry)), "Register core components");
        require(bool(entity::registerEntityRenderComponents(registry)), "Register render components");
        window = CreateWindowExW(0, L"STATIC", L"Workshop EditorSession acceptance", WS_OVERLAPPED, 0, 0,
                                 width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(window != nullptr, "Create hidden GPU window");
        render::InitDesc init;
        init.windowHandle = window;
        init.width = width;
        init.height = height;
        init.msaa = 0;
        init.vsync = false;
        init.backend = d3d12 ? render::Backend::Direct3D12 : render::Backend::Direct3D11;
        require(renderer.initialize(init), "Initialize renderer");
        renderer.setShadowsEnabled(false);
        renderer.setTaaEnabled(false);
        renderer.setFxaaEnabled(false);
        renderer.setSmaaEnabled(false);
        renderer.setPostProcessGamma(2.2f);
        renderer.setPostProcessTonemapMode(render::Renderer::TonemapMode::ACES);
        lights.add(render::Light::directional({-.45f, -.8f, .55f}, {2.2f, 2.05f, 1.9f}));
        lights.add(render::Light::directional({.8f, -.25f, .2f}, {.55f, .7f, 1.1f}));
        renderer.setSceneLights(&lights);
        require(gpu.initialize(renderer), "Initialize UI renderer");
        gpu.setFramebufferSize(width, height);
        editor::EditorSessionDesc desc;
        desc.uiBackend = &gpu;
        desc.projectRoot = utf8(output / "project");
        desc.engineAssetsRoot = AY_EDITOR_ENGINE_ASSETS_HINT;
        desc.layoutPath = utf8(fs::path(AY_EDITOR_ENGINE_ASSETS_HINT) / "AYEditor/ui/editor_shell.ui.json");
        desc.hostWindow = window;
        if (assetPreviews) {
            desc.createAssetPreviewTexture = [this](uint16_t w, uint16_t h, const void* pixels) {
                return gpu.createUiTexture(w, h, pixels);
            };
            desc.releaseAssetPreviewTexture = [this](void* texture) { gpu.releaseUiTexture(texture); };
        }
        require(session.initialize(desc), "Initialize actual EditorSession");
        session.setClientSize(width, height);
        require(session.document() && session.document()->scene().world().getAllEntities().size() == 26,
                "EditorSession opens the project's startup Scene");
        require(session.rescanAssetsNow(), "Scan native Workshop assets");
        const auto *record = session.assetDatabase().findByLogicalPath("Assets/materials/ceramic.mat");
        require(record != nullptr, "Find real material in actual asset browser database");
        materialId = record->id;
    }
    ~Fixture()
    {
        session.shutdown();
        for (auto &[path, handle] : materials)
            renderer.destroyMaterial(handle);
        for (auto &[path, handle] : meshes)
            renderer.destroyMesh(handle);
        gpu.shutdown();
        renderer.shutdown();
        if (window)
            DestroyWindow(window);
    }
    bool sceneVisible()
    {
        auto *card = session.ui().findById("card_viewport");
        return card && card->isVisible();
    }
    void activateScene()
    {
        session.ui().setFocus(nullptr);
        auto *card = session.ui().findById("card_viewport");
        require(card != nullptr, "Scene View card exists");
        auto *tabs = dynamic_cast<ui::DockTabGroup *>(card->getParent());
        require(tabs != nullptr, "Scene View is hosted in a Dock tab group");
        tabs->activateTabById("card_viewport");
        session.ui().invalidateLayout();
    }
    void openMaterial()
    {
        require(session.openAsset(materialId), "Open through EditorSession asset routing");
        auto *active = session.workspace().documents().active();
        require(active != nullptr, "Material becomes the active workspace document");
        material = std::dynamic_pointer_cast<editor::EditorMaterialDocument>(active->document);
        require(bool(material), "Workspace owns the typed material document");
        require(!material->readOnly(), "Project source material is writable");
    }
    render::RenderScene extract()
    {
        render::RenderScene result;
        for (auto *object : session.document()->scene().world().getAllEntities())
        {
            const auto *mesh = object->getComponent<entity::MeshComponent>();
            if (!mesh)
                continue;
            const auto meshPath = resource::resolveAssetPath({}, mesh->meshPath);
            const auto materialPath = resource::resolveAssetPath({}, mesh->materialPath);
            if (!meshes.contains(meshPath))
                meshes[meshPath] = renderer.loadMesh(meshPath);
            if (!materials.contains(materialPath))
                materials[materialPath] = renderer.loadMaterial(materialPath);
            require(meshes.at(meshPath).isValid() && materials.at(materialPath).isValid(),
                    "Native Scene assets load");
            result.add(meshes.at(meshPath), materials.at(materialPath), object->worldMatrix());
        }
        require(result.items().size() == 25, "Complete native Scene extraction");
        return result;
    }
    void frame(const char *screenshot = nullptr)
    {
        session.update(1.f / 60);
        session.ui().layout();
        const bool showScene = sceneVisible();
        render::ClearDesc clear;
        clear.r = .025f;
        clear.g = .035f;
        clear.b = .05f;
        renderer.beginCompositeFrame(clear, width, height);
        if (showScene)
        {
            math::FRectangle bounds;
            require(session.getViewportBounds(bounds), "Get real Scene panel bounds");
            const auto w = uint16_t(bounds.maxX - bounds.minX), h = uint16_t(bounds.maxY - bounds.minY);
            require(w > 0 && h > 0, "Scene panel has positive dimensions");
            renderer.setViewportRect(uint16_t(bounds.minX), uint16_t(bounds.minY), w, h);
            if(useEditorCamera){
                const auto camera=session.sceneCamera().frame();
                renderer.setMainCamera(camera.view,camera.projection,camera.position);
            }else{
                entity::PerspectiveCameraFrame camera;
                require(entity::evaluatePerspectiveCamera3D(session.document()->scene().world(), float(w) / h, 1,
                                                            camera),
                        "Evaluate native camera for actual panel aspect");
                renderer.setMainCamera(camera.view, camera.projection, camera.position);
            }
            renderer.render(renderSceneGeometry ? extract() : render::RenderScene{});
        }
        session.render(showScene);
        if (screenshot)
            require(renderer.captureScreenshot(utf8(output / screenshot)), "Capture actual EditorSession");
        renderer.endFrame();
    }
    auto capture(const char *name)
    {
        for (int i = 0; i < 12; ++i)
            frame();
        frame(name);
        for (int i = 0; i < 8; ++i)
            frame();
        auto result = render::detail::decodeImageFile(utf8(output / name));
        require(result.isValid(), "Decode EditorSession capture");
        require(result.width == width && result.height == height &&
                    result.rgba8.size() == size_t(width) * height * 4,
                "EditorSession capture dimensions match the actual shell viewport");
        return result;
    }
    math::FRectangle sceneBounds()
    {
        math::FRectangle result;
        require(session.getViewportBounds(result), "Get Scene comparison bounds");
        return result;
    }
    static size_t difference(const auto &a, const auto &b, const math::FRectangle &bounds, int tolerance)
    {
        size_t changed = 0;
        for (int y = int(bounds.minY) + 6; y < int(bounds.maxY) - 6; ++y)
            for (int x = int(bounds.minX) + 6; x < int(bounds.maxX) - 6; ++x)
            {
                const size_t p = (y * width + x) * 4;
                for (int c = 0; c < 3; ++c)
                    if (std::abs(int(a.rgba8[p + c]) - int(b.rgba8[p + c])) > tolerance)
                    {
                        ++changed;
                        break;
                    }
            }
        return changed;
    }
};
} // namespace
