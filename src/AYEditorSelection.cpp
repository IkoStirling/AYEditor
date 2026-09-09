#include "AYEditor/EditorSelection.h"

#include "AYEditor/EditorSelectionContext.h"
#include "AYEntity/World.h"

#include <limits>
#include <string>

namespace ayt::editor {

void EditorSelection::bind(EditorSelectionContext* context) noexcept
{
    _context = context;
    if (_context == nullptr) return;
    if (_entityId == 0) {
        (void)_context->clear();
    } else {
        (void)_context->select({_context->documentId(), "scene.entity",
            std::to_string(_entityId)});
    }
}

uint32_t EditorSelection::entityId() const noexcept
{
    if (_context == nullptr || _context->primary() == nullptr
        || _context->primary()->domain != "scene.entity") {
        return _entityId;
    }
    try {
        const unsigned long parsed = std::stoul(_context->primary()->objectId);
        return parsed <= (std::numeric_limits<uint32_t>::max)()
            ? static_cast<uint32_t>(parsed) : 0u;
    } catch (...) {
        return 0u;
    }
}

bool EditorSelection::select(uint32_t entityId) noexcept
{
    const bool changed = this->entityId() != entityId;
    _entityId = entityId;
    if (_context != nullptr) {
        if (entityId == 0) {
            (void)_context->clear();
        } else {
            (void)_context->select({_context->documentId(), "scene.entity",
                std::to_string(entityId)});
        }
    }
    return changed;
}

bool EditorSelection::clear() noexcept { return select(0); }

ayt::entity::Entity* EditorSelection::resolve(
    ayt::entity::World* world) const noexcept
{
    const uint32_t selected = entityId();
    return world != nullptr && selected != 0
        ? world->findEntity(selected) : nullptr;
}

const ayt::entity::Entity* EditorSelection::resolve(
    const ayt::entity::World* world) const noexcept
{
    const uint32_t selected = entityId();
    return world != nullptr && selected != 0
        ? world->findEntity(selected) : nullptr;
}

} // namespace ayt::editor
