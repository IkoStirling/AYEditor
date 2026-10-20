// Opt-in acceptance: a copied native Scene and the actual material authoring view.
// This fixture composes those two views directly; it is not a full Editor shell test.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <AYEditor/EditorMaterialDocument.h>
#include <AYEditor/EditorSceneDocument.h>
#include <AYEditor/EditorProjectDescriptor.h>
#include <AYEditor/EditorWorkspace.h>
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
#include <AYResource/assetsImpl/Material.h>
#include <AYIO/File.h>
#include <AYUI/UIManager.h>
#include "detail/TextureImageLoader.h"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <nlohmann/json.hpp>

namespace
{
namespace fs = std::filesystem;
using namespace ayt;
std::string utf8(const fs::path &path)
{
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}
void require(bool value, const std::string &message)
{
    if (!value)
        throw std::runtime_error(message);
}
struct Host : editor::IEditorHostServices
{
    editor::EditorWorkspace value;
    std::string root;
    editor::EditorWorkspace &workspace() noexcept override
    {
        return value;
    }
    const std::string &projectRoot() const noexcept override
    {
        return root;
    }
    void requestRepaint() override
    {
    }
    void setStatusText(const std::wstring &) override
    {
    }
};
struct Fixture
{
    static constexpr uint16_t height = 900, sceneWidth = 850, materialWidth = 942;
    static constexpr uint16_t secondaryX = sceneWidth + 16 + materialWidth;
    uint16_t width;
    HWND window = nullptr;
    render::Renderer renderer;
    render::UIRenderBackend gpu;
    ui::UIManager manager;
    Host host;
    fs::path output, assets;
    editor::EditorSceneDocument sceneDocument;
    std::shared_ptr<editor::EditorMaterialDocument> material =
        std::make_shared<editor::EditorMaterialDocument>();
    std::unique_ptr<editor::IEditorView> view;
    std::shared_ptr<editor::EditorMaterialDocument> secondary;
    std::unique_ptr<editor::IEditorView> secondaryView;
    std::unordered_map<std::string, render::MeshHandle> meshes;
    std::unordered_map<std::string, render::MaterialHandle> materials;
    render::SceneLights lights;
    size_t expectedObjects = 25;
    bool suppressSceneGeometry = false;
    explicit Fixture(fs::path destination, bool d3d12, bool forceSc, bool multiple, bool sparse)
        : width(multiple ? secondaryX + materialWidth : 1800), output(fs::absolute(destination))
    {
        require(!fs::exists(output), "Use a fresh output directory");
        fs::create_directories(output);
        fs::copy(fs::path(AY_MATERIAL_WORKSHOP_HINT), output / "project", fs::copy_options::recursive);
        host.root = utf8(output / "project");
        assets = output / "project" / "Assets";
        if (sparse)
        {
            for (const auto &entry : fs::directory_iterator(assets / "materials"))
            {
                if (entry.path().extension() != ".mat")
                    continue;
                std::ifstream input(entry.path(), std::ios::binary);
                std::vector<UInt8> bytes{std::istreambuf_iterator<char>(input), {}};
                input.close();
                resource::Material source, minimal;
                require(source.loadFromBinary(bytes.data(), bytes.size()), "Decode legacy sparse fixture");
                minimal.initialise(source.getName(), source.getShader());
                minimal.setGuid(source.getGuid());
                source.forEachParameter([&](const char *name, resource::MaterialParamType type) {
                    if (type == resource::MaterialParamType::Texture2D)
                        minimal.setTexture(name, source.getTexture(name));
                });
                require(minimal.saveToBinary(bytes), "Encode legacy sparse fixture");
                require(io::File::atomicWriteUtf8(utf8(entry.path()), bytes.data(), bytes.size()),
                        "Write only the copied sparse fixture");
            }
        }
        if (forceSc)
        {
            // Exercise the legacy raw-SC acquisition flavor against the same
            // Scene/preview lifecycle. Only this copied fixture is rewritten.
            _putenv_s("AY_SHADOW_USE_SC", "1");
            for (const auto &entry : fs::directory_iterator(assets / "materials"))
            {
                if (entry.path().extension() != ".mat")
                    continue;
                std::ifstream input(entry.path(), std::ios::binary);
                std::vector<UInt8> bytes{std::istreambuf_iterator<char>(input), {}};
                input.close();
                resource::Material source;
                require(source.loadFromBinary(bytes.data(), bytes.size()), "Decode SC fixture material");
                source.setShader("simple_lit_shadow.phoskia");
                const std::string albedo = source.getTexture("baseColorTexture");
                source.setTexture("albedoMap", albedo.c_str());
                require(source.saveToBinary(bytes), "Encode SC fixture material");
                require(io::File::atomicWriteUtf8(utf8(entry.path()), bytes.data(), bytes.size()),
                        "Write copied SC fixture material");
            }
        }
        resource::setAssetRoot(utf8(assets));
        require(resource::initializeLoaders(), "Register resource loaders");
        resource::ResourceManager::instance().setAutoWatchLoadedResources(false);
        auto &registry = entity::ComponentRegistry::instance();
        require(bool(entity::registerEntityCoreComponents(registry)), "Register core components");
        require(bool(entity::registerEntityRenderComponents(registry)), "Register render components");
        std::string error;
        require(sceneDocument.open(utf8(assets / "worlds/gallery.scn"), &error), error);
        require(material->initialize({utf8(assets / "materials/ceramic.mat")}, error), error);
        window = CreateWindowExW(0, L"STATIC", L"Material Workshop authoring acceptance", WS_OVERLAPPED, 0, 0,
                                 width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(window != nullptr, "Create hidden validation window");
        render::InitDesc init;
        init.windowHandle = window;
        init.width = width;
        init.height = height;
        init.msaa = 0;
        init.vsync = false;
        init.backend = d3d12 ? render::Backend::Direct3D12 : render::Backend::Direct3D11;
        require(renderer.initialize(init), "Initialize renderer");
        renderer.setViewportRect(0, 0, sceneWidth, height);
        renderer.setShadowsEnabled(false);
        renderer.setTaaEnabled(false);
        renderer.setFxaaEnabled(false);
        renderer.setSmaaEnabled(false);
        renderer.setPostProcessGamma(2.2f);
        renderer.setPostProcessTonemapMode(render::Renderer::TonemapMode::ACES);
        lights.add(render::Light::directional({-.45f, -.8f, .55f}, {2.2f, 2.05f, 1.9f}));
        lights.add(render::Light::directional({.8f, -.25f, .2f}, {.55f, .7f, 1.1f}));
        lights.add(render::Light::directional({-.3f, -.25f, -.9f}, {1.2f, .8f, .45f}));
        renderer.setSceneLights(&lights);
        require(gpu.initialize(renderer), "Initialize UI renderer");
        gpu.setFramebufferSize(width, height);
        manager.initialize(&gpu);
        manager.setClientSize(width, height);
        view = editor::makeEditorMaterialDescriptor().createView(material, host);
        require(bool(view), "Create material authoring view");
        view->rootWidget()->setPosition({sceneWidth + 8.f, 0});
        view->rootWidget()->setSize({float(materialWidth), float(height)});
        view->rootWidget()->performLayout();
        if (multiple)
        {
            secondary = std::make_shared<editor::EditorMaterialDocument>();
            require(secondary->initialize({utf8(assets / "materials/wood.mat")}, error), error);
            secondaryView = editor::makeEditorMaterialDescriptor().createView(secondary, host);
            require(bool(secondaryView), "Create second material authoring view");
            secondaryView->rootWidget()->setPosition({float(secondaryX), 0});
            secondaryView->rootWidget()->setSize({float(materialWidth), float(height)});
            secondaryView->rootWidget()->performLayout();
        }
    }
    ~Fixture()
    {
        if (view)
            view->prepareForUiShutdown();
        view.reset();
        if (secondaryView)
            secondaryView->prepareForUiShutdown();
        secondaryView.reset();
        manager.shutdown();
        for (auto &[path, handle] : materials)
            renderer.destroyMaterial(handle);
        for (auto &[path, handle] : meshes)
            renderer.destroyMesh(handle);
        gpu.shutdown();
        renderer.shutdown();
        if (window)
            DestroyWindow(window);
    }
    render::RenderScene extract()
    {
        render::RenderScene result;
        for (auto *object : sceneDocument.scene().world().getAllEntities())
        {
            const auto *component = object->getComponent<entity::MeshComponent>();
            if (!component)
                continue;
            const auto meshPath = resource::resolveAssetPath({}, component->meshPath);
            const auto materialPath = resource::resolveAssetPath({}, component->materialPath);
            if (!meshes.contains(meshPath))
                meshes[meshPath] = renderer.loadMesh(meshPath);
            if (!materials.contains(materialPath))
                materials[materialPath] = renderer.loadMaterial(materialPath);
            require(meshes.at(meshPath).isValid() && materials.at(materialPath).isValid(),
                    "Scene mesh/material bridge returned invalid handle");
            require(!resource::ResourceManager::instance().hasLoadFailed(meshPath) &&
                        !resource::ResourceManager::instance().hasLoadFailed(materialPath),
                    "Scene used a resource placeholder");
            result.add(meshes.at(meshPath), materials.at(materialPath), object->worldMatrix());
        }
        require(result.items().size() == expectedObjects, "Expected the complete authored gallery");
        return result;
    }
    void frame(const std::string &capture = {})
    {
        entity::PerspectiveCameraFrame camera;
        require(entity::evaluatePerspectiveCamera3D(sceneDocument.scene().world(), float(sceneWidth) / height,
                                                    1, camera),
                "Evaluate the saved Scene camera");
        renderer.setMainCamera(camera.view, camera.projection, camera.position);
        const auto scene = extract();
        render::ClearDesc clear;
        clear.r = .025f;
        clear.g = .035f;
        clear.b = .05f;
        renderer.beginCompositeFrame(clear, width, height);
        if (suppressSceneGeometry)
            renderer.render(render::RenderScene{});
        else
            renderer.render(scene);
        gpu.beginFrame();
        gpu.beginCanvas({0, 0, float(width), float(height)});
        gpu.drawText({16, 20, float(sceneWidth - 16), 58}, L"Editable native Scene / cached material", 20,
                     math::FVector4{.9f, .95f, 1, 1});
        view->tick(1.f / 60);
        view->rootWidget()->render(gpu);
        if (secondaryView)
        {
            secondaryView->tick(1.f / 60);
            secondaryView->rootWidget()->render(gpu);
        }
        const auto afterPreview = renderer.mainCameraPosition();
        require(afterPreview.x == camera.position.x && afterPreview.y == camera.position.y &&
                    afterPreview.z == camera.position.z,
                "All material previews preserve the main camera");
        gpu.endCanvas();
        gpu.endFrame();
        if (!capture.empty())
            require(renderer.captureScreenshot(utf8(output / capture)), "Capture workshop integration");
        renderer.endFrame();
    }
    auto capture(const char *name)
    {
        for (int i = 0; i < 10; ++i)
            frame();
        frame(name);
        for (int i = 0; i < 8; ++i)
            frame();
        auto pixels = render::detail::decodeImageFile(utf8(output / name));
        require(pixels.isValid(), "Decode workshop capture");
        require(pixels.width == width && pixels.height == height &&
                    pixels.rgba8.size() == size_t(width) * height * 4,
                "Workshop capture dimensions match both comparison viewports");
        return pixels;
    }
    void reopenPreview()
    {
        view->prepareForUiShutdown();
        view.reset();
        view = editor::makeEditorMaterialDescriptor().createView(material, host);
        require(bool(view), "Reopen material authoring view");
        view->rootWidget()->setPosition({sceneWidth + 8.f, 0});
        view->rootWidget()->setSize({float(materialWidth), float(height)});
        view->rootWidget()->performLayout();
    }
    void reloadSceneMaterial()
    {
        const auto path = resource::resolveAssetPath({}, utf8(assets / "materials/ceramic.mat"));
        auto found = materials.find(path);
        require(found != materials.end(), "Scene material is cached");
        renderer.destroyMaterial(found->second);
        materials.erase(found);
        resource::ResourceManager::instance().reloadResource(path);
    }
};
void renderCatalog(Fixture &fixture)
{
    std::string error;
    const auto project = editor::EditorProjectDescriptor::load(fixture.host.root, &error);
    require(bool(project) && !project.worlds.empty(), "Load the native project's complete Scene catalog");
    auto report = nlohmann::json::array();
    // Dark rubber can be clearly visible while having no channel above 100.
    // Compare against real geometry-free renders instead of an absolute
    // brightness threshold; leave all lighting, display and UI state unchanged.
    std::vector<render::detail::DecodedImage> backgrounds;
    fixture.suppressSceneGeometry = true;
    for (int path = 0; path < 2; ++path)
    {
        fixture.renderer.configurePipeline(path ? render::RenderPipelineDesc::makeDeferred()
                                                : render::RenderPipelineDesc::makeDefault());
        fixture.renderer.setShadowsEnabled(false);
        backgrounds.push_back(
            fixture.capture(path ? "catalog-empty-deferred.png" : "catalog-empty-forward.png"));
    }
    fixture.suppressSceneGeometry = false;
    for (const auto &world : project.worlds)
    {
        require(fixture.sceneDocument.open(utf8(fixture.assets / fs::u8path(world.scene)), &error), error);
        fixture.expectedObjects = 0;
        for (auto *object : fixture.sceneDocument.scene().world().getAllEntities())
            if (object->getComponent<entity::MeshComponent>())
                ++fixture.expectedObjects;
        require(fixture.expectedObjects >= 3, "Catalog Scene must contain real authored mesh objects");
        for (int path = 0; path < 2; ++path)
        {
            fixture.renderer.configurePipeline(path ? render::RenderPipelineDesc::makeDeferred()
                                                    : render::RenderPipelineDesc::makeDefault());
            fixture.renderer.setShadowsEnabled(false);
            const auto name = std::string(path ? "deferred-" : "forward-") + world.id + ".png";
            const auto image = fixture.capture(name.c_str());
            size_t visible = 0;
            for (int y = 100; y < Fixture::height - 20; ++y)
                for (int x = 20; x < Fixture::sceneWidth - 20; ++x)
                {
                    const size_t pixel = (size_t(y) * fixture.width + x) * 4;
                    for (int channel = 0; channel < 3; ++channel)
                        if (std::abs(int(image.rgba8[pixel + channel]) -
                                     int(backgrounds[path].rgba8[pixel + channel])) > 3)
                        {
                            ++visible;
                            break;
                        }
                }
            report.push_back({{"scene", world.id},
                              {"path", path ? "Deferred" : "Forward"},
                              {"objects", fixture.expectedObjects},
                              {"visiblePixels", visible},
                              {"capture", name}});
            std::ofstream receipt(fixture.output / "catalog.json");
            receipt << report.dump(2) << '\n';
            require(bool(receipt), "Write incremental native Scene catalog evidence");
            require(visible > 5000,
                    "Native catalog Scene must show loaded geometry through its saved camera");
            std::cout << "Catalog " << world.id << ' ' << (path ? "Deferred" : "Forward")
                      << " objects=" << fixture.expectedObjects << " visiblePixels=" << visible << std::endl;
        }
    }
    std::cout << "PASS native Scene catalog: " << project.worlds.size()
              << " saved Scenes, both paths, actual meshes/materials/cameras with authoring preview\n";
}
} // namespace
int main(int argc, char **argv)
{
    try
    {
        Fixture f(argc > 1 ? argv[1] : "out/material-workshop-render",
                  argc > 2 && std::string(argv[2]) == "d3d12",
                  argc > 3 && std::string(argv[3]) == "--force-sc",
                  argc > 3 && std::string(argv[3]) == "--multiple",
                  argc > 3 && std::string(argv[3]) == "--sparse");
        if (argc > 3 && std::string(argv[3]) == "--catalog")
        {
            renderCatalog(f);
            return 0;
        }
        const auto original = f.capture("workshop-open.png");
        size_t visible = 0;
        for (int y = 100; y < 780; ++y)
            for (int x = 20; x < 820; ++x)
            {
                const size_t p = (y * f.width + x) * 4;
                if (original.rgba8[p] > 80 || original.rgba8[p + 1] > 100 || original.rgba8[p + 2] > 100)
                    ++visible;
            }
        require(visible > 5000, "Saved Scene camera must show real loaded material geometry");
        auto edited = f.material->settings();
        edited.baseColor = {.9f, .18f, .06f, 1};
        edited.surface.uvScaleX = 2.7f;
        edited.surface.normalStrength = .2f;
        edited.surface.roughnessBias = .25f;
        if (argc > 3 && std::string(argv[3]) == "--sparse")
        {
            edited.metallic = .85f;
            edited.roughness = .1f;
            edited.ao = .15f;
            edited.emissive = {.1f, .2f, .3f};
        }
        std::string error;
        require(f.material->replaceSettings(edited, &error), error);
        const auto unsaved = f.capture("workshop-unsaved.png");
        size_t sceneChanged = 0, previewChanged = 0;
        for (int y = 100; y < 780; ++y)
            for (int x = 20; x < 1250; ++x)
            {
                const size_t p = (y * f.width + x) * 4;
                int delta = 0;
                for (int channel = 0; channel < 3; ++channel)
                    delta = std::max(
                        delta, std::abs(int(original.rgba8[p + channel]) - int(unsaved.rgba8[p + channel])));
                if (x < 820 && delta > 1)
                    ++sceneChanged;
                if (x > 900 && delta > 8)
                    ++previewChanged;
            }
        require(sceneChanged == 0, "Unsaved preview must not mutate cached Scene material");
        require(previewChanged > 1000, "Unsaved preview must visibly change");
        if (f.secondaryView)
        {
            const auto regionDifference = [&](const auto &a, const auto &b, int left, int right) {
                size_t differences = 0;
                for (int y = 100; y < 780; ++y)
                    for (int x = left; x < right; ++x)
                    {
                        const size_t p = (y * f.width + x) * 4;
                        for (int channel = 0; channel < 3; ++channel)
                            if (std::abs(int(a.rgba8[p + channel]) - int(b.rgba8[p + channel])) > 1)
                            {
                                ++differences;
                                break;
                            }
                    }
                return differences;
            };
            require(
                regionDifference(original, unsaved, Fixture::secondaryX + 40, Fixture::secondaryX + 480) == 0,
                "Editing one material preserves the other textured HDR preview");
            auto next = f.secondary->settings();
            next.baseColor = {.1f, .55f, .85f, 1};
            next.surface.uvScaleY = 1.8f;
            require(f.secondary->replaceSettings(next, &error), error);
            const auto secondEdited = f.capture("workshop-second-edited.png");
            require(regionDifference(unsaved, secondEdited, Fixture::secondaryX + 40,
                                     Fixture::secondaryX + 480) > 1000,
                    "The second material independently changes its textured HDR preview");
            require(regionDifference(unsaved, secondEdited, 900, 1250) == 0,
                    "Editing the second material preserves the first preview");
            require(regionDifference(original, secondEdited, 20, 820) == 0,
                    "Neither unsaved material mutates its cached Scene instances");
            f.secondaryView->prepareForUiShutdown();
            f.secondaryView.reset();
            const auto closed = f.capture("workshop-second-closed.png");
            require(regionDifference(secondEdited, closed, 900, 1250) == 0 &&
                        regionDifference(original, closed, 20, 820) == 0,
                    "Closing one preview preserves the Scene and the other preview program");
            std::cout << "PASS multiple textured HDR material previews: independent edits/close, shared "
                         "Scene program\n";
        }
        // Rebuild repeatedly, then close/reopen while the Scene still shares
        // the compiled program. Also inspect G/B, not only a material handle.
        for (int edit = 0; edit < 4; ++edit)
        {
            auto next = edited;
            next.surface.uvOffsetX = .1f * (edit + 1);
            require(f.material->replaceSettings(next, &error), error);
            f.frame();
        }
        require(f.material->replaceSettings(edited, &error), error);
        f.reopenPreview();
        const auto reopenedPreview = f.capture("workshop-preview-reopened.png");
        const auto sceneDifference = [&](const auto &a, const auto &b, int tolerance) {
            size_t changed = 0;
            for (int y = 100; y < 780; ++y)
                for (int x = 20; x < 820; ++x)
                {
                    const size_t pixel = (y * f.width + x) * 4;
                    for (int channel = 0; channel < 3; ++channel)
                        if (std::abs(int(a.rgba8[pixel + channel]) - int(b.rgba8[pixel + channel])) >
                            tolerance)
                        {
                            ++changed;
                            break;
                        }
                }
            return changed;
        };
        require(sceneDifference(original, reopenedPreview, 1) == 0,
                "Repeated preview rebuilds and close/reopen must preserve Scene pixels");
        require(f.material->save(&error), error);
        // This fixture explicitly disables file watching. Saving publishes
        // bytes; it does not itself replace the cached GPU Scene material.
        const auto savedBeforeReload = f.capture("workshop-saved-before-reload.png");
        require(sceneDifference(original, savedBeforeReload, 1) == 0,
                "With watching disabled, saved bytes wait for explicit Scene reload");
        f.reloadSceneMaterial();
        const auto saved = f.capture("workshop-saved-reloaded.png");
        size_t savedChanged = 0;
        for (int y = 100; y < 780; ++y)
            for (int x = 20; x < 820; ++x)
            {
                const size_t p = (y * f.width + x) * 4;
                int delta = 0;
                for (int channel = 0; channel < 3; ++channel)
                    delta = std::max(
                        delta, std::abs(int(original.rgba8[p + channel]) - int(saved.rgba8[p + channel])));
                if (delta > 8)
                    ++savedChanged;
            }
        require(savedChanged > 500,
                "Saved material must visibly change the native Scene after explicit reload");
        editor::EditorMaterialDocument reopened;
        require(reopened.initialize({f.material->path()}, error), error);
        require(reopened.settings() == edited, "Native material retains authored settings");
        const auto count = f.sceneDocument.scene().world().getAllEntities().size();
        require(f.sceneDocument.createEntity("Add editable material sample",
                                             [&](entity::Entity &object) {
                                                 object.setName("Authored extra surface");
                                                 object.transform()->position = {0, 3.7f, -1};
                                                 auto *mesh = object.addComponent<entity::MeshComponent>();
                                                 if (!mesh)
                                                     return false;
                                                 mesh->meshPath = "meshes/sphere.msh";
                                                 mesh->materialPath = "materials/ceramic.mat";
                                                 return true;
                                             }),
                "Edit native Scene through document commands");
        const auto editedScene = utf8(f.assets / "worlds/gallery-edited.scn");
        require(f.sceneDocument.saveAs(editedScene, &error), error);
        editor::EditorSceneDocument sceneReopened;
        require(sceneReopened.open(editedScene, &error), error);
        require(sceneReopened.scene().world().getAllEntities().size() == count + 1,
                "Scene authoring change survives save/reopen");
        require(f.sceneDocument.open(editedScene, &error), error);
        ++f.expectedObjects;
        const auto sceneEdited = f.capture("workshop-scene-edited-reopened.png");
        require(sceneDifference(saved, sceneEdited, 8) > 500,
                "Saved/reopened Scene edit must render its added native mesh");
        std::cout << "PASS editable Workshop: 25 real Scene objects, previewChanged=" << previewChanged
                  << " unsavedSceneChanges=" << sceneChanged << " savedSceneChanges=" << savedChanged
                  << ", material/Scene save-reopen\n";
        return 0;
    }
    catch (const std::exception &ex)
    {
        std::cerr << "FAIL: " << ex.what() << '\n';
        return 1;
    }
}
