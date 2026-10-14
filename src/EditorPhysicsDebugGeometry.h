#pragma once
#include <AYRenderer/RenderTypes.h>
#include <vector>
namespace ayt::entity { class World; }
namespace ayt::editor {
// Diagnostic authored/live ECS geometry. Does not initialize physics or modify
// components; matches bridge pose/offset/no-scale conventions. Unsupported
// hulls are omitted rather than presenting an AABB as exact geometry.
std::vector<ayt::render::EditorDebugLine> editorColliderDebugLines(const ayt::entity::World& world);
}
