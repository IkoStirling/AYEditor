#include <AYTest.h>
#include <AYEntity.h>
#include <AYScene.h>
#include <AYEditor/EditorSession.h>
#include "EditorPhysicsDebugGeometry.h"
#include "EditorDebugVisualization.h"
#include <AYEntity/components/PerspectiveCameraComponent.h>
#include <AYEntity/components/SkeletonComponent.h>
#include <AYResource/assetsImpl/Skeleton.h>
#include <cmath>
#include <limits>
#include <AYUI/MockRenderer.h>
#include <AYUI/CheckBox.h>
#include "fixtures/EditorShellLayout.h"
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
TEST_CASE(debug_filter_freeze_step_trail_and_pick_use_the_same_records) {
    using namespace ayt::physics;
    ayt::editor::EditorDebugVisualization model;model.options.categories=0x1ff;model.options.trailFrames=3;
    PhysicsDebugRecord r;r.category=PhysicsDebugCategory::Motion;r.body=7;r.entityId=42;r.layer=2;r.anchor={0,0,.5f};
    r.lines={{{-.5f,0,.5f},{.5f,0,.5f},0xff00ff00}};
    model.update({r},1,.1f);CHECK(model.records.size()==1);CHECK(model.visible(r,0));
    model.options.layerMask=1;CHECK(!model.visible(r,0));model.options.layerMask=1u<<2;
    model.options.selectedOnly=true;CHECK(!model.visible(r,41));CHECK(model.visible(r,42));
    ayt::entity::CameraFrame camera;CHECK(model.pick(camera,{0,0,100,100},{50,50},42)==0);CHECK(model.pick(camera,{0,0,100,100},{50,70},42)==-1);
    model.options.frozen=true;r.anchor.x=1;model.update({r},2,.1f);CHECK(model.frameIndex==1);
    model.stepRequested=true;model.update({r},2,.1f);CHECK(model.frameIndex==2);CHECK(!model.stepRequested);CHECK(model.records[0].lines.size()>1);
    r.anchor.x=2;model.update({r},3,.1f);CHECK(model.frameIndex==2);
    model.clear(int(PhysicsDebugCategory::Motion));CHECK(model.records.empty());
}
TEST_CASE(debug_ray_pick_clips_the_camera_near_plane) {
    ayt::editor::EditorDebugVisualization model;model.options.categories=2;
    ayt::physics::PhysicsDebugRecord r;r.lines={{{0,0,0},{0,0,10000},0xff00ff00}};
    model.update({r},1,.016f);ayt::entity::CameraFrame camera;camera.nearZ=.1f;camera.farZ=100;camera.fovYDegrees=90;
    CHECK(ayt::entity::rebuildCameraFrame(camera));CHECK(model.pick(camera,{0,0,100,100},{50,50},0)==0);
}
TEST_CASE(camera_frustum_is_the_component_projection_and_skeleton_has_joint_axes) {
    ayt::scene::Scene scene(ayt::scene::SceneMode::Edit,"Debug camera and skeleton");auto& world=scene.world();world.initialize();
    auto* camera=world.createEntity();camera->addComponent<ayt::entity::Transform>();
    auto* lens=camera->createComponent<ayt::entity::PerspectiveCameraComponent>();CHECK(lens!=nullptr);if(!lens)return;lens->nearZ=1;lens->farZ=10;lens->fovYDegrees=90;
    auto* secondLens=camera->createComponent<ayt::entity::PerspectiveCameraComponent>();CHECK(secondLens!=nullptr);if(!secondLens)return;secondLens->nearZ=2;secondLens->farZ=5;
    auto* actor=world.createEntity();actor->addComponent<ayt::entity::Transform>()->position={5,0,0};
    auto* component=actor->addComponent<ayt::entity::SkeletonComponent>();CHECK(component!=nullptr);if(!component)return;component->skeleton=std::make_shared<ayt::resource::Skeleton>();
    component->skeleton->setBoneCount(1);
    ayt::resource::Bone bone;bone.name="root";bone.parentIndex=-1;bone.inverseBindMatrix=ayt::math::Float4x4::identity();
    bone.localPosition={};bone.localRotation=ayt::math::FQuaternion::identity();bone.localScale={1,1,1};component->skeleton->setBone(0,bone);
    auto records=ayt::editor::editorSceneDebugRecords(world,1,(1u<<6)|(1u<<7),false);
    CHECK(records.size()==3);if(records.size()!=3)return;CHECK(records[0].lines.size()==12);CHECK(std::abs(records[0].lines[0].from.z-1)<1e-4f);
    CHECK(records[1].lines.size()==12);CHECK(std::abs(records[1].lines[0].from.z-2)<1e-4f);
    CHECK(records[2].lines.size()==3);CHECK(records[2].anchor.x==5);
}
TEST_CASE(debug_options_validate_without_requiring_renderer_or_native_world) {
    ayt::editor::EditorSession editor;ayt::editor::EditorDebugVisualizationOptions options;options.categories=0x1ff;
    CHECK(editor.setDebugVisualization(options));CHECK(editor.debugVisualizationOptions().categories==0x1ff);
    options.trailFrames=601;CHECK(!editor.setDebugVisualization(options));CHECK(editor.debugVisualizationOptions().trailFrames==120);
    options.trailFrames=120;options.categories=1u<<12;CHECK(!editor.setDebugVisualization(options));
    editor.clearDebugVisualization();
}
TEST_CASE(debug_card_is_nonmodal_and_can_reopen_without_duplicate_controls) {
    ayt::ui::MockRenderer backend;ayt::editor::EditorSession editor;
    CHECK(editor.initialize(&backend,ayt::editor::testfixtures::resolveEditorShellLayoutPath()));
    auto options=editor.debugVisualizationOptions();options.categories=0x1ff;CHECK(editor.setDebugVisualization(options));
    editor.showDebugVisualizationPanel();
    auto* panel=editor.ui().findById("debug_visualization_panel");CHECK(panel!=nullptr);
    CHECK(editor.ui().findById("debug_category_8")!=nullptr);CHECK(editor.ui().findById("debug_visualization_statistics")!=nullptr);
    editor.showDebugVisualizationPanel();CHECK(editor.ui().findById("debug_visualization_panel")==panel);
    editor.shutdown();
}
TEST_SUITE_END
