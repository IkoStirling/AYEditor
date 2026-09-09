#include "AYEditor/EditorSceneDocument.h"

#include "AYScene.h"
#include "AYSerializer/SerializeError.h"

#include <filesystem>

namespace ayt::editor {
namespace {

std::string titleForPath(const std::string& path)
{
    const std::string stem = std::filesystem::path(path).stem().string();
    return stem.empty() ? std::string("Untitled") : stem;
}

void setError(std::string* out, const std::string& message)
{
    if (out != nullptr) *out = message;
}

} // namespace

EditorSceneDocument::EditorSceneDocument()
    : _scene(std::make_unique<ayt::scene::Scene>(
          ayt::scene::SceneMode::Edit, "Untitled")),
      _title("Untitled")
{
}

EditorSceneDocument::~EditorSceneDocument() = default;

ayt::scene::Scene& EditorSceneDocument::scene() { return *_scene; }
const ayt::scene::Scene& EditorSceneDocument::scene() const { return *_scene; }

void EditorSceneDocument::newScene()
{
    _scene->clear();
    _path.clear();
    _title = "Untitled";
    _dirty = true;
    ++_revision;
}

bool EditorSceneDocument::open(const std::string& path, std::string* error)
{
    if (path.empty()) {
        setError(error, "Scene path is empty.");
        return false;
    }

    // Scene::load clears its target before reporting failure. Preflight keeps
    // a malformed file from destroying the currently open document.
    ayt::scene::Scene preflight(ayt::scene::SceneMode::Edit, "preflight");
    ayt::serializer::SerializeError preflightError;
    if (!preflight.load(path, &preflightError)) {
        setError(error, preflightError.message);
        return false;
    }

    ayt::serializer::SerializeError loadError;
    if (!_scene->load(path, &loadError)) {
        setError(error, loadError.message);
        return false;
    }

    _path = path;
    _title = titleForPath(path);
    _dirty = false;
    ++_revision;
    return true;
}

bool EditorSceneDocument::save(std::string* error)
{
    if (_path.empty()) {
        setError(error, "Scene has no file path. Use Save As.");
        return false;
    }
    return saveAs(_path, error);
}

bool EditorSceneDocument::saveAs(const std::string& path, std::string* error)
{
    if (path.empty()) {
        setError(error, "Scene path is empty.");
        return false;
    }
    if (!_scene->save(path)) {
        setError(error, "Unable to save scene: " + path);
        return false;
    }

    _path = path;
    _title = titleForPath(path);
    _dirty = false;
    ++_revision;
    return true;
}

bool EditorSceneDocument::writeRecoveryCopy(
    const std::string& path, std::string* error) const
{
    if (path.empty()) {
        setError(error, "Recovery scene path is empty.");
        return false;
    }
    std::error_code directoryError;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), directoryError);
    if (directoryError || !_scene->save(path)) {
        setError(error, "Unable to write scene recovery copy: " + path);
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

void EditorSceneDocument::markDirty() noexcept
{
    _dirty = true;
    ++_revision;
}

bool EditorSceneDocument::isDirty() const noexcept
{
    return _dirty || _scene->isDirty();
}

} // namespace ayt::editor
