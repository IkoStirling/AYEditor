#pragma once

#include "AYEditor/EditorExtension.h"

#include <cstdint>
#include <memory>
#include <string>

namespace ayt::scene { class Scene; }

namespace ayt::editor {

// Editor-owned scene document. The Scene instance remains stable because
// render systems may borrow its World through registered callbacks.
class EditorSceneDocument : public IEditorDocument {
public:
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

    void markDirty() noexcept;
    bool isDirty() const noexcept override;
    const std::string& path() const noexcept override { return _path; }
    const std::string& title() const noexcept override { return _title; }
    uint64_t revision() const noexcept override { return _revision; }

private:
    std::unique_ptr<ayt::scene::Scene> _scene;
    std::string _typeId = "ayeditor.scene.document";
    std::string _path;
    std::string _title;
    bool _dirty = false;
    uint64_t _revision = 0;
};

} // namespace ayt::editor
