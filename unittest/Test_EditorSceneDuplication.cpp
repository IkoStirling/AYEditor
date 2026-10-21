#include <AYTest.h>
#include <AYEditor/EditorSceneDocument.h>
#include <AYEntity.h>
#include <AYEntity/ComponentRegistry.h>
#include <AYEntity/components/CameraControllerComponent.h>
#include <AYEntity/components/MeshComponent.h>
#include <AYScene.h>
#include <filesystem>
#include <set>
#include <string>

using namespace ayt;
using namespace ayt::editor;
namespace {
entity::Entity* named(const std::vector<entity::Entity*>& objects,const char* name) {
    for(auto* object:objects)if(std::string(object->getName())==name)return object;return nullptr;
}
struct UnserializableDuplicateProbe:entity::IComponent {
    const char* getName() const override{return "UnserializableDuplicateProbe";}
};
}
TEST_SUITE(AYEditor_SceneDuplication)
TEST_CASE(duplicate_closes_spatial_and_owner_graph_preserves_components_and_remaps_camera_targets)
{
    EditorSceneDocument document;auto& world=document.scene().world();
    auto* outside=world.createSpatialEntity();outside->setName("Outside");outside->transform()->setPosition(10,2,0);
    outside->transform()->setScale(3,1,2);outside->transform()->rotation=math::FQuaternion::fromEulerAngles({.2f,.4f,0});
    auto* root=world.createSpatialEntity();root->setName("Cluster");CHECK(root->setSpatialParent(outside));CHECK(root->setOwner(outside));
    root->transform()->setPosition(1,2,3);root->transform()->setScale(1,2,1);root->transform()->rotation=math::FQuaternion::fromEulerAngles({.4f,.2f,0});
    auto* child=world.createSpatialEntity();child->setName("SpatialOnly");CHECK(child->setSpatialParent(root));
    child->transform()->setPosition(0,0,3);
    auto* mesh=child->createComponent<entity::MeshComponent>();mesh->meshPath="meshes/user.msh";mesh->materialPath="materials/user.mat";
    auto* owned=world.createSpatialEntity();owned->setName("OwnedElsewhere");CHECK(owned->setOwner(root));CHECK(owned->setSpatialParent(outside));
    auto* data=world.createDataEntity();data->setName("OwnedData");CHECK(data->setOwner(root));
    auto* nested=world.createSpatialEntity();nested->setName("Nested");CHECK(nested->setOwner(data));CHECK(nested->setSpatialParent(child));
    auto* internal=root->createComponent<entity::CameraControllerComponent>();CHECK(internal->setTarget(child));
    auto* external=owned->createComponent<entity::CameraControllerComponent>();CHECK(external->setTarget(outside));
    const auto originalCount=world.getAllEntities().size();const auto originalMatrix=child->worldMatrix();
    const auto componentId=child->componentInstance(mesh)->id;
    uint32_t cloneId=0;std::string error;CHECK_MSG(document.duplicateEntityHierarchy(root->getId(),&cloneId,&error),error.c_str());
    CHECK(world.getAllEntities().size()==originalCount+5);auto* clone=world.findEntity(cloneId);CHECK(clone);
    if(!clone)return;
    CHECK(std::string(clone->getName())=="Cluster Copy");CHECK(clone->persistentId()!=root->persistentId());
    CHECK(clone->spatialParent()==outside);CHECK(clone->owner()==outside);
    auto* childCopy=named(clone->spatialChildren(),"SpatialOnly");auto* ownedCopy=named(clone->ownedEntities(),"OwnedElsewhere");
    auto* dataCopy=named(clone->ownedEntities(),"OwnedData");CHECK(childCopy&&ownedCopy&&dataCopy);
    if(!childCopy||!ownedCopy||!dataCopy)return;
    auto* nestedCopy=named(childCopy->spatialChildren(),"Nested");CHECK(nestedCopy);if(!nestedCopy)return;
    CHECK(nestedCopy->owner()==dataCopy);CHECK(dataCopy->kind()==entity::EntityKind::Data);CHECK(dataCopy->transform()==nullptr);
    CHECK(ownedCopy->spatialParent()==outside);CHECK(childCopy->owner()==nullptr);
    CHECK(clone->getComponent<entity::CameraControllerComponent>()->resolveTarget()==childCopy);
    CHECK(ownedCopy->getComponent<entity::CameraControllerComponent>()->resolveTarget()==outside);
    auto* meshCopy=childCopy->getComponent<entity::MeshComponent>();CHECK(meshCopy);
    if(meshCopy){CHECK(meshCopy->meshPath==mesh->meshPath);CHECK(meshCopy->materialPath==mesh->materialPath);CHECK(childCopy->componentInstance(meshCopy)->id==componentId);}
    const auto matrix=childCopy->worldMatrix();for(int row=0;row<4;++row)for(int col=0;col<4;++col)CHECK_FLOAT_EQ(matrix(row,col),originalMatrix(row,col),1e-5f);
    const std::vector<std::string> cloneIdentities={clone->persistentId(),childCopy->persistentId(),ownedCopy->persistentId(),dataCopy->persistentId(),nestedCopy->persistentId()};
    CHECK(std::set<std::string>(cloneIdentities.begin(),cloneIdentities.end()).size()==5);
    const auto childIdentity=childCopy->persistentId(),rootIdentity=clone->persistentId();
    EditorTransformState moved{childCopy->transform()->position,childCopy->transform()->rotation,childCopy->transform()->scale};moved.position.x=7;
    CHECK(document.executeTransform(childCopy->getId(),moved));CHECK(document.undo());CHECK(document.undo());
    CHECK_FALSE(document.canUndo());CHECK(world.getAllEntities().size()==originalCount);
    for(const auto& id:cloneIdentities)CHECK(world.findEntityByPersistentId(id)==nullptr);
    CHECK(child->spatialParent()==root);CHECK(internal->resolveTarget()==child);
    CHECK(document.redo());CHECK(document.redo());
    childCopy=world.findEntityByPersistentId(childIdentity);clone=world.findEntityByPersistentId(rootIdentity);CHECK(childCopy&&clone);
    if(!childCopy||!clone)return;
    CHECK_FLOAT_EQ(childCopy->transform()->position.x,7,1e-5f);
    CHECK(clone->getComponent<entity::CameraControllerComponent>()->resolveTarget()==childCopy);
    CHECK_FLOAT_EQ(child->transform()->position.x,0,1e-5f);
    const auto path=test::testTmpDir()/"duplicate-authoring.scn";
    CHECK_MSG(document.saveAs(path.string(),&error),error.c_str());CHECK_MSG(document.open(path.string(),&error),error.c_str());
    for(const auto& id:cloneIdentities)CHECK(world.findEntityByPersistentId(id));
    clone=world.findEntityByPersistentId(rootIdentity);childCopy=world.findEntityByPersistentId(childIdentity);
    CHECK(clone&&childCopy);if(clone&&childCopy)CHECK(clone->getComponent<entity::CameraControllerComponent>()->resolveTarget()==childCopy);
}
TEST_CASE(data_root_duplicate_is_one_command_uses_unique_names_and_refuses_untracked_dependents)
{
    EditorSceneDocument document;auto& world=document.scene().world();auto* root=world.createDataEntity();root->setName("Group");
    auto* child=world.createSpatialEntity();child->setName("Child");CHECK(child->setOwner(root));
    auto* occupied=world.createDataEntity();occupied->setName("Group Copy");
    uint32_t cloneId=0;CHECK(document.duplicateEntityHierarchy(root->getId(),&cloneId));auto* clone=world.findEntity(cloneId);CHECK(clone);
    if(!clone)return;CHECK(std::string(clone->getName())=="Group Copy 2");CHECK(clone->kind()==entity::EntityKind::Data);CHECK(clone->ownedEntities().size()==1);
    auto* outside=world.createDataEntity();CHECK(outside->setOwner(clone));const auto revision=document.revision(),count=world.getAllEntities().size();
    CHECK_FALSE(document.undo());CHECK(document.revision()==revision);CHECK(world.getAllEntities().size()==count);CHECK(outside->owner()==clone);
    CHECK(outside->setOwner(nullptr));CHECK(document.undo());CHECK(world.findEntity(cloneId)==nullptr);CHECK(world.findEntity(outside->getId())==outside);
    CHECK(document.redo());CHECK(document.canUndo());
}
TEST_CASE(unserializable_component_and_invalid_selection_refuse_before_creating_entities_or_history)
{
    auto& registry=entity::ComponentRegistry::instance();
    if(!registry.find(typeid(UnserializableDuplicateProbe)))CHECK(registry.registerType<UnserializableDuplicateProbe>("test.UnserializableDuplicateProbe"));
    EditorSceneDocument document;auto& world=document.scene().world();auto* root=world.createDataEntity();
    CHECK(root->createComponent<UnserializableDuplicateProbe>());const auto count=world.getAllEntities().size(),revision=document.revision();
    std::string error;uint32_t result=123;CHECK_FALSE(document.duplicateEntityHierarchy(root->getId(),&result,&error));CHECK(!error.empty());CHECK(result==123);
    CHECK_FALSE(document.duplicateEntityHierarchy(0,&result,&error));CHECK(result==123);CHECK(world.getAllEntities().size()==count);
    CHECK(document.revision()==revision);CHECK_FALSE(document.canUndo());CHECK_FALSE(document.isDirty());
}
TEST_CASE(missing_external_parent_during_delete_undo_rolls_back_owned_graph_without_dangling_pointers)
{
    EditorSceneDocument document;auto& world=document.scene().world();auto* root=world.createDataEntity();
    auto* first=world.createDataEntity();CHECK(first->setOwner(root));auto* outside=world.createSpatialEntity();
    auto* last=world.createSpatialEntity();CHECK(last->setOwner(root));CHECK(last->setSpatialParent(outside));
    CHECK(document.deleteEntity(root->getId()));world.destroyEntity(outside);CHECK(world.getAllEntities().empty());
    const auto revision=document.revision();CHECK_FALSE(document.undo());CHECK(world.getAllEntities().empty());CHECK(document.revision()==revision);
}
TEST_CASE(oversized_spatial_owner_union_refuses_before_clone_allocation)
{
    EditorSceneDocument document;auto& world=document.scene().world();auto* root=world.createDataEntity();
    for(int i=0;i<4096;++i){auto* child=world.createDataEntity();CHECK(child->setOwner(root));}
    const auto count=world.getAllEntities().size();std::string error;CHECK_FALSE(document.duplicateEntityHierarchy(root->getId(),nullptr,&error));
    CHECK(error.find("4096")!=std::string::npos);CHECK(world.getAllEntities().size()==count);CHECK_FALSE(document.canUndo());
}
TEST_SUITE_END
