#pragma once

#include "AYEditor/EditorCommandSystem.h"
#include "AYEditor/EditorExtension.h"
#include "AYEditorCommand/EditorCommandHistory.h"
#include "AYMath/MathTypes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

namespace ayt::scene { class Scene; }
namespace ayt::entity { class Entity; class IComponent; }

namespace ayt::editor {

struct EditorTransformState {
    ayt::math::FVector3 position{};
    ayt::math::FQuaternion rotation{};
    ayt::math::FVector3 scale{1.0f, 1.0f, 1.0f};
};

// Editor-owned scene document. The Scene instance remains stable because
// render systems may borrow its World through registered callbacks.
class EditorSceneDocument : public IEditorDocument,
                            public IEditorCommandTarget {
public:
    using HistoryChangedCallback = std::function<void()>;

    EditorSceneDocument();
    ~EditorSceneDocument();

    EditorSceneDocument(const EditorSceneDocument&) = delete;
    EditorSceneDocument& operator=(const EditorSceneDocument&) = delete;

    ayt::scene::Scene& scene();
    const ayt::scene::Scene& scene() const;

    void newScene();
    bool open(const std::string& path, std::string* error = nullptr);
    const std::string& typeId() const noexcept override { return _typeId; }
    bool save(std::string* error = nullptr) override;
    bool canSaveAs() const noexcept override { return true; }
    bool saveAs(const std::string& path,
                std::string* error = nullptr) override;
    bool writeRecoveryCopy(const std::string& path,
                           std::string* error = nullptr) const override;

    bool executeTransform(uint32_t entityId,
                          const EditorTransformState& after,
                          std::string label = "Transform",
                          std::string mergeKey = {});
    bool createEntity(
        std::string label,
        const std::function<bool(ayt::entity::Entity&)>& configure,
        uint32_t* entityId = nullptr);
    bool deleteEntity(uint32_t entityId, std::string label = "Delete Entity");
    bool addComponent(uint32_t entityId, const std::string& componentType,
                      std::vector<std::string>* added = nullptr,
                      std::string* error = nullptr);
    bool removeComponent(uint32_t entityId, const std::string& componentType,
                         std::string* error = nullptr);
    bool mutateComponent(
        uint32_t entityId, const std::string& componentType,
        std::string label, std::string mergeKey,
        const std::function<bool(ayt::entity::IComponent&)>& mutation);
    bool undo();
    bool redo();
    bool canUndo() const noexcept { return _history.canUndo(); }
    bool canRedo() const noexcept { return _history.canRedo(); }
    EditorCommandHistory& commandHistory() noexcept { return _history; }
    const EditorCommandHistory& commandHistory() const noexcept {
        return _history;
    }
    void setHistoryChangedCallback(HistoryChangedCallback callback) {
        _historyChanged = std::move(callback);
    }

    bool handlesCommand(const std::string& commandId) const override;
    bool canExecuteCommand(const std::string& commandId) const override;
    bool executeCommand(const std::string& commandId) override;

    void markDirty() noexcept;
    bool isDirty() const noexcept override;
    const std::string& path() const noexcept override { return _path; }
    const std::string& title() const noexcept override { return _title; }
    uint64_t revision() const noexcept override { return _revision; }

private:
    class TransformCommand;
    class EntityCreateCommand;
    class EntityDeleteCommand;
    class AddComponentCommand;
    class RemoveComponentCommand;
    class ComponentMutationCommand;
    friend class TransformCommand;
    friend class EntityCreateCommand;
    friend class EntityDeleteCommand;
    friend class AddComponentCommand;
    friend class RemoveComponentCommand;
    friend class ComponentMutationCommand;

    struct ComponentSnapshot;
    struct EntitySnapshot;

    bool applyTransform(uint64_t generation, uint32_t entityId,
                        const EditorTransformState& state);
    bool snapshotComponent(ayt::entity::IComponent& component,
                           ComponentSnapshot& snapshot) const;
    bool restoreComponent(ayt::entity::Entity& entity,
                          const ComponentSnapshot& snapshot) const;
    bool snapshotEntity(ayt::entity::Entity& entity,
                        EntitySnapshot& snapshot) const;
    ayt::entity::Entity* restoreEntity(const EntitySnapshot& snapshot);
    uint32_t logicalEntityId(uint32_t currentId) const noexcept;
    ayt::entity::Entity* findCommandEntity(uint32_t logicalId);
    void remapEntity(uint32_t logicalId, uint32_t currentId);

    std::unique_ptr<ayt::scene::Scene> _scene;
    std::string _typeId = "ayeditor.scene.document";
    std::string _path;
    std::string _title;
    bool _dirty = false;
    uint64_t _revision = 0;
    uint64_t _contentGeneration = 1;
    std::unordered_map<uint32_t, uint32_t> _commandEntityIds;
    EditorCommandHistory _history;
    HistoryChangedCallback _historyChanged;
};

} // namespace ayt::editor
