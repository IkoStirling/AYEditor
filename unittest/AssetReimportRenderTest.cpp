#include "WorkshopSessionGpuFixture.h"
#include <AYEditor/EditorAssetImportQueue.h>
#include <AYResource/assetsDefs/IMesh.h>
#include <AYResource/assetsImpl/Texture.h>
#include <AYResource/assetsImpl/Material.h>
#include <AYResource/Loader/MaterialLoader.h>
#include <AYUI/Button.h>
#include <AYUI/ComboBox.h>
#include <AYUI/TileView.h>
#include <AYUI/TextInput.h>
#include <AYUI/ModalDialog.h>
#include <chrono>
#include <fstream>

namespace {
void writeFile(const fs::path& path, const std::string& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(bool(file), "Write authored source fixture");
}
void writeBox(const fs::path& source, float extent)
{
    const float positions[] = {-extent,-.5f,-.5f, extent,-.5f,-.5f,
        extent,.5f,-.5f, -extent,.5f,-.5f, -extent,-.5f,.5f,
        extent,-.5f,.5f, extent,.5f,.5f, -extent,.5f,.5f};
    const uint16_t indices[] = {0,2,1,0,3,2,4,5,6,4,6,7,0,1,5,0,5,4,
        3,7,6,3,6,2,0,4,7,0,7,3,1,2,6,1,6,5};
    const float uv[] = {0,0,1,0,1,1,0,1,0,0,1,0,1,1,0,1};
    std::string bytes(reinterpret_cast<const char*>(positions), sizeof(positions));
    bytes.append(reinterpret_cast<const char*>(indices), sizeof(indices));
    bytes.append(reinterpret_cast<const char*>(uv), sizeof(uv));
    writeFile(source.parent_path() / "UserBox.bin", bytes);
    writeFile(source, R"({"asset":{"version":"2.0"},"buffers":[{"uri":"UserBox.bin","byteLength":232}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":96},{"buffer":0,"byteOffset":96,"byteLength":72},{"buffer":0,"byteOffset":168,"byteLength":64}],
"accessors":[{"bufferView":0,"componentType":5126,"count":8,"type":"VEC3","min":[-3,-1,-1],"max":[3,1,1]},
{"bufferView":1,"componentType":5123,"count":36,"type":"SCALAR"},
{"bufferView":2,"componentType":5126,"count":8,"type":"VEC2","min":[0,0],"max":[1,1]}],
"images":[{"uri":"Surface.png"}],"textures":[{"source":0}],
"materials":[{"name":"Imported surface","pbrMetallicRoughness":{"baseColorTexture":{"index":0},"metallicFactor":0,"roughnessFactor":0.55}}],
"meshes":[{"name":"Box","primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":2},"indices":1,"material":0}]}],
"nodes":[{"name":"Box","mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})");
}
void writeTexture(const fs::path& source, bool red)
{
    const std::string hex = red
        ? "89504e470d0a1a0a0000000d494844520000000200000002080600000072b60d240000001549444154789c637c6620f69f81818181094480300021b8022f99fc55610000000049454e44ae426082"
        : "89504e470d0a1a0a0000000d494844520000000200000002080600000072b60d240000001549444154789c63140c3df39f818181810944803000208d0235933bc0420000000049454e44ae426082";
    std::string bytes;
    for(std::size_t i=0;i<hex.size();i+=2)bytes.push_back(char(std::stoi(hex.substr(i,2),nullptr,16)));
    writeFile(source,bytes);
}
void writeDds(const fs::path& path, int channels = 0)
{
    std::string bytes(channels == 2 ? 144 : 136,'\0');
    const auto put=[&](std::size_t offset,std::uint32_t value){
        for(int byte=0;byte<4;++byte)bytes[offset+std::size_t(byte)]=char((value>>(byte*8))&255);
    };
    put(0,0x20534444);put(4,124);put(8,0x81007);put(12,4);put(16,4);
    put(20,channels == 2 ? 16 : 8);put(28,1);put(76,32);put(80,4);
    put(84,channels == 1 ? 0x55344342 : channels == 2 ? 0x55354342 : 0x31545844);put(108,0x1000);
    if (channels == 0) {
        bytes[128]=0;bytes[129]=char(0xf8); // Opaque BC1 red, all pixel indices zero.
    } else {
        bytes[128]=96; // BC4 first endpoint, all pixel indices zero.
        if (channels == 2) bytes[136]=char(200); // Independent BC5 green block.
    }
    writeFile(path,bytes);
}
void writeSamplingDds(const fs::path& path)
{
    std::string bytes(152,'\0');
    const auto put=[&](size_t offset,uint32_t value){
        for(int i=0;i<4;++i)bytes[offset+size_t(i)]=char(value>>(8*i));
    };
    put(0,0x20534444);put(4,124);put(8,0xa1007);put(12,4);put(16,4);
    put(20,8);put(28,3);put(76,32);put(80,4);put(84,0x31545844);put(108,0x401008);
    bytes[128]=0;bytes[129]=char(0xf8); // BC1 red endpoint.
    bytes[130]=char(0xe0);bytes[131]=7; // BC1 green endpoint.
    for(int y=0;y<4;++y){
        uint8_t indices=0;
        for(int x=0;x<4;++x)indices|=uint8_t(((x+y)&1)<<(x*2));
        bytes[132+size_t(y)]=char(indices);
    }
    // Independent authored coarser colors, all pixel indices zero.
    bytes[136]=bytes[144]=char(0x10);bytes[137]=bytes[145]=char(0x84);
    writeFile(path,bytes);
}
const editor::EditorAssetImportJob& finish(Fixture& fixture, editor::EditorAssetImportJobId id)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    auto* queue = fixture.session.assetImportQueue();
    while (queue->find(id) && !queue->find(id)->finished()
        && std::chrono::steady_clock::now() < deadline) fixture.frame();
    fixture.frame(); // Deliver even an immediately failed completion through Session.
    require(queue->find(id) && queue->find(id)->finished(), "Import finishes within the bounded fixture");
    return *queue->find(id);
}
void click(Fixture& fixture, ui::Button& button)
{
    fixture.session.ui().layout();
    const auto bounds = button.getWorldBounds();
    const auto point = fixture.session.ui().logicalToPhysical(
        {(bounds.minX + bounds.maxX) * .5f, (bounds.minY + bounds.maxY) * .5f});
    fixture.session.onMouseMove(point.x, point.y);
    fixture.session.onMouseButtonDown(point.x, point.y, 0);
    fixture.session.onMouseButtonUp(point.x, point.y, 0);
}
}

int main(int argc, char** argv)
{
    try {
        Fixture fixture(argc > 1 ? argv[1] : "out/asset-reimport-render",
            argc > 2 && std::string(argv[2]) == "d3d12", true);
        // The fixture submits without Play, so mount the shipped shader
        // directory explicitly in place of Play's generated runtime root.
        resource::setAssetRoots({utf8(fixture.output / "project/.ayeditor_cache/assets"),
            utf8(fixture.assets), utf8(fixture.assets / "materials")});
        const auto source = fixture.output / "UserSources/UserBox.gltf";
        writeBox(source, .5f);
        const auto textureSource=source.parent_path()/"Surface.png";
        writeTexture(textureSource,false);
        resource::ImportOptions options;
        options.sourcePath = utf8(source);
        options.outputDir = utf8(fixture.output / "project/.ayeditor_cache/assets");
        options.requireCharacterAssets = false;
        options.sourceCoordinates.tag = "gpu-import-recipe";
        auto* queue = fixture.session.assetImportQueue();
        require(queue != nullptr, "Actual Session has its import queue");
        const auto firstId = queue->enqueue(options);
        const auto first = finish(fixture, firstId);
        require(first.state == editor::EditorAssetImportJobState::Succeeded, first.message);
        std::string meshPath;
        std::string materialPath;
        for (const auto& output : first.outputPaths)
            if (fs::path(output).extension() == ".msh") meshPath = output;

        require(!meshPath.empty(), "Real source import emits a native mesh");
        const auto importedMesh=resource::ResourceManager::instance().load<resource::IMesh>(
            utf8(fixture.output/"project/.ayeditor_cache/assets"/meshPath));
        require(importedMesh && importedMesh->getMaterialSlotCount()>0,
            "Imported mesh provides its actual material slot");
        materialPath=importedMesh->getMaterialSlot(0);
        require(!materialPath.empty(),"Imported mesh material binding is populated");
        uint32_t editedEntity = 0;
        for (auto* object : fixture.session.document()->scene().world().getAllEntities()) {
            if (!object->getComponent<entity::MeshComponent>()) continue;
            require(fixture.session.document()->mutateComponent(object->getId(), "MeshComponent",
                "Assign imported asset", {}, [&](entity::IComponent& component) {
                    static_cast<entity::MeshComponent&>(component).meshPath = meshPath;
                    static_cast<entity::MeshComponent&>(component).materialPath = materialPath;
                    return true;
                }), "Assign imported mesh through the real Scene document history");
            if (!editedEntity) editedEntity = object->getId();
        }
        require(editedEntity != 0, "Scene has editable imported instances");
        auto* object = fixture.session.document()->scene().world().findEntity(editedEntity);
        auto& transform = *object->transform();
        editor::EditorTransformState moved{transform.position, transform.rotation, transform.scale};
        moved.position.x += .25f;
        require(fixture.session.document()->executeTransform(editedEntity, moved), "Edit instance transform");
        std::string error;
        require(fixture.session.document()->save(&error), error);
        const auto scenePath = fixture.session.document()->path();
        fixture.activateScene();
        const auto opened = fixture.capture("import-open.png");
        const auto bounds = fixture.sceneBounds();
        fixture.renderSceneGeometry = false;
        const auto empty = fixture.capture("import-empty-reference.png");
        fixture.renderSceneGeometry = true;
        require(Fixture::difference(opened, empty, bounds, 8) > 5000,
            "Initially imported geometry is visible against an actual empty render");
        require(fixture.session.rescanAssetsNow(), "Index persisted import recipes");
        auto& database = fixture.session.assetDatabase();
        const auto* record = database.findByLogicalPath("Imported/" + meshPath);
        require(record != nullptr && database.importForAsset(record->id), "Imported mesh retains its source recipe");
        const auto stableId = record->id;
        const auto absoluteMesh = resource::resolveAssetPath({}, meshPath);
        const auto gpuHandle = fixture.meshes.at(absoluteMesh);
        const auto beforeCount = queue->jobs().size();
        writeBox(source, 1.25f);
        auto* mode = dynamic_cast<ui::ComboBox*>(fixture.session.ui().findById("cmb_assets_view"));
        auto* list = dynamic_cast<ui::TileView*>(fixture.session.ui().findById("list_assets"));
        require(mode && list, "Actual browser exposes its view and selection controls");
        mode->setSelectedIndexAndNotify(1);
        fixture.frame();
        bool selected = false;
        for (size_t index = 0; index < list->getItemCount(); ++index) {
            if (list->getItem(index).find(ui::decodeUtf8Text(meshPath)) == std::wstring::npos) continue;
            list->setSelectedIndex(static_cast<int>(index));
            selected = fixture.session.selectedAssetId() == stableId;
            if (selected) break;
        }
        require(selected, "Select imported mesh in the actual asset browser");
        const auto jobsBeforeSettings = queue->jobs().size();
        require(fixture.session.editSelectedAssetImportSettings(), "Open real selected asset import settings");
        fixture.capture("import-settings.png");
        fixture.session.onKeyDown(ui::UIKey_Escape);
        fixture.session.onKeyUp(ui::UIKey_Escape);
        require(queue->jobs().size() == jobsBeforeSettings, "Cancelling settings does not enqueue or mutate an import");
        const auto selectedBefore = fixture.capture("import-preview-before.png");
        auto* previewWidget = fixture.session.ui().findById("inspector_asset_preview");
        require(previewWidget && previewWidget->isVisible(), "Imported mesh has a visible asset preview");
        const auto logicalPreviewBounds = previewWidget->getWorldBounds();
        const auto previewMin = fixture.session.ui().logicalToPhysical(
            {logicalPreviewBounds.minX, logicalPreviewBounds.minY});
        const auto previewMax = fixture.session.ui().logicalToPhysical(
            {logicalPreviewBounds.maxX, logicalPreviewBounds.maxY});
        const math::FRectangle previewBounds{previewMin.x, previewMin.y, previewMax.x, previewMax.y};
        auto* reload = dynamic_cast<ui::Button*>(fixture.session.ui().findById("btn_asset_reload"));
        require(reload && reload->isEnabled() && fixture.session.assetDatabase().importForAsset(
            fixture.session.selectedAssetId()) != nullptr, "Derived asset exposes source reimport");
        click(fixture, *reload);
        require(queue->jobs().size() == beforeCount + 1, "Actual button queues the source recipe");
        const auto reimportId = queue->jobs().back().id;
        const auto second = finish(fixture, reimportId);
        require(second.state == editor::EditorAssetImportJobState::Succeeded, second.message);
        require(second.options.sourceCoordinates.tag == options.sourceCoordinates.tag, "Reimport preserves settings");
        fixture.activateScene();
        const auto changed = fixture.capture("import-reimported.png");
        const auto changedPixels = Fixture::difference(opened, changed, bounds, 8);
        require(changedPixels > 1000, "Session completion refreshes actual GPU geometry");
        require(Fixture::difference(changed, empty, bounds, 8) > 5000,
            "Reimported geometry remains visible against the empty reference");
        const auto previewChanges = Fixture::difference(selectedBefore, changed, previewBounds, 8);
        require(previewChanges > 50, "Imported mesh preview refreshes after source reimport");
        require(fixture.meshes.at(absoluteMesh).id == gpuHandle.id, "GPU resource handle remains stable");
        require(fixture.session.document()->scene().world().findEntity(editedEntity)->transform()->position.x
            == moved.position.x, "Reimport preserves instance edits");
        writeFile(source, "broken glTF");
        auto retry = second.options;
        retry.force = true;
        const auto failedId = queue->enqueue(retry);
        require(finish(fixture, failedId).state == editor::EditorAssetImportJobState::Failed,
            "Bad reimport reports failure through actual Session");
        const auto retained = fixture.capture("import-failed-retained.png");
        require(Fixture::difference(changed, retained, bounds, 1) == 0,
            "Failed reimport preserves rendered Scene and GPU resources");
        writeBox(source, 1.25f);
        const auto retryId = queue->retry(failedId);
        const auto retried = finish(fixture, retryId);
        require(retried.state == editor::EditorAssetImportJobState::Succeeded,
            "Retry recovers with the same source settings: " + retried.message);
        require(fixture.session.document()->open(scenePath, &error), error);
        const auto reopened = fixture.capture("import-scene-reopened.png");
        require(Fixture::difference(changed, reopened, bounds, 1) == 0,
            "Saved Scene retains portable imported references and edited transforms on reopen");
        const auto textureStamp=fs::last_write_time(textureSource);
        writeTexture(textureSource,true);
        fs::last_write_time(textureSource,textureStamp);
        auto textureOptions=second.options;
        textureOptions.force=false;
        const auto textureId=queue->enqueue(textureOptions);
        require(finish(fixture,textureId).state==editor::EditorAssetImportJobState::Succeeded,
            "Changed texture bytes invalidate the import cache even with preserved timestamps");
        const auto recolored=fixture.capture("import-texture-recolored.png");
        const auto texturePixels=Fixture::difference(reopened,recolored,bounds,8);
        require(texturePixels>2000,"Actual GPU materials display the changed imported texture");
        require(fixture.meshes.at(absoluteMesh).id==gpuHandle.id,"Texture dependency reimport retains mesh identity");
        fs::remove(textureSource);
        const auto missingTextureId=queue->enqueue(textureOptions);
        require(finish(fixture,missingTextureId).state==editor::EditorAssetImportJobState::Failed,
            "Missing source texture fails without publishing a partial imported model");
        const auto textureRetained=fixture.capture("import-texture-missing-retained.png");
        require(Fixture::difference(recolored,textureRetained,bounds,1)==0,
            "Missing texture leaves the last complete GPU scene visible");
        writeTexture(textureSource,true);
        const auto textureRetry=queue->retry(missingTextureId);
        require(finish(fixture,textureRetry).state==editor::EditorAssetImportJobState::Succeeded,
            "Restored dependency can be retried");
        require(fixture.session.document()->open(scenePath,&error),error);
        const auto textureReopened=fixture.capture("import-texture-scene-reopened.png");
        require(Fixture::difference(recolored,textureReopened,bounds,1)==0,
            "Reopened Scene retains reimported textures and instance edits");
        require(fixture.session.editSelectedAssetImportSettings(),"Open settings for the selected imported mesh");
        fixture.frame();
        const auto findOverlay=[](auto&& self,ui::Widget* widget,const char* id)->ui::Widget* {
            if(!widget)return nullptr;
            if(widget->getId()==id)return widget;
            for(auto* child:widget->getChildren())if(auto* found=self(self,child,id))return found;
            return nullptr;
        };
        const auto overlayWidget=[&](const char* id){
            return findOverlay(findOverlay,fixture.session.ui().getOverlayRoot(),id);
        };
        auto* settingsDialog=dynamic_cast<ui::ModalDialog*>(overlayWidget("asset_import_settings_dialog"));
        auto* units=dynamic_cast<ui::TextInput*>(overlayWidget("import_settings_units_input"));
        auto* coordinateMode=dynamic_cast<ui::ComboBox*>(overlayWidget("import_settings_coordinates"));
        require(settingsDialog && units && coordinateMode,"Actual overlay settings dialog exposes coordinate editing");
        coordinateMode->setSelectedIndexAndNotify(0);
        units->setText(L"0.5"); // Deliberately leave the pending field unsubmitted.
        ui::Button* applyButton=nullptr;
        for(auto* child:settingsDialog->getChildren())
            if(auto* button=dynamic_cast<ui::Button*>(child);button && button->getText()==settingsDialog->getAcceptText())
                applyButton=button;
        require(applyButton,"Actual modal accept button is present");
        const auto settingsJobs=queue->jobs().size();
        click(fixture,*applyButton);
        require(queue->jobs().size()==settingsJobs+1,"Clicking Apply queues the pending import settings");
        const auto settingsId=queue->jobs().back().id;
        const auto settingsResult=finish(fixture,settingsId);
        require(settingsResult.state==editor::EditorAssetImportJobState::Succeeded,settingsResult.message);
        require(settingsResult.options.sourceCoordinates.mode==resource::SourceCoordinateMode::Auto
            && settingsResult.options.sourceCoordinates.metersPerUnit==.5f,
            "Automatic source axes retain the explicit user unit override");
        fixture.activateScene();
        const auto settingsChanged=fixture.capture("import-settings-applied.png");
        const auto settingsPixels=Fixture::difference(textureReopened,settingsChanged,bounds,8);
        require(settingsPixels>1000,"Actual settings Apply changes imported GPU geometry");
        require(fixture.meshes.at(absoluteMesh).id==gpuHandle.id,"Settings Apply retains the GPU mesh identity");
        require(!fixture.session.document()->isDirty(),"Import settings do not dirty or overwrite instance Scene data");
        require(fixture.session.document()->open(scenePath,&error),error);
        const auto settingsReopened=fixture.capture("import-settings-scene-reopened.png");
        require(Fixture::difference(settingsChanged,settingsReopened,bounds,1)==0,
            "Reopened Scene displays the configured imported geometry");
        const auto ddsSource=source.parent_path()/"Surface.dds";
        writeDds(ddsSource);
        std::ifstream sourceFile(source,std::ios::binary);
        std::string sourceText{std::istreambuf_iterator<char>(sourceFile),std::istreambuf_iterator<char>()};
        sourceFile.close();
        const auto imageName=sourceText.find("Surface.png");
        require(imageName!=std::string::npos,"Native source fixture has its external image reference");
        sourceText.replace(imageName,std::string("Surface.png").size(),"Surface.dds");
        writeFile(source,sourceText);
        auto ddsOptions=settingsResult.options;
        ddsOptions.force=true;
        const auto ddsId=queue->enqueue(ddsOptions);
        const auto ddsResult=finish(fixture,ddsId);
        require(ddsResult.state==editor::EditorAssetImportJobState::Succeeded,ddsResult.message);
        const auto ddsVisible=fixture.capture("import-dds-visible.png");
        const auto ddsPixels=Fixture::difference(settingsReopened,ddsVisible,bounds,8);
        require(ddsPixels>1000,"DDS source blocks become a visible imported GPU texture");
        writeFile(ddsSource,"bad DDS");
        const auto badDdsId=queue->enqueue(ddsOptions);
        require(finish(fixture,badDdsId).state==editor::EditorAssetImportJobState::Failed,
            "Invalid DDS payload is refused before publication");
        const auto ddsRetained=fixture.capture("import-dds-failed-retained.png");
        require(Fixture::difference(ddsVisible,ddsRetained,bounds,1)==0,
            "Invalid DDS retains the last complete rendered model");
        writeDds(ddsSource);
        const auto ddsRetry=queue->retry(badDdsId);
        require(finish(fixture,ddsRetry).state==editor::EditorAssetImportJobState::Succeeded,"DDS source repair retries successfully");
        for (const int channels : {1, 2}) {
            writeDds(ddsSource, channels);
            const auto channelJob = queue->enqueue(ddsOptions);
            const auto channelImport = finish(fixture, channelJob);
            require(channelImport.state == editor::EditorAssetImportJobState::Succeeded,
                "Import BC4/BC5 source into the actual material: " + channelImport.message);
            const auto channelPixels = fixture.capture(channels == 1
                ? "import-dds-bc4-visible.png" : "import-dds-bc5-visible.png");
            require(Fixture::difference(ddsVisible, channelPixels, bounds, 8) > 500,
                "BC4/BC5 sampling produces actual changed Scene pixels");
            // Compare against an independently authored RGBA8 resource, so a
            // fallback placeholder, channel swizzle or skipped sRGB conversion
            // cannot satisfy the changed-pixels check.
            resource::Texture rgbaReference;
            rgbaReference.createSolidColor(4, 4, 96, channels == 2 ? 200 : 0, 0, 255);
            std::vector<resource::UInt8> referenceBytes;
            require(rgbaReference.saveToBinary(referenceBytes), "Encode RGBA8 DDS sampling reference");
            const auto referencePath = source.parent_path()
                / (channels == 1 ? "BC4Reference.txr" : "BC5Reference.txr");
            writeFile(referencePath, std::string(reinterpret_cast<const char*>(referenceBytes.data()),
                referenceBytes.size()));
            const auto absoluteMaterial = resource::resolveAssetPath({}, materialPath);
            resource::MaterialLoader referenceLoader;
            const auto referenceMaterial = std::dynamic_pointer_cast<resource::Material>(
                referenceLoader.load(absoluteMaterial));
            require(referenceMaterial && referenceMaterial->setTextureReference(
                "baseColorTexture", utf8(referencePath)), "Build independent RGBA8 material reference");
            std::vector<resource::UInt8> materialBytes;
            require(referenceMaterial->saveToBinary(materialBytes), "Encode DDS sampling reference material");
            writeFile(fs::u8path(absoluteMaterial), std::string(
                reinterpret_cast<const char*>(materialBytes.data()), materialBytes.size()));
            require(resource::ResourceManager::instance().refreshPublishedResources(
                {absoluteMaterial}, &error), error);
            const auto referencePixels = fixture.capture(channels == 1
                ? "import-dds-bc4-rgba-reference.png" : "import-dds-bc5-rgba-reference.png");
            require(Fixture::difference(channelPixels, referencePixels, bounds, 2) == 0,
                "BC4/BC5 color sampling agrees with equivalent RGBA8 sRGB data");
        }
        writeDds(ddsSource);
        const auto restoreBc1 = queue->enqueue(ddsOptions);
        const auto restoredBc1 = finish(fixture, restoreBc1);
        require(restoredBc1.state == editor::EditorAssetImportJobState::Succeeded, restoredBc1.message);
        const auto restoredDdsPixels = fixture.capture("import-dds-bc1-restored.png");
        require(Fixture::difference(ddsVisible, restoredDdsPixels, bounds, 1) == 0,
            "DDS format changes return to the original BC1 surface without stale GPU sampling");
        require(fixture.session.document()->open(scenePath,&error),error);
        const auto ddsReopened=fixture.capture("import-dds-scene-reopened.png");
        require(Fixture::difference(ddsVisible,ddsReopened,bounds,1)==0,
            "Saved Scene retains imported DDS material and instance transforms");
        const auto relocatedDirectory=fixture.output/"Relocated author source";
        fs::rename(source.parent_path(),relocatedDirectory);
        const auto renamedSource=relocatedDirectory/"RenamedBox.gltf";
        fs::rename(relocatedDirectory/source.filename(),renamedSource);
        auto relinkOptions=ddsOptions;
        relinkOptions.sourcePath=utf8(renamedSource);
        require(!relinkOptions.outputBaseName.empty(),"Project recipe pins the initial output file stem");
        const auto relinkId=queue->enqueue(relinkOptions);
        const auto relinked=finish(fixture,relinkId);
        require(relinked.state==editor::EditorAssetImportJobState::Succeeded,relinked.message);
        require(fixture.session.rescanAssetsNow(),"Rescan relinked source association");
        const auto* relinkedRecord=fixture.session.assetDatabase().findByLogicalPath("Imported/"+meshPath);
        require(relinkedRecord && relinkedRecord->id==stableId,"Relink retains the asset browser identity");
        const auto* relinkedImport=fixture.session.assetDatabase().importForAsset(stableId);
        require(relinkedImport && relinkedImport->record.options.sourceId==relinkOptions.sourceId,
            "Relink retains the author's recipe identity");
        const auto relinkVisible=fixture.capture("import-source-relinked.png");
        require(Fixture::difference(ddsReopened,relinkVisible,bounds,1)==0,
            "Moving and renaming the author source preserves every rendered Scene reference");
        require(fixture.meshes.at(absoluteMesh).id==gpuHandle.id,"Relink preserves existing GPU handle identity");
        require(fixture.session.document()->open(scenePath,&error),error);
        const auto relinkReopened=fixture.capture("import-source-relinked-scene-reopened.png");
        require(Fixture::difference(relinkVisible,relinkReopened,bounds,1)==0,
            "Saved Scene reopens after the original author source no longer exists");
        require(fixture.session.rescanAssetsNow(),"Find imported material for an already-open authoring Dock");
        const auto* materialRecord=fixture.session.assetDatabase().findByLogicalPath("Imported/"+materialPath);
        require(materialRecord,"Imported material is visible in the actual asset database");const auto importedMaterialId=materialRecord->id;
        require(fixture.session.openAsset(importedMaterialId),"Open actual imported material Dock before reimport");
        auto importedDocument=std::dynamic_pointer_cast<editor::EditorMaterialDocument>(fixture.session.workspace().documents().active()->document);
        require(importedDocument&&importedDocument->readOnly(),"Imported Dock is a read-only decoded snapshot");
        const auto materialRevision=importedDocument->revision();
        const auto materialBefore=fixture.capture("import-open-material-before.png");
        auto* importedPreviewWidget=fixture.session.ui().findById("material_preview");require(importedPreviewWidget,"Actual material preview Widget exists");const auto previewRect=importedPreviewWidget->getWorldBounds();
        std::ifstream materialSource(renamedSource,std::ios::binary);std::string authored{std::istreambuf_iterator<char>(materialSource),{}};materialSource.close();
        const std::string oldFactor="\"roughnessFactor\":0.55";const auto factor=authored.find(oldFactor);require(factor!=std::string::npos,"Fixture exposes an authored material factor");
        authored.replace(factor,oldFactor.size(),"\"roughnessFactor\":0.12,\"baseColorFactor\":[0.2,1,0.25,1]");writeFile(renamedSource,authored);
        auto materialOptions=relinkOptions;materialOptions.force=true;const auto materialJob=queue->enqueue(materialOptions);
        require(finish(fixture,materialJob).state==editor::EditorAssetImportJobState::Succeeded,"Reimport changed material factors with its Dock already open");
        require(importedDocument->revision()>materialRevision,"Successful reimport refreshes the already-open imported material snapshot");
        require(std::fabs(importedDocument->settings().roughness-.12f)<1e-5f,"Open imported document adopts the new authored factors");
        const auto materialAfter=fixture.capture("import-open-material-refreshed.png");
        const auto openMaterialPixels=Fixture::difference(materialBefore,materialAfter,previewRect,6);require(openMaterialPixels>500,"Already-open GPU material preview changes after reimport");
        fixture.activateScene();const auto factorScene=fixture.capture("import-material-factor-scene.png");
        require(Fixture::difference(relinkReopened,factorScene,bounds,6)>500,"Scene and the imported material Dock agree on new factors");
        require(importedDocument->saveEditableCopy("materials/ReimportProtectedCopy.mat",&error),error);
        require(fixture.session.rescanAssetsNow(),"Index editable copy for unsaved preservation");const auto* copyRecord=fixture.session.assetDatabase().findByLogicalPath("Assets/materials/ReimportProtectedCopy.mat");
        require(copyRecord&&fixture.session.openAsset(copyRecord->id),"Open independent editable material copy");
        auto copyDocument=std::dynamic_pointer_cast<editor::EditorMaterialDocument>(fixture.session.workspace().documents().active()->document);
        require(copyDocument&&!copyDocument->readOnly(),"The copy belongs to project source assets");auto draft=copyDocument->settings();draft.roughness=.42f;
        require(copyDocument->replaceSettings(draft,&error),error);const auto copyRevision=copyDocument->revision();
        const auto newFactor=authored.find("\"roughnessFactor\":0.12");authored.replace(newFactor,std::string("\"roughnessFactor\":0.12").size(),"\"roughnessFactor\":0.85");writeFile(renamedSource,authored);
        const auto again=queue->enqueue(materialOptions);require(finish(fixture,again).state==editor::EditorAssetImportJobState::Succeeded,"Reimport while an unrelated editable draft is open");
        require(copyDocument->isDirty()&&copyDocument->revision()==copyRevision&&copyDocument->settings()==draft,"Reimport preserves the editable unsaved document and its history");
        require(std::fabs(importedDocument->settings().roughness-.85f)<1e-5f,"Inactive imported Dock also refreshes to the new source");
        fixture.activateScene();const auto factorAgain=fixture.capture("import-material-factor-again.png");
        const auto beforeExternalRevision = importedDocument->revision();
        const std::string roughFactor = "\"roughnessFactor\":0.85";
        const auto roughOffset = authored.find(roughFactor);
        require(roughOffset != std::string::npos, "Find the latest authored roughness factor");
        authored.replace(roughOffset, roughFactor.size(), "\"roughnessFactor\":0.32");
        const std::string greenFactor = "\"baseColorFactor\":[0.2,1,0.25,1]";
        const auto colorOffset = authored.find(greenFactor);
        require(colorOffset != std::string::npos, "Find the latest authored color factor");
        authored.replace(colorOffset, greenFactor.size(), "\"baseColorFactor\":[1,0.1,0.1,1]");
        writeFile(renamedSource, authored);
        const auto externalImport = resource::importAsset(materialOptions);
        require(externalImport.ok, externalImport.error);
        require(importedDocument->revision() == beforeExternalRevision,
            "External publication alone does not replace the open document snapshot");
        // Long sessions may evict all CPU snapshots while the Renderer keeps
        // stable GPU handles loaded through the project junction spelling.
        resource::ResourceManager::instance().setCacheConfig(resource::ResourceCache::Config{});
        require(resource::ResourceManager::instance().getResourceCount() == 0u,
            "CPU cache eviction leaves no resident resources before explicit synchronization");
        materialOptions.force = false;
        const auto cacheHitJob = queue->enqueue(materialOptions);
        const auto cacheHit = finish(fixture, cacheHitJob);
        require(cacheHit.state == editor::EditorAssetImportJobState::CacheHit,
            "Editor reuses current external outputs: " + cacheHit.message);
        require(importedDocument->revision() > beforeExternalRevision
            && std::fabs(importedDocument->settings().roughness - .32f) < 1e-5f,
            "Cache-hit completion synchronizes the inactive imported material document");
        require(copyDocument->isDirty() && copyDocument->settings() == draft
            && copyDocument->revision() == copyRevision,
            "Cache-hit completion also preserves the independent unsaved draft");
        fixture.activateScene();
        const auto factorCached = fixture.capture("import-cache-hit-refreshed.png");
        const auto cacheHitPixels = Fixture::difference(factorAgain, factorCached, bounds, 6);
        require(cacheHitPixels > 500, "Cache-hit completion synchronizes actual Scene GPU material data");
        require(fixture.meshes.at(absoluteMesh).id == gpuHandle.id,
            "CPU eviction and cache-hit synchronization preserve the Scene GPU mesh handle");
        require(fixture.session.document()->open(scenePath,&error),error);const auto factorReopened=fixture.capture("import-material-factor-reopened.png");
        require(Fixture::difference(factorCached,factorReopened,bounds,1)==0,"Scene reopen retains material reimport while preserving the independent draft");
        writeSamplingDds(renamedSource.parent_path()/"Surface.dds");
        auto samplingOptions=materialOptions;samplingOptions.force=false;samplingOptions.cookTextures=true;
        samplingOptions.overrideTextureSampler=true;
        samplingOptions.textureSampler.minFilter=resource::TextureFilter::Point;
        samplingOptions.textureSampler.magFilter=resource::TextureFilter::Point;
        const auto nearestId=queue->enqueue(samplingOptions);
        const auto nearestResult=finish(fixture,nearestId);
        require(nearestResult.state==editor::EditorAssetImportJobState::Succeeded,nearestResult.message);
        fixture.activateScene();const auto nearestScene=fixture.capture("sampling-nearest-scene.png");
        require(fixture.session.openAsset(importedMaterialId),"Open imported material with nearest filtering");
        const auto nearestPreview=fixture.capture("sampling-nearest-preview.png");
        auto* samplingPreview=fixture.session.ui().findById("material_preview");
        require(samplingPreview,"Sampling workflow has an actual material preview");
        const auto samplingPreviewBounds=samplingPreview->getWorldBounds();
        fixture.activateScene();
        require(fixture.session.rescanAssetsNow(),"Discover current sampling recipe");
        list=dynamic_cast<ui::TileView*>(fixture.session.ui().findById("list_assets"));require(list,"Asset browser remains available");
        selected=false;
        for(size_t index=0;index<list->getItemCount();++index){
            if(list->getItem(index).find(ui::decodeUtf8Text(meshPath))==std::wstring::npos)continue;
            list->setSelectedIndex(static_cast<int>(index));selected=fixture.session.selectedAssetId()==stableId;
            if(selected)break;
        }
        require(selected && fixture.session.editSelectedAssetImportSettings(),"Open actual persisted sampling settings from imported mesh");
        fixture.frame();
        auto* samplingDialog=dynamic_cast<ui::ModalDialog*>(overlayWidget("asset_import_settings_dialog"));
        auto* filtering=dynamic_cast<ui::ComboBox*>(overlayWidget("import_settings_sampling"));
        auto* magnification=dynamic_cast<ui::ComboBox*>(overlayWidget("import_settings_sampling_mag"));
        require(samplingDialog && filtering && magnification,"Actual sampling settings fields exist");
        require(filtering->getSelectedIndex()==1 && magnification->getSelectedIndex()==0,
            "Reopened settings recover the persisted nearest sampler");
        filtering->setSelectedIndexAndNotify(3);magnification->setSelectedIndexAndNotify(1);
        (void)fixture.capture("sampling-settings-form.png");
        ui::Button* samplingCancel=nullptr;
        for(auto* child:samplingDialog->getChildren())
            if(auto* button=dynamic_cast<ui::Button*>(child);button && button->getText()==samplingDialog->getRejectText())samplingCancel=button;
        require(samplingCancel,"Sampling Cancel button is present");
        const auto jobsBeforeCancel=queue->jobs().size();click(fixture,*samplingCancel);
        require(queue->jobs().size()==jobsBeforeCancel,"Cancel does not enqueue or publish pending sampler changes");
        fixture.activateScene();const auto cancelledSampling=fixture.capture("sampling-cancel-scene.png");
        require(Fixture::difference(nearestScene,cancelledSampling,bounds,1)==0,"Cancelled sampler edits leave actual Scene pixels unchanged");
        require(fixture.session.editSelectedAssetImportSettings(),"Reopen sampling settings after Cancel");fixture.frame();
        samplingDialog=dynamic_cast<ui::ModalDialog*>(overlayWidget("asset_import_settings_dialog"));
        filtering=dynamic_cast<ui::ComboBox*>(overlayWidget("import_settings_sampling"));
        magnification=dynamic_cast<ui::ComboBox*>(overlayWidget("import_settings_sampling_mag"));
        require(samplingDialog && filtering && magnification && filtering->getSelectedIndex()==1,
            "Reopened dialog retains the persisted sampler after Cancel");
        filtering->setSelectedIndexAndNotify(3);magnification->setSelectedIndexAndNotify(1);
        ui::Button* samplingApply=nullptr;
        for(auto* child:samplingDialog->getChildren())
            if(auto* button=dynamic_cast<ui::Button*>(child);button && button->getText()==samplingDialog->getAcceptText())samplingApply=button;
        require(samplingApply,"Sampling Apply button is present");
        const auto samplingJobs=queue->jobs().size();click(fixture,*samplingApply);
        require(queue->jobs().size()==samplingJobs+1,"Actual Apply pointer routing enqueues sampler changes");
        const auto linearId=queue->jobs().back().id;
        const auto linearResult=finish(fixture,linearId);
        require(linearResult.state==editor::EditorAssetImportJobState::Succeeded,linearResult.message);
        require(linearResult.options.textureSampler.minFilter==resource::TextureFilter::Trilinear
            && linearResult.options.textureSampler.magFilter==resource::TextureFilter::Bilinear,
            "Actual Apply publishes new min/mag sampler settings");
        fixture.activateScene();const auto linearScene=fixture.capture("sampling-trilinear-scene.png");
        const auto samplingScenePixels=Fixture::difference(nearestScene,linearScene,bounds,3);
        require(samplingScenePixels>500,"Sampler-only reimport visibly changes actual Scene geometry");
        require(fixture.session.openAsset(importedMaterialId),"Reopen imported preview after sampler change");
        const auto linearPreview=fixture.capture("sampling-trilinear-preview.png");
        const auto samplingPreviewPixels=Fixture::difference(nearestPreview,linearPreview,samplingPreviewBounds,3);
        require(samplingPreviewPixels>1000,"Sampler-only reimport visibly changes actual material preview");
        bool checkedMips=false;
        for(const auto& entry:linearResult.outputPaths){
            if(fs::u8path(entry).extension()!=".txr")continue;
            resource::Texture texture;
            if(!texture.load((fs::u8path(linearResult.options.outputDir)/fs::u8path(entry)).string()))continue;
            require(texture.getMipmapCount()==3,"DDS complete chain survives the actual authoring publication");
            require(texture.getSampler().minFilter==resource::TextureFilter::Trilinear,"Sampler is persisted in typed texture output");
            checkedMips=true;
        }
        require(checkedMips,"Read at least one actual published texture");
        fixture.activateScene();require(fixture.session.document()->save(&error),error);
        require(fixture.session.document()->open(scenePath,&error),error);
        const auto samplingReopened=fixture.capture("sampling-scene-reopened.png");
        require(Fixture::difference(linearScene,samplingReopened,bounds,1)==0,"Scene save/reopen keeps the reimported sampler");
        writeFile(renamedSource.parent_path()/"Surface.dds","broken sampling DDS");
        const auto brokenSamplingId=queue->enqueue(linearResult.options);
        require(finish(fixture,brokenSamplingId).state==editor::EditorAssetImportJobState::Failed,"Reject broken sampler source reimport");
        const auto retainedSampling=fixture.capture("sampling-failed-source-retained.png");
        require(Fixture::difference(samplingReopened,retainedSampling,bounds,1)==0,"Failed sampler reimport preserves the last visible GPU scene");
        writeSamplingDds(renamedSource.parent_path()/"Surface.dds");
        const auto retrySampling=queue->retry(brokenSamplingId);
        require(finish(fixture,retrySampling).state==editor::EditorAssetImportJobState::Succeeded,"Repair and retry sampling source");
        const auto repairedSampling=fixture.capture("sampling-source-repaired.png");
        require(Fixture::difference(samplingReopened,repairedSampling,bounds,1)==0,"Sampling source repair restores identical valid output");
        require(copyDocument->isDirty() && copyDocument->revision()==copyRevision,"Sampling reimports preserve the unrelated unsaved draft");
        std::cout << "PASS sampling Scene pixels=" << samplingScenePixels << "; sampling preview pixels=" << samplingPreviewPixels << '\n';
        std::cout << "PASS source import, source relocation/relink, actual browser reimport, GPU refresh, stable handle, "
            "failed import retention, retry, open imported material refresh, unsaved draft preservation, Scene save/reopen; changed pixels=" << changedPixels
            << "; preview pixels=" << previewChanges << "; texture pixels=" << texturePixels << "; settings pixels=" << settingsPixels << "; DDS pixels=" << ddsPixels << "; open material pixels=" << openMaterialPixels << "; cache-hit pixels=" << cacheHitPixels << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
