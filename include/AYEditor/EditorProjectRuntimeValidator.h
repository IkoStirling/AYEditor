#pragma once

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

struct EditorRuntimeValidationResult {
    EditorRuntimeValidationProfile profile =
        EditorRuntimeValidationProfile::Headless;
    std::size_t scenes = 0;
    std::size_t uiLayouts = 0;
    std::size_t tilemaps = 0;
    std::vector<EditorRuntimeValidationIssue> issues;

    explicit operator bool() const noexcept { return issues.empty(); }
    std::size_t checked() const noexcept { return scenes + uiLayouts + tilemaps; }
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
