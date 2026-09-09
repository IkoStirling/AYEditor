#pragma once

#include <cstdint>

namespace ayt::entity { class Entity; class World; }

namespace ayt::editor {

class EditorSelectionContext;

class EditorSelection {
public:
    void bind(EditorSelectionContext* context) noexcept;
    bool select(uint32_t entityId) noexcept;
    bool clear() noexcept;
    uint32_t entityId() const noexcept;
    bool empty() const noexcept { return entityId() == 0; }
    ayt::entity::Entity* resolve(ayt::entity::World* world) const noexcept;
    const ayt::entity::Entity* resolve(const ayt::entity::World* world) const noexcept;

private:
    uint32_t _entityId = 0;
    EditorSelectionContext* _context = nullptr;
};

} // namespace ayt::editor
