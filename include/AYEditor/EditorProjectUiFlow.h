#pragma once

#include "AYEditor/EditorProjectDescriptor.h"

#include <AYUI/UIFlow.h>

#include <string>
#include <string_view>
#include <vector>

namespace ayt::editor {

enum class EditorProjectUiFlowSource {
    None,
    Asset,
    LegacyWorldLayouts,
};

struct EditorProjectWorldUiContextBinding {
    std::string worldId;
    std::string contextId;
};

// Pure project-to-Flow composition result. Stage 1 does not instantiate a
// UIManager or subscribe to Scene events; later host/runtime stages consume
// this same object.
struct EditorProjectUiFlowResolution {
    EditorProjectUiFlowSource source = EditorProjectUiFlowSource::None;
    std::string flowAsset;
    std::string entry;
    ayt::ui::UIFlowDocument compatibilityFlow;
    std::vector<EditorProjectWorldUiContextBinding> worldContexts;

    explicit operator bool() const noexcept {
        return source != EditorProjectUiFlowSource::None;
    }

    std::string_view contextForWorld(std::string_view worldId) const noexcept;
};

// Resolves the additive project UI contract:
// - project ui.flow + per-World uiContext selects an external .uiflow.json;
// - old per-World ui strings become a validated implicit World-scope Flow;
// - mixing both forms is rejected rather than creating duplicate Screens.
bool resolveEditorProjectUiFlow(
    const EditorProjectDescriptor& descriptor,
    EditorProjectUiFlowResolution& resolution,
    std::string* error = nullptr);

} // namespace ayt::editor
