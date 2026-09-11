#pragma once

#include <AYUI/UIFlow.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

// Logical live preview backed by the production UIFlowRuntime. The screen host
// records mounted presentation rather than loading pixels; Stage 5 performs
// full visual layout integration. This still exercises the exact Context,
// Slot, Scope, transition, guard, graph, and action runtime paths.
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
