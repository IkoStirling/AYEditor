#include <AYTest.h>
#include <AYScene.h>
#include <AYEditor/EditorCameraVisualization.h>
#include <AYEditor/EditorSession.h>
#include <AYEditor/EditorSceneDocument.h>
#include <AYEditor/EditorWorkspace.h>
#include <AYEntity.h>
#include <AYEntity/World.h>
#include <AYEntity/components/MeshComponent.h>
#include <AYEntity/components/PerspectiveCameraComponent.h>
#include <AYApplication/IEngineHost.h>
#include <AYRenderer.h>
#include <AYRenderer/RendererSubSystem.h>
#include <AYUI/MockRenderer.h>
#include <AYUI/MenuItem.h>
#include "EditorGameViewTestAccess.h"
#include "fixtures/EditorShellLayout.h"
#include <filesystem>
#include <limits>

using namespace ayt::editor;
using namespace ayt::math;
namespace {
EditorSceneCameraFrame observer() {
    EditorSceneCameraFrame view;view.projection=lh::perspective(radians(50.f),1.5f,.1f,1000.f);return view;
}
auto camera(ayt::entity::World& world,float z=10.f) {
    auto* entity=world.createSpatialEntity();entity->transform()->position={0,0,z};
    entity->createComponent<ayt::entity::PerspectiveCameraComponent>();return entity;
}
auto visuals(ayt::entity::World& world,uint32_t selected=0) {
    return buildEditorCameraVisuals(world,observer(),720,1.5f,selected,EditorCameraDisplayMode::Solid,true);
}
bool closeCameraFloat(float a,float b){return std::fabs(a-b)<1e-4f;}
std::string assetsRoot(){return (std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path()/"EngineAssets").string();}
}
TEST_SUITE(AYEditor_CameraVisualization)
TEST_CASE(hierarchy_and_multi_lens_poses_ignore_scale_and_keep_entity_clean) {
    ayt::app::EngineHostScope scope(ayt::app::defaultEngineHost());EditorSceneDocument document;
    auto& world=document.scene().world();auto* parent=world.createSpatialEntity();parent->transform()->position={1,0,8};
    auto* e=camera(world,1);CHECK(e->setSpatialParent(parent,ayt::entity::ReparentMode::KeepLocal));
    e->transform()->scale={5,0,-3};
    auto* first=e->getComponents<ayt::entity::PerspectiveCameraComponent>().at(0);first->localOffset={2,1,0};
    auto* second=e->createComponent<ayt::entity::PerspectiveCameraComponent>();second->localOffset={-2,0,0};second->priority=9;
    auto list=visuals(world,e->getId());CHECK_INT_EQ(int(list.size()),2);
    if(list.size()!=2)return;
    auto a=list[0].pose.transformPoint({0,0,0}),b=list[1].pose.transformPoint({0,0,0});
    CHECK(closeCameraFloat(a.x,3)&&closeCameraFloat(a.y,1)&&closeCameraFloat(a.z,9));CHECK(closeCameraFloat(b.x,-1)&&closeCameraFloat(b.z,9));
    CHECK(list[0].selected&&list[1].selected);CHECK(!list[0].runtimeActive&&list[1].runtimeActive);
    CHECK(!e->hasComponent<ayt::entity::MeshComponent>());CHECK(!document.isDirty());
}
TEST_CASE(local_camera_rotation_controls_forward_and_up) {
    ayt::app::EngineHostScope scope(ayt::app::defaultEngineHost());EditorSceneDocument document;auto* e=camera(document.scene().world());
    auto* lens=e->getComponents<ayt::entity::PerspectiveCameraComponent>().at(0);
    lens->localRotation={0,.70710678f,0,.70710678f};auto list=visuals(document.scene().world());
    CHECK_INT_EQ(int(list.size()),1);if(list.empty())return;
    auto forward=list[0].pose.transformDirection({0,0,1});auto up=list[0].pose.transformDirection({0,1,0});
    CHECK(closeCameraFloat(std::fabs(forward.x),1)&&closeCameraFloat(forward.z,0));CHECK(closeCameraFloat(up.y,1));
}
TEST_CASE(display_size_tracks_perspective_depth_and_orthographic_zoom) {
    ayt::app::EngineHostScope scope(ayt::app::defaultEngineHost());EditorSceneDocument document;auto& world=document.scene().world();auto* e=camera(world);
    auto first=visuals(world);e->transform()->position.z=20;auto second=visuals(world);
    CHECK(closeCameraFloat(second[0].scale,first[0].scale*2));
    auto view=observer();view.orthographic=true;view.projection=lh::ortho(-5.f,5.f,-5.f,5.f,.1f,100.f);
    auto a=buildEditorCameraVisuals(world,view,720,1.5,0,EditorCameraDisplayMode::Solid,true);
    e->transform()->position.z=10;auto b=buildEditorCameraVisuals(world,view,720,1.5,0,EditorCameraDisplayMode::Solid,true);
    CHECK(closeCameraFloat(a[0].scale,b[0].scale));
    view.projection=lh::ortho(-10.f,10.f,-10.f,10.f,.1f,100.f);
    auto c=buildEditorCameraVisuals(world,view,720,1.5,0,EditorCameraDisplayMode::Solid,true);CHECK(closeCameraFloat(c[0].scale,b[0].scale*2));
}
TEST_CASE(hidden_play_behind_observer_and_invalid_lenses_have_no_icons) {
    ayt::app::EngineHostScope scope(ayt::app::defaultEngineHost());EditorSceneDocument document;auto& world=document.scene().world();auto* e=camera(world);
    CHECK(buildEditorCameraVisuals(world,observer(),720,1.5,0,EditorCameraDisplayMode::Hidden,true).empty());
    CHECK(buildEditorCameraVisuals(world,observer(),720,1.5,0,EditorCameraDisplayMode::Solid,false).empty());
    e->transform()->position.z=-1;CHECK(visuals(world).empty());e->transform()->position.z=10;
    auto* lens=e->getComponents<ayt::entity::PerspectiveCameraComponent>().at(0);lens->localRotation={0,0,0,0};CHECK(visuals(world).empty());
    lens->localRotation={0,0,0,1};lens->localOffset.x=std::numeric_limits<float>::quiet_NaN();CHECK(visuals(world).empty());
}
TEST_CASE(picking_uses_lens_offset_and_display_scale) {
    ayt::app::EngineHostScope scope(ayt::app::defaultEngineHost());EditorSceneDocument document;auto& world=document.scene().world();auto* e=camera(world);
    e->getComponents<ayt::entity::PerspectiveCameraComponent>().at(0)->localOffset={3,0,0};auto list=visuals(world);
    auto target=list[0].iconWorld.transformPoint({0,0,-.28f});float distance=-1;
    CHECK(hitEditorCameraVisual(list[0],{0,0,0},target.normalize(),distance));CHECK(distance>0&&distance<target.length());
    CHECK(!hitEditorCameraVisual(list[0],{0,0,0},{0,0,1},distance));
}
TEST_CASE(frustums_match_lens_projection_and_not_icon_display_scale) {
    ayt::app::EngineHostScope scope(ayt::app::defaultEngineHost());EditorSceneDocument document;auto& world=document.scene().world();auto* e=camera(world);
    auto* lens=e->getComponents<ayt::entity::PerspectiveCameraComponent>().at(0);lens->nearZ=.25f;lens->farZ=12;auto a=visuals(world,e->getId());
    CHECK(closeCameraFloat(a[0].frustum[0].z,.25f)&&closeCameraFloat(a[0].frustum[4].z,12));
    CHECK(closeCameraFloat(a[0].frustum[5].x/a[0].frustum[1].x,48));
    e->transform()->position.z=25;auto b=visuals(world,e->getId());CHECK(closeCameraFloat(a[0].frustum[5].x,b[0].frustum[5].x));
    lens->projectionMode=1;lens->orthographicHeight=8;auto c=visuals(world);CHECK(closeCameraFloat(c[0].frustum[0].x,c[0].frustum[4].x));CHECK(closeCameraFloat(c[0].frustum[2].y,4));
}
TEST_CASE(runtime_active_state_is_independent_per_viewport) {
    ayt::app::EngineHostScope scope(ayt::app::defaultEngineHost());EditorSceneDocument document;auto& world=document.scene().world();
    auto* a=camera(world);auto* b=camera(world);auto* c=camera(world);
    auto* bl=b->getComponents<ayt::entity::PerspectiveCameraComponent>().at(0);bl->viewportId=1;
    auto* cl=c->getComponents<ayt::entity::PerspectiveCameraComponent>().at(0);cl->active=false;
    auto list=visuals(world);CHECK_INT_EQ(int(list.size()),3);CHECK(list[0].runtimeActive);CHECK(list[1].runtimeActive);CHECK(!list[2].runtimeActive);
}
TEST_CASE(menu_modes_persist_without_dirtying_scene_or_play_world) {
    ayt::app::EngineHostScope scope(ayt::app::defaultEngineHost());ayt::ui::MockRenderer backend;EditorSession session;
    EditorSessionDesc desc;desc.uiBackend=&backend;desc.layoutPath=testfixtures::resolveEditorShellLayoutPath();desc.engineAssetsRoot=assetsRoot();
    EditorPreferences saved;int writes=0;desc.preferences.cameraDisplayMode=EditorCameraDisplayMode::Wireframe;
    desc.onPreferencesChanged=[&](const EditorPreferences& value){saved=value;++writes;};
    CHECK(session.initialize(desc));CHECK(session.cameraDisplayMode()==EditorCameraDisplayMode::Wireframe);
    CHECK(session.ui().findById("menu_camera_icons_solid")!=nullptr);CHECK(session.ui().findById("menu_camera_icons_hidden")!=nullptr);
    session.setCameraDisplayMode(EditorCameraDisplayMode::Hidden);session.savePreferencesNow();
    CHECK(writes>0);CHECK(saved.cameraDisplayMode==EditorCameraDisplayMode::Hidden);CHECK(!session.document()->isDirty());
    EditorGameViewTestAccess::forceModeAndNotify(session.gameView(),EditorMode::Play);
    CHECK(session.cameraDisplayMode()==EditorCameraDisplayMode::Hidden);
    EditorGameViewTestAccess::forceModeAndNotify(session.gameView(),EditorMode::Edit);session.shutdown();
}
TEST_CASE(noop_submission_uses_real_assets_highlights_and_selected_frustum) {
    ayt::app::EngineHostScope scope(ayt::app::defaultEngineHost());EditorSceneDocument document;auto& world=document.scene().world();auto* e=camera(world);
    ayt::render::Renderer renderer;ayt::render::InitDesc desc;desc.backend=ayt::render::Backend::Noop;desc.width=320;desc.height=240;
    CHECK(renderer.initialize(desc));if(!renderer.isInitialized())return;
    EditorCameraVisualRenderer helper;ayt::render::RenderScene scene;auto list=visuals(world,e->getId());
    helper.append(renderer,scene,assetsRoot(),list,EditorCameraDisplayMode::Solid);CHECK_INT_EQ(int(scene.items().size()),5);
    for(const auto& item:scene.items()){CHECK(item.mesh.isValid());CHECK(item.material.isValid());CHECK_INT_EQ(int(item.shadowFlags),0);}
    auto view=observer(); renderer.setMainCamera(view.view,view.projection,view.position);
    renderer.beginFrame(); renderer.render(scene); renderer.endFrame();
    CHECK(renderer.getFrameStats().drawCalls>0);
    auto selectedMaterial=scene.items().empty()?ayt::render::MaterialHandle{}:scene.items()[0].material;
    scene.clear();helper.append(renderer,scene,assetsRoot(),visuals(world),EditorCameraDisplayMode::Solid);CHECK_INT_EQ(int(scene.items().size()),4);
    if(!scene.items().empty())CHECK(scene.items()[0].material.id!=selectedMaterial.id);
    scene.clear();renderer.setWireframeEnabled(true);helper.append(renderer,scene,assetsRoot(),list,EditorCameraDisplayMode::Wireframe);CHECK_INT_EQ(int(scene.items().size()),4);
    scene.clear();helper.append(renderer,scene,assetsRoot(),{},EditorCameraDisplayMode::Hidden);CHECK(scene.empty());
    helper.release(renderer);renderer.shutdown();CHECK(!document.isDirty());
}

TEST_CASE(session_scene_builder_switches_icons_and_detaches_on_shutdown) {
    ayt::app::EngineHostScope scope(ayt::app::defaultEngineHost());
    static int sentinel=0;
    ayt::render::RendererSubSystem::setBootstrapBackend(ayt::render::Backend::Noop);
    ayt::render::RendererSubSystem::setBootstrapWindow(&sentinel,1280,720);
    ayt::render::RendererSubSystem::setBootstrapViewport(0,0,1280,720);
    ayt::render::RendererSubSystem::registerSubSystem();
    auto* sub=ayt::render::RendererSubSystem::findRegistered();CHECK(sub!=nullptr);if(!sub)return;
    CHECK(sub->initialize());if(!sub->isReady())return;
    ayt::ui::MockRenderer backend;EditorSession session;EditorSessionDesc desc;
    desc.uiBackend=&backend;desc.layoutPath=testfixtures::resolveEditorShellLayoutPath();desc.engineAssetsRoot=assetsRoot();
    desc.preferences.cameraPoseValid=true;desc.preferences.cameraEye={0,0,0};desc.preferences.cameraYawRadians=0;desc.preferences.cameraPitchRadians=0;
    CHECK(session.initialize(desc));session.setClientSize(1280,720);
    auto* e=camera(session.document()->scene().world());e->setName("Pick Camera Icon");
    session.update(.016f);sub->update(.016f);CHECK_INT_EQ(int(sub->renderScene().items().size()),4);
    FRectangle bounds{};CHECK(session.getViewportBounds(bounds));
    auto point=session.ui().logicalToPhysical({(bounds.minX+bounds.maxX)*.5f,(bounds.minY+bounds.maxY)*.5f});
    session.onMouseMove(point.x,point.y);session.onMouseButtonDown(point.x,point.y,0);session.onMouseButtonUp(point.x,point.y,0);
    CHECK_INT_EQ(int(session.selectedEntityId()),int(e->getId()));
    session.update(.016f);sub->update(.016f);CHECK_INT_EQ(int(sub->renderScene().items().size()),5);
    session.setCameraDisplayMode(EditorCameraDisplayMode::Wireframe);session.update(.016f);sub->update(.016f);
    CHECK_INT_EQ(int(sub->renderScene().items().size()),4);
    session.setCameraDisplayMode(EditorCameraDisplayMode::Hidden);session.update(.016f);sub->update(.016f);CHECK(sub->renderScene().empty());
    // A selected Entity still owns its transform gizmo in Hidden mode. Clear it
    // by clicking empty space before checking that the hidden icon is unpickable.
    auto empty=session.ui().logicalToPhysical({bounds.minX+90,bounds.minY+90});
    session.onMouseMove(empty.x,empty.y);session.onMouseButtonDown(empty.x,empty.y,0);session.onMouseButtonUp(empty.x,empty.y,0);
    CHECK_INT_EQ(int(session.selectedEntityId()),0);
    session.onMouseMove(point.x,point.y);session.onMouseButtonDown(point.x,point.y,0);session.onMouseButtonUp(point.x,point.y,0);
    CHECK_INT_EQ(int(session.selectedEntityId()),0);
    session.setCameraDisplayMode(EditorCameraDisplayMode::Solid);
    EditorGameViewTestAccess::forceModeAndNotify(session.gameView(),EditorMode::Play);
    session.update(.016f);sub->update(.016f);CHECK(sub->renderScene().empty());
    EditorGameViewTestAccess::forceModeAndNotify(session.gameView(),EditorMode::Edit);
    session.update(.016f);sub->update(.016f);CHECK(!sub->renderScene().empty());
    session.shutdown();sub->update(.016f);CHECK(sub->renderScene().empty());sub->shutdown();
}

TEST_SUITE_END
