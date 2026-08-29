#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace ayt::scene { class Scene; }

namespace ayt::editor {

// Editor-owned scene document. The Scene instance remains stable because
// render systems may borrow its World through registered callbacks.
class EditorSceneDocument {
public:
    EditorSceneDocument();
    ~EditorSceneDocument();

    EditorSceneDocument(const EditorSceneDocument&) = delete;
    EditorSceneDocument& operator=(const EditorSceneDocument&) = delete;

    ayt::scene::Scene& scene();
    const ayt::scene::Scene& scene() const;

    void newScene();
    bool open(const std::string& path, std::string* error = nullptr);
    bool save(std::string* error = nullptr);
    bool saveAs(const std::string& path, std::string* error = nullptr);

    void markDirty() noexcept;
    bool isDirty() const noexcept;
    const std::string& path() const noexcept { return _path; }
    const std::string& title() const noexcept { return _title; }
    uint64_t revision() const noexcept { return _revision; }

private:
    std::unique_ptr<ayt::scene::Scene> _scene;
    std::string _path;
    std::string _title;
    bool _dirty = false;
    uint64_t _revision = 0;
};

} // namespace ayt::editor
