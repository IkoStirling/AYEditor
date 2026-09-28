#pragma once
#include <filesystem>
#include <string>
namespace ayt::editor::testfixtures {
inline std::string resolveEditorShellLayoutPath() {
    const std::filesystem::path path = AY_EDITOR_TEST_SOURCE_DIR "/ui/editor_shell.ui.json";
    return std::filesystem::is_regular_file(path) ? path.string() : std::string{};
}
}
