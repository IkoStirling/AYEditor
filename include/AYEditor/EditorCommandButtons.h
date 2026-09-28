#pragma once
#include <AYEditor/EditorExtension.h>
#include <AYUI/Box.h>
#include <AYUI/Button.h>
#include <memory>
#include <utility>
#include <vector>

namespace ayt::editor {
/** @brief Bind toolbar buttons to the same command target used by shortcuts.
 * Resolve the current target per invocation; no commands or history are registered here.
 * Host calls refresh after state changes and detach before its Widget tree is destroyed.
 * Detach/destruction invalidates retained callbacks without dereferencing old widgets.
 */
class EditorCommandButtons {
public:
    using Target = std::function<IEditorCommandTarget*()>;
    explicit EditorCommandButtons(Target target) : _state(std::make_shared<State>()) {
        _state->target = std::move(target);
    }
    EditorCommandButtons(const EditorCommandButtons&) = delete;
    EditorCommandButtons& operator=(const EditorCommandButtons&) = delete;
    ~EditorCommandButtons() { detach(); }
    ayt::ui::Button* add(ayt::ui::HBox& row, const std::wstring& label,
                        const std::string& commandId, float width) {
        auto* button = new ayt::ui::Button(); button->setText(label);
        button->setId("command_" + commandId); button->setPadding(7, 3, 7, 3);
        const auto state = _state;
        button->setOnClicked([state, commandId] { state->invoke(commandId); });
        row.addWidget(button, width); _buttons.push_back({button, commandId}); refresh(); return button;
    }
    void refresh() {
        auto* target = _state->target ? _state->target() : nullptr;
        for (const auto& item : _buttons) {
            const bool enabled = target && target->handlesCommand(item.id) && target->canExecuteCommand(item.id);
            if (item.button->isEnabled() != enabled) item.button->setEnabled(enabled);
        }
    }
    bool invoke(const std::string& id) { return _state->invoke(id); }
    void detach() noexcept { _state->target = {}; _buttons.clear(); }
private:
    struct State {
        Target target;
        bool invoke(const std::string& id) {
            auto* owner = target ? target() : nullptr;
            return owner && owner->handlesCommand(id) && owner->canExecuteCommand(id) && owner->executeCommand(id);
        }
    };
    struct Binding { ayt::ui::Button* button; std::string id; };
    std::shared_ptr<State> _state;
    std::vector<Binding> _buttons;
};
} // namespace ayt::editor
