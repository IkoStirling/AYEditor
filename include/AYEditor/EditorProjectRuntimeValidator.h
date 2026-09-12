#pragma once

#include <AYEditor/EditorVersion.h>

#include <cstddef>
#include <string>
#include <vector>

namespace ayt::editor {

enum class EditorRuntimeValidationProfile {
    Headless,
    FullClient,
};

struct EditorRuntimeValidationIssue {
    std::string path;
    std::string message;
};

struct EditorRuntimeUiFlowDependency {
    std::string flowAsset;
    std::string layoutAsset;
    std::vector<std::string> screens;
};

struct EditorRuntimeGameFlowDependency {
    std::string kind;
    std::string source;
    std::string target;
};

struct EditorRuntimeValidationResult {
    EditorRuntimeValidationProfile profile =
        EditorRuntimeValidationProfile::Headless;
    std::size_t scenes = 0;
    std::size_t uiLayouts = 0;
    std::size_t uiFlows = 0;
    std::size_t tilemaps = 0;
    std::size_t gameFlows = 0;
    std::vector<EditorRuntimeValidationIssue> issues;
    std::vector<EditorRuntimeUiFlowDependency> uiFlowDependencies;
    std::vector<EditorRuntimeGameFlowDependency> gameFlowDependencies;

    explicit operator bool() const noexcept { return issues.empty(); }
    std::size_t checked() const noexcept {
        return scenes + uiLayouts + uiFlows + tilemaps + gameFlows;
    }
};

// Loads every project Scene and Tilemap through their data serializers without
// initializing a window or renderer. Headless UI validation checks the JSON
// document/widget structure without constructing Widgets; FullClient also
// builds the tree through UILayoutLoader.
class EditorProjectRuntimeValidator final {
public:
    static EditorRuntimeValidationResult validate(
        const std::string& projectRoot,
        EditorRuntimeValidationProfile profile);
};

} // namespace ayt::editor
