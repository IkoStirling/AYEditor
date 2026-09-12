#pragma once

#include "AYEditor/EditorExtension.h"

#include <AYApplication/GameFlowActionRegistry.h>
#include <AYApplication/GameFlowCoordinator.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ayt::editor
{

inline constexpr std::size_t kEditorGameFlowNoIndex =
    (std::numeric_limits<std::size_t>::max)();

enum class EditorGameFlowObjectKind : std::uint8_t
{
    Document,
    Intent,
    IntentField,
    State,
    Transition,
    Guard,
    Action,
    ActionArgument,
};

// Selections contain serialized identities and ordinals instead of addresses,
// so document copies, undo/redo, and vector reallocations cannot dangle them.
struct EditorGameFlowSelection
{
    EditorGameFlowObjectKind kind = EditorGameFlowObjectKind::Document;
    std::string id;
    // Intent for a field; Transition for a guard/action/argument.
    std::string ownerId;
    // Action ordinal for Action and ActionArgument.
    std::size_t index = kEditorGameFlowNoIndex;

    friend bool operator==(const EditorGameFlowSelection&,
                           const EditorGameFlowSelection&) = default;
};

struct EditorGameFlowOutlineItem
{
    EditorGameFlowSelection selection;
    std::string label;
    int depth = 0;
};

// Fixed projection consumed by a generic inspector. The labels method defines
// the meaning of each slot for the selected object kind.
struct EditorGameFlowProperties
{
    std::string id;
    std::string first;
    std::string second;
    std::string third;
    std::string fourth;
    std::string fifth;
    std::string sixth;
    double number = 0.0;
    std::int32_t integer = 0;
    bool flag = false;
    ayt::app::GameFlowValue value;
};

struct EditorGameFlowPropertyLabels
{
    std::string first;
    std::string second;
    std::string third;
    std::string fourth;
    std::string fifth;
    std::string sixth;
    std::string number;
    std::string integer;
    std::string flag;
    std::string value;
};

struct EditorGameFlowArgumentView
{
    std::string id;
    ayt::app::GameFlowValueType type = ayt::app::GameFlowValueType::String;
    ayt::app::GameFlowValue value;
    bool required = false;
    bool authored = false;
    bool known = false;
};

class EditorGameFlowDocument final : public IEditorDocument
{
public:
    using ChangedHandler = std::function<void()>;

    bool initialize(const std::string& path,
                    const std::string& displayPath,
                    std::string* error = nullptr);
    void createNew();

    const std::string& typeId() const noexcept override { return _typeId; }
    const std::string& path() const noexcept override { return _path; }
    const std::string& title() const noexcept override { return _title; }
    bool isDirty() const noexcept override { return _dirty; }
    std::uint64_t revision() const noexcept override { return _revision; }

    bool save(std::string* error = nullptr) override;
    bool canSaveAs() const noexcept override { return true; }
    bool saveAs(const std::string& path,
                std::string* error = nullptr) override;
    bool canReload() const noexcept override { return !_path.empty(); }
    bool reload(std::string* error = nullptr) override;
    bool writeRecoveryCopy(const std::string& path,
                           std::string* error = nullptr) const override;

    const ayt::app::GameFlowDocument& flow() const noexcept { return _flow; }
    const std::vector<ayt::app::GameFlowDiagnostic>& diagnostics() const noexcept
    {
        return _diagnostics;
    }
    bool isValid() const noexcept { return _valid; }

    ayt::app::GameFlowActionRegistry& actionRegistry() noexcept
    {
        return _registry;
    }
    const ayt::app::GameFlowActionRegistry& actionRegistry() const noexcept
    {
        return _registry;
    }
    // Call after composing metadata-only action/guard registrations.
    void actionRegistryChanged();
    bool buildPlan(ayt::app::GameFlowPlan& plan,
                   std::vector<ayt::app::GameFlowDiagnostic>* diagnostics = nullptr)
        const;

    const EditorGameFlowSelection& selection() const noexcept
    {
        return _selection;
    }
    bool select(EditorGameFlowSelection selection);
    std::vector<EditorGameFlowOutlineItem> outline() const;
    EditorGameFlowProperties selectedProperties() const;
    EditorGameFlowPropertyLabels selectedPropertyLabels() const;
    bool applySelectedProperties(const EditorGameFlowProperties& properties,
                                 std::string* error = nullptr);

    bool addObject(EditorGameFlowObjectKind kind,
                   std::string ownerId = {},
                   std::string* error = nullptr);
    bool addIntentField(std::string intentId,
                        ayt::app::GameFlowFieldDefinition field,
                        std::string* error = nullptr);
    bool addAction(std::string transitionId,
                   std::string actionType,
                   std::string* error = nullptr);
    bool setTransitionGuard(std::string transitionId,
                            std::string guardType,
                            std::string* error = nullptr);
    bool moveSelectedAction(std::ptrdiff_t offset,
                            std::string* error = nullptr);
    bool deleteSelection(std::string* error = nullptr);

    std::vector<EditorGameFlowArgumentView> selectedArguments() const;
    bool setSelectedArgument(std::string argumentId,
                             ayt::app::GameFlowValue value,
                             std::string* error = nullptr);
    bool clearSelectedArgument(std::string_view argumentId,
                               std::string* error = nullptr);

    bool canUndo() const noexcept { return !_undo.empty(); }
    bool canRedo() const noexcept { return !_redo.empty(); }
    bool undo();
    bool redo();

    void setChangedHandler(ChangedHandler handler)
    {
        _changed = std::move(handler);
    }

    static const char* kindName(EditorGameFlowObjectKind kind) noexcept;

private:
    struct Snapshot
    {
        ayt::app::GameFlowDocument flow;
        EditorGameFlowSelection selection;
        bool dirty = false;
    };

    bool loadFromPath(const std::string& path,
                      const std::string& displayPath,
                      std::string* error);
    bool writeToPath(const std::string& path, std::string* error) const;
    void updateTitle(const std::string& displayPath = {});
    void refreshDiagnostics();
    void commitMutation(Snapshot before);
    Snapshot snapshot() const;
    void restore(Snapshot value);
    bool selectionExists(const EditorGameFlowSelection& selection) const;
    std::string uniqueId(EditorGameFlowObjectKind kind,
                         std::string_view ownerId = {}) const;
    bool renameSelection(std::string newId, std::string* error);
    bool selectionIsReferenced(std::string& reference) const;
    void notifyChanged();

    std::string _typeId = "ayeditor.game-flow.document";
    std::string _path;
    std::string _title = "Untitled Game Flow";
    ayt::app::GameFlowDocument _flow;
    ayt::app::GameFlowActionRegistry _registry;
    std::vector<ayt::app::GameFlowDiagnostic> _diagnostics;
    EditorGameFlowSelection _selection;
    std::vector<Snapshot> _undo;
    std::vector<Snapshot> _redo;
    ChangedHandler _changed;
    bool _valid = false;
    bool _dirty = false;
    std::uint64_t _revision = 1;
};

} // namespace ayt::editor
