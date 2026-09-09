#include "AYEditor/EditorProjectRuntimeValidator.h"
#include "AYEditor/EditorProjectDescriptor.h"

#include <AY2DEditor/TilemapDocument.h>
#include <AYResource/Loader/TilemapLoader.h>
#include <AYScene.h>
#include <AYUI/LayoutLoader.h>
#include <AYUI/Widget.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace ayt::editor {
namespace {

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
    return value;
}

bool endsWith(const std::string& value, const char* suffix)
{
    const std::size_t length = std::char_traits<char>::length(suffix);
    return value.size() >= length
        && value.compare(value.size() - length, length, suffix) == 0;
}

bool validateHeadlessWidget(const nlohmann::json& widget,
                            std::string& error)
{
    if (!widget.is_object()
        || !widget.contains("type")
        || !widget["type"].is_string()
        || widget["type"].get_ref<const std::string&>().empty()) {
        error = "UI layout widget is missing a string type.";
        return false;
    }
    auto validateChild = [&error](const nlohmann::json& value) {
        return validateHeadlessWidget(value, error);
    };
    if (const auto children = widget.find("children");
        children != widget.end()) {
        if (!children->is_array()) {
            error = "UI layout children must be an array.";
            return false;
        }
        for (const auto& child : *children) {
            if (!validateChild(child)) return false;
        }
    }
    for (const char* key : {"content", "bodyContent"}) {
        if (const auto child = widget.find(key); child != widget.end()
            && !child->is_null() && !validateChild(*child)) {
            return false;
        }
    }
    if (const auto tabs = widget.find("tabs"); tabs != widget.end()) {
        if (!tabs->is_array()) {
            error = "UI layout tabs must be an array.";
            return false;
        }
        for (const auto& tab : *tabs) {
            if (!tab.is_object()) {
                error = "UI layout tab must be an object.";
                return false;
            }
            const auto content = tab.find("content");
            if (content != tab.end() && !content->is_null()
                && !validateChild(*content)) {
                return false;
            }
        }
    }
    return true;
}

bool validateHeadlessUi(const std::filesystem::path& path,
                        std::string& error)
{
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            error = "UI layout file could not be opened.";
            return false;
        }
        nlohmann::json document;
        input >> document;
        const nlohmann::json* root = &document;
        if (const auto wrapped = document.find("root");
            wrapped != document.end()) {
            root = &*wrapped;
        }
        return validateHeadlessWidget(*root, error);
    } catch (const std::exception& exception) {
        error = std::string("UI layout JSON is invalid: ") + exception.what();
        return false;
    }
}

} // namespace

EditorRuntimeValidationResult EditorProjectRuntimeValidator::validate(
    const std::string& projectRoot, EditorRuntimeValidationProfile profile)
{
    EditorRuntimeValidationResult result;
    result.profile = profile;
    const std::filesystem::path root(projectRoot);
    std::string descriptorError;
    const bool hasDescriptor = std::filesystem::is_regular_file(
        root / kEditorProjectDescriptorFile);
    const EditorProjectDescriptor descriptor = hasDescriptor
        ? EditorProjectDescriptor::load(projectRoot, &descriptorError)
        : EditorProjectDescriptor{};
    if (hasDescriptor && !descriptor) {
        result.issues.push_back({
            (root / kEditorProjectDescriptorFile).string(), descriptorError});
        return result;
    }
    const std::filesystem::path assets = root /
        (descriptor ? descriptor.assetRoot : std::string("Assets"));
    if (descriptor) {
        auto requireContent = [&](const std::string& relative,
                                  const char* role) {
            if (relative.empty()) return;
            const std::filesystem::path path = assets / relative;
            if (!std::filesystem::is_regular_file(path)) {
                result.issues.push_back({path.string(),
                    std::string("Project World references a missing ") + role + "."});
            }
        };
        for (const EditorProjectWorldDescriptor& world : descriptor.worlds) {
            requireContent(world.scene, "Scene");
            requireContent(world.ui, "UI layout");
            for (const std::string& tilemap : world.tilemaps) {
                requireContent(tilemap, "Tilemap");
            }
        }
    }
    std::error_code scanError;
    for (std::filesystem::recursive_directory_iterator it(assets,
             std::filesystem::directory_options::skip_permission_denied,
             scanError), end;
         !scanError && it != end; it.increment(scanError)) {
        if (!it->is_regular_file(scanError)) continue;
        const std::string name = lower(it->path().filename().string());
        if (endsWith(name, ".ayscene")) {
            ++result.scenes;
            ayt::scene::Scene scene(ayt::scene::SceneMode::Edit, "validation");
            ayt::serializer::SerializeError error;
            if (!scene.load(it->path().string(), &error)) {
                result.issues.push_back({it->path().string(), error.message});
            }
        } else if (endsWith(name, ".ui.json")) {
            ++result.uiLayouts;
            if (profile == EditorRuntimeValidationProfile::Headless) {
                std::string error;
                if (!validateHeadlessUi(it->path(), error)) {
                    result.issues.push_back({it->path().string(), error});
                }
            } else {
                ayt::ui::UILayoutLoader loader;
                ayt::ui::Widget* root = loader.loadFromFile(
                    it->path().string());
                loader.stopHotReload();
                if (root == nullptr) {
                    result.issues.push_back({it->path().string(),
                        "UI layout loader rejected the file."});
                } else {
                    ayt::ui::destroyWidgetTree(root);
                }
            }
        } else if (endsWith(name, ".aytilemap.json")) {
            ++result.tilemaps;
            ayt::ay2d::editor::TilemapDocument document;
            std::string error;
            if (!document.load(it->path().string(), &error)) {
                result.issues.push_back({it->path().string(), error});
            }
        } else if (endsWith(name, ".aytilemap")) {
            ++result.tilemaps;
            ayt::resource::TilemapLoader loader;
            if (loader.load(it->path().string()) == nullptr) {
                result.issues.push_back({it->path().string(),
                    "Runtime Tilemap loader rejected the cooked file."});
            }
        }
    }
    if (scanError) {
        result.issues.push_back({assets.string(),
            "Project scan failed: " + scanError.message()});
    }
    return result;
}

} // namespace ayt::editor
