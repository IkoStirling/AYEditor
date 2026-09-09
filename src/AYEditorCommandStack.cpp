#include "AYEditor/EditorCommandStack.h"

#include "AYEntity/EntityImpl.h"
#include "AYEntity/World.h"
#include "AYEntity/components/TransformComponent.h"

#include <cmath>

namespace ayt::editor {
namespace {

EditorTransformState readTransform(const ayt::entity::Transform& transform)
{
    return {transform.position, transform.rotation, transform.scale};
}

bool nearlyEqual(float a, float b)
{
    return std::fabs(a - b) <= 1.0e-6f;
}

bool sameTransform(const EditorTransformState& a, const EditorTransformState& b)
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

bool EditorCommandStack::executeTransform(ayt::entity::World& world,
                                          uint32_t entityId,
                                          const EditorTransformState& after)
{
    ayt::entity::Entity* entity = world.findEntity(entityId);
    auto* transform = entity != nullptr
        ? entity->getComponent<ayt::entity::Transform>() : nullptr;
    if (transform == nullptr) return false;

    TransformCommand command;
    command.world = &world;
    command.entityId = entityId;
    command.before = readTransform(*transform);
    command.after = after;
    if (sameTransform(command.before, command.after)) return false;
    if (!apply(command, true)) return false;

    _undo.push_back(command);
    _redo.clear();
    notifyChanged();
    return true;
}

bool EditorCommandStack::undo()
{
    if (_undo.empty()) return false;
    TransformCommand command = _undo.back();
    if (!apply(command, false)) return false;
    _undo.pop_back();
    _redo.push_back(command);
    notifyChanged();
    return true;
}

bool EditorCommandStack::redo()
{
    if (_redo.empty()) return false;
    TransformCommand command = _redo.back();
    if (!apply(command, true)) return false;
    _redo.pop_back();
    _undo.push_back(command);
    notifyChanged();
    return true;
}

void EditorCommandStack::clear() noexcept
{
    _undo.clear();
    _redo.clear();
}

bool EditorCommandStack::apply(const TransformCommand& command, bool useAfter)
{
    if (command.world == nullptr) return false;
    ayt::entity::Entity* entity = command.world->findEntity(command.entityId);
    auto* transform = entity != nullptr
        ? entity->getComponent<ayt::entity::Transform>() : nullptr;
    if (transform == nullptr) return false;

    const EditorTransformState& state = useAfter ? command.after : command.before;
    transform->setPosition(state.position.x, state.position.y, state.position.z);
    transform->setRotation(state.rotation.x, state.rotation.y,
                           state.rotation.z, state.rotation.w);
    transform->setScale(state.scale.x, state.scale.y, state.scale.z);
    return true;
}

void EditorCommandStack::notifyChanged()
{
    if (_changed) _changed();
}

bool EditorCommandStack::handlesCommand(const std::string& commandId) const
{
    return commandId == "edit.undo" || commandId == "edit.redo";
}

bool EditorCommandStack::canExecuteCommand(
    const std::string& commandId) const
{
    if (commandId == "edit.undo") return canUndo();
    if (commandId == "edit.redo") return canRedo();
    return false;
}

bool EditorCommandStack::executeCommand(const std::string& commandId)
{
    if (commandId == "edit.undo") return undo();
    if (commandId == "edit.redo") return redo();
    return false;
}

} // namespace ayt::editor
