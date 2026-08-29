#pragma once

#include <cstdint>

namespace ayt::entity { class Entity; class World; }

namespace ayt::editor {

class EditorSelection {
public:
    bool select(uint32_t entityId) noexcept;
    bool clear() noexcept;
    uint32_t entityId() const noexcept { return _entityId; }
    bool empty() const noexcept { return _entityId == 0; }
    ayt::entity::Entity* resolve(ayt::entity::World* world) const noexcept;
    const ayt::entity::Entity* resolve(const ayt::entity::World* world) const noexcept;

private:
    uint32_t _entityId = 0;
};

} // namespace ayt::editor
