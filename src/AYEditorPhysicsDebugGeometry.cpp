#include "EditorPhysicsDebugGeometry.h"
#include <AYEntity.h>
#include <AYEntity/components/ColliderComponent.h>
#include <AYEntity/components/RigidBodyComponent.h>
#include <AYPhysics/PhysicsDebugDraw.h>

namespace ayt::editor {
std::vector<ayt::render::EditorDebugLine> editorColliderDebugLines(const ayt::entity::World& world) {
    std::vector<ayt::render::EditorDebugLine> lines;
    for(auto* entity:world.getAllEntities()) {
        if(!entity) continue;
        auto* transform=entity->getComponent<ayt::entity::Transform>();
        auto* collider=entity->getComponent<ayt::entity::ColliderComponent>();
        auto* body=entity->getComponent<ayt::entity::RigidBodyComponent>();
        if(!transform||!collider) continue;
        const bool twoD=body && body->getPhysicsDimension()==ayt::entity::RigidBodyComponent::PhysicsDimension::TwoD;
        for(const auto& spec:collider->shapes) {
            ayt::physics::ColliderDesc shape;
            switch(spec.shape) {
            case ayt::entity::ColliderShapeType::Box: shape.shape=ayt::physics::ColliderShape::Box; break;
            case ayt::entity::ColliderShapeType::Sphere: shape.shape=ayt::physics::ColliderShape::Sphere; break;
            case ayt::entity::ColliderShapeType::Capsule: shape.shape=ayt::physics::ColliderShape::Capsule; break;
            default: continue;
            }
            shape.halfExtents=spec.halfExtents; shape.radius=spec.radius; shape.height=spec.height; shape.offset=spec.offset;
            const auto geometry=ayt::physics::PhysicsDebugDraw::colliderLines(shape,transform->position,transform->rotation,
                twoD?ayt::physics::PhysicsDimension::TwoD:ayt::physics::PhysicsDimension::ThreeD,
                spec.isTrigger?0xffffff00u:0xff00c8ffu);
            if(geometry.size()>ayt::render::kMaxEditorDebugLines-lines.size()) return lines;
            for(const auto& l:geometry) lines.push_back({l.from,l.to,l.color});
        }
    }
    return lines;
}
}
