// Real EditorSession asset/Dock/field routing with a real GPU backend.
// Scene extraction is an explicit fixture Host, not default GameLoop assembly.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
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
    explicit Fixture(fs::path destination, bool d3d12) : output(fs::absolute(destination))
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
            entity::PerspectiveCameraFrame camera;
            require(entity::evaluatePerspectiveCamera3D(session.document()->scene().world(), float(w) / h, 1,
                                                        camera),
                    "Evaluate native camera for actual panel aspect");
            renderer.setMainCamera(camera.view, camera.projection, camera.position);
            renderer.render(extract());
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
int main(int argc, char **argv)
{
    try
    {
        Fixture f(argc > 1 ? argv[1] : "out/material-workshop-session",
                  argc > 2 && std::string(argv[2]) == "d3d12");
        const auto original = f.capture("session-scene-open.png");
        const auto bounds = f.sceneBounds();
        size_t visible = 0;
        for (int y = int(bounds.minY) + 6; y < int(bounds.maxY) - 6; ++y)
            for (int x = int(bounds.minX) + 6; x < int(bounds.maxX) - 6; ++x)
            {
                const auto p = (y * Fixture::width + x) * 4;
                if (original.rgba8[p] > 80 || original.rgba8[p + 1] > 100 || original.rgba8[p + 2] > 100)
                    ++visible;
            }
        require(visible > 5000, "Actual EditorSession Scene initially renders visible geometry");
        f.openMaterial();
        const auto preview = f.capture("session-material-open.png");
        auto *field = dynamic_cast<ui::TextInput *>(f.session.ui().findById("material_r"));
        require(field != nullptr, "Actual Dock contains the material color field");
        const auto revision = f.material->revision();
        f.session.ui().setFocus(field);
        field->setText(L"0.1");
        f.session.onKeyDown(ui::UIKey_Enter);
        require(f.material->revision() > revision && f.material->isDirty(),
                "Enter commits actual material field");
        require(f.material->settings().baseColor[0] == .1f, "Actual field reaches the document");
        const auto editedPreview = f.capture("session-material-unsaved.png");
        require(Fixture::difference(preview, editedPreview,
                                    {bounds.minX, bounds.minY, bounds.maxX - 450, bounds.maxY}, 8) > 1000,
                "Actual EditorSession preview changes after Enter");
        f.activateScene();
        const auto unsaved = f.capture("session-scene-unsaved.png");
        require(Fixture::difference(original, unsaved, bounds, 1) == 0,
                "Switching back to Scene retains the cached material after an unsaved Dock edit");
        f.openMaterial();
        auto *pending = dynamic_cast<ui::TextInput *>(f.session.ui().findById("material_g"));
        require(pending != nullptr, "Actual Dock contains the pending color field");
        f.session.ui().setFocus(pending);
        pending->setText(L"0.4");
        f.session.onKeyDown(ui::UIKey_Control);
        f.session.onKeyDown(ui::UIKey_S);
        f.session.onKeyUp(ui::UIKey_S);
        f.session.onKeyUp(ui::UIKey_Control);
        require(!f.material->isDirty() && f.material->settings().baseColor[1] == .4f,
                "Actual Ctrl+S commits pending fields and saves the material");
        f.activateScene();
        const auto materialPath = resource::resolveAssetPath({}, utf8(f.assets / "materials/ceramic.mat"));
        auto &cached = f.materials.at(materialPath);
        f.renderer.destroyMaterial(cached);
        f.materials.erase(materialPath);
        resource::ResourceManager::instance().reloadResource(materialPath);
        const auto saved = f.capture("session-scene-saved-reloaded.png");
        const auto changes = Fixture::difference(original, saved, bounds, 8);
        require(changes > 500, "Actual Session Scene changes after saved material reload");

        // Exercise the actual New Material action as well as the pre-authored
        // sample. Its companion shader must work without any source textures.
        require(f.session.createProjectAsset(editor::EditorAssetType::Material),
                "Actual Session creates and opens a standalone PBR material");
        const auto *created = f.session.workspace().documents().active();
        require(created != nullptr, "New material becomes the active document");
        auto authored = std::dynamic_pointer_cast<editor::EditorMaterialDocument>(created->document);
        require(authored && fs::u8path(authored->path()).filename() == "NewMaterial.mat",
                "Actual new action opens its created native material");
        require(fs::is_regular_file(
                    fs::u8path(resource::resolveAssetPath(authored->path(), authored->shaderReference()))),
                "New material has its own existing shader companion");
        require(authored->textureReferences().empty(),
                "New surface starts without external texture dependencies");
        const auto freshPreview = f.capture("session-new-material.png");
        require(Fixture::difference(editedPreview, freshPreview,
                                    {bounds.minX, bounds.minY, bounds.maxX - 450, bounds.maxY}, 8) > 1000,
                "New texture-free material renders its actual studio preview");
        const auto *record = f.session.assetDatabase().findByLogicalPath("Assets/materials/NewMaterial.mat");
        require(record != nullptr, "New native material is revealed in the asset database");
        auto *dock = dynamic_cast<ui::DockArea *>(f.session.ui().findById("main_dock"));
        require(dock != nullptr, "Actual shell owns the main DockArea");
        // DockCard's stable docking id is separate from Widget's lookup id.
        auto *card = dock->findCard("card_material_" + std::to_string(record->id));
        require(card != nullptr, "New material has its own Dock card");
        const auto findField = [](auto &&self, ui::Widget *root, const std::string &id) -> ui::TextInput * {
            if (root->getId() == id)
                return dynamic_cast<ui::TextInput *>(root);
            for (auto *child : root->getChildren())
                if (auto *found = self(self, child, id))
                    return found;
            return nullptr;
        };
        auto *newField = findField(findField, card, "material_g");
        require(newField != nullptr,
                "Resolve a field inside the new card, independently of hidden old cards");
        f.session.ui().setFocus(newField);
        newField->setText(L"0.2");
        f.session.onKeyDown(ui::UIKey_Enter);
        require(authored->isDirty() && authored->settings().baseColor[1] == .2f,
                "New card Enter edits its own material document");
        require(f.material->settings().baseColor[1] == .4f && !f.material->isDirty(),
                "Editing the new card leaves the previously saved material unchanged");
        const auto authoredPreview = f.capture("session-new-material-edited.png");
        require(Fixture::difference(freshPreview, authoredPreview,
                                    {bounds.minX, bounds.minY, bounds.maxX - 450, bounds.maxY}, 8) > 1000,
                "New material preview responds to its actual field");
        auto *roughness = findField(findField, card, "material_roughness");
        require(roughness != nullptr, "Resolve the new material's pending roughness field");
        f.session.ui().setFocus(roughness);
        roughness->setText(L"0.6");
        f.session.onKeyDown(ui::UIKey_Control);
        f.session.onKeyDown(ui::UIKey_S);
        f.session.onKeyUp(ui::UIKey_S);
        f.session.onKeyUp(ui::UIKey_Control);
        require(!authored->isDirty() && authored->settings().roughness == .6f,
                "Ctrl+S commits and saves the new active material's pending field");
        editor::EditorMaterialDocument reopened;
        std::string error;
        require(reopened.initialize({authored->path()}, error), error);
        require(reopened.settings() == authored->settings(), "New material's actual UI edits survive reopen");
        f.activateScene();
        const auto afterCreation = f.capture("session-scene-after-material-creation.png");
        require(Fixture::difference(saved, afterCreation, bounds, 1) == 0,
                "Creating/editing a separate material preserves the saved Scene's GPU output");
        std::cout
            << "PASS actual EditorSession: startup Scene, asset/Dock routing, Enter edit, preview change, "
               "tab-return Scene isolation, Ctrl+S pending fields, new standalone shader material, saved "
               "Scene changes="
            << changes << '\n';
        return 0;
    }
    catch (const std::exception &ex)
    {
        std::cerr << "FAIL: " << ex.what() << '\n';
        return 1;
    }
}
