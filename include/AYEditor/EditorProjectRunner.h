#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ayt::editor {

struct EditorProjectRunConfig {
    std::string executable;
    std::string workingDirectory;
    std::vector<std::string> arguments;
    std::string source;

    explicit operator bool() const noexcept { return !executable.empty(); }
};

struct EditorProjectLaunchResult {
    std::uint64_t processId = 0;
    std::string error;

    explicit operator bool() const noexcept {
        return processId != 0 && error.empty();
    }
};

enum class EditorProjectProcessState : std::uint8_t {
    Unavailable,
    Running,
    Exited,
};

// Resolves an app executable from a project-local run manifest or conventional
// CMake output folders, then launches it without coupling app code to editor
// startup. The same service works for BSimmer and future projects.
class EditorProjectRunner final {
public:
    static EditorProjectRunConfig resolve(
        const std::string& projectRoot, std::string* error = nullptr);
    static EditorProjectLaunchResult launch(
        const EditorProjectRunConfig& config);
    static EditorProjectProcessState processState(
        std::uint64_t processId) noexcept;
    static bool focusProcessWindow(std::uint64_t processId) noexcept;
};

} // namespace ayt::editor
