#pragma once

#include <AYUI/UIFlow.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ayt::ui {
class UIManager;
class Widget;
}

namespace ayt::editor {

struct EditorUiFlowPreviewScreen {
    std::uint64_t mountId = 0;
    std::string screenId;
    std::string layerId;
    std::string slotId;
    std::string contextId;
    int layerOrder = 0;
    std::uint32_t orderInLayer = 0;
};

struct EditorUiFlowPreviewTrace {
    std::string category;
    std::string id;
    std::string detail;
};

// Live preview backed by the production UIFlowRuntime. It always records the
// mounted presentation for diagnostics and can additionally mount the real
// Screen widget trees into an editor-owned, clipped viewport.
class EditorUiFlowPreview {
public:
    EditorUiFlowPreview();
    ~EditorUiFlowPreview();

    EditorUiFlowPreview(const EditorUiFlowPreview&) = delete;
    EditorUiFlowPreview& operator=(const EditorUiFlowPreview&) = delete;

    bool rebuild(const ayt::ui::UIFlowDocument& document,
                 std::string_view entry = {},
                 std::string* error = nullptr);
    void stop() noexcept;
    void configureVisualHost(ayt::ui::UIManager& manager,
                             ayt::ui::Widget& parent,
                             std::string assetRoot);
    void clearVisualHost() noexcept;
    void tick(float deltaSeconds);
    bool isRunning() const noexcept;

    bool emitSignal(std::string_view signalId,
                    std::string* error = nullptr);
    bool invokeAction(std::string_view actionId,
                      std::string* error = nullptr);
    void setGuardResult(std::string expression, bool value);
    void clearTrace();

    const std::vector<EditorUiFlowPreviewScreen>& mountedScreens() const;
    const std::map<std::string, std::string>& activeStates() const;
    const std::vector<EditorUiFlowPreviewTrace>& trace() const;
    std::string_view lastError() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ayt::editor
