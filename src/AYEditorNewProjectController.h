#pragma once

#include <AYProject/ProjectScaffold.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace ayt::ui { class UIManager; }

namespace ayt::editor {

struct EditorNewProjectRequest {
    std::string parentDirectory;
    std::string engineSourceRoot;
    std::string displayName;
    ayt::project::GameProjectTemplateProfile profile =
        ayt::project::GameProjectTemplateProfile::Client3D;
};

struct EditorNewProjectResult {
    bool success = false;
    std::string projectRoot;
    std::string error;
};

// Thin Editor adapter over AYProject's shared scaffold planner/writer.
// Folder naming follows the same public project-id suggestion as the CLI.
[[nodiscard]] EditorNewProjectResult createEditorGameProject(
    const EditorNewProjectRequest& request);

struct EditorNewProjectConfig {
    std::string engineSourceRoot;
    std::string initialParentDirectory;
    std::function<std::string()> chooseParentDirectory;
    std::function<bool(const std::string&, std::string*)> openProject;
    std::function<std::wstring(std::string_view, std::wstring_view)> localize;
};

class EditorNewProjectController final {
public:
    explicit EditorNewProjectController(EditorNewProjectConfig config);
    ~EditorNewProjectController();

    EditorNewProjectController(const EditorNewProjectController&) = delete;
    EditorNewProjectController& operator=(
        const EditorNewProjectController&) = delete;

    bool attach(ayt::ui::UIManager& ui, std::string* error = nullptr);
    void detach();
    bool createAndOpen(std::string* error = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ayt::editor
