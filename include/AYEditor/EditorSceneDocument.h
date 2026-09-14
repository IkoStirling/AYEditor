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

namespace ayt::scene { class Scene; }

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
    friend class TransformCommand;

    bool applyTransform(uint64_t generation, uint32_t entityId,
                        const EditorTransformState& state);

    std::unique_ptr<ayt::scene::Scene> _scene;
    std::string _typeId = "ayeditor.scene.document";
    std::string _path;
    std::string _title;
    bool _dirty = false;
    uint64_t _revision = 0;
    uint64_t _contentGeneration = 1;
    EditorCommandHistory _history;
    HistoryChangedCallback _historyChanged;
};

} // namespace ayt::editor
