#include "AYEditor/EditorProjectDescriptor.h"

#include <AYIO/File.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <utility>

namespace ayt::editor {
namespace {

namespace fs = std::filesystem;

bool hasWindowsDrivePrefix(std::string_view value) noexcept
{
    if (value.size() < 2 || value[1] != ':') return false;
    const char letter = value.front();
    return (letter >= 'A' && letter <= 'Z')
        || (letter >= 'a' && letter <= 'z');
}

bool isPortableRelativePath(const std::string& value)
{
    if (value.empty() || value.find('\\') != std::string::npos
        || hasWindowsDrivePrefix(value)) return false;
    const fs::path path(value);
    if (path.is_absolute() || path.has_root_name()
        || path.has_root_directory()) return false;
    for (const fs::path& part : path) {
        if (part == "..") return false;
    }
    return true;
}

bool endsWith(std::string_view value, std::string_view suffix) noexcept
{
    if (value.size() < suffix.size()) return false;
    const std::size_t offset = value.size() - suffix.size();
    for (std::size_t index = 0; index < suffix.size(); ++index) {
        const auto actual = static_cast<unsigned char>(value[offset + index]);
        const auto expected = static_cast<unsigned char>(suffix[index]);
        if (std::tolower(actual) != std::tolower(expected)) return false;
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

bool readOptionalString(const nlohmann::json& object, const char* key,
                        std::string& value, std::string& error)
{
    const auto found = object.find(key);
    if (found == object.end()) return true;
    if (!found->is_string()) {
        error = std::string("Project descriptor '") + key
            + "' must be a string.";
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

bool EditorProjectDescriptor::validate(std::string* error) const
{
    if (error != nullptr) error->clear();
    const auto reject = [error](std::string message) {
        if (error != nullptr) *error = std::move(message);
        return false;
    };
    if (schemaVersion != kEditorProjectDescriptorSchemaVersion) {
        return reject("Unsupported project descriptor schemaVersion: "
            + std::to_string(schemaVersion));
    }
    if (id.empty()) {
        return reject("Project descriptor requires a non-empty 'id' string.");
    }
    if (!isPortableRelativePath(assetRoot)
        || (!gameAssembly.empty() && !isPortableRelativePath(gameAssembly))
        || (!gameCodeRoot.empty() && !isPortableRelativePath(gameCodeRoot))) {
        return reject(
            "Project descriptor paths must stay inside the project root.");
    }
    if (defaultSceneView != "Auto" && defaultSceneView != "2D"
        && defaultSceneView != "3D") {
        return reject("editor.defaultSceneView must be Auto, 2D, or 3D.");
    }
    if ((!ui.flow.empty() && !isPortableRelativePath(ui.flow))
        || (ui.flow.empty() && !ui.entry.empty())) {
        return reject(ui.flow.empty()
            ? "Project ui.entry requires ui.flow."
            : "Project ui.flow must stay inside the asset root.");
    }
    if (!startupFlow.empty()
        && (!isPortableRelativePath(startupFlow)
            || !endsWith(startupFlow, ".gameflow.json"))) {
        return reject("Project startupFlow must be a relative "
            "*.gameflow.json path inside the asset root.");
    }
    if (!gameFlowContract.empty()
        && !isPortableRelativePath(gameFlowContract)) {
        return reject("Project gameFlow.contract must stay inside the asset root.");
    }

    std::unordered_set<std::string> worldIds;
    for (const EditorProjectWorldDescriptor& world : worlds) {
        if (world.id.empty()) {
            return reject("Project World id must not be empty.");
        }
        if (!worldIds.insert(world.id).second) {
            return reject("Duplicate project World id: " + world.id);
        }
        if (!isPortableRelativePath(world.scene)
            || (!world.ui.empty() && !isPortableRelativePath(world.ui))) {
            return reject(
                "World content paths must stay inside the asset root.");
        }
        for (const std::string& tilemap : world.tilemaps) {
            if (!isPortableRelativePath(tilemap)) {
                return reject("World tilemap paths must be relative strings.");
            }
        }
    }
    if (!startupWorld.empty() && !worldIds.contains(startupWorld)) {
        return reject("startupWorld is not present in worlds: "
            + startupWorld);
    }
    return true;
}

bool EditorProjectDescriptor::serialize(
    std::string& jsonText, std::string* error) const
{
    if (error != nullptr) error->clear();
    if (!validate(error)) return false;
    try {
        nlohmann::json root = {
            {"schemaVersion", schemaVersion},
            {"id", id},
            {"displayName", displayName},
            {"engineProfile", engineProfile},
            {"paths", {
                {"assets", assetRoot},
                {"gameAssembly", gameAssembly},
                {"gameCode", gameCodeRoot},
            }},
            {"editor", {{"defaultSceneView", defaultSceneView}}},
            {"worlds", nlohmann::json::array()},
            {"run", {
                {"executable", run.executable},
                {"workingDirectory", run.workingDirectory},
                {"arguments", run.arguments},
            }},
        };
        if (!ui.flow.empty() || !ui.entry.empty()) {
            root["ui"] = {{"flow", ui.flow}, {"entry", ui.entry}};
        }
        if (!startupFlow.empty()) root["startupFlow"] = startupFlow;
        if (!gameFlowContract.empty()) {
            root["gameFlow"] = {{"contract", gameFlowContract}};
        }
        if (!startupWorld.empty()) root["startupWorld"] = startupWorld;
        for (const EditorProjectWorldDescriptor& world : worlds) {
            nlohmann::json value = {
                {"id", world.id},
                {"scene", world.scene},
                {"tilemaps", world.tilemaps},
            };
            if (!world.ui.empty()) value["ui"] = world.ui;
            if (!world.uiContext.empty()) {
                value["uiContext"] = world.uiContext;
            }
            root["worlds"].push_back(std::move(value));
        }
        jsonText = root.dump(2);
        jsonText.push_back('\n');
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = std::string("Could not serialize project descriptor: ")
                + exception.what();
        }
        return false;
    }
}

bool EditorProjectDescriptor::save(
    const std::string& projectRoot, std::string* error) const
{
    std::string encoded;
    if (!serialize(encoded, error)) return false;
    try {
        const fs::path root = fs::absolute(projectRoot.empty()
            ? fs::current_path() : fs::path(projectRoot)).lexically_normal();
        std::error_code directoryError;
        fs::create_directories(root, directoryError);
        if (directoryError) {
            if (error != nullptr) {
                *error = "Could not create project directory: "
                    + directoryError.message();
            }
            return false;
        }
        const fs::path path = root / kEditorProjectDescriptorFile;
        if (!ayt::io::File::atomicWrite(
                path.string(), encoded.data(), encoded.size())) {
            if (error != nullptr) {
                *error = "Atomic project descriptor save failed: "
                    + path.string();
            }
            return false;
        }
        if (error != nullptr) error->clear();
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = std::string("Could not save project descriptor: ")
                + exception.what();
        }
        return false;
    }
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
        const auto schemaVersion = json.find("schemaVersion");
        bool supportedSchema = false;
        if (schemaVersion != json.end()) {
            if (schemaVersion->is_number_unsigned()) {
                supportedSchema = schemaVersion->get<std::uint64_t>()
                    == kEditorProjectDescriptorSchemaVersion;
            } else if (schemaVersion->is_number_integer()) {
                supportedSchema = schemaVersion->get<std::int64_t>()
                    == static_cast<std::int64_t>(
                        kEditorProjectDescriptorSchemaVersion);
            }
        }
        if (!supportedSchema) {
            if (error != nullptr) {
                *error = "Project descriptor requires integer schemaVersion "
                    + std::to_string(kEditorProjectDescriptorSchemaVersion)
                    + ".";
            }
            return {};
        }
        result.schemaVersion = kEditorProjectDescriptorSchemaVersion;
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

        const nlohmann::json editor = json.value(
            "editor", nlohmann::json::object());
        if (!editor.is_object()) {
            if (error != nullptr) *error = "Project editor settings must be an object.";
            return {};
        }
        result.defaultSceneView = editor.value(
            "defaultSceneView", std::string("Auto"));
        if (result.defaultSceneView != "Auto"
            && result.defaultSceneView != "2D"
            && result.defaultSceneView != "3D") {
            if (error != nullptr) {
                *error = "editor.defaultSceneView must be Auto, 2D, or 3D.";
            }
            return {};
        }

        if (const auto ui = json.find("ui"); ui != json.end()) {
            if (!ui->is_object()) {
                if (error != nullptr) {
                    *error = "Project ui settings must be an object.";
                }
                return {};
            }
            if (!readOptionalString(*ui, "flow", result.ui.flow, parseError)
                || !readOptionalString(*ui, "entry", result.ui.entry,
                                       parseError)) {
                if (error != nullptr) *error = parseError;
                return {};
            }
            if ((!result.ui.flow.empty()
                    && !isPortableRelativePath(result.ui.flow))
                || (result.ui.flow.empty() && !result.ui.entry.empty())) {
                if (error != nullptr) {
                    *error = result.ui.flow.empty()
                        ? "Project ui.entry requires ui.flow."
                        : "Project ui.flow must stay inside the asset root.";
                }
                return {};
            }
        }

        if (!readOptionalString(
                json, "startupWorld", result.startupWorld, parseError)
            || !readOptionalString(
                json, "startupFlow", result.startupFlow, parseError)) {
            if (error != nullptr) *error = parseError;
            return {};
        }
        if (!result.startupFlow.empty()
            && (!isPortableRelativePath(result.startupFlow)
                || !endsWith(result.startupFlow, ".gameflow.json"))) {
            if (error != nullptr) {
                *error = "Project startupFlow must be a relative "
                    "*.gameflow.json path inside the asset root.";
            }
            return {};
        }
        if (const auto gameFlow = json.find("gameFlow");
            gameFlow != json.end()) {
            if (!gameFlow->is_object()
                || !readOptionalString(*gameFlow, "contract",
                    result.gameFlowContract, parseError)) {
                if (error != nullptr) {
                    *error = parseError.empty()
                        ? "Project gameFlow settings must be an object."
                        : parseError;
                }
                return {};
            }
            if (!result.gameFlowContract.empty()
                && !isPortableRelativePath(result.gameFlowContract)) {
                if (error != nullptr) {
                    *error = "Project gameFlow.contract must stay inside "
                        "the asset root.";
                }
                return {};
            }
        }
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
            if (!readOptionalString(value, "ui", world.ui, parseError)
                || !readOptionalString(value, "uiContext", world.uiContext,
                                       parseError)) {
                if (error != nullptr) *error = parseError;
                return {};
            }
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
        if (!result.validate(&parseError)) {
            if (error != nullptr) *error = std::move(parseError);
            return {};
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

std::string resolveEditorProjectRoot(
    const std::string& selectedPath, std::string* error)
{
    if (error != nullptr) error->clear();
    if (selectedPath.empty()) {
        if (error != nullptr) *error = "Choose a project folder or project manifest.";
        return {};
    }

    try {
        std::error_code filesystemError;
        fs::path selected = fs::absolute(
            fs::u8path(selectedPath), filesystemError).lexically_normal();
        if (filesystemError) {
            if (error != nullptr) {
                *error = "Cannot resolve the selected project path: "
                    + filesystemError.message();
            }
            return {};
        }

        fs::path root;
        if (fs::is_directory(selected, filesystemError)) {
            root = selected;
        } else if (!filesystemError
                   && fs::is_regular_file(selected, filesystemError)) {
            if (selected.filename() != kEditorProjectDescriptorFile) {
                if (error != nullptr) {
                    *error = "Select project.ayproject.json or its containing folder.";
                }
                return {};
            }
            root = selected.parent_path();
        } else {
            if (error != nullptr) {
                *error = filesystemError
                    ? "Cannot inspect the selected project path: "
                        + filesystemError.message()
                    : "The selected project path does not exist.";
            }
            return {};
        }

        filesystemError.clear();
        const fs::path canonical = fs::weakly_canonical(root, filesystemError);
        if (!filesystemError) root = canonical;

        std::string descriptorError;
        const EditorProjectDescriptor descriptor =
            EditorProjectDescriptor::load(root.string(), &descriptorError);
        if (!descriptor) {
            if (error != nullptr) {
                *error = descriptorError.empty()
                    ? "The selected folder is not a valid Aliyat project."
                    : descriptorError;
            }
            return {};
        }
        return root.string();
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = std::string("Cannot open the selected project: ")
                + exception.what();
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
        result.assetRootPath =
            (root / fs::path(descriptor.assetRoot)).lexically_normal().string();
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
