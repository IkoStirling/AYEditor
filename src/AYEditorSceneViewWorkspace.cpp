#include "AYEditor/EditorSceneViewWorkspace.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace ayt::editor {
namespace fs = std::filesystem;
namespace {

constexpr int kWorkspaceSchemaVersion = 1;

const char* modeName(SceneViewMode mode) noexcept
{
    return mode == SceneViewMode::TwoD ? "2D" : "3D";
}

const char* projectionName(ProjectionMode mode) noexcept
{
    return mode == ProjectionMode::Orthographic
        ? "Orthographic" : "Perspective";
}

SceneViewMode parseMode(const nlohmann::json& value)
{
    return value.is_string() && value.get<std::string>() == "2D"
        ? SceneViewMode::TwoD : SceneViewMode::ThreeD;
}

ProjectionMode parseProjection(const nlohmann::json& value)
{
    return value.is_string() && value.get<std::string>() == "Orthographic"
        ? ProjectionMode::Orthographic : ProjectionMode::Perspective;
}

float finiteFloat(const nlohmann::json& object,
                  const char* key,
                  float fallback)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number()) return fallback;
    const float value = found->get<float>();
    return std::isfinite(value) ? value : fallback;
}

} // namespace

bool EditorSceneViewWorkspace::open(const std::string& projectRoot,
                                    std::string* error)
{
    if (error != nullptr) error->clear();
    _states.clear();
    _visibility.clear();
    _projectRoot.clear();
    _path.clear();
    if (projectRoot.empty()) return true;

    try {
        _projectRoot = fs::absolute(fs::path(projectRoot)).lexically_normal().string();
        _path = (fs::path(_projectRoot) / ".ayeditor" / "workspace.json").string();
        if (!fs::is_regular_file(_path)) return true;

        std::ifstream input(_path, std::ios::binary);
        nlohmann::json root;
        input >> root;
        if (!root.is_object()
            || root.value("schemaVersion", 0) != kWorkspaceSchemaVersion) {
            if (error != nullptr) {
                *error = "Unsupported editor workspace schema.";
            }
            return false;
        }
        const auto scenes = root.find("scenes");
        if (scenes == root.end()) return true;
        if (!scenes->is_object()) {
            if (error != nullptr) *error = "Workspace scenes must be an object.";
            return false;
        }
        for (auto it = scenes->begin(); it != scenes->end(); ++it) {
            if (!it.value().is_object()) continue;
            const nlohmann::json& entry = it.value();
            EditorSceneCameraState state;
            state.mode = parseMode(entry.value("mode", "3D"));
            state.threeDProjection = parseProjection(
                entry.value("threeDProjection", "Perspective"));
            if (const auto center = entry.find("twoDCenter");
                center != entry.end() && center->is_array()
                && center->size() == 2u
                && (*center)[0].is_number() && (*center)[1].is_number()) {
                state.twoDCenter = {
                    (*center)[0].get<float>(), (*center)[1].get<float>()};
            }
            state.twoDViewHeight = finiteFloat(
                entry, "twoDViewHeight", state.twoDViewHeight);
            state.threeDOrthoHeight = finiteFloat(
                entry, "threeDOrthoHeight", state.threeDOrthoHeight);
            if (const auto eye = entry.find("threeDEye");
                eye != entry.end() && eye->is_array() && eye->size() == 3u
                && (*eye)[0].is_number() && (*eye)[1].is_number()
                && (*eye)[2].is_number()) {
                state.threeDEye = {(*eye)[0].get<float>(),
                                   (*eye)[1].get<float>(),
                                   (*eye)[2].get<float>()};
            }
            state.threeDYawRadians = finiteFloat(
                entry, "threeDYawRadians", state.threeDYawRadians);
            state.threeDPitchRadians = finiteFloat(
                entry, "threeDPitchRadians", state.threeDPitchRadians);
            state.threeDMoveSpeed = finiteFloat(
                entry, "threeDMoveSpeed", state.threeDMoveSpeed);
            _states.emplace(it.key(), state);

            EditorSceneVisibility visibility;
            if (const auto value = entry.find("visibility");
                value != entry.end() && value->is_object()) {
                visibility.meshes = value->value("meshes", true);
                visibility.worldLit2D = value->value("worldLit2D", true);
                visibility.cameraOverlay2D = value->value(
                    "cameraOverlay2D", true);
                visibility.ui = value->value("ui", true);
            }
            _visibility.emplace(it.key(), visibility);
        }
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = std::string("Invalid editor workspace: ") + exception.what();
        }
        _states.clear();
        _visibility.clear();
        return false;
    }
}

bool EditorSceneViewWorkspace::save(std::string* error) const
{
    if (error != nullptr) error->clear();
    if (_path.empty()) return true;
    try {
        nlohmann::json root;
        root["schemaVersion"] = kWorkspaceSchemaVersion;
        root["scenes"] = nlohmann::json::object();
        for (const auto& [key, state] : _states) {
            nlohmann::json entry;
            entry["mode"] = modeName(state.mode);
            entry["threeDProjection"] = projectionName(
                state.threeDProjection);
            entry["twoDPlane"] = "XY";
            entry["twoDCenter"] = {
                state.twoDCenter.x, state.twoDCenter.y};
            entry["twoDViewHeight"] = state.twoDViewHeight;
            entry["threeDOrthoHeight"] = state.threeDOrthoHeight;
            entry["threeDEye"] = {
                state.threeDEye.x, state.threeDEye.y, state.threeDEye.z};
            entry["threeDYawRadians"] = state.threeDYawRadians;
            entry["threeDPitchRadians"] = state.threeDPitchRadians;
            entry["threeDMoveSpeed"] = state.threeDMoveSpeed;
            const EditorSceneVisibility visibility = [&]() {
                const auto found = _visibility.find(key);
                return found != _visibility.end()
                    ? found->second : EditorSceneVisibility{};
            }();
            entry["visibility"] = {
                {"meshes", visibility.meshes},
                {"worldLit2D", visibility.worldLit2D},
                {"cameraOverlay2D", visibility.cameraOverlay2D},
                {"ui", visibility.ui},
            };
            root["scenes"][key] = std::move(entry);
        }
        const fs::path path(_path);
        std::error_code directoryError;
        fs::create_directories(path.parent_path(), directoryError);
        if (directoryError) {
            if (error != nullptr) *error = directoryError.message();
            return false;
        }
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            if (error != nullptr) *error = "Unable to open workspace for writing.";
            return false;
        }
        output << root.dump(2) << '\n';
        if (!output.good()) {
            if (error != nullptr) *error = "Unable to write editor workspace.";
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = std::string("Unable to save editor workspace: ")
                + exception.what();
        }
        return false;
    }
}

const EditorSceneCameraState* EditorSceneViewWorkspace::find(
    const std::string& scenePath) const noexcept
{
    const std::string key = keyForScene(scenePath);
    const auto found = _states.find(key);
    return found != _states.end() ? &found->second : nullptr;
}

void EditorSceneViewWorkspace::set(
    const std::string& scenePath,
    const EditorSceneCameraState& state)
{
    const std::string key = keyForScene(scenePath);
    if (!key.empty()) _states[key] = state;
}

const EditorSceneVisibility* EditorSceneViewWorkspace::findVisibility(
    const std::string& scenePath) const noexcept
{
    const std::string key = keyForScene(scenePath);
    const auto found = _visibility.find(key);
    return found != _visibility.end() ? &found->second : nullptr;
}

void EditorSceneViewWorkspace::setVisibility(
    const std::string& scenePath,
    const EditorSceneVisibility& visibility)
{
    const std::string key = keyForScene(scenePath);
    if (!key.empty()) _visibility[key] = visibility;
}

std::string EditorSceneViewWorkspace::keyForScene(
    const std::string& scenePath) const
{
    if (scenePath.empty()) return {};
    try {
        const fs::path absolute = fs::absolute(fs::path(scenePath)).lexically_normal();
        if (!_projectRoot.empty()) {
            const fs::path relative = absolute.lexically_relative(
                fs::path(_projectRoot));
            if (!relative.empty()) {
                const std::string generic = relative.generic_string();
                if (generic != ".." && !generic.starts_with("../")) {
                    return generic;
                }
            }
        }
        return absolute.generic_string();
    } catch (...) {
        return scenePath;
    }
}

} // namespace ayt::editor
