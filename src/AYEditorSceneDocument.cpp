#include "AYEditor/EditorSceneDocument.h"

#include "AYEntity/EntityImpl.h"
#include "AYEntity/World.h"
#include "AYEntity/components/TransformComponent.h"
#include "AYScene.h"
#include "AYSerializer/SerializeError.h"

#include <cmath>
#include <filesystem>
#include <utility>

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

EditorTransformState readTransform(const ayt::entity::Transform& transform)
{
    return {transform.position, transform.rotation, transform.scale};
}

bool nearlyEqual(float a, float b)
{
    return std::fabs(a - b) <= 1.0e-6f;
}

bool sameTransform(const EditorTransformState& a,
                   const EditorTransformState& b)
{
    return nearlyEqual(a.position.x, b.position.x)
        && nearlyEqual(a.position.y, b.position.y)
        && nearlyEqual(a.position.z, b.position.z)
        && nearlyEqual(a.rotation.x, b.rotation.x)
        && nearlyEqual(a.rotation.y, b.rotation.y)
        && nearlyEqual(a.rotation.z, b.rotation.z)
        && nearlyEqual(a.rotation.w, b.rotation.w)
        && nearlyEqual(a.scale.x, b.scale.x)
        && nearlyEqual(a.scale.y, b.scale.y)
        && nearlyEqual(a.scale.z, b.scale.z);
}

} // namespace

class EditorSceneDocument::TransformCommand final : public IEditorCommand {
public:
    TransformCommand(EditorSceneDocument& document, uint64_t generation,
                     uint32_t entityId, EditorTransformState before,
                     EditorTransformState after, std::string label,
                     std::string mergeKey)
        : _document(&document), _generation(generation), _entityId(entityId),
          _before(std::move(before)), _after(std::move(after)),
          _label(std::move(label)), _mergeKey(std::move(mergeKey)) {}

    const std::string& label() const noexcept override { return _label; }
    bool execute() override {
        return _document != nullptr
            && _document->applyTransform(_generation, _entityId, _after);
    }
    bool undo() override {
        return _document != nullptr
            && _document->applyTransform(_generation, _entityId, _before);
    }
    bool isAlive() const noexcept override {
        return _document != nullptr
            && _document->_contentGeneration == _generation;
    }
    std::string mergeKey() const override { return _mergeKey; }
    bool mergeFrom(const IEditorCommand& newer) override {
        const auto* transform = dynamic_cast<const TransformCommand*>(&newer);
        if (transform == nullptr || transform->_document != _document
            || transform->_generation != _generation
            || transform->_entityId != _entityId) {
            return false;
        }
        _after = transform->_after;
        return true;
    }

private:
    EditorSceneDocument* _document = nullptr;
    uint64_t _generation = 0;
    uint32_t _entityId = 0;
    EditorTransformState _before;
    EditorTransformState _after;
    std::string _label;
    std::string _mergeKey;
};

EditorSceneDocument::EditorSceneDocument()
    : _scene(std::make_unique<ayt::scene::Scene>(
          ayt::scene::SceneMode::Edit, "Untitled")),
      _title("Untitled")
{
    _history.setChangedCallback([this]() {
        ++_revision;
        if (_historyChanged) _historyChanged();
    });
}

EditorSceneDocument::~EditorSceneDocument() = default;

ayt::scene::Scene& EditorSceneDocument::scene() { return *_scene; }
const ayt::scene::Scene& EditorSceneDocument::scene() const { return *_scene; }

void EditorSceneDocument::newScene()
{
    _scene->clear();
    ++_contentGeneration;
    _path.clear();
    _title = "Untitled";
    _dirty = true;
    _history.discardHistory(EditorHistoryDiscardState::KeepDirty);
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
    ++_contentGeneration;
    _history.discardHistory(EditorHistoryDiscardState::MarkClean);
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
    (void)_history.markSaved();
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

bool EditorSceneDocument::executeTransform(
    uint32_t entityId, const EditorTransformState& after,
    std::string label, std::string mergeKey)
{
    ayt::entity::Entity* entity = _scene->world().findEntity(entityId);
    auto* transform = entity != nullptr
        ? entity->getComponent<ayt::entity::Transform>() : nullptr;
    if (transform == nullptr) return false;

    const EditorTransformState before = readTransform(*transform);
    if (sameTransform(before, after)) return false;
    return _history.execute(std::make_unique<TransformCommand>(
        *this, _contentGeneration, entityId, before, after,
        std::move(label), std::move(mergeKey)));
}

bool EditorSceneDocument::undo() { return _history.undo(); }
bool EditorSceneDocument::redo() { return _history.redo(); }

bool EditorSceneDocument::handlesCommand(const std::string& commandId) const
{
    return commandId == "edit.undo" || commandId == "edit.redo";
}

bool EditorSceneDocument::canExecuteCommand(
    const std::string& commandId) const
{
    if (commandId == "edit.undo") return canUndo();
    if (commandId == "edit.redo") return canRedo();
    return false;
}

bool EditorSceneDocument::executeCommand(const std::string& commandId)
{
    if (commandId == "edit.undo") return undo();
    if (commandId == "edit.redo") return redo();
    return false;
}

bool EditorSceneDocument::applyTransform(
    uint64_t generation, uint32_t entityId,
    const EditorTransformState& state)
{
    if (generation != _contentGeneration) return false;
    ayt::entity::Entity* entity = _scene->world().findEntity(entityId);
    auto* transform = entity != nullptr
        ? entity->getComponent<ayt::entity::Transform>() : nullptr;
    if (transform == nullptr) return false;
    transform->setPosition(state.position.x, state.position.y,
                           state.position.z);
    transform->setRotation(state.rotation.x, state.rotation.y,
                           state.rotation.z, state.rotation.w);
    transform->setScale(state.scale.x, state.scale.y, state.scale.z);
    return true;
}

void EditorSceneDocument::markDirty() noexcept
{
    _dirty = true;
    _history.discardHistory(EditorHistoryDiscardState::KeepDirty);
}

bool EditorSceneDocument::isDirty() const noexcept
{
    return _dirty || _history.isDirty() || _scene->isDirty();
}

} // namespace ayt::editor
