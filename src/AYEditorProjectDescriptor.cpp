#include "AYEditor/EditorProjectDescriptor.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <utility>

namespace ayt::editor {
namespace {

namespace fs = std::filesystem;

bool isPortableRelativePath(const std::string& value) noexcept
{
    if (value.empty()) return false;
    const fs::path path(value);
    if (path.is_absolute()) return false;
    for (const fs::path& part : path) {
        if (part == "..") return false;
    }
    return true;
}

bool readRequiredString(const nlohmann::json& object, const char* key,
                        std::string& value, std::string& error)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()
        || found->get_ref<const std::string&>().empty()) {
        error = std::string("Project descriptor requires a non-empty '")
            + key + "' string.";
        return false;
    }
    value = found->get<std::string>();
    return true;
}

} // namespace

EditorProjectDescriptor::operator bool() const noexcept
{
    return schemaVersion == kEditorProjectDescriptorSchemaVersion
        && !id.empty() && !assetRoot.empty() && !sourcePath.empty();
}

const EditorProjectWorldDescriptor* EditorProjectDescriptor::findWorld(
    std::string_view worldId) const noexcept
{
    for (const EditorProjectWorldDescriptor& world : worlds) {
        if (world.id == worldId) return &world;
    }
    return nullptr;
}

EditorProjectDescriptor EditorProjectDescriptor::load(
    const std::string& projectRoot, std::string* error)
{
    if (error != nullptr) error->clear();
    EditorProjectDescriptor result;
    try {
        const fs::path root = fs::absolute(projectRoot.empty()
            ? fs::current_path() : fs::path(projectRoot)).lexically_normal();
        const fs::path path = root / kEditorProjectDescriptorFile;
        if (!fs::is_regular_file(path)) {
            if (error != nullptr) {
                *error = "Project descriptor was not found: " + path.string();
            }
            return {};
        }
        std::ifstream input(path, std::ios::binary);
        nlohmann::json json;
        input >> json;
        if (!json.is_object()) {
            if (error != nullptr) *error = "Project descriptor root must be an object.";
            return {};
        }
        result.schemaVersion = json.value("schemaVersion", 0u);
        if (result.schemaVersion != kEditorProjectDescriptorSchemaVersion) {
            if (error != nullptr) {
                *error = "Unsupported project descriptor schemaVersion: "
                    + std::to_string(result.schemaVersion);
            }
            return {};
        }
        std::string parseError;
        if (!readRequiredString(json, "id", result.id, parseError)) {
            if (error != nullptr) *error = parseError;
            return {};
        }
        result.displayName = json.value("displayName", result.id);
        result.engineProfile = json.value("engineProfile", std::string{});

        const nlohmann::json paths = json.value(
            "paths", nlohmann::json::object());
        result.assetRoot = paths.value("assets", std::string("Assets"));
        result.gameAssembly = paths.value("gameAssembly", std::string{});
        result.gameCodeRoot = paths.value("gameCode", std::string{});
        if (!isPortableRelativePath(result.assetRoot)
            || (!result.gameAssembly.empty()
                && !isPortableRelativePath(result.gameAssembly))
            || (!result.gameCodeRoot.empty()
                && !isPortableRelativePath(result.gameCodeRoot))) {
            if (error != nullptr) {
                *error = "Project descriptor paths must stay inside the project root.";
            }
            return {};
        }

        result.startupWorld = json.value("startupWorld", std::string{});
        const nlohmann::json worlds = json.value(
            "worlds", nlohmann::json::array());
        if (!worlds.is_array()) {
            if (error != nullptr) *error = "Project descriptor worlds must be an array.";
            return {};
        }
        std::unordered_set<std::string> worldIds;
        for (const nlohmann::json& value : worlds) {
            if (!value.is_object()) {
                if (error != nullptr) *error = "Each project World must be an object.";
                return {};
            }
            EditorProjectWorldDescriptor world;
            if (!readRequiredString(value, "id", world.id, parseError)
                || !readRequiredString(value, "scene", world.scene, parseError)) {
                if (error != nullptr) *error = parseError;
                return {};
            }
            if (!worldIds.insert(world.id).second) {
                if (error != nullptr) *error = "Duplicate project World id: " + world.id;
                return {};
            }
            world.ui = value.value("ui", std::string{});
            if (!isPortableRelativePath(world.scene)
                || (!world.ui.empty() && !isPortableRelativePath(world.ui))) {
                if (error != nullptr) {
                    *error = "World content paths must stay inside the asset root.";
                }
                return {};
            }
            if (const auto tilemaps = value.find("tilemaps");
                tilemaps != value.end()) {
                if (!tilemaps->is_array()) {
                    if (error != nullptr) *error = "World tilemaps must be an array.";
                    return {};
                }
                for (const auto& tilemap : *tilemaps) {
                    if (!tilemap.is_string()
                        || !isPortableRelativePath(tilemap.get<std::string>())) {
                        if (error != nullptr) {
                            *error = "World tilemap paths must be relative strings.";
                        }
                        return {};
                    }
                    world.tilemaps.push_back(tilemap.get<std::string>());
                }
            }
            result.worlds.push_back(std::move(world));
        }
        if (!result.startupWorld.empty()
            && result.findWorld(result.startupWorld) == nullptr) {
            if (error != nullptr) {
                *error = "startupWorld is not present in worlds: "
                    + result.startupWorld;
            }
            return {};
        }

        const nlohmann::json run = json.value("run", nlohmann::json::object());
        result.run.executable = run.value("executable", std::string{});
        result.run.workingDirectory = run.value(
            "workingDirectory", std::string("."));
        if (const auto arguments = run.find("arguments");
            arguments != run.end()) {
            if (!arguments->is_array()) {
                if (error != nullptr) *error = "Project run.arguments must be an array.";
                return {};
            }
            for (const auto& argument : *arguments) {
                if (!argument.is_string()) {
                    if (error != nullptr) {
                        *error = "Project run arguments must be strings.";
                    }
                    return {};
                }
                result.run.arguments.push_back(argument.get<std::string>());
            }
        }
        result.sourcePath = path.string();
        return result;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = std::string("Invalid project descriptor: ") + exception.what();
        }
        return {};
    }
}

EditorProjectStartupSceneResolution resolveEditorProjectStartupScene(
    const std::string& projectRoot)
{
    EditorProjectStartupSceneResolution result;
    try {
        const fs::path root = fs::absolute(projectRoot.empty()
            ? fs::current_path() : fs::path(projectRoot)).lexically_normal();
        const fs::path descriptorPath = root / kEditorProjectDescriptorFile;
        std::error_code fileError;
        const bool descriptorExists = fs::exists(descriptorPath, fileError);
        if (fileError) {
            result.projectDescriptorPresent = true;
            result.error = "Unable to inspect project descriptor: "
                + descriptorPath.string() + " (" + fileError.message() + ")";
            return result;
        }
        if (!descriptorExists) {
            return result;
        }

        result.projectDescriptorPresent = true;
        if (!fs::is_regular_file(descriptorPath, fileError) || fileError) {
            result.error = "Project descriptor is not a regular file: "
                + descriptorPath.string();
            return result;
        }
        std::string descriptorError;
        const EditorProjectDescriptor descriptor =
            EditorProjectDescriptor::load(root.string(), &descriptorError);
        if (!descriptor) {
            result.error = descriptorError.empty()
                ? std::string("Project descriptor is invalid.")
                : std::move(descriptorError);
            return result;
        }
        if (descriptor.startupWorld.empty()) {
            return result;
        }

        const EditorProjectWorldDescriptor* startup =
            descriptor.findWorld(descriptor.startupWorld);
        if (startup == nullptr) {
            result.error = "startupWorld is not present in worlds: "
                + descriptor.startupWorld;
            return result;
        }

        const fs::path scenePath =
            (root / fs::path(descriptor.assetRoot) / fs::path(startup->scene))
                .lexically_normal();
        fileError.clear();
        if (!fs::is_regular_file(scenePath, fileError)) {
            result.error = "Project startup Scene was not found: "
                + scenePath.string();
            return result;
        }
        result.scenePath = scenePath.string();
        return result;
    } catch (const std::exception& exception) {
        result.error = std::string("Unable to resolve project startup Scene: ")
            + exception.what();
        return result;
    }
}

} // namespace ayt::editor
