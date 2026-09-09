#pragma once

#include "AYEditor/EditorExtension.h"
#include "AYMath/MathTypes.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ayt::entity { class World; }

namespace ayt::editor {

struct EditorTransformState {
    ayt::math::FVector3 position{};
    ayt::math::FQuaternion rotation{};
    ayt::math::FVector3 scale{1.0f, 1.0f, 1.0f};
};

class EditorCommandStack : public IEditorCommandTarget {
public:
    using ChangedCallback = std::function<void()>;

    bool executeTransform(ayt::entity::World& world, uint32_t entityId,
                          const EditorTransformState& after);
    bool undo();
    bool redo();
    void clear() noexcept;
    bool canUndo() const noexcept { return !_undo.empty(); }
    bool canRedo() const noexcept { return !_redo.empty(); }
    void setChangedCallback(ChangedCallback callback) { _changed = std::move(callback); }

    bool handlesCommand(const std::string& commandId) const override;
    bool canExecuteCommand(const std::string& commandId) const override;
    bool executeCommand(const std::string& commandId) override;

private:
    struct TransformCommand {
        ayt::entity::World* world = nullptr;
        uint32_t entityId = 0;
        EditorTransformState before;
        EditorTransformState after;
    };

    static bool apply(const TransformCommand& command, bool useAfter);
    void notifyChanged();

    std::vector<TransformCommand> _undo;
    std::vector<TransformCommand> _redo;
    ChangedCallback _changed;
};

} // namespace ayt::editor
