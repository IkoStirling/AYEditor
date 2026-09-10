#pragma once

#include "AYEditor/EditorSceneCamera.h"

#include <string>
#include <unordered_map>

namespace ayt::editor {

// Project-local, user-owned Scene View state. The file lives at
// .ayeditor/workspace.json and never enters .ayscene serialization.
class EditorSceneViewWorkspace {
public:
    bool open(const std::string& projectRoot, std::string* error = nullptr);
    bool save(std::string* error = nullptr) const;

    const EditorSceneCameraState* find(
        const std::string& scenePath) const noexcept;
    void set(const std::string& scenePath,
             const EditorSceneCameraState& state);

    const std::string& projectRoot() const noexcept { return _projectRoot; }
    const std::string& path() const noexcept { return _path; }
    bool enabled() const noexcept { return !_path.empty(); }

private:
    std::string keyForScene(const std::string& scenePath) const;

    std::string _projectRoot;
    std::string _path;
    std::unordered_map<std::string, EditorSceneCameraState> _states;
};

} // namespace ayt::editor
