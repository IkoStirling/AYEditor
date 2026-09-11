#pragma once

#include "AYEditor/EditorExtensionRegistry.h"

#include <AYUI/UIFlowGraphNodeRegistry.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ayt::ui {
class UIManager;
}

namespace ayt::editor {

class EditorUiFlowDocument;

inline constexpr const char* kEditorUiFlowExtensionId =
    "ayeditor.ui-flow";

struct EditorUiFlowExtensionConfig {
    std::function<std::string()> chromePath;
    std::function<std::string()> openPathPicker;
    std::function<std::string()> savePathPicker;
    std::string assetRoot;
    std::vector<ayt::ui::UIFlowGraphNodeTypeDefinition> graphNodeTypes;
};

class EditorUiFlowController {
public:
    using StateChanged = std::function<void()>;

    EditorUiFlowController(
        std::shared_ptr<EditorUiFlowDocument> document,
        EditorUiFlowExtensionConfig config);
    ~EditorUiFlowController();

    EditorUiFlowController(const EditorUiFlowController&) = delete;
    EditorUiFlowController& operator=(const EditorUiFlowController&) = delete;

    bool attach(ayt::ui::UIManager& ui);
    void detach();
    bool isAttached() const noexcept;
    void tick(float deltaSeconds);
    bool restartPreview(std::string* error = nullptr);

    void setStateChanged(StateChanged changed);
    const std::shared_ptr<EditorUiFlowDocument>& document() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

EditorDescriptor makeEditorUiFlowDescriptor(
    EditorUiFlowExtensionConfig config = {});
bool registerEditorUiFlowExtension(
    EditorExtensionRegistry& registry,
    EditorUiFlowExtensionConfig config = {},
    std::string* error = nullptr);

} // namespace ayt::editor
