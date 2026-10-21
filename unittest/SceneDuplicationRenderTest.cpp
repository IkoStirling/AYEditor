#include "WorkshopSessionGpuFixture.h"
#include <AYEntity/CameraFrame.h>

int main(int argc,char** argv){try{
    Fixture f(argc>1?argv[1]:"out/scene-duplication-render",argc>2&&std::string(argv[2])=="d3d12");
    f.useEditorCamera=true;f.capture("duplicate-open.png");const auto sceneRect=f.sceneBounds();
    auto& world=f.session.document()->scene().world();const auto camera=f.session.sceneCamera().frame();
    entity::CameraFrame frame;frame.view=camera.view;frame.projection=camera.projection;frame.position=camera.position;
    entity::Entity* source=nullptr;
    for(auto* object:world.getAllEntities()){
        if(std::string(object->getName()).find("sphere")==std::string::npos||!object->getComponent<entity::MeshComponent>())continue;
        math::FVector3 screen;
        if(!entity::worldToScreen(frame,{sceneRect.minX,sceneRect.minY,sceneRect.width(),sceneRect.height()},
            object->worldMatrix().transformPoint({}),screen))continue;
        const auto point=f.session.ui().logicalToPhysical({screen.x,screen.y});
        f.session.onMouseMove(point.x,point.y);f.session.onMouseButtonDown(point.x,point.y,0);f.session.onMouseButtonUp(point.x,point.y,0);
        if(f.session.selectedEntityId()==object->getId()){source=object;break;}
    }
    require(source,"An actual visible sphere is selected through the Scene pointer route");
    const auto sourceIdentity=source->persistentId();const auto* sourceMesh=source->getComponent<entity::MeshComponent>();
    auto* child=world.createSpatialEntity();child->setName("Duplicate fixture child");
    child->transform()->setPosition(0,1.6f,0);child->transform()->setScale(.4f);
    require(child->setSpatialParent(source)&&child->setOwner(source),"Fixture child has both spatial and lifecycle relationships");
    auto* childMesh=child->createComponent<entity::MeshComponent>();childMesh->meshPath=sourceMesh->meshPath;childMesh->materialPath=sourceMesh->materialPath;
    auto* data=world.createDataEntity();data->setName("Duplicate fixture data");require(data->setOwner(source),"Fixture data is lifecycle-owned");
    const auto childIdentity=child->persistentId(),dataIdentity=data->persistentId();
    f.expectedMeshCount=26;const auto original=f.capture("duplicate-source-ready.png");const auto count=world.getAllEntities().size();
    f.session.onKeyDown(ui::UIKey_Control);require(f.session.onKeyDown('D'),"Actual Ctrl+D routes to Scene duplication");f.session.onKeyUp('D');f.session.onKeyUp(ui::UIKey_Control);
    auto* copy=world.findEntity(f.session.selectedEntityId());require(copy&&copy!=source,"Ctrl+D selects a new root");
    require(world.getAllEntities().size()==count+3,"Spatial/owner union duplicates each child once");
    require(copy->spatialChildren().size()==1&&copy->ownedEntities().size()==2,"Duplicate preserves both relationship domains");
    const auto copyIdentity=copy->persistentId();const auto copiedChildIdentity=copy->spatialChildren().front()->persistentId();
    require(copy->getComponent<entity::MeshComponent>()->materialPath==sourceMesh->materialPath,"Duplicate shares the original material reference");
    f.expectedMeshCount=28;f.capture("duplicate-coincident.png");
    editor::EditorTransformState moved{copy->transform()->position,copy->transform()->rotation,copy->transform()->scale};
    moved.position.x+=2.2f;moved.position.y+=1.4f;
    require(f.session.document()->executeTransform(copy->getId(),moved),"Move copied hierarchy through the document transform command");
    const auto placed=f.capture("duplicate-placed.png");const auto changed=Fixture::difference(original,placed,sceneRect,8);
    require(changed>1000,"The relocated copied hierarchy changes real Scene pixels");
    require(f.session.document()->undo(),"Undo the placement");require(f.session.document()->undo(),"Undo the whole duplicate as one edit");
    f.expectedMeshCount=26;require(world.getAllEntities().size()==count,"Undo removes exactly the duplicate closure");
    require(world.findEntityByPersistentId(childIdentity)==child&&world.findEntityByPersistentId(dataIdentity)==data,"Undo retains original children");
    const auto undone=f.capture("duplicate-undone.png");require(Fixture::difference(original,undone,sceneRect,1)==0,"Undo restores the original rendered Scene");
    require(f.session.document()->redo()&&f.session.document()->redo(),"Redo duplication and placement");f.expectedMeshCount=28;
    const auto redone=f.capture("duplicate-redone.png");require(Fixture::difference(placed,redone,sceneRect,1)==0,"Redo restores the same rendered hierarchy");
    std::string error;const auto path=f.session.document()->path();require(f.session.document()->save(&error),error);require(f.session.document()->open(path,&error),error);
    require(world.findEntityByPersistentId(copyIdentity)&&world.findEntityByPersistentId(copiedChildIdentity),"Copied identities persist on Scene reopen");
    require(world.findEntityByPersistentId(sourceIdentity),"Original identity also persists");
    const auto reopened=f.capture("duplicate-reopened.png");require(Fixture::difference(redone,reopened,sceneRect,1)==0,"Scene reopen retains the rendered copy and placement");
    std::cout<<"PASS actual Scene selection -> Ctrl+D mixed hierarchy -> shared resource references -> placement -> single duplicate undo -> redo -> Scene save/reopen; changed pixels="<<changed<<'\n';return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
