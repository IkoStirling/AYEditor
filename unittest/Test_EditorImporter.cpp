// Test_EditorImporter.cpp - Phase 1 ED-01 unit tests.
//
// Covers the AYEditor Importer + ImportDialog surface. Goals:
//   * Confirm unsupported extension → clean error result, no crash.
//   * Confirm missing file → "does not exist" error.
//   * Confirm ImportDialog::importFromPath is a one-line pass-through
//     to Importer::importFile (same return shape, same error text).
//
// The lifecycle cases use an authored triangle and an external glTF buffer
// through the real Assimp/AY writers, queue, persisted recipe and asset DB.

#include "AYTest.h"
#include "AYEditor/Importer.h"
#include "AYEditor/EditorAssetImportQueue.h"
#include "AYEditor/ImportDialog.h"
#include "AYEditor/EditorSession.h"
#include "AYEditor/EditorWorkspace.h"
#include "AYEditor/EditorMaterialDocument.h"
#include <AYUI/MockRenderer.h>
#include <AYUI/Button.h>
#include <AYUI/ComboBox.h>
#include <AYUI/TileView.h>
#include "../src/EditorImportRecipeStore.h"
#include "../src/EditorImportSettingsDialog.h"
#include <AYUI/ScrollView.h>
#include <AYUI/TextInput.h>
#include <AYUI/TextLabel.h>
#include <AYResource/ResourceManager.h>
#include "AYIO/File.h"
#include <AYIO/Path.h>

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <algorithm>

using namespace ayt::editor;

namespace {

// Tiny helper: write `bytes` to `path` (creating parents). We avoid
// AYFile APIs here because the AYIO migration is mid-flight and the
// simple ofstream path is the least surprising one for a unit test.
bool writeStubFile(const std::string& path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return f.good();
}

struct EditorImportFixture {
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / ("ay-editor-import-loop-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    ~EditorImportFixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    std::filesystem::path source() const { return root / "External/Triangle.gltf"; }
    std::filesystem::path project() const { return root / "Project"; }
    std::filesystem::path output() const { return project() / ".ayeditor_cache/assets"; }
    void write(const std::filesystem::path& path, const std::string& text) const {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    void triangle(float extent = 1.0f) const {
        const float positions[] = {0, 0, 0, extent, 0, 0, 0, extent, 0};
        write(source().parent_path() / "Triangle.bin", std::string(
            reinterpret_cast<const char*>(positions), sizeof(positions)));
        write(source(), R"({"asset":{"version":"2.0"},"buffers":[{"uri":"Triangle.bin","byteLength":36}],
"bufferViews":[{"buffer":0,"byteLength":36}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[4,4,0]}],
"meshes":[{"name":"Triangle","primitives":[{"attributes":{"POSITION":0}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})");
    }
    ayt::resource::ImportOptions options() const {
        ayt::resource::ImportOptions options;
        options.sourcePath = source().string();
        options.outputDir = output().string();
        options.requireCharacterAssets = false;
        options.loadOption = ayt::resource::IConverter::LoadOption::MeshOnly;
        return options;
    }
    bool finish(EditorAssetImportQueue& queue) const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (queue.busy() && std::chrono::steady_clock::now() < deadline) {
            (void)queue.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return !queue.busy();
    }
};

} // namespace

TEST_SUITE(AYEditor_Importer)

TEST_CASE(session_reports_consumer_retention_and_failure_without_claiming_the_file_import_failed)
{
    EditorImportFixture fixture; fixture.triangle();
    std::filesystem::create_directories(fixture.project()/"Assets");
    ayt::ui::MockRenderer backend; EditorSession session; EditorSessionDesc desc;
    desc.uiBackend=&backend; desc.projectRoot=fixture.project().string();
    desc.layoutPath=std::string(AY_EDITOR_TEST_SOURCE_DIR)+"/ui/editor_shell.ui.json";
    CHECK(session.initialize(desc)); CHECK(session.setLanguage("en-US"));
    session.setClientSize(1500,1000);
    auto& manager=ayt::resource::ResourceManager::instance();
    auto* queue=session.assetImportQueue(); CHECK(queue);
    if(!queue){session.shutdown();return;}
    using ayt::resource::ResourceRefreshState;
    const ResourceRefreshState states[]={ResourceRefreshState::Retained,ResourceRefreshState::Failed,
        ResourceRefreshState::NotResident,ResourceRefreshState::Unconfirmed,ResourceRefreshState::Updated};
    for(size_t index=0;index<std::size(states);++index){
        const auto state=states[index];
        const auto token=manager.setRefreshObserver("test consumer",[state](const std::string&){
            return std::vector<ayt::resource::ResourceConsumerRefresh>{{state,"linear",17,31,"fixture acknowledgement"}};
        });
        fixture.triangle(float(index+2)); auto options=fixture.options(); options.force=true;
        const auto id=queue->enqueue(options);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(queue->busy()&&std::chrono::steady_clock::now()<deadline){
            session.update(.016f);std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        session.update(.016f);
        const auto* job=queue->find(id);CHECK(job&&job->state==EditorAssetImportJobState::Succeeded);
        // Let the ordinary asset scan complete after the import notification;
        // it must not hide a retained/failed consumer acknowledgement.
        CHECK(session.rescanAssetsNow());
        for(int frame=0;frame<8;++frame){session.update(.016f);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        auto* status=dynamic_cast<ayt::ui::TextLabel*>(session.ui().findById("assets_status"));
        auto* console=dynamic_cast<ayt::ui::TextLabel*>(session.ui().findById("console_output"));
        CHECK(status);CHECK(console);
        if(status&&console){
            const bool previous=status->getText().find(L"previous version")!=std::wstring::npos;
            const bool failed=status->getText().find(L"could not be refreshed")!=std::wstring::npos;
            CHECK(previous==(state==ResourceRefreshState::Retained));
            CHECK(failed==(state==ResourceRefreshState::Failed));
            if(previous||failed) CHECK(console->getText()==status->getText());
            CHECK(console->getText().find(L"Import failed:")==std::wstring::npos);
        }
        CHECK(manager.clearRefreshObserver(token));
    }
    session.shutdown();
}

TEST_CASE(session_unrelated_import_cannot_clear_another_sources_unresolved_refresh)
{
    EditorImportFixture fixture;fixture.triangle();
    std::filesystem::create_directories(fixture.project()/"Assets");
    const auto other=fixture.source().parent_path()/"Other.gltf";
    CHECK(std::filesystem::copy_file(fixture.source(),other));
    ayt::ui::MockRenderer backend;EditorSession session;EditorSessionDesc desc;
    desc.uiBackend=&backend;desc.projectRoot=fixture.project().string();
    desc.layoutPath=std::string(AY_EDITOR_TEST_SOURCE_DIR)+"/ui/editor_shell.ui.json";
    CHECK(session.initialize(desc));CHECK(session.setLanguage("en-US"));session.setClientSize(1500,1000);
    auto& manager=ayt::resource::ResourceManager::instance();
    using ayt::resource::ResourceRefreshState;
    auto state=ResourceRefreshState::Retained;
    const auto token=manager.setRefreshObserver("test consumer",[&](const std::string&){
        return std::vector<ayt::resource::ResourceConsumerRefresh>{{state,"linear",17,31,{}}};
    });
    struct Reset {ayt::resource::ResourceManager& manager;uint64_t token;
        ~Reset(){manager.clearRefreshObserver(token);}} reset{manager,token};
    auto* queue=session.assetImportQueue();CHECK(queue);if(!queue){session.shutdown();return;}
    const auto runImport=[&](const std::filesystem::path& source,ResourceRefreshState result){
        state=result;auto options=fixture.options();options.sourcePath=source.string();options.force=true;
        const auto id=queue->enqueue(options);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(queue->busy()&&std::chrono::steady_clock::now()<deadline){session.update(.016f);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        session.update(.016f);const auto* job=queue->find(id);
        CHECK(job&&job->state==EditorAssetImportJobState::Succeeded);
        CHECK(session.rescanAssetsNow());session.update(.016f);
        auto* status=dynamic_cast<ayt::ui::TextLabel*>(session.ui().findById("assets_status"));
        CHECK(status);return status?status->getText():std::wstring{};
    };
    auto status=runImport(fixture.source(),ResourceRefreshState::Retained);
    CHECK(status.find(L"Triangle.gltf")!=std::wstring::npos);
    CHECK(status.find(L"previous version")!=std::wstring::npos);
    status=runImport(other,ResourceRefreshState::Updated);
    CHECK(status.find(L"Triangle.gltf")!=std::wstring::npos);
    CHECK(status.find(L"previous version")!=std::wstring::npos);
    status=runImport(fixture.source(),ResourceRefreshState::Unconfirmed);
    CHECK(status.find(L"unconfirmed")!=std::wstring::npos);
    status=runImport(other,ResourceRefreshState::Updated);
    CHECK(status.find(L"unconfirmed")!=std::wstring::npos);
    status=runImport(fixture.source(),ResourceRefreshState::Updated);
    CHECK(status.find(L"previous version")==std::wstring::npos);
    CHECK(status.find(L"unconfirmed")==std::wstring::npos);
    session.shutdown();
}

TEST_CASE(session_reimport_refreshes_open_readonly_material_retains_bad_source_and_independent_unsaved_copy)
{
    EditorImportFixture fixture;fixture.triangle();std::filesystem::create_directories(fixture.project()/"Assets");
    const auto materialSource=[&](float roughness){fixture.triangle();std::ifstream input(fixture.source(),std::ios::binary);
        std::string text{std::istreambuf_iterator<char>(input),{}};input.close();
        const auto meshes=text.find("\"meshes\":");text.insert(meshes,"\"materials\":[{\"name\":\"Surface\",\"pbrMetallicRoughness\":{\"roughnessFactor\":"+std::to_string(roughness)+"}}],");
        const auto attributes=text.find("\"attributes\":");text.insert(attributes,"\"material\":0,");fixture.write(fixture.source(),text);};
    materialSource(.2f);auto options=fixture.options();options.loadOption=ayt::resource::IConverter::LoadOption::Full;std::string error;
    CHECK(detail::saveImportRecipe(fixture.project().string(),options,&error));const auto imported=Importer::importAssetFile(options);CHECK_MSG(imported.success,imported.errorMessage.c_str());
    std::string materialPath;for(const auto& resource:imported.conversion.resources)if(resource.type=="Material"){materialPath=resource.path;break;}
    CHECK_FALSE(materialPath.empty());if(materialPath.empty())return;
    ayt::ui::MockRenderer backend;EditorSession session;EditorSessionDesc desc;desc.uiBackend=&backend;desc.projectRoot=fixture.project().string();
    desc.layoutPath=std::string(AY_EDITOR_TEST_SOURCE_DIR)+"/ui/editor_shell.ui.json";CHECK(session.initialize(desc));session.setClientSize(1500,1000);CHECK(session.rescanAssetsNow());
    const auto* record=session.assetDatabase().findByLogicalPath("Imported/"+materialPath);CHECK(record);if(!record){session.shutdown();return;}
    CHECK(session.openAsset(record->id));auto material=std::dynamic_pointer_cast<EditorMaterialDocument>(session.workspace().documents().active()->document);
    CHECK(material&&material->readOnly());if(!material){session.shutdown();return;}CHECK_FLOAT_EQ(material->settings().roughness,.2f,1e-5f);
    auto* queue=session.assetImportQueue();const auto finishSession=[&](EditorAssetImportJobId id){
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(queue->busy()&&std::chrono::steady_clock::now()<deadline){session.update(.016f);std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        session.update(.016f);return queue->find(id)&&queue->find(id)->finished();};
    const auto revision=material->revision();materialSource(.8f);options.force=true;auto job=queue->enqueue(options);CHECK(finishSession(job));
    CHECK_MSG(queue->find(job)->state==EditorAssetImportJobState::Succeeded,queue->find(job)->message.c_str());CHECK(material->revision()>revision);CHECK_FLOAT_EQ(material->settings().roughness,.8f,1e-5f);
    CHECK(material->saveEditableCopy("materials/Protected.mat",&error));CHECK(session.rescanAssetsNow());record=session.assetDatabase().findByLogicalPath("Assets/materials/Protected.mat");
    CHECK(record);if(!record){session.shutdown();return;}CHECK(session.openAsset(record->id));auto copy=std::dynamic_pointer_cast<EditorMaterialDocument>(session.workspace().documents().active()->document);
    CHECK(copy&&!copy->readOnly());if(!copy){session.shutdown();return;}auto draft=copy->settings();draft.roughness=.42f;CHECK(copy->replaceSettings(draft,&error));const auto draftRevision=copy->revision();
    materialSource(.6f);job=queue->enqueue(options);CHECK(finishSession(job));CHECK_MSG(queue->find(job)->state==EditorAssetImportJobState::Succeeded,queue->find(job)->message.c_str());
    CHECK_FLOAT_EQ(material->settings().roughness,.6f,1e-5f);CHECK(copy->settings()==draft);CHECK(copy->isDirty()&&copy->revision()==draftRevision);
    // Another authoring tool can update the published files and metadata while
    // this Session still owns the previous CPU/GPU/document snapshots.
    materialSource(.4f);
    const auto external=Importer::importAssetFile(options);
    CHECK_MSG(external.success,external.errorMessage.c_str());
    CHECK_FLOAT_EQ(material->settings().roughness,.6f,1e-5f);
    options.force=false;job=queue->enqueue(options);CHECK(finishSession(job));
    CHECK_MSG(queue->find(job)->state==EditorAssetImportJobState::CacheHit,queue->find(job)->message.c_str());
    CHECK_FLOAT_EQ(material->settings().roughness,.4f,1e-5f);
    CHECK(copy->settings()==draft&&copy->isDirty()&&copy->revision()==draftRevision);
    const auto retained=material->revision();fixture.write(fixture.source(),"broken glTF");job=queue->enqueue(options);CHECK(finishSession(job));CHECK(queue->find(job)->state==EditorAssetImportJobState::Failed);
    CHECK(material->revision()==retained);CHECK_FLOAT_EQ(material->settings().roughness,.4f,1e-5f);CHECK(copy->settings()==draft&&copy->isDirty());session.shutdown();
}

TEST_CASE(project_directory_alias_preserves_source_and_output_import_associations_and_asset_ids)
{
    EditorImportFixture fixture;fixture.triangle();const auto model=fixture.project()/"Assets/Models/Triangle.gltf";
    std::filesystem::create_directories(model.parent_path());std::filesystem::copy_file(fixture.source(),model);
    std::filesystem::copy_file(fixture.source().parent_path()/"Triangle.bin",model.parent_path()/"Triangle.bin");
    EditorAssetImportQueue queue;queue.configureProject(fixture.project().string(),(fixture.project()/"Assets").string());
    auto options=fixture.options();options.sourcePath=model.string();const auto job=queue.enqueue(options);CHECK(fixture.finish(queue));
    CHECK(queue.find(job)->state==EditorAssetImportJobState::Succeeded);
    const auto alias=fixture.root/"LinkedProject";
#if defined(_WIN32)
    const auto command=L"cmd /d /c mklink /J \""+alias.wstring()+L"\" \""+fixture.project().wstring()+L"\" >nul 2>&1";
    if(_wsystem(command.c_str())!=0){SKIP_TEST("Directory junction creation is unavailable");return;}
#else
    std::error_code linkError;std::filesystem::create_directory_symlink(fixture.project(),alias,linkError);
    if(linkError){SKIP_TEST("Directory symlink creation is unavailable");return;}
#endif
    EditorAssetDatabase physical,linked;std::string error;CHECK(physical.open(fixture.project().string(),&error));CHECK(physical.scanNow(&error));
    CHECK(linked.open(alias.string(),&error));CHECK(linked.scanNow(&error));
    const auto* source=linked.findByLogicalPath("Assets/Models/Triangle.gltf");CHECK(source);CHECK(source&&linked.importForAsset(source->id));
    for(const auto& output:queue.find(job)->outputPaths){const auto logical="Imported/"+output;
        const auto* first=physical.findByLogicalPath(logical);const auto* second=linked.findByLogicalPath(logical);CHECK(first&&second);
        if(first&&second){CHECK(first->id==second->id);CHECK(linked.importForAsset(second->id));}}
    linked.close();CHECK(linked.open(alias.string(),&error));CHECK(linked.scanNow(&error));
    source=linked.findByLogicalPath("Assets/Models/Triangle.gltf");CHECK(source&&linked.importForAsset(source->id));
}

TEST_CASE(EditorAssetImportQueueReportsFailureAndSupportsRetry)
{
    EditorAssetImportQueue queue;
    const auto id = queue.enqueue(
        "D:/no/such/path/missing.fbx", "D:/tmp/ayeditor-import-queue");
    CHECK(id != 0u);
    for (int attempt = 0; attempt < 200 && queue.busy(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        (void)queue.poll();
    }
    const EditorAssetImportJob* failed = queue.find(id);
    CHECK(failed != nullptr);
    CHECK(failed != nullptr && failed->state == EditorAssetImportJobState::Failed);
    CHECK(failed != nullptr && !failed->message.empty());
    CHECK_FLOAT_EQ(queue.overallProgress(), 1.0f, 1e-5f);

    const auto retryId = queue.retry(id);
    CHECK(retryId != 0u);
    CHECK(retryId != id);
    CHECK(queue.busy());
    while (queue.busy()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        (void)queue.poll();
    }
    const EditorAssetImportJob* retried = queue.find(retryId);
    CHECK(retried != nullptr);
    CHECK(retried != nullptr && retried->force);
    CHECK(retried != nullptr && retried->state == EditorAssetImportJobState::Failed);
}

TEST_CASE(extension_of_lowercases_and_strips_dot)
{
    CHECK_TRUE(Importer::extensionOf("foo.fbx")      == "fbx");
    CHECK_TRUE(Importer::extensionOf("Foo.FBX")      == "fbx");
    CHECK_TRUE(Importer::extensionOf("a/b/c.Png")    == "png");
    CHECK_TRUE(Importer::extensionOf("noext")        == "");
    CHECK_TRUE(Importer::extensionOf("trailing.")    == "");
}

TEST_CASE(is_supported_extension_matches_factory_table)
{
    // Mirror AYResource::isImportSupportedExtension().
    CHECK_TRUE(Importer::isSupportedExtension("a.fbx"));
    CHECK_TRUE(Importer::isSupportedExtension("a.glb"));
    CHECK_TRUE(Importer::isSupportedExtension("a.gltf"));
    CHECK_TRUE(Importer::isSupportedExtension("a.png"));
    CHECK_TRUE(Importer::isSupportedExtension("a.bmp"));
    CHECK_TRUE(Importer::isSupportedExtension("a.tga"));
    CHECK_TRUE(Importer::isSupportedExtension("a.dds"));
    CHECK_TRUE(Importer::isSupportedExtension("a.jpg"));
    CHECK_TRUE(Importer::isSupportedExtension("a.jpeg"));
    CHECK_TRUE(Importer::isSupportedExtension("a.wav"));
    CHECK_FALSE(Importer::isSupportedExtension("a.mp4"));
    CHECK_FALSE(Importer::isSupportedExtension("a"));
}

TEST_CASE(import_empty_sourcepath_returns_error)
{
    Importer::Result r = Importer::importFile("", "C:/tmp/cache");
    CHECK_FALSE(r.success);
    CHECK_TRUE(!r.errorMessage.empty());
    CHECK_TRUE(r.errorMessage.find("empty") != std::string::npos);
}

TEST_CASE(import_missing_file_returns_error)
{
    Importer::Result r =
        Importer::importFile("D:/no/such/path/missing.fbx", "D:/tmp/cache");
    CHECK_FALSE(r.success);
    CHECK_TRUE(r.errorMessage.find("does not exist") != std::string::npos);
}

TEST_CASE(import_animation_missing_file_returns_error)
{
    Importer::Result r = Importer::importAnimationFile(
        "D:/no/such/path/missing_animation.fbx", "D:/tmp/cache");
    CHECK_FALSE(r.success);
    CHECK_TRUE(r.errorMessage.find("does not exist") != std::string::npos);
}

TEST_CASE(import_unsupported_extension_returns_error_with_supported_list)
{
    // Create a real on-disk file with an unsupported extension; the
    // extension check runs *after* the file-exists check.
    const std::string path = (std::filesystem::temp_directory_path()
        / "ayeditor_test_unsupported.mp4").string();
    CHECK_TRUE(writeStubFile(path, "fake video bytes"));

    Importer::Result r = Importer::importFile(path, "D:/tmp/cache");
    CHECK_FALSE(r.success);
    CHECK_TRUE(r.errorMessage.find("unsupported") != std::string::npos);
    CHECK_TRUE(r.errorMessage.find("Supported:")   != std::string::npos);

    ayt::io::File::remove(path);
}

TEST_CASE(import_dialog_is_thin_passthrough_to_importer)
{
    // Calling through `ImportDialog::importFromPath` with an empty
    // source must produce the same error shape as a direct call to
    // `Importer::importFile`. The dialog layer is one line; the test
    // pins that contract so a future menu refactor can't silently
    // change the return semantics.
    Importer::Result direct =
        Importer::importFile("", "D:/tmp/cache");
    Importer::Result viaDialog =
        ImportDialog::importFromPath("", "D:/tmp/cache");
    CHECK_FALSE(direct.success);
    CHECK_FALSE(viaDialog.success);
    CHECK_TRUE(direct.errorMessage == viaDialog.errorMessage);
}

TEST_CASE(reimport_queue_recovers_full_persisted_recipe)
{
    EditorImportFixture fixture;
    fixture.triangle();
    auto options = fixture.options();
    options.materialPolicy.tag = "keep-policy";
    options.materialPolicy.maskNames = "Grate";
    options.sourceCoordinates.tag = "keep-coordinates";
    options.sourceCoordinates.uvOrigin = ayt::resource::ImportUvOrigin::BottomLeft;
    CHECK(Importer::importAssetFile(options).success);
    EditorAssetImportQueue queue;
    const auto id = queue.enqueue(options.sourcePath, options.outputDir, true);
    const auto* queued = queue.find(id);
    CHECK(queued != nullptr);
    if (!queued) return;
    CHECK(queued->options.loadOption == options.loadOption);
    CHECK(queued->options.materialPolicy.maskNames == options.materialPolicy.maskNames);
    CHECK(queued->options.sourceCoordinates.uvOrigin == options.sourceCoordinates.uvOrigin);
    CHECK(fixture.finish(queue));
    CHECK_MSG(queue.find(id)->state == EditorAssetImportJobState::Succeeded, queue.find(id)->message.c_str());
    ayt::resource::ImportRecord record;
    CHECK(ayt::resource::readImportRecord((fixture.output() / "Triangle.dep").string(), record));
    CHECK(record.options.materialPolicy.tag == options.materialPolicy.tag);
}

TEST_CASE(import_queue_delivers_immediate_failure_and_background_completion_once)
{
    EditorImportFixture fixture;
    fixture.triangle();
    EditorAssetImportQueue queue;
    const auto valid = queue.enqueue(fixture.options());
    const auto invalid = queue.enqueue("invalid.unsupported", fixture.output().string());
    std::vector<EditorAssetImportJobId> completions;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (completions.size() < 2 && std::chrono::steady_clock::now() < deadline) {
        if (queue.poll()) completions.insert(completions.end(), queue.completedJobIds().begin(),
            queue.completedJobIds().end());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(std::count(completions.begin(), completions.end(), valid) == 1);
    CHECK(std::count(completions.begin(), completions.end(), invalid) == 1);
    CHECK(queue.find(valid)->state == EditorAssetImportJobState::Succeeded);
    CHECK(queue.find(invalid)->state == EditorAssetImportJobState::Failed);
    CHECK_FALSE(queue.poll());
    CHECK(queue.completedJobIds().empty());
}

TEST_CASE(import_database_associates_external_source_and_rechecks_its_dependencies)
{
    EditorImportFixture fixture;
    fixture.triangle();
    auto options = fixture.options();
    options.sourceCoordinates.tag = "external-recipe";
    CHECK(Importer::importAssetFile(options).success);
    EditorAssetDatabase database;
    CHECK(database.open(fixture.project().string()));
    CHECK(database.scanNow());
    CHECK(database.imports().size() == 1u);
    const auto& imports = database.imports();
    if (imports.empty()) return;
    CHECK(imports[0].state == EditorAssetImportState::Ready);
    const auto resource = imports[0].record.conversion.resources.front().path;
    const auto* mesh = database.findByLogicalPath("Imported/" + resource);
    CHECK(mesh != nullptr);
    if (!mesh) return;
    const auto stableId = mesh->id;
    CHECK(database.importForAsset(stableId) != nullptr);
    CHECK(database.importForAsset(stableId)->record.options.sourceCoordinates.tag == "external-recipe");
    fixture.triangle(2.0f); // External source is deliberately outside both watched asset roots.
    EditorAssetImportQueue queue;
    CHECK(queue.enqueueChangedDependencies(database) == 1u);
    CHECK(fixture.finish(queue));
    CHECK(queue.jobs()[0].state == EditorAssetImportJobState::Succeeded);
    CHECK(database.scanNow());
    mesh = database.find(stableId);
    CHECK(mesh != nullptr);
    CHECK(database.importForAsset(stableId)->state == EditorAssetImportState::Ready);
    database.close();
    CHECK(database.open(fixture.project().string()));
    CHECK(database.scanNow());
    CHECK(database.importForAsset(stableId) != nullptr);
    CHECK(database.importForAsset(stableId)->record.options.sourceCoordinates.tag == "external-recipe");
}

TEST_CASE(selected_asset_inspector_updates_when_a_scan_discovers_its_import_source)
{
    EditorImportFixture fixture;
    fixture.triangle();
    auto options = fixture.options();
    const auto imported = Importer::importAssetFile(options);
    CHECK(imported.success);
    std::wstring meshName;
    for (const auto& resource : imported.conversion.resources) {
        if (resource.type != "Mesh") continue;
        meshName = std::filesystem::path(resource.path).filename().wstring();
        break;
    }
    CHECK_FALSE(meshName.empty());
    std::filesystem::remove(fixture.output() / "Triangle.dep");
    ayt::ui::MockRenderer backend;
    EditorSession session;
    EditorSessionDesc description;
    description.uiBackend = &backend;
    description.projectRoot = fixture.project().string();
    description.layoutPath = std::string(AY_EDITOR_TEST_SOURCE_DIR) + "/ui/editor_shell.ui.json";
    CHECK(session.initialize(description));
    session.setClientSize(1280, 720);
    CHECK(session.rescanAssetsNow());
    auto* mode = dynamic_cast<ayt::ui::ComboBox*>(session.ui().findById("cmb_assets_view"));
    auto* list = dynamic_cast<ayt::ui::TileView*>(session.ui().findById("list_assets"));
    auto* reload = dynamic_cast<ayt::ui::Button*>(session.ui().findById("btn_asset_reload"));
    CHECK(mode && list && reload);
    if (!mode || !list || !reload) return;
    mode->setSelectedIndexAndNotify(1);
    session.update(0);
    bool found = false;
    for (std::size_t index = 0; index < list->getItemCount(); ++index) {
        if (list->getItem(index).find(meshName) == std::wstring::npos) continue;
        list->setSelectedIndex(static_cast<int>(index));
        found = true;
        break;
    }
    CHECK(found);
    if (!found) { session.shutdown(); return; }
    CHECK(session.selectedAssetId() != 0);
    const auto stableId = session.selectedAssetId();
    CHECK(reload->isEnabled());
    CHECK(session.assetDatabase().importForAsset(stableId) == nullptr);
    options.force = true;
    CHECK(Importer::importAssetFile(options).success);
    CHECK(session.rescanAssetsNow());
    CHECK(session.selectedAssetId() == stableId);
    CHECK(reload->isEnabled());
    CHECK(session.assetDatabase().importForAsset(stableId) != nullptr);
    fixture.triangle(2.0f);
    session.ui().layout();
    const auto bounds = reload->getWorldBounds();
    const auto point = session.ui().logicalToPhysical(
        {(bounds.minX + bounds.maxX) * .5f, (bounds.minY + bounds.maxY) * .5f});
    session.onMouseMove(point.x, point.y);
    session.onMouseButtonDown(point.x, point.y, 0);
    session.onMouseButtonUp(point.x, point.y, 0);
    CHECK(session.assetImportQueue()->jobs().size() == 1u);
    if (session.assetImportQueue()->jobs().empty()) { session.shutdown(); return; }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (session.assetImportQueue()->busy() && std::chrono::steady_clock::now() < deadline) {
        session.update(1.0f / 60);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_FALSE(session.assetImportQueue()->busy());
    CHECK(session.assetImportQueue()->jobs().back().state == EditorAssetImportJobState::Succeeded);
    session.shutdown();
}

TEST_CASE(project_import_recipe_survives_cache_deletion_and_project_relocation)
{
    EditorImportFixture fixture;
    fixture.triangle();
    const auto source = fixture.project() / "Assets/Models/Triangle.gltf";
    std::filesystem::create_directories(source.parent_path());
    std::filesystem::copy_file(fixture.source(), source);
    std::filesystem::copy_file(fixture.source().parent_path() / "Triangle.bin",
        source.parent_path() / "Triangle.bin");
    auto options = fixture.options();
    options.sourcePath = source.string();
    options.materialPolicy.tag = "durable-authoring-settings";
    options.materialPolicy.maskNames = "Grate";
    options.sourceCoordinates.tag = "persisted-coordinates";
    const auto sourceRoot = fixture.project() / "Assets";
    {
        EditorAssetImportQueue queue;
        queue.configureProject(fixture.project().string(), sourceRoot.string());
        const auto id = queue.enqueue(options);
        CHECK(fixture.finish(queue));
        CHECK_MSG(queue.find(id)->state == EditorAssetImportJobState::Succeeded, queue.find(id)->message.c_str());
    }
    const auto recipes = detail::loadImportRecipes(fixture.project().string());
    CHECK(recipes.size() == 1u);
    if (recipes.empty()) return;
    CHECK(recipes[0].options.materialPolicy.tag == options.materialPolicy.tag);
    CHECK(recipes[0].options.sourceCoordinates.tag == options.sourceCoordinates.tag);
    CHECK(std::filesystem::is_regular_file(recipes[0].path));
    // Only this fixture's explicitly derived cache is removed; source and
    // .ayeditor/imports author data remain in the temporary project.
    std::filesystem::remove_all(fixture.output());
    const auto moved = fixture.root / "RelocatedProject";
    std::filesystem::rename(fixture.project(), moved);
    const auto relocated = detail::loadImportRecipes(moved.string());
    CHECK(relocated.size() == 1u);
    if (relocated.empty()) return;
    CHECK(relocated[0].id == recipes[0].id);
    CHECK(std::filesystem::equivalent(std::filesystem::u8path(relocated[0].options.sourcePath),
        moved / "Assets/Models/Triangle.gltf"));
    EditorAssetDatabase database;
    CHECK(database.open(moved.string()));
    CHECK(database.scanNow());
    CHECK(database.imports().size() == 1u);
    CHECK(database.imports()[0].state == EditorAssetImportState::NeedsImport);
    const auto* record = database.findByLogicalPath("Assets/Models/Triangle.gltf");
    CHECK(record && database.importForAsset(record->id));
    EditorAssetImportQueue queue;
    queue.configureProject(moved.string(), (moved / "Assets").string());
    CHECK(queue.enqueueChangedDependencies(database) == 1u);
    CHECK(fixture.finish(queue));
    CHECK(queue.jobs()[0].state == EditorAssetImportJobState::Succeeded);
    CHECK(queue.jobs()[0].options.materialPolicy.maskNames == "Grate");
    CHECK(queue.jobs()[0].options.sourceCoordinates.tag == "persisted-coordinates");
    CHECK(database.scanNow());
    CHECK(database.imports()[0].state == EditorAssetImportState::Ready);
}

TEST_CASE(import_settings_form_validates_pending_fields_before_applying)
{
    ayt::ui::MockRenderer backend;
    ayt::ui::UIManager manager;
    manager.initialize(&backend);
    manager.setClientSize(1280, 720);
    ayt::resource::ImportOptions original;
    original.sourcePath = "Source/Model.gltf";
    original.outputDir = "Imported";
    original.requireCharacterAssets = false;
    original.sourceCoordinates.tag = "preserve-source-tag";
    original.materialPolicy.opaqueIndices = "2;5";
    original.sourceCoordinates.metersPerUnit = 1e-8f;
    int applications = 0;
    ayt::resource::ImportOptions applied;
    EditorImportSettingsDialog dialog(original,
        [&](const auto& options, std::string&) { ++applications; applied = options; return true; },
        [](const char*, const wchar_t* fallback) { return std::wstring(fallback); });
    manager.root()->addChildExternal(&dialog);
    dialog.fitAndCenterInViewport({1280, 720}, 12);
    dialog.openModal();
    const auto find = [](auto&& self, ayt::ui::Widget* widget, const char* id) -> ayt::ui::Widget* {
        if (widget->getId() == id) return widget;
        for (auto* child : widget->getChildren()) if (auto* result = self(self, child, id)) return result;
        return nullptr;
    };
    const auto widget = [&](const char* id) { return find(find, dialog.getBodyContent(), id); };
    auto* units = dynamic_cast<ayt::ui::TextInput*>(widget("import_settings_units_input"));
    auto* mode = dynamic_cast<ayt::ui::ComboBox*>(widget("import_settings_coordinates"));
    auto* up = dynamic_cast<ayt::ui::ComboBox*>(widget("import_settings_up"));
    auto* forward = dynamic_cast<ayt::ui::ComboBox*>(widget("import_settings_forward"));
    auto* normal = dynamic_cast<ayt::ui::ComboBox*>(widget("import_settings_normal_y"));
    auto* mask = dynamic_cast<ayt::ui::TextInput*>(widget("import_settings_mask_input"));
    auto* error = dynamic_cast<ayt::ui::TextLabel*>(widget("import_settings_error"));
    CHECK(units && mode && up && forward && normal && mask && error);
    if (!units || !mode || !up || !forward || !normal || !mask || !error) {
        dialog.closeModal(); manager.root()->removeChild(&dialog); manager.shutdown(); return;
    }
    CHECK_FLOAT_EQ(std::stof(units->getText()), original.sourceCoordinates.metersPerUnit, 1e-12f);
    units->setText(L"nan");
    dialog.acceptDialog();
    CHECK(dialog.isOpen());
    CHECK(applications == 0);
    CHECK_FALSE(error->getText().empty());
    units->setText(L"0.01");
    mode->setSelectedIndexAndNotify(1);
    up->setSelectedIndexAndNotify(2);
    forward->setSelectedIndexAndNotify(3); // Both directions belong to the Y axis.
    dialog.acceptDialog();
    CHECK(dialog.isOpen());
    CHECK(applications == 0);
    forward->setSelectedIndexAndNotify(4);
    normal->setSelectedIndexAndNotify(1);
    mask->setText(L"Grate;Fence"); // Pending text is read even without Enter/focus loss.
    dialog.acceptDialog();
    CHECK_FALSE(dialog.isOpen());
    CHECK(applications == 1);
    CHECK_FLOAT_EQ(applied.sourceCoordinates.metersPerUnit, .01f, 1e-6f);
    CHECK(applied.sourceCoordinates.tag == original.sourceCoordinates.tag);
    CHECK(applied.materialPolicy.normalMapYSign == -1);
    CHECK(applied.materialPolicy.maskNames == "Grate;Fence");
    CHECK(applied.materialPolicy.opaqueIndices == "2;5");
    manager.root()->removeChild(&dialog);
    manager.shutdown();
}

TEST_CASE(project_imports_isolate_two_sources_with_the_same_filename)
{
    EditorImportFixture fixture;
    fixture.triangle();
    std::filesystem::create_directories(fixture.project() / "Assets");
    const auto other = fixture.root / "Other/Triangle.gltf";
    std::filesystem::create_directories(other.parent_path());
    std::filesystem::copy_file(fixture.source(), other);
    std::filesystem::copy_file(fixture.source().parent_path() / "Triangle.bin", other.parent_path() / "Triangle.bin");
    EditorAssetImportQueue queue;
    queue.configureProject(fixture.project().string(), (fixture.project() / "Assets").string());
    auto options = fixture.options();
    const auto firstId = queue.enqueue(options);
    CHECK(fixture.finish(queue));
    CHECK(queue.find(firstId)->state == EditorAssetImportJobState::Succeeded);
    const auto firstNamespace = queue.find(firstId)->options.outputNamespace;
    const auto firstOutputs = queue.find(firstId)->outputPaths;
    CHECK_FALSE(firstNamespace.empty());
    options.sourcePath = other.string();
    const auto secondId = queue.enqueue(options);
    CHECK(fixture.finish(queue));
    CHECK(queue.find(secondId)->state == EditorAssetImportJobState::Succeeded);
    CHECK_FALSE(queue.find(secondId)->options.outputNamespace.empty());
    CHECK(queue.find(secondId)->options.outputNamespace != firstNamespace);
    CHECK(queue.find(secondId)->outputPaths != firstOutputs);
    CHECK(detail::loadImportRecipes(fixture.project().string()).size() == 2u);
    // The ordinary source command recovers the same namespace and settings.
    const auto reimportId = queue.enqueue(fixture.source().string(), fixture.output().string());
    CHECK(fixture.finish(queue));
    CHECK(queue.find(reimportId)->options.outputNamespace == firstNamespace);
    CHECK(queue.find(reimportId)->state == EditorAssetImportJobState::CacheHit);
}

TEST_CASE(project_recipe_identity_survives_relocation_with_existing_derived_outputs)
{
    EditorImportFixture fixture;
    fixture.triangle();
    const auto source = fixture.project() / "Assets/Models/Triangle.gltf";
    std::filesystem::create_directories(source.parent_path());
    std::filesystem::copy_file(fixture.source(), source);
    std::filesystem::copy_file(fixture.source().parent_path() / "Triangle.bin", source.parent_path() / "Triangle.bin");
    auto options = fixture.options();
    options.sourcePath = source.string();
    std::string identity, outputNamespace;
    std::vector<std::string> outputs;
    {
        EditorAssetImportQueue queue;
        queue.configureProject(fixture.project().string(), (fixture.project() / "Assets").string());
        const auto id = queue.enqueue(options);
        CHECK(fixture.finish(queue));
        CHECK_MSG(queue.find(id)->state == EditorAssetImportJobState::Succeeded, queue.find(id)->message.c_str());
        identity = queue.find(id)->options.sourceId;
        outputNamespace = queue.find(id)->options.outputNamespace;
        outputs = queue.find(id)->outputPaths;
    }
    const auto moved = fixture.root / "MovedProject";
    std::filesystem::rename(fixture.project(), moved);
    EditorAssetDatabase database;
    CHECK(database.open(moved.string()));
    CHECK(database.scanNow());
    EditorAssetImportQueue queue;
    queue.configureProject(moved.string(), (moved / "Assets").string());
    CHECK(database.imports().size() == 1u);
    // Use the persisted recipe, not an absolute source path from the old cache.
    const auto recipes = detail::loadImportRecipes(moved.string());
    CHECK(recipes.size() == 1u);
    if (recipes.empty()) return;
    const auto id = queue.enqueue(recipes[0].options);
    CHECK(fixture.finish(queue));
    CHECK_MSG(queue.find(id)->state == EditorAssetImportJobState::Succeeded, queue.find(id)->message.c_str());
    CHECK(queue.find(id)->options.sourceId == identity);
    CHECK(queue.find(id)->options.outputNamespace == outputNamespace);
    CHECK(queue.find(id)->outputPaths == outputs);
    CHECK(database.scanNow());
}


TEST_CASE(import_queue_keeps_changed_settings_and_force_requests)
{
    EditorImportFixture fixture;
    fixture.triangle();
    EditorAssetImportQueue queue;
    auto options = fixture.options();
    const auto first = queue.enqueue(options);
    CHECK(queue.enqueue(options) == first);
    options.force = true;
    const auto forced = queue.enqueue(options);
    CHECK(forced != first);
    CHECK(queue.enqueue(options) == forced);
    options.sourceCoordinates.metersPerUnit = .01f;
    const auto changed = queue.enqueue(options);
    CHECK(changed != first && changed != forced);
    CHECK(fixture.finish(queue));
    CHECK(queue.find(first)->finished());
    CHECK(queue.find(forced)->finished());
    CHECK(queue.find(changed)->state == EditorAssetImportJobState::Succeeded);
}


TEST_CASE(project_recipe_file_changes_refresh_import_settings_without_listing_author_data_as_assets)
{
    EditorImportFixture fixture;
    fixture.triangle();
    auto options = fixture.options();
    options.sourceAssetRoot.clear();
    std::filesystem::create_directories(fixture.project()/"Assets");
    EditorAssetImportQueue queue;
    queue.configureProject(fixture.project().string(), (fixture.project()/"Assets").string());
    const auto id = queue.enqueue(options);
    CHECK(fixture.finish(queue));
    CHECK_MSG(queue.find(id)->state == EditorAssetImportJobState::Succeeded, queue.find(id)->message.c_str());
    EditorAssetDatabase database;
    CHECK(database.open(fixture.project().string()));
    CHECK(database.scanNow());
    CHECK(database.imports().size()==1u);
    if(database.imports().size()!=1)return;
    CHECK(database.imports()[0].state==EditorAssetImportState::Ready);
    const auto count=database.records().size();
    options=queue.find(id)->options;
    options.sourceCoordinates.metersPerUnit=.25f;
    CHECK(detail::saveImportRecipe(fixture.project().string(),options));
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    bool observed=false;
    while(std::chrono::steady_clock::now()<deadline){
        (void)database.pollFileChanges();
        (void)database.pollScan();
        if(database.imports().size()==1u
            && database.imports()[0].record.options.sourceCoordinates.metersPerUnit==.25f){
            observed=true;break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(observed);
    CHECK(database.records().size()==count);
    if(observed)CHECK(database.imports()[0].state==EditorAssetImportState::NeedsImport);
}

TEST_CASE(source_relink_dialog_validates_picker_and_defers_import_until_apply)
{
    EditorImportFixture fixture;
    fixture.triangle();
    const auto moved=fixture.root/"Moved/Renamed.gltf";
    fixture.write(moved,"source picker fixture");
    ayt::ui::MockRenderer backend;
    ayt::ui::UIManager manager;
    manager.initialize(&backend);manager.setClientSize(1280,900);
    const auto original=fixture.options();
    auto chosen=(fixture.root/"Moved/Wrong.png").string();
    int applications=0;
    ayt::resource::ImportOptions applied;
    EditorImportSettingsDialog dialog(original,
        [&](const auto& options,std::string&){++applications;applied=options;return true;},
        [](const char*,const wchar_t* fallback){return std::wstring(fallback);},
        [&]{return chosen;});
    manager.root()->addChildExternal(&dialog);
    dialog.fitAndCenterInViewport({1280,900},12);dialog.openModal();manager.layout();
    const auto find=[](auto&& self,ayt::ui::Widget* widget,const char* id)->ayt::ui::Widget* {
        if(widget->getId()==id)return widget;
        for(auto* child:widget->getChildren())if(auto* found=self(self,child,id))return found;
        return nullptr;
    };
    auto* button=dynamic_cast<ayt::ui::Button*>(find(find,dialog.getBodyContent(),"import_settings_relink"));
    auto* error=dynamic_cast<ayt::ui::TextLabel*>(find(find,dialog.getBodyContent(),"import_settings_error"));
    CHECK(button && error);
    if(!button || !error){dialog.closeModal();manager.shutdown();return;}
    const auto browse=[&]{
        manager.layout();const auto bounds=button->getWorldBounds();
        const auto p=manager.logicalToPhysical({(bounds.minX+bounds.maxX)*.5f,(bounds.minY+bounds.maxY)*.5f});
        manager.onMouseMove(p.x,p.y);manager.onMouseButtonDown(p.x,p.y,0);manager.onMouseButtonUp(p.x,p.y,0);
    };
    browse();CHECK_FALSE(error->getText().empty());CHECK(applications==0);
    chosen=(fixture.root/"Missing.gltf").string();browse();CHECK_FALSE(error->getText().empty());
    chosen=moved.string();browse();CHECK(error->getText().empty());CHECK(applications==0);
    dialog.acceptDialog();
    CHECK(applications==1);CHECK(applied.sourcePath==moved.string());
    CHECK(applied.outputBaseName=="Triangle");CHECK(original.sourcePath==fixture.source().string());
    EditorImportSettingsDialog cancelled(original,
        [&](const auto&,std::string&){++applications;return true;},
        [](const char*,const wchar_t* fallback){return std::wstring(fallback);},
        [&]{return chosen;});
    manager.root()->addChildExternal(&cancelled);
    cancelled.fitAndCenterInViewport({1280,900},12);cancelled.openModal();
    button=dynamic_cast<ayt::ui::Button*>(find(find,cancelled.getBodyContent(),"import_settings_relink"));
    CHECK(button!=nullptr);if(button)browse();
    cancelled.rejectDialog();CHECK(applications==1);
    manager.root()->removeChild(&cancelled);manager.root()->removeChild(&dialog);manager.shutdown();
}

TEST_CASE(project_source_relink_preserves_recipe_outputs_and_rejects_another_owner)
{
    EditorImportFixture fixture;fixture.triangle();
    std::filesystem::create_directories(fixture.project()/"Assets");
    EditorAssetImportQueue queue;
    queue.configureProject(fixture.project().string(),(fixture.project()/"Assets").string());
    const auto firstId=queue.enqueue(fixture.options());CHECK(fixture.finish(queue));
    const auto* first=queue.find(firstId);CHECK(first && first->state==EditorAssetImportJobState::Succeeded);
    if(!first || first->state!=EditorAssetImportJobState::Succeeded)return;
    auto options=first->options;
    const auto resourcePath=first->outputPaths.front();
    CHECK(options.outputBaseName=="Triangle");
    const auto moved=fixture.root/"Moved/Renamed.gltf";
    std::filesystem::create_directories(moved.parent_path());
    std::filesystem::copy_file(fixture.source(),moved);
    std::filesystem::copy_file(fixture.source().parent_path()/"Triangle.bin",moved.parent_path()/"Triangle.bin");
    std::filesystem::remove(fixture.source());
    options.sourcePath=moved.string();options.force=true;
    const auto relinkId=queue.enqueue(options);CHECK(fixture.finish(queue));
    const auto* relinked=queue.find(relinkId);
    CHECK(relinked && relinked->state==EditorAssetImportJobState::Succeeded);
    if(!relinked || relinked->state!=EditorAssetImportJobState::Succeeded)return;
    CHECK(relinked->outputPaths.front()==resourcePath);
    auto recipes=detail::loadImportRecipes(fixture.project().string());
    CHECK(recipes.size()==1u);if(recipes.empty())return;
    CHECK(recipes[0].options.sourceId==options.sourceId);
    CHECK(detail::sameImportSource(recipes[0].options.sourcePath,moved.string()));
    auto conflicting=options;conflicting.sourceId="source-other-owner";
    std::string error;
    CHECK_FALSE(detail::saveImportRecipe(fixture.project().string(),conflicting,&error));
    CHECK(error.find("already belongs")!=std::string::npos);
    recipes=detail::loadImportRecipes(fixture.project().string());
    CHECK(recipes.size()==1u);
    CHECK(recipes[0].options.sourceId==options.sourceId);
}

TEST_CASE(import_settings_revision_refuses_changed_removed_and_newly_created_recipes)
{
    EditorImportFixture fixture;fixture.triangle();auto options=fixture.options();std::string error;
    std::filesystem::create_directories(fixture.project()/"Assets");
    detail::ImportRecipeRevision empty;
    CHECK(detail::readImportRecipeForEdit(fixture.project().string(),options,empty,&error));CHECK_FALSE(empty.existed);
    CHECK(detail::verifyImportRecipeRevision(empty,&error));
    CHECK(detail::saveImportRecipe(fixture.project().string(),options,&error));
    CHECK_FALSE(detail::verifyImportRecipeRevision(empty,&error));CHECK_FALSE(error.empty());
    detail::ImportRecipeRevision current;CHECK(detail::readImportRecipeForEdit(fixture.project().string(),options,current,&error));CHECK(current.existed);
    CHECK(detail::verifyImportRecipeRevision(current,&error));
    auto forceOnly=options;forceOnly.force=true;CHECK(detail::saveImportRecipe(fixture.project().string(),forceOnly,&error));
    CHECK(detail::verifyImportRecipeRevision(current,&error));
    auto changed=options;changed.sourceCoordinates.metersPerUnit=.5f;CHECK(detail::saveImportRecipe(fixture.project().string(),changed,&error));
    CHECK_FALSE(detail::verifyImportRecipeRevision(current,&error));
    auto stale=options;detail::ImportRecipeRevision reopened;
    CHECK(detail::readImportRecipeForEdit(fixture.project().string(),stale,reopened,&error));CHECK_FLOAT_EQ(stale.sourceCoordinates.metersPerUnit,.5f,1e-6f);
    CHECK(detail::verifyImportRecipeRevision(reopened,&error));
    const auto recipes=detail::loadImportRecipes(fixture.project().string(),&error);CHECK(recipes.size()==1);
    if(recipes.empty())return;
    std::filesystem::remove(std::filesystem::u8path(recipes.front().path));CHECK_FALSE(detail::verifyImportRecipeRevision(reopened,&error));
}

TEST_CASE(import_settings_revision_ignores_other_sources_and_refuses_broken_project_recipes)
{
    EditorImportFixture fixture;fixture.triangle();auto options=fixture.options();std::string error;
    std::filesystem::create_directories(fixture.project()/"Assets");
    CHECK(detail::saveImportRecipe(fixture.project().string(),options,&error));detail::ImportRecipeRevision original;
    CHECK(detail::readImportRecipeForEdit(fixture.project().string(),options,original,&error));
    auto other=fixture.options();other.sourcePath=(fixture.root/"Other.gltf").string();
    CHECK(detail::saveImportRecipe(fixture.project().string(),other,&error));CHECK(detail::verifyImportRecipeRevision(original,&error));
    fixture.write(fixture.project()/".ayeditor/imports/source-broken.json","broken JSON");
    CHECK_FALSE(detail::verifyImportRecipeRevision(original,&error));CHECK_FALSE(error.empty());
    detail::ImportRecipeRevision unused;CHECK_FALSE(detail::readImportRecipeForEdit(fixture.project().string(),options,unused,&error));
}

TEST_CASE(actual_import_settings_dialog_rejects_external_recipe_changes_then_reopens_current_values)
{
    EditorImportFixture fixture;fixture.triangle();auto options=fixture.options();std::string error;
    std::filesystem::create_directories(fixture.project()/"Assets");
    CHECK(detail::saveImportRecipe(fixture.project().string(),options,&error));const auto imported=Importer::importAssetFile(options);CHECK(imported.success);
    std::wstring meshName;for(const auto& resource:imported.conversion.resources)if(resource.type=="Mesh"){meshName=std::filesystem::path(resource.path).filename().wstring();break;}
    ayt::ui::MockRenderer backend;EditorSession session;EditorSessionDesc desc;desc.uiBackend=&backend;desc.projectRoot=fixture.project().string();
    desc.layoutPath=std::string(AY_EDITOR_TEST_SOURCE_DIR)+"/ui/editor_shell.ui.json";CHECK(session.initialize(desc));session.setClientSize(1500,1000);CHECK(session.rescanAssetsNow());
    auto* mode=dynamic_cast<ayt::ui::ComboBox*>(session.ui().findById("cmb_assets_view"));auto* list=dynamic_cast<ayt::ui::TileView*>(session.ui().findById("list_assets"));CHECK(mode&&list);
    if(!mode||!list){session.shutdown();return;}mode->setSelectedIndexAndNotify(1);session.update(0);bool selected=false;
    for(size_t i=0;i<list->getItemCount();++i)if(!meshName.empty()&&list->getItem(i).find(meshName)!=std::wstring::npos){list->setSelectedIndex(int(i));selected=true;break;}
    CHECK(selected);if(!selected){session.shutdown();return;}CHECK(session.editSelectedAssetImportSettings());
    const auto find=[](auto&& self,ayt::ui::Widget* root,const char* id)->ayt::ui::Widget*{
        if(!root)return nullptr;if(root->getId()==id)return root;for(auto* child:root->getChildren())if(auto* found=self(self,child,id))return found;return nullptr;};
    auto* dialog=dynamic_cast<EditorImportSettingsDialog*>(find(find,session.ui().getOverlayRoot(),"asset_import_settings_dialog"));CHECK(dialog);
    if(!dialog){session.shutdown();return;}auto* units=dynamic_cast<ayt::ui::TextInput*>(find(find,dialog,"import_settings_units_input"));CHECK(units);
    auto* message=dynamic_cast<ayt::ui::TextLabel*>(find(find,dialog,"import_settings_error"));CHECK(message);if(!units||!message){session.shutdown();return;}
    units->setText(L"0.25");auto external=options;external.sourceCoordinates.metersPerUnit=.5f;CHECK(detail::saveImportRecipe(fixture.project().string(),external,&error));
    const auto jobs=session.assetImportQueue()->jobs().size();dialog->acceptDialog();CHECK(dialog->isOpen());CHECK_FALSE(message->getText().empty());
    CHECK(session.assetImportQueue()->jobs().size()==jobs);auto recipes=detail::loadImportRecipes(fixture.project().string(),&error);CHECK(recipes.size()==1);
    if(!recipes.empty())CHECK_FLOAT_EQ(recipes.front().options.sourceCoordinates.metersPerUnit,.5f,1e-6f);
    dialog->rejectDialog();session.update(.25f);CHECK(session.editSelectedAssetImportSettings());
    dialog=dynamic_cast<EditorImportSettingsDialog*>(find(find,session.ui().getOverlayRoot(),"asset_import_settings_dialog"));CHECK(dialog);
    if(dialog){units=dynamic_cast<ayt::ui::TextInput*>(find(find,dialog,"import_settings_units_input"));CHECK(units);if(units)CHECK_FLOAT_EQ(std::stof(units->getText()),.5f,1e-6f);dialog->rejectDialog();}
    session.shutdown();
}

TEST_CASE(texture_sampling_form_requires_cooked_output_and_keeps_validation_visible)
{
    ayt::ui::MockRenderer backend; ayt::ui::UIManager manager;
    manager.initialize(&backend); manager.setClientSize(1280,720);
    ayt::resource::ImportOptions original;
    original.sourcePath="Source/Surface.png";original.outputDir="Imported";
    int applications=0;ayt::resource::ImportOptions applied;
    EditorImportSettingsDialog dialog(original,
        [&](const auto& options,std::string&){++applications;applied=options;return true;},
        [](const char*,const wchar_t* fallback){return std::wstring(fallback);});
    manager.root()->addChildExternal(&dialog);
    dialog.fitAndCenterInViewport({1280,720},12);dialog.openModal();manager.layout();
    const auto find=[](auto&& self,ayt::ui::Widget* widget,const char* id)->ayt::ui::Widget* {
        if(widget->getId()==id)return widget;
        for(auto* child:widget->getChildren())if(auto* found=self(self,child,id))return found;
        return nullptr;
    };
    const auto choice=[&](const char* id){return dynamic_cast<ayt::ui::ComboBox*>(find(find,&dialog,id));};
    auto* filter=choice("import_settings_sampling");
    auto* mag=choice("import_settings_sampling_mag");
    auto* wrapU=choice("import_settings_sampling_u");auto* wrapV=choice("import_settings_sampling_v");
    auto* mips=choice("import_settings_texture_mips");auto* output=choice("import_settings_textures");
    auto* message=dynamic_cast<ayt::ui::TextLabel*>(find(find,&dialog,"import_settings_error"));
    auto* scroll=dynamic_cast<ayt::ui::ScrollView*>(find(find,&dialog,"import_settings_scroll"));
    CHECK(filter && mag && wrapU && wrapV && mips && output && message && scroll);
    if(filter && mag && wrapU && wrapV && mips && output && message && scroll){
        filter->setSelectedIndexAndNotify(4);
        CHECK(mag->getSelectedIndex()==1);CHECK_FALSE(mag->isEnabled());
        filter->setSelectedIndexAndNotify(3);mag->setSelectedIndexAndNotify(0);
        CHECK(mag->isEnabled());
        wrapU->setSelectedIndexAndNotify(2);wrapV->setSelectedIndexAndNotify(1);mips->setSelectedIndexAndNotify(1);
        dialog.acceptDialog();CHECK(dialog.isOpen());CHECK(applications==0);CHECK_FALSE(message->getText().empty());
        manager.layout();
        const auto errorBounds=message->getWorldBounds();const auto dialogBounds=dialog.getWorldBounds();
        CHECK(errorBounds.minY>=dialogBounds.minY && errorBounds.maxY<=dialogBounds.maxY);
        CHECK(message->getParent()!=scroll->getContent());
        output->setSelectedIndexAndNotify(1);dialog.acceptDialog();
        CHECK_FALSE(dialog.isOpen());CHECK(applications==1);
        CHECK(applied.overrideTextureSampler && applied.cookTextures && !applied.keepTextureMipmaps);
        CHECK(applied.textureSampler.minFilter==ayt::resource::TextureFilter::Trilinear);
        CHECK(applied.textureSampler.magFilter==ayt::resource::TextureFilter::Point);
        CHECK(applied.textureSampler.wrapU==ayt::resource::TextureWrap::Mirror);
        CHECK(applied.textureSampler.wrapV==ayt::resource::TextureWrap::Clamp);
        CHECK_FALSE(applied.textureSampler.sRGB);
    }
    dialog.closeModal();manager.root()->removeChild(&dialog);manager.shutdown();
}

TEST_SUITE_END
