#include "WorkshopSessionGpuFixture.h"
#include <AYEditor/EditorAssetImportQueue.h>
#include <AYResource/assetsDefs/IMesh.h>
#include <AYEntity/CameraFrame.h>
#include <AYUI/Button.h>
#include <AYUI/ComboBox.h>
#include <AYUI/ListView.h>
#include <AYUI/ModalDialog.h>
#include <AYUI/ScrollView.h>
#include <AYUI/TextInput.h>
#include <AYUI/TileView.h>
#include <fstream>
#include <chrono>

namespace {
void write(const fs::path& path,const std::string& bytes){fs::create_directories(path.parent_path());
    std::ofstream f(path,std::ios::binary|std::ios::trunc);f.write(bytes.data(),std::streamsize(bytes.size()));require(bool(f),"Write author fixture");}
std::string read(const fs::path& path){std::ifstream f(path,std::ios::binary);require(bool(f),"Read authored file");return {std::istreambuf_iterator<char>(f),{}};}
void png(const fs::path& path,bool red){const std::string hex=red
    ?"89504e470d0a1a0a0000000d494844520000000200000002080600000072b60d240000001549444154789c637c6620f69f81818181094480300021b8022f99fc55610000000049454e44ae426082"
    :"89504e470d0a1a0a0000000d494844520000000200000002080600000072b60d240000001549444154789c63140c3df39f818181810944803000208d0235933bc0420000000049454e44ae426082";
    std::string bytes;for(std::size_t i=0;i<hex.size();i+=2)bytes.push_back(char(std::stoi(hex.substr(i,2),nullptr,16)));write(path,bytes);}
ui::Widget* find(ui::Widget* root,const char* id,bool visible=false){if(!root||(visible&&!root->isVisible()))return nullptr;
    if(root->getId()==id)return root;for(auto* child:root->getChildren())if(auto* found=find(child,id,visible))return found;return nullptr;}
void click(Fixture& f,ui::Widget* widget){require(widget,"Widget exists for actual pointer click");f.session.ui().layout();
    for(auto* parent=widget->getParent();parent;parent=parent->getParent())if(auto* scroll=dynamic_cast<ui::ScrollView*>(parent)){
        const auto bounds=widget->getWorldBounds(),client=scroll->getClientRect();scroll->scrollBy({0,bounds.minY-client.minY-12});f.session.ui().layout();break;}
    const auto b=widget->getWorldBounds();const auto p=f.session.ui().logicalToPhysical({(b.minX+b.maxX)*.5f,(b.minY+b.maxY)*.5f});
    f.session.onMouseMove(p.x,p.y);require(f.session.onMouseButtonDown(p.x,p.y,0),"Pointer down reaches actual widget");
    require(f.session.onMouseButtonUp(p.x,p.y,0),"Pointer up reaches actual widget");}
ui::Button* accept(ui::ModalDialog* dialog){require(dialog,"Actual modal is open");for(auto* child:dialog->getChildren())
    if(auto* button=dynamic_cast<ui::Button*>(child);button&&button->getText()==dialog->getAcceptText())return button;return nullptr;}
std::shared_ptr<editor::EditorMaterialDocument> active(Fixture& f){const auto* record=f.session.workspace().documents().active();
    require(record,"Material becomes an active document");auto doc=std::dynamic_pointer_cast<editor::EditorMaterialDocument>(record->document);
    require(bool(doc),"Active document has the material authoring type");return doc;}
math::FRectangle previewBounds(Fixture& f){auto* preview=find(f.session.ui().root(),"material_preview",true);require(preview,"Active preview is visible");
    const auto b=preview->getWorldBounds();const auto a=f.session.ui().logicalToPhysical({b.minX,b.minY}),c=f.session.ui().logicalToPhysical({b.maxX,b.maxY});return {a.x,a.y,c.x,c.y};}
}
int main(int argc,char** argv){try{
    Fixture f(argc>1?argv[1]:"out/material-authoring-workflow",argc>2&&std::string(argv[2])=="d3d12",true);
    f.useEditorCamera=true;resource::setAssetRoots({utf8(f.assets),f.session.assetDatabase().derivedRoot(),utf8(f.assets/"materials")});
    const auto source=f.output/"Author source/Triangle.gltf";const float positions[]={0,0,0,1,0,0,0,1,0};
    write(source.parent_path()/"Triangle.bin",std::string(reinterpret_cast<const char*>(positions),sizeof(positions)));
    png(source.parent_path()/"Surface.png",true);
    write(source,R"({"asset":{"version":"2.0"},"buffers":[{"uri":"Triangle.bin","byteLength":36}],
"bufferViews":[{"buffer":0,"byteLength":36}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],
"images":[{"uri":"Surface.png"}],"textures":[{"source":0}],"materials":[{"name":"Imported author surface","pbrMetallicRoughness":{"baseColorTexture":{"index":0},"metallicFactor":0,"roughnessFactor":0.5}}],
"meshes":[{"name":"Triangle","primitives":[{"attributes":{"POSITION":0},"material":0}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})");
    resource::ImportOptions options;options.sourcePath=utf8(source);options.outputDir=f.session.assetDatabase().derivedRoot();options.requireCharacterAssets=false;
    auto* queue=f.session.assetImportQueue();require(queue,"Session owns an import queue");const auto job=queue->enqueue(options);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    while(queue->find(job)&&!queue->find(job)->finished()&&std::chrono::steady_clock::now()<deadline)f.frame();f.frame();
    const auto* result=queue->find(job);require(result&&result->state==editor::EditorAssetImportJobState::Succeeded,result?result->message:"Import job disappeared");
    std::string meshPath;for(const auto& path:result->outputPaths)if(fs::u8path(path).extension()==".msh"){meshPath=path;break;}
    require(!meshPath.empty(),"Real model import produced a mesh");const auto mesh=resource::ResourceManager::instance().load<resource::IMesh>(utf8(fs::u8path(f.session.assetDatabase().derivedRoot())/fs::u8path(meshPath)));
    require(mesh&&mesh->getMaterialSlotCount()>0,"Actual mesh reports its material slot");const std::string importedPath=mesh->getMaterialSlot(0);
    require(f.session.rescanAssetsNow(),"Index the actual imported material");const auto* importedRecord=f.session.assetDatabase().findByLogicalPath("Imported/"+importedPath);
    require(importedRecord,"The imported material is a browser asset");const auto importedAbsolute=importedRecord->absolutePath;const auto importedAssetId=importedRecord->id;const auto sourceBytes=read(fs::u8path(importedAbsolute));
    require(f.session.openAsset(importedAssetId),"Open imported material through the actual asset browser route");auto imported=active(f);require(imported->readOnly(),"Imported material remains read-only");
    const auto importedId=f.session.workspace().documents().activeDocumentId();const auto importedSettings=imported->settings();
    const auto readonlyPreview=f.capture("workflow-imported-readonly.png");const auto previewRect=previewBounds(f);
    click(f,find(f.session.ui().root(),"material_create_copy",true));
    auto* copyDialog=dynamic_cast<ui::ModalDialog*>(find(f.session.ui().getOverlayRoot(),"material_copy_dialog"));require(copyDialog,"Copy dialog is mounted in the real overlay");
    auto* path=dynamic_cast<ui::TextInput*>(find(copyDialog,"material_copy_path"));require(path,"Copy dialog exposes the destination");path->setText(L"materials/WorkflowCopy.mat");click(f,accept(copyDialog));
    auto copied=active(f);require(copied!=imported&&!copied->readOnly(),"Copy opens a separate writable document");
    const auto copiedId=f.session.workspace().documents().activeDocumentId();const auto copiedPreview=f.capture("workflow-copy-open.png");
    require(Fixture::difference(readonlyPreview,copiedPreview,previewRect,2)==0,"Copy preserves the actual imported appearance");
    require(imported->settings()==importedSettings&&read(fs::u8path(importedAbsolute))==sourceBytes,"Copy does not alter the imported document or file");
    f.activateScene();const auto originalScene=f.capture("workflow-scene-unassigned.png");const auto sceneRect=f.sceneBounds();
    png(f.assets/"textures/WorkflowBlue.png",false);require(f.session.rescanAssetsNow(),"New user texture enters the project catalog");
    const auto* copyRecord=f.session.assetDatabase().findByLogicalPath("Assets/materials/WorkflowCopy.mat");require(copyRecord,"Copied file enters Assets");const auto copyAssetId=copyRecord->id;
    require(f.session.openAsset(copyAssetId),"Reopen/focus the copy through Assets");
    click(f,find(f.session.ui().root(),"material_browse_baseColorTexture",true));
    auto* picker=dynamic_cast<ui::ModalDialog*>(find(f.session.ui().getOverlayRoot(),"material_texture_dialog"));require(picker,"Actual catalog picker is open");
    auto* search=dynamic_cast<ui::TextInput*>(find(picker,"material_texture_search"));auto* choices=dynamic_cast<ui::ListView*>(find(picker,"material_texture_choices"));
    require(search&&choices,"Picker has filtering and selection controls");search->setText(L"WorkflowBlue");require(choices->getItemCount()==2,"Filtering finds the actual new texture and clear entry");choices->setSelectedIndex(1);
    f.capture("workflow-texture-picker.png");click(f,accept(picker));require(copied->isDirty(),"Choosing a texture is an unsaved material edit");
    const auto editedPreview=f.capture("workflow-copy-preview-edited.png");const auto previewPixels=Fixture::difference(copiedPreview,editedPreview,previewBounds(f),8);require(previewPixels>1000,"Selected source texture changes the real GPU preview");
    f.activateScene();const auto unsavedScene=f.capture("workflow-scene-copy-unsaved.png");require(Fixture::difference(originalScene,unsavedScene,sceneRect,1)==0,"Unassigned unsaved copy does not mutate Scene materials");
    require(f.session.openAsset(copyAssetId),"Focus copy for actual Ctrl+S");f.session.onKeyDown(ui::UIKey_Control);require(f.session.onKeyDown(ui::UIKey_S),"Ctrl+S reaches the active material view");f.session.onKeyUp(ui::UIKey_S);f.session.onKeyUp(ui::UIKey_Control);require(!copied->isDirty(),"Copy saves through the standard command route");
    f.activateScene();auto* mode=dynamic_cast<ui::ComboBox*>(f.session.ui().findById("cmb_assets_view"));auto* list=dynamic_cast<ui::TileView*>(f.session.ui().findById("list_assets"));require(mode&&list,"Actual Assets tile source exists");
    mode->setSelectedIndexAndNotify(1);f.frame();int index=-1;for(std::size_t i=0;i<list->getItemCount();++i)if(list->getItem(i).find(L"materials/WorkflowCopy.mat")!=std::wstring::npos){index=int(i);break;}
    require(index>=0,"Find copied material in the actual browser");list->scrollToIndex(index);f.session.ui().layout();auto* cell=list->cellForLogicalIndex(index);require(cell,"Copied material has a visible drag cell");
    const auto thumb=cell->getThumbnailRect();const auto press=f.session.ui().logicalToPhysical({(thumb.minX+thumb.maxX)*.5f,(thumb.minY+thumb.maxY)*.5f});
    const auto frame=f.session.sceneCamera().frame();entity::CameraFrame camera;camera.view=frame.view;camera.projection=frame.projection;camera.position=frame.position;
    math::FVector2 drop{};bool found=false;std::unordered_map<std::string,std::string> previous;
    for(auto* object:f.session.document()->scene().world().getAllEntities())if(auto* component=object->getComponent<entity::MeshComponent>()){
        previous[object->persistentId()]=component->materialPath;if(found)continue;math::FVector3 screen;
        if(entity::worldToScreen(camera,{sceneRect.minX,sceneRect.minY,sceneRect.width(),sceneRect.height()},object->worldMatrix().transformPoint({}),screen)
            &&screen.x>sceneRect.minX+30&&screen.x<sceneRect.maxX-30&&screen.y>sceneRect.minY+30&&screen.y<sceneRect.maxY-30){drop=f.session.ui().logicalToPhysical({screen.x,screen.y});found=true;}
    }
    require(found,"An actual Scene mesh projects into the viewport");const auto entityCount=f.session.document()->scene().world().getAllEntities().size();
    f.session.onMouseMove(press.x,press.y);require(f.session.onMouseButtonDown(press.x,press.y,0),"Press actual material drag source");require(f.session.onMouseMove(drop.x,drop.y),"Move drag over the Scene");require(f.session.ui().isDragging(),"Material starts an actual UI drag");require(f.session.onMouseButtonUp(drop.x,drop.y,0),"Drop material onto the actual viewport");
    auto& world=f.session.document()->scene().world();require(world.getAllEntities().size()==entityCount,"Material assignment does not spawn geometry");
    const auto countAssigned=[&]{std::size_t count=0;for(auto* object:world.getAllEntities())if(auto* component=object->getComponent<entity::MeshComponent>())if(component->materialPath=="materials/WorkflowCopy.mat")++count;return count;};
    require(countAssigned()==1,"One picked Mesh receives the copied material");require(f.session.document()->undo(),"Assignment is undoable");require(countAssigned()==0,"Undo restores previous materials");require(f.session.document()->redo(),"Assignment is redoable");require(countAssigned()==1,"Redo restores the exact assignment");
    const auto assigned=f.capture("workflow-scene-assigned.png");const auto scenePixels=Fixture::difference(originalScene,assigned,sceneRect,8);require(scenePixels>500,"Assigned copy changes actual Scene pixels");
    std::string error;const auto scenePath=f.session.document()->path();require(f.session.document()->save(&error),error);require(f.session.document()->open(scenePath,&error),error);
    const auto reopened=f.capture("workflow-scene-reopened.png");require(Fixture::difference(assigned,reopened,sceneRect,1)==0,"Scene reopens with the same rendered material assignment");
    require(bool(f.session.workspace().documents().close(importedId,editor::EditorDocumentCloseAction::Discard)),"Close the original imported preview");
    require(bool(f.session.workspace().documents().close(copiedId,editor::EditorDocumentCloseAction::Discard)),"Close the copied preview");f.activateScene();
    const auto previewsClosed=f.capture("workflow-scene-previews-closed.png");require(Fixture::difference(reopened,previewsClosed,sceneRect,1)==0,"Closing shared-program previews retains the Scene material");
    require(imported->settings()==importedSettings&&read(fs::u8path(imported->path()))==sourceBytes,"Original imported material remains unchanged throughout authoring");
    std::cout<<"PASS real import -> read-only preview -> editable copy -> catalog texture edit -> isolated preview -> Ctrl+S -> actual material drag -> undo/redo -> Scene save/reopen -> preview teardown; preview pixels="<<previewPixels<<"; Scene pixels="<<scenePixels<<'\n';return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}}
