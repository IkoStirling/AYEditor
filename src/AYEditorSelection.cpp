#include "AYEditor/EditorSelection.h"

#include "AYEntity/World.h"

namespace ayt::editor {

bool EditorSelection::select(uint32_t entityId) noexcept
{
    const bool changed = _entityId != entityId;
    _entityId = entityId;
    return changed;
}

bool EditorSelection::clear() noexcept { return select(0); }

ayt::entity::Entity* EditorSelection::resolve(ayt::entity::World* world) const noexcept
{
    return world != nullptr && _entityId != 0 ? world->findEntity(_entityId) : nullptr;
}

const ayt::entity::Entity* EditorSelection::resolve(
    const ayt::entity::World* world) const noexcept
{
    return world != nullptr && _entityId != 0 ? world->findEntity(_entityId) : nullptr;
}

} // namespace ayt::editor
