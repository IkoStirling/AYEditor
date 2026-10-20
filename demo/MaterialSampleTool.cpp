// Rebuild the editable Material Workshop using the existing Mesh/Scene codecs.
#include <AYEditor/EditorSceneDocument.h>
#include <AYEditor/EditorProjectDescriptor.h>
#include <AYEditor/EditorProjectRuntimeValidator.h>
#include <AYEditor/EditorSceneViewWorkspace.h>
#include <AYEntity/EntityModule.h>
#include <AYEntity/EntityRenderIntegrationModule.h>
#include <AYEntity.h>
#include <AYEntity/components/MeshComponent.h>
#include <AYEntity/components/PerspectiveCameraComponent.h>
#include <AYEntity/PerspectiveCameraSelection.h>
#include <AYScene.h>
#include <AYResource/Converter/MeshConverter.h>
#include <AYResource/assetsImpl/Mesh.h>
#include <AYResource/assetsImpl/Material.h>
#include <AYResource/ResourceManager.h>
#include <AYResource/ResourceBootstrap.h>
#include <AYIO/File.h>
#include <AYStorage/Guid.h>
#include <AYAssetFormat/AssetFormat.h>
#include "../../AYRenderer/demo/MaterialExhibitContent.h"
#include "../../AYRenderer/demo/MaterialLibraryValidation.h"
#include "../../AYRenderer/demo/ProceduralGalleryGeometry.h"
#include "../../AYRenderer/demo/DemoUtf8Arguments.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <limits>
#include <cmath>

namespace
{
namespace fs = std::filesystem;
using namespace ayt;
std::string utf8(const fs::path &p)
{
    const auto s = p.u8string();
    return {s.begin(), s.end()};
}
void require(bool value, const std::string &message)
{
    if (!value)
        throw std::runtime_error(message);
}
const char *meshNames[] = {"cube", "sphere", "cylinder", "torus"};
const std::string meshSuffix{asset_format::suffix(asset_format::Id::Mesh)};
void writeMesh(const fs::path &assets, const char *name, const render::demo::GalleryGeometry &geometry)
{
    resource::MeshData mesh;
    mesh.name = name;
    mesh.attributeMask = 15;
    for (int c = 0; c < 3; ++c)
    {
        mesh.boundsMin[c] = std::numeric_limits<float>::max();
        mesh.boundsMax[c] = -std::numeric_limits<float>::max();
    }
    for (const auto &v : geometry.vertices)
    {
        mesh.positions.insert(mesh.positions.end(), {v.x, v.y, v.z});
        mesh.normals.insert(mesh.normals.end(), {v.nx, v.ny, v.nz});
        mesh.tangents.insert(mesh.tangents.end(), {v.tx, v.ty, v.tz, v.tw});
        mesh.uvs.insert(mesh.uvs.end(), {v.u, v.v});
        const float p[] = {v.x, v.y, v.z};
        for (int c = 0; c < 3; ++c)
        {
            mesh.boundsMin[c] = std::min(mesh.boundsMin[c], p[c]);
            mesh.boundsMax[c] = std::max(mesh.boundsMax[c], p[c]);
        }
    }
    mesh.indices.assign(geometry.indices.begin(), geometry.indices.end());
    mesh.submeshes.push_back({0, uint32_t(mesh.indices.size()), 0, 0});
    mesh.materialSlots.push_back("");
    resource::MeshConverter converter;
    converter.setOutputDir(utf8(assets));
    converter.setVirtualPath(std::string("meshes/") + name + meshSuffix);
    require(converter.convert(mesh), std::string("Mesh conversion failed: ") + name);
    resource::Mesh loaded;
    require(loaded.load(utf8(assets / converter.getLastOutputPath())), "Saved mesh did not load");
    require(loaded.getVertexCount() == geometry.vertices.size() &&
                loaded.getIndexCount() == geometry.indices.size(),
            "Mesh topology changed during conversion");
    const auto runtime = resource::ResourceManager::instance().load<resource::IMesh>(
        utf8(assets / converter.getLastOutputPath()));
    require(runtime && runtime->getVertexCount() == geometry.vertices.size(),
            "Generated mesh is not loadable through the runtime registry");
}
int build(const fs::path &projectRoot, bool overwrite)
{
    const auto root = fs::absolute(projectRoot);
    const auto assets = root / "Assets";
    if (fs::exists(root / "project.prj") && !overwrite)
        throw std::runtime_error(
            "Project already exists; use --overwrite only to rebuild this generated sample");
    const auto &variants = render::demo::kMaterialVariantSuffixes;
    const auto recipes = render::demo::materialSceneRecipes();
    // Refuse partial replacement before touching any generated mesh or scene.
    if (!overwrite)
    {
        for (const auto *name : meshNames)
            require(!fs::exists(assets / "meshes" / (std::string(name) + meshSuffix)),
                    "Generated mesh exists; rebuild in a new directory or use --overwrite");
        for (const auto &recipe : recipes)
            require(!fs::exists(assets / "worlds" / (recipe.id + ".scn")),
                    "Generated scene exists; rebuild in a new directory or use --overwrite");
    }
    for (const auto *variant : variants)
    {
        const auto check = [&](const char *id) {
            const auto path = assets / "materials" / (std::string(id) + variant + ".mat");
            resource::Material material;
            std::string error;
            require(render::demo::readWorkshopMaterial(path, material, error), error);
        };
        for (const auto *id : render::demo::kMaterialIds)
            check(id);
        for (const auto &study : render::demo::kMaterialStudies)
            check(study.id);
        for (const auto *id : render::demo::kMaterialCutoutIds)
            check(id);
    }
    fs::create_directories(assets / "meshes");
    fs::create_directories(assets / "worlds");
    require(resource::initializeLoaders(), "Resource loader registration failed");
    writeMesh(assets, "cube", render::demo::galleryCube());
    writeMesh(assets, "sphere", render::demo::gallerySphere());
    writeMesh(assets, "cylinder", render::demo::galleryCylinder());
    writeMesh(assets, "torus", render::demo::galleryTorus());
    auto &registry = entity::ComponentRegistry::instance();
    require(bool(entity::registerEntityCoreComponents(registry)), "Core component registration failed");
    require(bool(entity::registerEntityRenderComponents(registry)), "Render component registration failed");
    editor::EditorProjectDescriptor project;
    project.schemaVersion = 2;
    project.id = "material-workshop";
    project.displayName = "Material Workshop";
    project.engineProfile = "CLIENT_3D";
    project.assetRoot = "Assets";
    project.defaultSceneView = "3D";
    project.startupWorld = "gallery";
    editor::EditorSceneViewWorkspace views;
    std::string viewError;
    require(views.open(utf8(root), &viewError), viewError);
    for (const auto &recipe : recipes)
    {
        const auto *variant = variants[recipe.variant];
        const auto exhibit = render::demo::makeMaterialExhibit(recipe.scene, recipe.hero);
        const auto &id = recipe.id;
        const auto path = assets / "worlds" / (id + ".scn");
        if (fs::exists(path) && !overwrite)
            throw std::runtime_error("Refusing to replace existing scene: " + utf8(path));
        editor::EditorSceneDocument document;
        size_t serial = 0;
        for (const auto &object : exhibit.objects)
        {
            const auto material =
                std::string(object.cutout >= 0  ? render::demo::kMaterialCutoutIds[size_t(object.cutout)]
                            : object.study >= 0 ? render::demo::kMaterialStudies[size_t(object.study)].id
                                                : render::demo::kMaterialIds[object.material]) +
                variant;
            require(document.createEntity("Add material exhibit",
                                          [&](entity::Entity &e) {
                                              const auto name = material + " / " +
                                                                meshNames[size_t(object.mesh)] + " " +
                                                                std::to_string(++serial);
                                              e.setName(name.c_str());
                                              auto *transform = e.transform();
                                              if (!transform)
                                                  return false;
                                              transform->position = object.position;
                                              transform->scale = object.scale;
                                              transform->rotation =
                                                  math::FQuaternion::fromAxisAngle({0, 1, 0}, object.angle);
                                              auto *mesh = e.addComponent<entity::MeshComponent>();
                                              if (!mesh)
                                                  return false;
                                              mesh->meshPath = std::string("meshes/") +
                                                               meshNames[size_t(object.mesh)] + meshSuffix;
                                              mesh->materialPath = "materials/" + material + ".mat";
                                              return true;
                                          }),
                    "Could not create material object");
        }
        require(document.createEntity(
                    "Add showcase camera",
                    [&](entity::Entity &e) {
                        e.setName("Showcase Camera");
                        auto *transform = e.transform();
                        if (!transform)
                            return false;
                        transform->position = exhibit.cameraPosition;
                        const auto direction = exhibit.cameraTarget - exhibit.cameraPosition;
                        const float yaw = std::atan2(direction.x, direction.z);
                        const float pitch = std::atan2(
                            -direction.y, std::sqrt(direction.x * direction.x + direction.z * direction.z));
                        transform->rotation = math::FQuaternion::fromAxisAngle({0, 1, 0}, yaw) *
                                              math::FQuaternion::fromAxisAngle({1, 0, 0}, pitch);
                        auto *camera = e.createComponent<entity::PerspectiveCameraComponent>();
                        if (!camera)
                            return false;
                        camera->fovYDegrees = exhibit.fov;
                        camera->priority = 10;
                        return true;
                    }),
                "Could not create showcase camera");
        std::string error;
        require(document.saveAs(utf8(path), &error), error);
        editor::EditorSceneDocument reopened;
        require(reopened.open(utf8(path), &error), error);
        require(reopened.scene().world().getAllEntities().size() ==
                    document.scene().world().getAllEntities().size(),
                "Scene entity count changed on reopen");
        entity::PerspectiveCameraFrame cameraFrame;
        require(entity::evaluatePerspectiveCamera3D(reopened.scene().world(), 1.6f, 1, cameraFrame),
                "Saved Scene camera could not be evaluated");
        const auto targetInView =
            cameraFrame.view *
            math::FVector4{exhibit.cameraTarget.x, exhibit.cameraTarget.y, exhibit.cameraTarget.z, 1};
        require(targetInView.z > 0 && std::abs(targetInView.x) < .001f && std::abs(targetInView.y) < .001f,
                "Saved Scene camera does not face its authored target");
        project.worlds.push_back({id, "worlds/" + id + ".scn"});
        if (!views.find(utf8(path)))
        {
            const auto direction = exhibit.cameraTarget - exhibit.cameraPosition;
            editor::EditorSceneCameraState view;
            view.threeDEye = exhibit.cameraPosition;
            view.threeDYawRadians = std::atan2(direction.x, direction.z);
            view.threeDPitchRadians =
                std::atan2(direction.y, std::sqrt(direction.x * direction.x + direction.z * direction.z));
            views.set(utf8(path), view);
        }
        std::cout << "Saved " << id << " (" << serial << " objects + camera)\n";
    }
    std::string error;
    require(project.save(utf8(root), &error), error);
    require(views.save(&error), error);
    const auto validation = editor::EditorProjectRuntimeValidator::validate(
        utf8(root), editor::EditorRuntimeValidationProfile::FullClient);
    for (const auto &issue : validation.issues)
        std::cerr << issue.path << ": " << issue.message << '\n';
    require(bool(validation) && validation.scenes == project.worlds.size(),
            "Material Workshop runtime validation failed");
    std::cout << "Material Workshop ready: " << utf8(root / "project.prj") << '\n';
    return 0;
}
int run(int argc, char **argv)
{
    try
    {
        if (argc < 2 || argc > 3 || (argc == 3 && std::string(argv[2]) != "--overwrite"))
        {
            std::cerr << "AYEditor_MaterialSampleTool <MaterialWorkshop-directory> [--overwrite]\n";
            return 2;
        }
        return build(fs::u8path(argv[1]), argc == 3);
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
} // namespace
#ifdef _WIN32
int wmain(int argc, wchar_t **argv)
{
    ayt::render::demo::Utf8Arguments args(argc, argv);
    return run(argc, args.pointers.data());
}
#else
int main(int argc, char **argv)
{
    return run(argc, argv);
}
#endif
