#include <AYTest.h>
#include <AYEntity.h>
#include <AYScene.h>
#include <AYEditor/EditorSession.h>
#include "EditorPhysicsDebugGeometry.h"
#include <cmath>
#include <limits>
TEST_SUITE(AYEditor_PhysicsDebug)
TEST_CASE(colliders_follow_live_pose_and_offset_without_modifying_authoring) {
    ayt::scene::Scene scene(ayt::scene::SceneMode::Edit,"Debug geometry");
    auto& world=scene.world(); CHECK(world.initialize());
    auto* entity=world.createEntity(); CHECK(entity!=nullptr);
    auto* pose=entity->addComponent<ayt::entity::Transform>();
    auto* collider=entity->addComponent<ayt::entity::ColliderComponent>();
    pose->position={3,2,1}; pose->scale={10,20,30};
    auto& spec=collider->addShape(); spec.halfExtents={1,2,3}; spec.offset={2,0,0};
    const auto revision=collider->revision;
    auto lines=ayt::editor::editorColliderDebugLines(world);
    CHECK(lines.size()==12); CHECK(lines[0].from.x==4); CHECK(lines[0].from.z==-2);
    CHECK(collider->revision==revision); CHECK(pose->scale.x==10);
    pose->position.x=5; lines=ayt::editor::editorColliderDebugLines(world); CHECK(lines[0].from.x==6);
    collider->clearShapes(); CHECK(ayt::editor::editorColliderDebugLines(world).empty());
}
TEST_CASE(two_d_trigger_uses_xy_outline_and_body_rotation) {
    ayt::scene::Scene scene(ayt::scene::SceneMode::Edit,"Debug XY geometry");
    auto& world=scene.world(); CHECK(world.initialize()); auto* entity=world.createEntity();
    auto* pose=entity->addComponent<ayt::entity::Transform>();
    pose->rotation=ayt::math::FQuaternion::fromAxisAngle({0,0,1},3.14159265359f*0.5f);
    auto* body=entity->addComponent<ayt::entity::RigidBodyComponent>();
    body->setPhysicsDimension(ayt::entity::RigidBodyComponent::PhysicsDimension::TwoD);
    auto& shape=entity->addComponent<ayt::entity::ColliderComponent>()->addShape();
    shape.halfExtents={1,2,3}; shape.offset={1,0,5}; shape.isTrigger=true;
    auto lines=ayt::editor::editorColliderDebugLines(world); CHECK(lines.size()==4);
    CHECK(std::abs(lines[0].from.x-2)<1e-5f); CHECK(std::abs(lines[0].from.y)<1e-5f);
    CHECK(lines[0].from.z==0); CHECK(lines[0].color==0xffffff00u);
}
TEST_CASE(invalid_settings_do_not_enable_or_mutate_the_editor) {
    ayt::editor::EditorSession editor;
    CHECK(editor.setPhysicsDebugDraw(true,true,1,2.5));
    CHECK(!editor.setPhysicsDebugDraw(true,true,3,2));
    CHECK(!editor.setPhysicsDebugDraw(true,true,1,-1));
    CHECK(!editor.setPhysicsDebugDraw(true,true,1,std::numeric_limits<double>::quiet_NaN()));
    CHECK(editor.setPhysicsDebugDraw(false,false)); editor.clearPhysicsDebugDraw();
}
TEST_SUITE_END
