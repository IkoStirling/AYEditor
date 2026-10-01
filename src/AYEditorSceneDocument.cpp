#include "AYEditor/EditorSceneDocument.h"
#include <AYAssetFormat/AssetFormat.h>
#include "AYEditor/EditorComponentPolicy.h"

#include "AYEntity/ComponentFactory.h"
#include "AYEntity/ActorClassAsset.h"
#include "AYEntity/ComponentRegistry.h"
#include "AYEntity/EntityImpl.h"
#include "AYEntity/World.h"
#include "AYEntity/components/TransformComponent.h"
#include "AYEntity/components/ActorInstanceComponent.h"
#include "AYScene.h"
#include "AYSerializer/SerializeError.h"
#include "AYSerializer/SerializerCore.h"

#include <cmath>
#include <filesystem>
#include <unordered_set>
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

bool validateScenePersistence(const ayt::scene::Scene& scene,
                              std::string* error)
{
    for (const ayt::entity::Entity* entity : scene.world().getAllEntities()) {
        if (entity == nullptr) continue;
        for (const ayt::entity::IComponent* component : entity->getComponents()) {
            if (component == nullptr) continue;
            const char* typeName =
                ayt::entity::ComponentFactory::registeredTypeName(*component);
            if (typeName != nullptr
                && ayt::entity::ComponentFactory::isSceneSerializable(typeName)) {
                continue;
            }
            const char* entityName = entity->getName();
            const char* componentName = component->getName();
            setError(error, "Scene contains a component that cannot be saved: "
                + std::string(typeName != nullptr ? typeName
                    : (componentName != nullptr ? componentName : "unknown"))
                + " on entity "
                + (entityName != nullptr && *entityName != '\0'
                    ? entityName : std::to_string(entity->getId())));
            return false;
        }
    }
    return true;
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

class EditorSceneDocument::EntityRenameCommand final : public IEditorCommand {
public:
    EntityRenameCommand(EditorSceneDocument& document, uint64_t generation,
                        uint32_t entityId, std::string before,
                        std::string after)
        : _document(&document), _generation(generation), _entityId(entityId),
          _before(std::move(before)), _after(std::move(after)) {}

    const std::string& label() const noexcept override { return _label; }
    bool execute() override { return apply(_after); }
    bool undo() override { return apply(_before); }
    bool isAlive() const noexcept override {
        return _document != nullptr
            && _document->_contentGeneration == _generation;
    }

private:
    bool apply(const std::string& name) {
        return _document != nullptr
            && _document->applyEntityName(_generation, _entityId, name);
    }

    EditorSceneDocument* _document = nullptr;
    uint64_t _generation = 0;
    uint32_t _entityId = 0;
    std::string _before;
    std::string _after;
    std::string _label = "Rename Entity";
};

struct EditorSceneDocument::ComponentSnapshot {
    std::string typeName;
    std::string payload;
    std::string actorAssetsRoot;
    std::string actorClassDefaultsJson;
};

struct EditorSceneDocument::EntitySnapshot {
    std::string name;
    std::vector<ComponentSnapshot> components;
};

class EditorSceneDocument::EntityCreateCommand final : public IEditorCommand {
public:
    EntityCreateCommand(EditorSceneDocument& document, uint64_t generation,
                        uint32_t entityId, EntitySnapshot snapshot,
                        std::string label)
        : _document(&document), _generation(generation), _entityId(entityId),
          _snapshot(std::move(snapshot)), _label(std::move(label)) {}

    const std::string& label() const noexcept override { return _label; }
    bool execute() override {
        if (!isAlive()) return false;
        if (_initiallyApplied) {
            _initiallyApplied = false;
            return _document->findCommandEntity(_entityId) != nullptr;
        }
        ayt::entity::Entity* entity = _document->restoreEntity(_snapshot);
        if (entity == nullptr) return false;
        _document->remapEntity(_entityId, entity->getId());
        return true;
    }
    bool undo() override {
        if (!isAlive()) return false;
        ayt::entity::Entity* entity = _document->findCommandEntity(_entityId);
        if (entity == nullptr) return false;
        _document->_scene->world().destroyEntity(entity);
        return true;
    }
    bool isAlive() const noexcept override {
        return _document != nullptr
            && _document->_contentGeneration == _generation;
    }
private:
    EditorSceneDocument* _document = nullptr;
    uint64_t _generation = 0;
    uint32_t _entityId = 0;
    EntitySnapshot _snapshot;
    std::string _label;
    bool _initiallyApplied = true;
};

class EditorSceneDocument::EntityDeleteCommand final : public IEditorCommand {
public:
    EntityDeleteCommand(EditorSceneDocument& document, uint64_t generation,
                        uint32_t entityId, EntitySnapshot snapshot,
                        std::string label)
        : _document(&document), _generation(generation), _entityId(entityId),
          _snapshot(std::move(snapshot)), _label(std::move(label)) {}

    const std::string& label() const noexcept override { return _label; }
    bool execute() override {
        if (!isAlive()) return false;
        ayt::entity::Entity* entity = _document->findCommandEntity(_entityId);
        if (entity == nullptr) return false;
        _document->_scene->world().destroyEntity(entity);
        return true;
    }
    bool undo() override {
        if (!isAlive()) return false;
        ayt::entity::Entity* entity = _document->restoreEntity(_snapshot);
        if (entity == nullptr) return false;
        _document->remapEntity(_entityId, entity->getId());
        return true;
    }
    bool isAlive() const noexcept override {
        return _document != nullptr
            && _document->_contentGeneration == _generation;
    }

private:
    EditorSceneDocument* _document = nullptr;
    uint64_t _generation = 0;
    uint32_t _entityId = 0;
    EntitySnapshot _snapshot;
    std::string _label;
};

class EditorSceneDocument::AddComponentCommand final : public IEditorCommand {
public:
    AddComponentCommand(EditorSceneDocument& document, uint64_t generation,
                        uint32_t entityId, std::string componentType)
        : _document(&document), _generation(generation), _entityId(entityId),
          _componentType(std::move(componentType)),
          _label("Add " + _componentType) {}

    const std::string& label() const noexcept override { return _label; }
    bool execute() override {
        if (!isAlive()) return false;
        ayt::entity::Entity* entity = _document->findCommandEntity(_entityId);
        if (entity == nullptr) return false;
        _added.clear();
        _error.clear();
        return EditorComponentPolicyRegistry::instance().addWithRequirements(
            *entity, _componentType, &_added, &_error) && !_added.empty();
    }
    bool undo() override {
        if (!isAlive()) return false;
        ayt::entity::Entity* entity = _document->findCommandEntity(_entityId);
        if (entity == nullptr) return false;
        for (auto it = _added.rbegin(); it != _added.rend(); ++it) {
            const auto* descriptor =
                ayt::entity::ComponentRegistry::instance().find(*it);
            if (descriptor == nullptr || descriptor->has == nullptr
                || descriptor->remove == nullptr || !descriptor->has(*entity)) {
                return false;
            }
        }
        for (auto it = _added.rbegin(); it != _added.rend(); ++it) {
            const auto* descriptor =
                ayt::entity::ComponentRegistry::instance().find(*it);
            descriptor->remove(*entity);
        }
        return true;
    }
    bool isAlive() const noexcept override {
        return _document != nullptr
            && _document->_contentGeneration == _generation;
    }
    const std::vector<std::string>& added() const noexcept { return _added; }

private:
    EditorSceneDocument* _document = nullptr;
    uint64_t _generation = 0;
    uint32_t _entityId = 0;
    std::string _componentType;
    std::string _label;
    std::vector<std::string> _added;
    std::string _error;
};

class EditorSceneDocument::RemoveComponentCommand final : public IEditorCommand {
public:
    RemoveComponentCommand(EditorSceneDocument& document, uint64_t generation,
                           uint32_t entityId, ComponentSnapshot snapshot)
        : _document(&document), _generation(generation), _entityId(entityId),
          _snapshot(std::move(snapshot)),
          _label("Remove " + _snapshot.typeName) {}

    const std::string& label() const noexcept override { return _label; }
    bool execute() override {
        if (!isAlive()) return false;
        ayt::entity::Entity* entity = _document->findCommandEntity(_entityId);
        const auto* descriptor = ayt::entity::ComponentRegistry::instance()
            .find(_snapshot.typeName);
        if (entity == nullptr || descriptor == nullptr
            || descriptor->has == nullptr || descriptor->remove == nullptr
            || !descriptor->has(*entity)) {
            return false;
        }
        descriptor->remove(*entity);
        return true;
    }
    bool undo() override {
        if (!isAlive()) return false;
        ayt::entity::Entity* entity = _document->findCommandEntity(_entityId);
        return entity != nullptr
            && _document->restoreComponent(*entity, _snapshot);
    }
    bool isAlive() const noexcept override {
        return _document != nullptr
            && _document->_contentGeneration == _generation;
    }

private:
    EditorSceneDocument* _document = nullptr;
    uint64_t _generation = 0;
    uint32_t _entityId = 0;
    ComponentSnapshot _snapshot;
    std::string _label;
};

class EditorSceneDocument::ComponentMutationCommand final
    : public IEditorCommand {
public:
    ComponentMutationCommand(EditorSceneDocument& document,
                             uint64_t generation, uint32_t entityId,
                             ComponentSnapshot before,
                             ComponentSnapshot after, std::string label,
                             std::string mergeKey)
        : _document(&document), _generation(generation), _entityId(entityId),
          _before(std::move(before)), _after(std::move(after)),
          _label(std::move(label)), _mergeKey(std::move(mergeKey)) {}

    const std::string& label() const noexcept override { return _label; }
    bool execute() override { return apply(_after); }
    bool undo() override { return apply(_before); }
    bool isAlive() const noexcept override {
        return _document != nullptr
            && _document->_contentGeneration == _generation;
    }
    std::string mergeKey() const override { return _mergeKey; }
    bool mergeFrom(const IEditorCommand& newer) override {
        const auto* mutation =
            dynamic_cast<const ComponentMutationCommand*>(&newer);
        if (mutation == nullptr || mutation->_document != _document
            || mutation->_generation != _generation
            || mutation->_entityId != _entityId
            || mutation->_after.typeName != _after.typeName) {
            return false;
        }
        _after = mutation->_after;
        return true;
    }

private:
    bool apply(const ComponentSnapshot& snapshot) {
        if (!isAlive()) return false;
        ayt::entity::Entity* entity = _document->findCommandEntity(_entityId);
        if (entity == nullptr) return false;
        const auto* descriptor = ayt::entity::ComponentRegistry::instance()
            .find(snapshot.typeName);
        ayt::entity::IComponent* component = descriptor != nullptr
            && descriptor->get != nullptr ? descriptor->get(*entity) : nullptr;
        if (component == nullptr) return false;

        auto reader = ayt::serializer::createSerializer(
            ayt::serializer::Format::Json);
        reader->deserialize(snapshot.payload);
        reader->beginObject(nullptr);
        const bool restored = ayt::entity::ComponentFactory::deserializeComponent(
            *reader, snapshot.typeName.c_str(), *component);
        reader->endObject();
        if (!restored || !reader->lastError().ok()) return false;
        ayt::entity::ComponentFactory::afterSceneDeserialize(
            *entity, snapshot.typeName.c_str(), *component);
        return true;
    }

    EditorSceneDocument* _document = nullptr;
    uint64_t _generation = 0;
    uint32_t _entityId = 0;
    ComponentSnapshot _before;
    ComponentSnapshot _after;
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
    _commandEntityIds.clear();
    _path.clear();
    _title = "Untitled";
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
    ++_contentGeneration;
    _commandEntityIds.clear();
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
    if (!ayt::asset_format::isCanonicalPath(path, ayt::asset_format::Id::Scene)) {
        setError(error, "New Scene files must use the current Scene extension.");
        return false;
    }
    if (!validateScenePersistence(*_scene, error)) return false;
    if (!_scene->save(path)) {
        setError(error, "Unable to save scene: " + path);
        return false;
    }

    _path = path;
    _title = titleForPath(path);
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
    if (!validateScenePersistence(*_scene, error)) return false;
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
        *this, _contentGeneration, logicalEntityId(entityId), before, after,
        std::move(label), std::move(mergeKey)));
}

bool EditorSceneDocument::renameEntity(uint32_t entityId, std::string name,
                                       std::string* error)
{
    const auto first = name.find_first_not_of(" \t\r\n");
    const auto last = name.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) {
        setError(error, "entity name cannot be empty");
        return false;
    }
    name = name.substr(first, last - first + 1u);
    ayt::entity::Entity* entity = _scene->world().findEntity(entityId);
    if (entity == nullptr) {
        setError(error, "entity no longer exists");
        return false;
    }
    const char* current = entity->getName();
    const std::string before = current != nullptr ? current : "";
    if (before == name) {
        if (error != nullptr) error->clear();
        return true;
    }
    const bool renamed = _history.execute(
        std::make_unique<EntityRenameCommand>(
            *this, _contentGeneration, logicalEntityId(entityId),
            before, std::move(name)));
    if (!renamed) setError(error, "failed to rename entity");
    else if (error != nullptr) error->clear();
    return renamed;
}

bool EditorSceneDocument::createEntity(
    std::string label,
    const std::function<bool(ayt::entity::Entity&)>& configure,
    uint32_t* entityId)
{
    ayt::entity::Entity* entity = _scene->world().createEntity();
    if (entity == nullptr) return false;
    if (configure && !configure(*entity)) {
        _scene->world().destroyEntity(entity);
        return false;
    }

    EntitySnapshot snapshot;
    if (!snapshotEntity(*entity, snapshot)) {
        _scene->world().destroyEntity(entity);
        return false;
    }
    const uint32_t createdId = entity->getId();
    remapEntity(createdId, createdId);
    auto command = std::make_unique<EntityCreateCommand>(
        *this, _contentGeneration, createdId, std::move(snapshot),
        std::move(label));
    if (!_history.execute(std::move(command))) {
        if (ayt::entity::Entity* created =
                _scene->world().findEntity(createdId)) {
            _scene->world().destroyEntity(created);
        }
        return false;
    }
    if (entityId != nullptr) *entityId = createdId;
    return true;
}

bool EditorSceneDocument::deleteEntity(uint32_t entityId, std::string label)
{
    ayt::entity::Entity* entity = _scene->world().findEntity(entityId);
    if (entity == nullptr) return false;
    EntitySnapshot snapshot;
    if (!snapshotEntity(*entity, snapshot)) return false;
    return _history.execute(std::make_unique<EntityDeleteCommand>(
        *this, _contentGeneration, logicalEntityId(entityId), std::move(snapshot),
        std::move(label)));
}

bool EditorSceneDocument::addComponent(
    uint32_t entityId, const std::string& componentType,
    std::vector<std::string>* added, std::string* error)
{
    const auto* descriptor =
        ayt::entity::ComponentRegistry::instance().find(componentType);
    if (descriptor != nullptr && !descriptor->sceneSerializable) {
        setError(error, "Component cannot be saved in a Scene: " + componentType);
        return false;
    }
    auto command = std::make_unique<AddComponentCommand>(
        *this, _contentGeneration, logicalEntityId(entityId), componentType);
    AddComponentCommand* result = command.get();
    if (!_history.execute(std::move(command))) {
        setError(error, "Unable to add component: " + componentType);
        return false;
    }
    if (added != nullptr) *added = result->added();
    if (error != nullptr) error->clear();
    return true;
}

bool EditorSceneDocument::removeComponent(
    uint32_t entityId, const std::string& componentType, std::string* error)
{
    ayt::entity::Entity* entity = _scene->world().findEntity(entityId);
    const auto* descriptor =
        ayt::entity::ComponentRegistry::instance().find(componentType);
    ayt::entity::IComponent* component = entity != nullptr
        && descriptor != nullptr && descriptor->get != nullptr
        ? descriptor->get(*entity) : nullptr;
    std::string removeReason;
    if (entity != nullptr
        && !EditorComponentPolicyRegistry::instance().canRemove(
            *entity, componentType, &removeReason)) {
        setError(error, removeReason);
        return false;
    }
    if (component != nullptr && descriptor != nullptr
        && !descriptor->sceneSerializable && descriptor->remove != nullptr) {
        // Transient components predate the Scene authoring restriction. They
        // have no serializable state to capture for undo, but must be removable
        // so a document can be made persistable again.
        descriptor->remove(*entity);
        if (error != nullptr) error->clear();
        return true;
    }
    ComponentSnapshot snapshot;
    if (component == nullptr || !snapshotComponent(*component, snapshot)) {
        setError(error, "Unable to snapshot component: " + componentType);
        return false;
    }
    if (!_history.execute(std::make_unique<RemoveComponentCommand>(
            *this, _contentGeneration, logicalEntityId(entityId),
            std::move(snapshot)))) {
        setError(error, "Unable to remove component: " + componentType);
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

bool EditorSceneDocument::mutateComponent(
    uint32_t entityId, const std::string& componentType,
    std::string label, std::string mergeKey,
    const std::function<bool(ayt::entity::IComponent&)>& mutation)
{
    ayt::entity::Entity* entity = _scene->world().findEntity(entityId);
    const auto* descriptor =
        ayt::entity::ComponentRegistry::instance().find(componentType);
    ayt::entity::IComponent* component = entity != nullptr
        && descriptor != nullptr && descriptor->get != nullptr
        ? descriptor->get(*entity) : nullptr;
    if (component == nullptr || !mutation) return false;

    ComponentSnapshot before;
    if (!snapshotComponent(*component, before) || !mutation(*component)) {
        return false;
    }
    ComponentSnapshot after;
    if (!snapshotComponent(*component, after)) {
        (void)restoreComponent(*entity, before);
        return false;
    }
    if (before.payload == after.payload
        && before.actorAssetsRoot == after.actorAssetsRoot
        && before.actorClassDefaultsJson == after.actorClassDefaultsJson) {
        (void)restoreComponent(*entity, before);
        return false;
    }
    if (!restoreComponent(*entity, before)) return false;
    return _history.execute(std::make_unique<ComponentMutationCommand>(
        *this, _contentGeneration, logicalEntityId(entityId), std::move(before),
        std::move(after), std::move(label), std::move(mergeKey)));
}

bool EditorSceneDocument::restoreActorDefaults(uint32_t entityId,
                                               std::string* error)
{
    auto* entity = _scene->world().findEntity(entityId);
    auto* instance = entity != nullptr
        ? entity->getComponent<ayt::entity::ActorInstanceComponent>() : nullptr;
    if (!instance) {
        setError(error, "Entity is not an Actor instance");
        return false;
    }
    auto* baseline = _scene->world().createEntity();
    if (!baseline) return false;
    auto* baselineInstance = baseline->addComponent<ayt::entity::ActorInstanceComponent>();
    baselineInstance->classPath = instance->classPath;
    baselineInstance->assetsRoot = instance->assetsRoot;
    std::string actorError;
    const bool expanded = ayt::entity::expandActorInstance(
        *baseline, *baselineInstance, &actorError);
    EntitySnapshot defaults;
    const bool captured = expanded && snapshotEntity(*baseline, defaults);
    const std::string defaultSnapshot = baselineInstance->classDefaultsJson;
    _scene->world().destroyEntity(baseline);
    if (!captured) {
        setError(error, actorError.empty()
            ? "Could not capture Actor class defaults" : actorError);
        return false;
    }
    if (!_history.beginTransaction("Restore Actor Defaults")) return false;
    bool changed = false;
    auto abort = [&]() {
        (void)_history.cancelTransaction();
        setError(error, "Could not restore Actor defaults");
        return false;
    };
    std::unordered_set<std::string> expected;
    for (const auto& component : defaults.components)
        expected.insert(component.typeName);
    std::vector<std::string> remove;
    for (auto* component : entity->getComponents()) {
        const char* type = component != nullptr
            ? ayt::entity::ComponentFactory::registeredTypeName(*component) : nullptr;
        if (type && !expected.contains(type)) remove.emplace_back(type);
    }
    for (const auto& type : remove) {
        if (!removeComponent(entityId, type)) return abort();
        changed = true;
    }
    for (const auto& component : defaults.components) {
        if (component.typeName == "ActorInstanceComponent") continue;
        const auto* descriptor = ayt::entity::ComponentRegistry::instance()
            .find(component.typeName);
        if (!descriptor || !descriptor->has) return abort();
        if (!descriptor->has(*entity)) {
            if (!addComponent(entityId, component.typeName)) return abort();
            changed = true;
        }
        ComponentSnapshot current;
        if (!snapshotComponent(*descriptor->get(*entity), current)) return abort();
        if (current.payload != component.payload) {
            if (!mutateComponent(entityId, component.typeName,
                    "Restore Actor component", {},
                    [this, entity, component](ayt::entity::IComponent&) {
                        return restoreComponent(*entity, component);
                    })) return abort();
            changed = true;
        }
    }
    if (instance->propertyOverridesJson != "{}"
        || instance->componentOverridesJson != "{}"
        || instance->classDefaultsJson != defaultSnapshot) {
        if (!mutateComponent(entityId, "ActorInstanceComponent",
                "Restore Actor properties", {},
                [defaultSnapshot](ayt::entity::IComponent& component) {
                    auto& actor = static_cast<ayt::entity::ActorInstanceComponent&>(component);
                    actor.propertyOverridesJson = "{}";
                    actor.componentOverridesJson = "{}";
                    actor.classDefaultsJson = defaultSnapshot;
                    return true;
                })) return abort();
        changed = true;
    }
    if (!changed) {
        (void)_history.cancelTransaction();
        if (error) error->clear();
        return true;
    }
    if (!_history.commitTransaction()) return abort();
    if (error) error->clear();
    return true;
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
    ayt::entity::Entity* entity = findCommandEntity(entityId);
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

bool EditorSceneDocument::applyEntityName(
    uint64_t generation, uint32_t entityId, const std::string& name)
{
    if (generation != _contentGeneration) return false;
    ayt::entity::Entity* entity = findCommandEntity(entityId);
    if (entity == nullptr || name.empty()) return false;
    entity->setName(name.c_str());
    return true;
}

bool EditorSceneDocument::snapshotComponent(
    ayt::entity::IComponent& component, ComponentSnapshot& snapshot) const
{
    const char* typeName =
        ayt::entity::ComponentFactory::registeredTypeName(component);
    if (typeName == nullptr
        || !ayt::entity::ComponentFactory::isSceneSerializable(typeName)) {
        return false;
    }
    auto writer = ayt::serializer::createSerializer(
        ayt::serializer::Format::Json);
    writer->beginObject(nullptr);
    ayt::entity::ComponentFactory::serializeComponent(*writer, component);
    writer->endObject();
    if (!writer->lastError().ok()) return false;
    snapshot.typeName = typeName;
    snapshot.payload = writer->output();
    if (auto* actor = dynamic_cast<ayt::entity::ActorInstanceComponent*>(
            &component)) {
        snapshot.actorAssetsRoot = actor->assetsRoot;
        snapshot.actorClassDefaultsJson = actor->classDefaultsJson;
    }
    return !snapshot.payload.empty();
}

bool EditorSceneDocument::restoreComponent(
    ayt::entity::Entity& entity, const ComponentSnapshot& snapshot) const
{
    const auto* descriptor = ayt::entity::ComponentRegistry::instance()
        .find(snapshot.typeName);
    if (descriptor == nullptr || descriptor->get == nullptr
        || descriptor->add == nullptr) {
        return false;
    }
    const bool existed = descriptor->has != nullptr && descriptor->has(entity);
    ayt::entity::IComponent* component = existed
        ? descriptor->get(entity) : descriptor->add(entity);
    if (component == nullptr) return false;

    auto reader = ayt::serializer::createSerializer(
        ayt::serializer::Format::Json);
    reader->deserialize(snapshot.payload);
    reader->beginObject(nullptr);
    const bool restored = ayt::entity::ComponentFactory::deserializeComponent(
        *reader, snapshot.typeName.c_str(), *component);
    reader->endObject();
    if (!restored || !reader->lastError().ok()) {
        if (!existed && descriptor->remove != nullptr) descriptor->remove(entity);
        return false;
    }
    ayt::entity::ComponentFactory::afterSceneDeserialize(
        entity, snapshot.typeName.c_str(), *component);
    if (auto* actor = dynamic_cast<ayt::entity::ActorInstanceComponent*>(
            component)) {
        actor->assetsRoot = snapshot.actorAssetsRoot;
        actor->classDefaultsJson = snapshot.actorClassDefaultsJson;
    }
    return true;
}

bool EditorSceneDocument::snapshotEntity(
    ayt::entity::Entity& entity, EntitySnapshot& snapshot) const
{
    snapshot.name = entity.getName() != nullptr ? entity.getName() : "";
    snapshot.components.clear();
    for (ayt::entity::IComponent* component : entity.getComponents()) {
        if (component == nullptr) continue;
        const char* typeName =
            ayt::entity::ComponentFactory::registeredTypeName(*component);
        if (typeName == nullptr
            || !ayt::entity::ComponentFactory::isSceneSerializable(typeName)) {
            return false;
        }
        ComponentSnapshot componentSnapshot;
        if (!snapshotComponent(*component, componentSnapshot)) return false;
        snapshot.components.push_back(std::move(componentSnapshot));
    }
    return true;
}

ayt::entity::Entity* EditorSceneDocument::restoreEntity(
    const EntitySnapshot& snapshot)
{
    ayt::entity::Entity* entity = _scene->world().createEntity();
    if (entity == nullptr) return nullptr;
    entity->setName(snapshot.name.c_str());
    for (const ComponentSnapshot& component : snapshot.components) {
        if (!restoreComponent(*entity, component)) {
            _scene->world().destroyEntity(entity);
            return nullptr;
        }
    }
    return entity;
}

uint32_t EditorSceneDocument::logicalEntityId(uint32_t currentId) const noexcept
{
    for (const auto& [logicalId, mappedId] : _commandEntityIds) {
        if (mappedId == currentId) return logicalId;
    }
    return currentId;
}

ayt::entity::Entity* EditorSceneDocument::findCommandEntity(uint32_t logicalId)
{
    const auto found = _commandEntityIds.find(logicalId);
    const uint32_t currentId = found != _commandEntityIds.end()
        ? found->second : logicalId;
    return _scene->world().findEntity(currentId);
}

void EditorSceneDocument::remapEntity(uint32_t logicalId, uint32_t currentId)
{
    _commandEntityIds[logicalId] = currentId;
}

void EditorSceneDocument::markDirty() noexcept
{
    _commandEntityIds.clear();
    _history.discardHistory(EditorHistoryDiscardState::KeepDirty);
}

bool EditorSceneDocument::isDirty() const noexcept
{
    return _history.isDirty() || _scene->isDirty();
}

} // namespace ayt::editor
