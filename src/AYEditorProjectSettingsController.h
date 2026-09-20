#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace ayt::ui { class UIManager; }

namespace ayt::editor {

enum class EditorProjectPathKind {
    GameFlow,
    GameFlowContract,
    UiFlow,
    Scene,
    Tilemap,
    GameAssembly,
    Executable,
    BuildArtifact,
};

struct EditorProjectSettingsConfig {
    std::string projectRoot;
    std::function<std::string(EditorProjectPathKind)> choosePath;
    std::function<bool(const std::string&)> openGameFlow;
    std::function<bool(const std::string&)> openUiFlow;
    std::function<bool()> runProject;
    std::function<std::wstring(std::string_view, std::wstring_view)> localize;
};

// Modeless controller for the Project Settings / Build & Package window.
// The controller owns editable snapshots; nothing is written until Save All.
class EditorProjectSettingsController final {
public:
    explicit EditorProjectSettingsController(EditorProjectSettingsConfig config);
    ~EditorProjectSettingsController();

    EditorProjectSettingsController(
        const EditorProjectSettingsController&) = delete;
    EditorProjectSettingsController& operator=(
        const EditorProjectSettingsController&) = delete;

    bool attach(ayt::ui::UIManager& ui, std::string* error = nullptr);
    void detach();
    void tick(float dtSeconds);
    bool save(std::string* error = nullptr);

    bool isAttached() const noexcept;
    bool isBusy() const noexcept;
    bool isDirty() const noexcept;
    const std::string& lastError() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ayt::editor
