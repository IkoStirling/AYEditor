#include "AYTest.h"
#include "fixtures/EditorShellLayout.h"
#include <AYEditor/EditorSession.h>
#include <AYEditor/EditorSceneDocument.h>
#include <AYEditor/EditorWorkspace.h>
#include <AYApplication/IEngineHost.h>
#include <AYApplication.h>
#include <AYEntity/EntityImpl.h>
#include <AYEntity/World.h>
#include <AYEntity/CameraFrame.h>
#include <AYEntity/components/TransformComponent.h>
#include <AYEntity/components/MeshComponent.h>
#include <AYResource/assetsImpl/Mesh.h>
#include <AYResource/ResourceBootstrap.h>
#include <AYResource/AssetPath.h>
#include <AYIO/File.h>
#include <stdexcept>
#include <AYScene.h>
#include <AYUI/MockRenderer.h>
#include <AYUI/Button.h>
#include <AYUI/CheckBox.h>
#include <AYUI/MenuItem.h>
#include <AYUI/ModalDialog.h>
#include <AYUI/TextInput.h>
#include <cmath>
#include <limits>
using namespace ayt;
using namespace ayt::editor;
namespace {
ui::Widget* find(ui::Widget* widget,const char* id){
    if(!widget)return nullptr;if(widget->getId()==id)return widget;
    for(auto* child:widget->getChildren())if(auto* found=find(child,id))return found;
    return nullptr;
}
bool click(EditorSession& session,ui::Widget* widget){
    if(!widget)return false;session.ui().layout();const auto bounds=widget->getWorldBounds();
    const auto p=session.ui().logicalToPhysical({(bounds.minX+bounds.maxX)*.5f,(bounds.minY+bounds.maxY)*.5f});
    session.onMouseMove(p.x,p.y);
    const bool down=session.onMouseButtonDown(p.x,p.y,0);
    const bool up=session.onMouseButtonUp(p.x,p.y,0);return down&&up;
}
struct SessionFixture {
    app::EngineHostScope host{app::defaultEngineHost()};ui::MockRenderer backend;EditorSession session;
    SessionFixture(){EditorSessionDesc desc;desc.uiBackend=&backend;desc.layoutPath=testfixtures::resolveEditorShellLayoutPath();
        if(!session.initialize(desc))throw std::runtime_error("Scene efficiency Session initialization failed");
        session.setClientSize(1280,900);session.ui().layout();}
    ~SessionFixture(){session.shutdown();}
    entity::Entity* selectOrigin(){
        auto* object=session.document()->scene().world().createSpatialEntity();
        auto* viewport=session.ui().findById("panel_viewport");
        if(!click(session,viewport)||session.selectedEntityId()!=object->getId())
            throw std::runtime_error("Scene pointer selection failed");
        return object;
    }
};
bool close(float a,float b){return std::fabs(a-b)<1e-4f;}
}
TEST_SUITE(AYEditor_SceneEfficiency)
TEST_CASE(snap_settings_modal_validates_pending_values_and_restores_preferences)
{
    SessionFixture fixture;auto& session=fixture.session;
    auto* settings=session.ui().findById("menu_transform_snap_settings");
    auto* options=session.ui().findById("btn_view_options");
    CHECK(settings&&options);if(!settings||!options)return;
    CHECK(click(session,options));CHECK(click(session,settings));
    auto* dialog=dynamic_cast<ui::ModalDialog*>(find(session.ui().getOverlayRoot(),"transform_snap_dialog"));
    CHECK(dialog);if(!dialog)return;
    auto* move=dynamic_cast<ui::TextInput*>(find(dialog,"transform_snap_translation"));
    auto* rotation=dynamic_cast<ui::TextInput*>(find(dialog,"transform_snap_rotation"));
    auto* scale=dynamic_cast<ui::TextInput*>(find(dialog,"transform_snap_scale"));
    auto* enabled=dynamic_cast<ui::CheckBox*>(find(dialog,"transform_snap_enabled"));
    CHECK(move&&rotation&&scale&&enabled);if(!move||!rotation||!scale||!enabled)return;
    const auto before=session.transformSnap();const auto revision=session.document()->revision();
    move->setText(L"nan");dialog->acceptDialog();
    CHECK(dialog->isOpen());CHECK(session.transformSnap()==before);
    move->setText(L"0.25");rotation->setText(L"30");scale->setText(L"0.25");enabled->setChecked(true);
    ui::Button* apply=nullptr;
    for(auto* child:dialog->getChildren())if(auto* button=dynamic_cast<ui::Button*>(child);
        button&&button->getText()==dialog->getAcceptText())apply=button;
    CHECK(click(session,apply));CHECK_FALSE(dialog->isOpen());
    CHECK(session.transformSnap().enabled);CHECK(close(session.transformSnap().translation,.25f));
    CHECK(close(session.transformSnap().rotationDegrees,30));CHECK(close(session.transformSnap().scale,.25f));
    CHECK(session.document()->revision()==revision);CHECK_FALSE(session.document()->isDirty());
    const auto saved=session.currentPreferences();
    auto invalid=saved.transformSnap;invalid.scale=std::numeric_limits<float>::infinity();
    CHECK_FALSE(session.setTransformSnap(invalid));CHECK(session.transformSnap()==saved.transformSnap);
    session.shutdown();
    EditorSession restored;EditorSessionDesc desc;desc.uiBackend=&fixture.backend;
    desc.layoutPath=testfixtures::resolveEditorShellLayoutPath();desc.preferences=saved;
    CHECK(restored.initialize(desc));CHECK(restored.transformSnap()==saved.transformSnap);
    CHECK(restored.currentPreferences().schemaVersion==EditorPreferences::kCurrentSchemaVersion);
    restored.shutdown();
}
TEST_CASE(frame_selection_fits_spatial_children_without_editing_scene_and_respects_text_focus)
{
    SessionFixture fixture;auto& session=fixture.session;auto* parent=fixture.selectOrigin();
    auto* child=session.document()->scene().world().createSpatialEntity();
    CHECK(child->setSpatialParent(parent,entity::ReparentMode::KeepLocal));
    child->transform()->setPosition(4,0,0);
    parent->transform()->setPosition(12,4,2);parent->transform()->setScale(3,2,1);
    const auto revision=session.document()->revision();const auto dirty=session.document()->isDirty();
    const auto undo=session.document()->canUndo();const auto parentMatrix=parent->worldMatrix();
    CHECK(session.frameSelectedEntity());
    const math::FVector3 expectedCenter{18,4,2};
    const auto direction=(expectedCenter-session.freecam().eye()).normalize();
    CHECK(direction.dot(session.freecam().forward())>.99999f);
    CHECK((expectedCenter-session.freecam().eye()).length()>8.f);
    CHECK(session.document()->revision()==revision);CHECK(session.document()->isDirty()==dirty);
    CHECK(session.document()->canUndo()==undo);
    CHECK(close(parent->worldMatrix().transformPoint({0,0,0}).x,parentMatrix.transformPoint({0,0,0}).x));
    parent->transform()->setPosition(-8,4,2);
    session.ui().setFocus(nullptr);
    CHECK(session.onKeyDown('F'));session.onKeyUp('F');
    const math::FVector3 nextCenter{-2,4,2};
    CHECK((nextCenter-session.freecam().eye()).normalize().dot(session.freecam().forward())>.99999f);
    const auto eye=session.freecam().eye();
    auto* text=new ui::TextInput();session.ui().root()->addChild(text);session.ui().setFocus(text);
    parent->transform()->setPosition(20,4,2);
    session.onKeyDown('F');session.onKeyUp('F');
    CHECK(close(session.freecam().eye().x,eye.x));CHECK(close(session.freecam().eye().y,eye.y));
    session.ui().setFocus(nullptr);
    parent->transform()->setScale(10000,10000,10000);
    CHECK_FALSE(session.frameSelectedEntity());CHECK(close(session.freecam().eye().x,eye.x));
    CHECK(click(session,session.ui().findById("btn_view_mode")));
    CHECK(session.sceneViewMode()==SceneViewMode::TwoD);
    CHECK_FALSE(session.frameSelectedEntity());
}
TEST_CASE(actual_viewport_drag_snaps_as_one_undoable_edit_with_ctrl_override_and_scene_roundtrip)
{
    SessionFixture fixture;auto& session=fixture.session;auto* object=fixture.selectOrigin();
    const auto identity=object->persistentId();
    EditorTransformSnap snap;snap.enabled=true;snap.translation=.5f;CHECK(session.setTransformSnap(snap));
    math::FRectangle viewport;CHECK(session.getViewportBounds(viewport));
    const auto frame=session.sceneCamera().frame();entity::CameraFrame camera;
    camera.view=frame.view;camera.projection=frame.projection;camera.position=frame.position;
    const auto project=[&](math::FVector3 p){math::FVector3 screen;
        if(!entity::worldToScreen(camera,{viewport.minX,viewport.minY,viewport.width(),viewport.height()},p,screen))
            throw std::runtime_error("Project genuine Gizmo handle into the Session viewport");
        return session.ui().logicalToPhysical({screen.x,screen.y});};
    const float size=EditorTransformGizmo::worldScale({},session.freecam().eye());
    const math::FVector3 start{size*.78f,0,0};
    const auto from=project(start),to=project(start+math::FVector3{.74f,0,0});
    const auto drag=[&]{session.onMouseMove(from.x,from.y);CHECK(session.onMouseButtonDown(from.x,from.y,0));
        CHECK(session.onMouseMove(to.x,to.y));CHECK(session.onMouseButtonUp(to.x,to.y,0));};
    drag();CHECK(close(object->transform()->position.x,.5f));CHECK(close(object->transform()->position.y,0));
    CHECK(session.document()->canUndo());
    session.onKeyDown(ui::UIKey_Control);CHECK(session.onKeyDown('Z'));session.onKeyUp('Z');session.onKeyUp(ui::UIKey_Control);
    CHECK(close(object->transform()->position.x,0));CHECK_FALSE(session.document()->canUndo());
    session.onKeyDown(ui::UIKey_Control);drag();session.onKeyUp(ui::UIKey_Control);
    CHECK_MSG(close(object->transform()->position.x,.74f),std::to_string(object->transform()->position.x).c_str());CHECK(close(object->transform()->position.y,0));
    std::string error;const auto path=ayt::test::testTmpDir()/"scene-efficiency-roundtrip.scn";
    CHECK_MSG(session.document()->saveAs(path.string(),&error),error.c_str());
    CHECK_MSG(session.document()->open(path.string(),&error),error.c_str());
    auto* reopened=session.document()->scene().world().findEntityByPersistentId(identity);
    CHECK(reopened);if(reopened)CHECK(close(reopened->transform()->position.x,.74f));
}
TEST_CASE(pitched_scene_camera_rays_match_actual_perspective_and_orthographic_projection)
{
    EditorSceneCamera camera;camera.setViewport(900,500);
    camera.threeD().setPose({4,3,5},-2.4f,-.5f);
    for(const auto mode:{ProjectionMode::Perspective,ProjectionMode::Orthographic}){
        camera.setThreeDProjection(mode);const auto f=camera.frame();entity::CameraFrame frame;
        frame.view=f.view;frame.projection=f.projection;frame.position=f.position;
        for(const math::FVector3 point:{math::FVector3{0,0,0},math::FVector3{1.5f,1,0},math::FVector3{2,-1,2}}){
            math::FVector3 screen,origin,direction;
            CHECK(entity::worldToScreen(frame,{0,0,900,500},point,screen));
            CHECK(camera.ray({screen.x,screen.y},origin,direction));
            const float miss=(point-origin).cross(direction).length();
            const auto message=std::string(mode==ProjectionMode::Perspective?"perspective":"orthographic")
                +" projection/ray world miss="+std::to_string(miss);
            CHECK_MSG(miss<.001f,message.c_str());
            CHECK((point-origin).dot(direction)>0);
        }
    }
}
TEST_CASE(actual_ctrl_d_duplicates_selection_as_one_edit_and_text_focus_keeps_its_shortcut)
{
    SessionFixture fixture;auto& session=fixture.session;auto* root=fixture.selectOrigin();root->setName("Assembly");
    auto& world=session.document()->scene().world();auto* child=world.createSpatialEntity();child->setName("Child");
    child->transform()->setPosition(3,0,0);CHECK(child->setSpatialParent(root));
    const auto originalIdentity=root->persistentId();const auto count=world.getAllEntities().size();
    session.onKeyDown(ui::UIKey_Control);CHECK(session.onKeyDown('D'));session.onKeyUp('D');session.onKeyUp(ui::UIKey_Control);
    auto* copied=world.findEntity(session.selectedEntityId());CHECK(copied&&copied!=root);if(!copied||copied==root)return;
    CHECK(world.getAllEntities().size()==count+2);CHECK(copied->spatialChildren().size()==1);
    const auto identity=copied->persistentId();CHECK(session.document()->canUndo());CHECK(session.document()->undo());
    CHECK(world.getAllEntities().size()==count);CHECK_FALSE(session.document()->canUndo());CHECK(session.document()->redo());
    CHECK(world.findEntityByPersistentId(identity));CHECK(world.findEntityByPersistentId(originalIdentity)==root);
    auto* field=dynamic_cast<ui::TextInput*>(session.ui().findById("inspector_entity_name"));
    CHECK(field);if(field){session.ui().setFocus(field);const auto before=world.getAllEntities().size();
        session.onKeyDown(ui::UIKey_Control);(void)session.onKeyDown('D');session.onKeyUp('D');session.onKeyUp(ui::UIKey_Control);
        CHECK(world.getAllEntities().size()==before);session.ui().setFocus(nullptr);}
}
TEST_CASE(virtual_mesh_bounds_drive_picking_and_framing_without_a_scaled_floor_stealing_selection)
{
    SessionFixture fixture;auto& session=fixture.session;const auto previous=resource::assetRoots();
    struct RestoreRoots {std::vector<std::string> roots;~RestoreRoots(){resource::setAssetRoots(roots);}} restore{previous};
    CHECK(resource::initializeLoaders());const auto assets=test::testTmpDir()/"scene-bound-assets";
    std::filesystem::create_directories(assets/"meshes");resource::Mesh unitMesh,largeMesh;
    unitMesh.createCube(1);largeMesh.createCube(20);std::vector<UInt8> bytes;
    CHECK(unitMesh.saveToBinary(bytes));CHECK(io::File::atomicWriteUtf8((assets/"meshes/unit.msh").string(),bytes.data(),bytes.size()));
    CHECK(largeMesh.saveToBinary(bytes));CHECK(io::File::atomicWriteUtf8((assets/"meshes/large.msh").string(),bytes.data(),bytes.size()));
    resource::setAssetRoots({assets.string()});auto& world=session.document()->scene().world();
    auto* target=world.createSpatialEntity();target->createComponent<entity::MeshComponent>()->meshPath="meshes/unit.msh";
    auto* floor=world.createSpatialEntity();floor->createComponent<entity::MeshComponent>()->meshPath="meshes/unit.msh";
    floor->transform()->setPosition(0,-3,0);floor->transform()->setScale(20,.1f,20);
    CHECK(click(session,session.ui().findById("panel_viewport")));CHECK(session.selectedEntityId()==target->getId());
    target->getComponent<entity::MeshComponent>()->meshPath="meshes/large.msh";
    const auto revision=session.document()->revision();CHECK(session.frameSelectedEntity());
    CHECK(session.freecam().eye().length()>30.f);CHECK(session.document()->revision()==revision);
}
TEST_SUITE_END
