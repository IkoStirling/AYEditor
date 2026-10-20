#include <AYEditor/EditorMaterialDocument.h>
#include <AYEditor/EditorProjectDescriptor.h>
#include <AYEditor/EditorWorkspace.h>
#include <AYEditor/EditorSession.h>
#include <AYEditor/EditorProjectAssetFactory.h>
#include <AYResource/assetsImpl/Material.h>
#include <AYResource/assetsImpl/Texture.h>
#include <AYResource/AssetPath.h>
#include "../src/MaterialCopyPublication.h"
#include <thread>
#include <barrier>
#include <atomic>
#include <AYResource/Loader/MaterialFile.h>
#include <AYIO/File.h>
#include <AYUI/TextInput.h>
#include <AYUI/UIKeyCode.h>
#include <AYUI/Button.h>
#include <AYUI/ComboBox.h>
#include <AYUI/MockRenderer.h>
#include <AYUI/ModalDialog.h>
#include <AYUI/ScrollView.h>
#include <AYUI/ListView.h>
#include <AYUI/TileView.h>
#include <AYEntity/EntityImpl.h>
#include <AYEntity/World.h>
#include <AYEntity/components/MeshComponent.h>
#include <AYScene.h>
#include <AYApplication/IEngineHost.h>
#include <AYTest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <cstring>

using namespace ayt::editor;
namespace
{
namespace fs = std::filesystem;
std::string utf8(const fs::path &p)
{
    const auto s = p.u8string();
    return {s.begin(), s.end()};
}
std::vector<UInt8> read(const fs::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
struct MaterialFixture
{
    fs::path root =
        ayt::test::testTmpDir() /
        ("material-editor-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path assets = root / "Content", path = assets / fs::u8path("材质.mat");
    std::shared_ptr<EditorMaterialDocument> document = std::make_shared<EditorMaterialDocument>();
    MaterialFixture()
    {
        fs::create_directories(assets);
        EditorProjectDescriptor project;
        project.schemaVersion = 2;
        project.id = "materials";
        project.displayName = "Materials";
        project.assetRoot = "Content";
        std::string error;
        CHECK(project.save(utf8(root), &error));
        ayt::resource::Material m;
        m.initialise("Authored surface", "pbr.phoskia");
        m.setFloat("roughness", .63456789f);
        m.setFloat("metallic", .123456789f);
        const float color[] = {.23456789f, .34567891f, .45678912f, 1};
        m.setFloat4("baseColor", color);
        m.setString("__ayImportSource", "untouched metadata");
        m.setTexture("normalTexture", "./textures/normal.png");
        float matrix[16];
        for (int i = 0; i < 16; ++i)
            matrix[i] = float(i) * .125f;
        m.setMatrix("customMatrix", matrix);
        std::vector<UInt8> bytes;
        CHECK(m.saveToBinary(bytes));
        CHECK(ayt::io::File::atomicWriteUtf8(utf8(path), bytes.data(), bytes.size()));
        CHECK(document->initialize({utf8(path)}, error));
        document->configureSourceRoot(utf8(assets));
    }
};
struct Host : IEditorHostServices
{
    EditorWorkspace value;
    std::string root;
    EditorWorkspace &workspace() noexcept override
    {
        return value;
    }
    const std::string &projectRoot() const noexcept override
    {
        return root;
    }
    void requestRepaint() override
    {
    }
    void setStatusText(const std::wstring &) override
    {
    }
};
ayt::ui::Widget *find(ayt::ui::Widget *root, const std::string &id)
{
    if (root->getId() == id)
        return root;
    for (auto *child : root->getChildren())
        if (auto *result = find(child, id))
            return result;
    return nullptr;
}
ayt::ui::Widget* findVisible(ayt::ui::Widget* root,const std::string& id){
    if(!root||!root->isVisible())return nullptr;if(root->getId()==id)return root;
    for(auto* child:root->getChildren())if(auto* found=findVisible(child,id))return found;
    return nullptr;
}
bool clickMaterialWidget(EditorSession& session,ayt::ui::Widget* widget){
    if(!widget)return false;session.ui().layout();
    for(auto* parent=widget->getParent();parent;parent=parent->getParent())
        if(auto* scroll=dynamic_cast<ayt::ui::ScrollView*>(parent)){
            const auto bounds=widget->getWorldBounds(),client=scroll->getClientRect();
            scroll->scrollBy({0,bounds.minY-client.minY-12});session.ui().layout();break;
        }
    const auto bounds=widget->getWorldBounds();
    const auto p=session.ui().logicalToPhysical({(bounds.minX+bounds.maxX)*.5f,(bounds.minY+bounds.maxY)*.5f});
    session.onMouseMove(p.x,p.y);const bool down=session.onMouseButtonDown(p.x,p.y,0);
    const bool up=session.onMouseButtonUp(p.x,p.y,0);
    if (!down || !up) {
        const auto logical=session.ui().physicalToLogical(p);
        auto* hit=session.ui().root()->hitTest(logical);
        printf("Material click %s bounds=(%.1f,%.1f)-(%.1f,%.1f) down=%d up=%d hit=%s\n",
            widget->getId().c_str(),bounds.minX,bounds.minY,bounds.maxX,bounds.maxY,down,up,hit?hit->getId().c_str():"none");
        for (auto* parent=widget->getParent();parent;parent=parent->getParent()) {
            const auto b=parent->getWorldBounds();
            printf(" ancestor %s visible=%d bounds=(%.1f,%.1f)-(%.1f,%.1f)\n",parent->getId().c_str(),parent->isVisible(),b.minX,b.minY,b.maxX,b.maxY);
        }
    }
    return down&&up;
}
ayt::ui::Button* materialAccept(ayt::ui::ModalDialog* dialog){
    if(!dialog)return nullptr;for(auto* child:dialog->getChildren())
        if(auto* button=dynamic_cast<ayt::ui::Button*>(child);button&&button->getText()==dialog->getAcceptText())return button;
    return nullptr;
}
} // namespace
TEST_SUITE(AYEditor_Material)
TEST_CASE(session_opens_material_from_assets_deduplicates_and_new_action_reveals_document)
{
    MaterialFixture f;
    ayt::app::EngineHostScope hostScope(ayt::app::defaultEngineHost());
    ayt::ui::MockRenderer backend;
    EditorSessionDesc desc;
    desc.uiBackend = &backend;
    desc.layoutPath = std::string(AY_EDITOR_TEST_SOURCE_DIR) + "/ui/editor_shell.ui.json";
    desc.projectRoot = utf8(f.root);
    EditorSession session;
    CHECK(session.initialize(desc));
    session.setClientSize(1280, 800);
    CHECK(session.rescanAssetsNow());
    const auto *record = session.assetDatabase().findByLogicalPath("Assets/材质.mat");
    CHECK(record != nullptr);
    if (record)
    {
        CHECK(record->type == EditorAssetType::Material);
        CHECK(session.openAsset(record->id));
        const auto *active = session.workspace().documents().active();
        CHECK(active != nullptr);
        CHECK(active && active->editorId == kEditorMaterialExtensionId);
        const auto size = session.workspace().documents().size();
        CHECK(session.openAsset(record->id));
        CHECK(session.workspace().documents().size() == size);
        auto material =
            active ? std::dynamic_pointer_cast<EditorMaterialDocument>(active->document) : nullptr;
        CHECK(material != nullptr);
        CHECK(material && !material->readOnly());
    }
    CHECK(session.createProjectAsset(EditorAssetType::Material));
    const auto *created = session.workspace().documents().active();
    CHECK(created && created->editorId == kEditorMaterialExtensionId);
    CHECK(created && created->document->path().find("NewMaterial.mat") != std::string::npos);
    session.shutdown();
}
TEST_CASE(opacity_texture_channel_is_authored_atomically_saved_and_undoable)
{
    MaterialFixture f;
    Host host;
    host.root = utf8(f.root);
    auto view = makeEditorMaterialDescriptor().createView(f.document, host);
    CHECK(view != nullptr);
    if (!view)
        return;
    auto *source = dynamic_cast<ayt::ui::ComboBox *>(find(view->rootWidget(), "material_opacity_source"));
    CHECK(source != nullptr);
    if (!source)
        return;
    CHECK(source->getSelectedIndex() == 0);
    source->setSelectedIndexAndNotify(1);
    CHECK(f.document->settings().opacitySource == 1);
    CHECK(f.document->isDirty());
    std::string error;
    CHECK(f.document->save(&error));
    ayt::resource::Material saved;
    const auto bytes = read(f.path);
    CHECK(saved.loadFromBinary(bytes.data(), bytes.size()));
    CHECK(saved.getFloat("opacitySource") == 1);
    CHECK(f.document->undo());
    CHECK(f.document->settings().opacitySource == 0);
    CHECK(f.document->redo());
    CHECK(f.document->settings().opacitySource == 1);
    auto invalid = f.document->settings();
    invalid.opacitySource = 3;
    CHECK_FALSE(f.document->replaceSettings(invalid, &error));
    CHECK(f.document->settings().opacitySource == 1);
    saved.setFloat("opacitySource", .5f);
    std::vector<UInt8> invalidBytes;
    CHECK(saved.saveToBinary(invalidBytes));
    CHECK(ayt::io::File::atomicWriteUtf8(utf8(f.path), invalidBytes.data(), invalidBytes.size()));
    CHECK_FALSE(f.document->reload(&error));
    CHECK(f.document->settings().opacitySource == 1);
    view->prepareForUiShutdown();
    source->setSelectedIndexAndNotify(2);
    CHECK(f.document->settings().opacitySource == 1);
}
TEST_CASE(new_material_template_has_its_own_shader_and_round_trips_without_external_textures)
{
    MaterialFixture f;
    fs::create_directories(f.assets / "materials");
    const auto orphan = f.assets / "materials" / "NewMaterial.phoskia";
    std::ofstream(orphan, std::ios::binary) << "authored orphan shader";
    const auto first = createEditorProjectAsset(utf8(f.root), EditorAssetType::Material);
    const auto second = createEditorProjectAsset(utf8(f.root), EditorAssetType::Material);
    CHECK(bool(first));
    CHECK(bool(second));
    CHECK(first.absolutePath != second.absolutePath);
    const auto orphanBytes = read(orphan);
    CHECK(std::string(orphanBytes.begin(), orphanBytes.end()) == "authored orphan shader");
    CHECK(fs::u8path(first.absolutePath).stem() != "NewMaterial");
    if (!first || !second)
        return;
    std::string error;
    EditorMaterialDocument doc;
    CHECK(doc.initialize({first.absolutePath}, error));
    doc.configureSourceRoot(utf8(f.assets));
    CHECK_FALSE(doc.readOnly());
    CHECK(doc.settings().surface == ayt::render::PbrSurfaceSettings{});
    const auto shader = fs::u8path(first.absolutePath).parent_path() / fs::u8path(doc.shaderReference());
    CHECK(fs::exists(shader));
    for (const auto &texture : doc.settings().textures)
        CHECK(texture.empty());
    const auto bytes = read(fs::u8path(first.absolutePath));
    ayt::resource::Material source;
    CHECK(source.loadFromBinary(bytes.data(), bytes.size()));
    CHECK(source.getFloat("normalYSign") == 1);
    CHECK(source.getFloat("roughness") == .5f);
    CHECK(source.getFloat("opacity") == 1);
    CHECK(first.logicalPath.find("Assets/materials/") == 0);
}
TEST_CASE(texture_choices_are_owner_relative_atomic_undoable_and_confined_to_source_assets)
{
    MaterialFixture f;
    fs::create_directories(f.assets / "maps");
    const auto file = f.assets / "maps" / "rough.png";
    std::ofstream(file, std::ios::binary) << "path validation fixture";
    std::string error;
    const auto original = f.document->settings();
    CHECK(f.document->setTextureAsset(2, "maps/rough.png", &error));
    CHECK(f.document->settings().textures[2] == "./maps/rough.png");
    CHECK(f.document->textureAssetPath(2) == "maps/rough.png");
    const auto edited = f.document->settings();
    const auto revision = f.document->revision();
    CHECK_FALSE(f.document->setTextureAsset(2, "../outside.png", &error));
    CHECK_FALSE(f.document->setTextureAsset(2, "maps/missing.png", &error));
    CHECK_FALSE(f.document->setTextureAsset(99, "maps/rough.png", &error));
    CHECK(f.document->settings() == edited);
    CHECK(f.document->revision() == revision);
    CHECK(f.document->undo());
    CHECK(f.document->settings() == original);
    CHECK(f.document->redo());
    CHECK(f.document->save(&error));
    EditorMaterialDocument reopened;
    CHECK(reopened.initialize({utf8(f.path)}, error));
    CHECK(reopened.settings() == edited);
    CHECK(f.document->setTextureAsset(2, "", &error));
    CHECK(f.document->settings().textures[2].empty());
    CHECK(f.document->save(&error));
    CHECK(reopened.reload(&error));
    CHECK(reopened.settings().textures[2].empty());
}
TEST_CASE(view_save_commits_pending_texture_and_scalar_fields_as_one_transaction)
{
    MaterialFixture f;
    fs::create_directories(f.assets / "maps");
    std::ofstream(f.assets / "maps" / "color.png") << "path fixture";
    Host host;
    host.root = utf8(f.root);
    auto view = makeEditorMaterialDescriptor().createView(f.document, host);
    auto *root = view->rootWidget();
    auto *roughness = dynamic_cast<ayt::ui::TextInput *>(find(root, "material_roughness"));
    auto *texture = dynamic_cast<ayt::ui::TextInput *>(find(root, "material_texture_baseColorTexture"));
    CHECK(roughness && texture);
    if (!roughness || !texture)
        return;
    const auto original = f.document->settings();
    roughness->setText(L"0.2");
    texture->setText(L"maps/missing.png");
    CHECK(view->commandTarget()->canExecuteCommand("file.save"));
    CHECK_FALSE(view->commandTarget()->executeCommand("file.save"));
    CHECK(f.document->settings() == original);
    texture->setText(L"maps/color.png");
    CHECK(view->commandTarget()->executeCommand("file.save"));
    CHECK(f.document->settings().roughness == .2f);
    CHECK(f.document->settings().textures[0] == "./maps/color.png");
    CHECK_FALSE(f.document->isDirty());
    CHECK(f.document->undo());
    CHECK(f.document->settings() == original);
    view->tick(0);
    std::string error;
    EditorMaterialDocument reopened;
    CHECK(reopened.initialize({utf8(f.path)}, error));
    CHECK(reopened.settings().textures[0] == "./maps/color.png");
}
TEST_CASE(save_unicode_material_preserves_unknown_matrix_texture_shader_and_history)
{
    MaterialFixture f;
    std::string error;
    CHECK_FALSE(f.document->readOnly());
    CHECK_FALSE(f.document->isDirty());
    const auto original = f.document->settings();
    auto s = original;
    s.surface.uvScaleX = 3;
    s.surface.uvOffsetY = .27f;
    s.surface.normalStrength = 2;
    s.surface.roughnessBias = -.15f;
    CHECK(f.document->replaceSettings(s, &error));
    CHECK(f.document->isDirty());
    CHECK(f.document->undo());
    CHECK(f.document->settings() == original);
    CHECK(f.document->redo());
    CHECK(f.document->save(&error));
    CHECK_FALSE(f.document->isDirty());
    const auto bytes = read(f.path);
    ayt::resource::Material decoded;
    CHECK(decoded.loadFromBinary(bytes.data(), bytes.size()));
    CHECK(std::string(decoded.getString("__ayImportSource")) == "untouched metadata");
    CHECK(std::string(decoded.getTexture("normalTexture")) == "./textures/normal.png");
    CHECK(std::string(decoded.getShader()) == "pbr.phoskia");
    const auto matrix = decoded.getMatrix("customMatrix");
    CHECK(matrix != nullptr);
    if (matrix)
        for (int i = 0; i < 16; ++i)
            CHECK(matrix[i] == float(i) * .125f);
    CHECK(decoded.getFloat("normalStrength") == 2);
    CHECK(decoded.getVector4("uvTransform").x == 3);
    CHECK(f.document->undo());
    CHECK(f.document->isDirty());
    CHECK(f.document->redo());
    CHECK_FALSE(f.document->isDirty());
    EditorMaterialDocument reopened;
    CHECK(reopened.initialize({utf8(f.path)}, error));
    CHECK(reopened.settings() == s);
}
TEST_CASE(invalid_edit_reload_and_external_save_conflict_preserve_authoring_state)
{
    MaterialFixture f;
    std::string error;
    const auto original = f.document->settings();
    const auto revision = f.document->revision();
    auto bad = original;
    bad.surface.normalStrength = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(f.document->replaceSettings(bad, &error));
    bad = original;
    bad.surface.uvScaleX = 0;
    CHECK_FALSE(f.document->replaceSettings(bad, &error));
    CHECK(f.document->settings() == original);
    CHECK(f.document->revision() == revision);
    auto s = original;
    s.roughness = .2f;
    CHECK(f.document->replaceSettings(s));
    const auto bytes = read(f.path);
    auto truncated = bytes;
    truncated.pop_back();
    CHECK(ayt::io::File::atomicWriteUtf8(utf8(f.path), truncated.data(), truncated.size()));
    CHECK_FALSE(f.document->reload(&error));
    CHECK(f.document->settings() == s);
    CHECK(f.document->isDirty());
    CHECK_FALSE(f.document->save(&error));
    CHECK(read(f.path) == truncated);
    CHECK(ayt::io::File::atomicWriteUtf8(utf8(f.path), bytes.data(), bytes.size()));
    CHECK(f.document->reload(&error));
    CHECK(f.document->settings() == original);
    CHECK_FALSE(f.document->isDirty());
}
TEST_CASE(imported_files_are_inspectable_read_only_and_bundle_files_are_not_overwritten)
{
    MaterialFixture f;
    f.document->configureSourceRoot(utf8(f.root / "OtherAssets"));
    const auto bytes = read(f.path);
    auto s = f.document->settings();
    s.roughness = .1f;
    std::string error;
    CHECK(f.document->readOnly());
    CHECK_FALSE(f.document->replaceSettings(s, &error));
    CHECK_FALSE(f.document->save(&error));
    CHECK(read(f.path) == bytes);
    ayt::resource::MaterialFile bundle;
    auto m = std::make_shared<ayt::resource::Material>();
    m->initialise("Bundle", "pbr.phoskia");
    bundle.addMaterial(m);
    bundle.addMaterial(m);
    std::vector<UInt8> bundled;
    CHECK(bundle.saveToBinary(bundled));
    const auto path = f.assets / "bundle.mat";
    CHECK(ayt::io::File::atomicWriteUtf8(utf8(path), bundled.data(), bundled.size()));
    EditorMaterialDocument doc;
    CHECK_FALSE(doc.initialize({utf8(path)}, error));
    CHECK(read(path) == bundled);
}
TEST_CASE(view_uses_project_custom_root_round_trips_untouched_fields_and_prepares_callbacks_for_teardown)
{
    MaterialFixture f;
    Host host;
    host.root = utf8(f.root);
    auto descriptor = makeEditorMaterialDescriptor();
    auto view = descriptor.createView(f.document, host);
    CHECK(view != nullptr);
    if (!view)
        return;
    auto *root = view->rootWidget();
    CHECK(find(root, "material_preview") != nullptr);
    CHECK(find(root, "material_references") != nullptr);
    auto *roughness = dynamic_cast<ayt::ui::TextInput *>(find(root, "material_roughness"));
    CHECK(roughness != nullptr);
    if (!roughness)
        return;
    const auto original = f.document->settings();
    roughness->setFocus(true);
    roughness->onKeyDown(ayt::ui::UIKey_Enter);
    CHECK(f.document->settings() == original);
    CHECK_FALSE(f.document->isDirty());
    roughness->setText(L"0.25");
    roughness->onKeyDown(ayt::ui::UIKey_Enter);
    CHECK(f.document->settings().roughness == .25f);
    CHECK(f.document->isDirty());
    CHECK(f.document->executeCommand("edit.undo"));
    view->tick(0);
    CHECK(f.document->settings() == original);
    roughness->setText(L"nan");
    roughness->onKeyDown(ayt::ui::UIKey_Enter);
    CHECK(f.document->settings() == original);
    view->prepareForUiShutdown();
    roughness->setText(L"0.8");
    roughness->onKeyDown(ayt::ui::UIKey_Enter);
    CHECK(f.document->settings() == original);
}
TEST_CASE(workspace_registration_deduplicates_documents_and_honors_dirty_close)
{
    MaterialFixture f;
    EditorWorkspace workspace;
    CHECK(registerEditorMaterialExtension(workspace.registry()));
    const auto first = workspace.documents().open({utf8(f.path)});
    CHECK(bool(first));
    if (!first)
        return;
    const auto duplicate = workspace.documents().open({utf8(f.path)});
    CHECK(duplicate.documentId == first.documentId);
    CHECK(workspace.documents().size() == 1);
    auto doc = std::dynamic_pointer_cast<EditorMaterialDocument>(first.document);
    CHECK(doc != nullptr);
    if (!doc)
        return;
    doc->configureSourceRoot(utf8(f.assets));
    auto s = doc->settings();
    s.surface.normalStrength = 0;
    CHECK(doc->replaceSettings(s));
    CHECK_FALSE(bool(workspace.documents().close(first.documentId, EditorDocumentCloseAction::Cancel)));
    CHECK(workspace.documents().size() == 1);
    CHECK(bool(workspace.documents().close(first.documentId, EditorDocumentCloseAction::Save)));
    CHECK(workspace.documents().size() == 0);
}
TEST_CASE(editable_copy_preserves_snapshot_unknown_values_and_original_history)
{
    MaterialFixture f;
    fs::create_directories(f.assets/"textures");
    const std::string shader="author shader fixture";
    CHECK(ayt::io::File::atomicWriteUtf8(utf8(f.assets/"pbr.phoskia"),shader.data(),shader.size()));
    auto settings=f.document->settings();settings.roughness=.21f;CHECK(f.document->replaceSettings(settings));
    const auto original=read(f.path);const auto revision=f.document->revision();const auto originalPath=f.document->path();
    std::string error;
    const auto copy=f.assets/"materials/copies/Editable.mat";
    CHECK_MSG(f.document->saveEditableCopy("materials/copies/Editable.mat",&error),error.c_str());
    CHECK(f.document->path()==originalPath);CHECK(f.document->revision()==revision);CHECK(f.document->isDirty());
    CHECK(read(f.path)==original);
    ayt::resource::Material loaded,source;
    const auto copied=read(copy);CHECK(loaded.loadFromBinary(copied.data(),copied.size()));
    CHECK(source.loadFromBinary(original.data(),original.size()));
    CHECK(loaded.getGuid()!=source.getGuid());CHECK_FLOAT_EQ(loaded.getFloat("roughness"),.21f,1e-6f);
    CHECK(std::string(loaded.getString("__ayImportSource"))=="untouched metadata");
    const auto* matrix=loaded.getMatrix("customMatrix");CHECK(matrix);
    if(matrix)for(int i=0;i<16;++i)CHECK_FLOAT_EQ(matrix[i],float(i)*.125f,1e-6f);
    CHECK(fs::weakly_canonical(fs::u8path(ayt::resource::resolveAssetPath(utf8(copy),loaded.getShader())))
        ==fs::weakly_canonical(f.assets/"pbr.phoskia"));
    CHECK(fs::weakly_canonical(fs::u8path(ayt::resource::resolveAssetPath(utf8(copy),loaded.getTexture("normalTexture"))))
        ==fs::weakly_canonical(f.assets/"textures/normal.png"));
    CHECK_FALSE(f.document->saveEditableCopy("materials/copies/Editable.mat",&error));CHECK(read(copy)==copied);
    CHECK_FALSE(f.document->saveEditableCopy("../outside.mat",&error));
    CHECK_FALSE(f.document->saveEditableCopy("legacy.aymat",&error));
    CHECK_FALSE(f.document->saveEditableCopy(utf8(f.path),&error));CHECK(read(f.path)==original);
    CHECK(f.document->undo());CHECK_FLOAT_EQ(f.document->settings().roughness,.63456789f,1e-7f);
    CHECK(read(copy)==copied);
}

TEST_CASE(copy_and_save_refuse_a_snapshot_that_cannot_be_reopened_within_document_size_limit)
{
    MaterialFixture f;ayt::resource::Material source;source.initialise("Large","pbr.phoskia");
    const std::string metadata(4*1024*1024-256,'x');source.setString("unknownLargeMetadata",metadata.c_str());
    std::vector<UInt8> bytes;CHECK(source.saveToBinary(bytes));CHECK(bytes.size()<4*1024*1024);
    CHECK(ayt::io::File::atomicWriteUtf8(utf8(f.path),bytes.data(),bytes.size()));
    std::string error;CHECK(f.document->reload(&error));const auto original=read(f.path);
    auto settings=f.document->settings();settings.roughness=.23f;CHECK(f.document->replaceSettings(settings));
    const auto revision=f.document->revision();
    CHECK_FALSE(f.document->saveEditableCopy("too-large.mat",&error));CHECK_FALSE(fs::exists(f.assets/"too-large.mat"));
    CHECK_FALSE(f.document->save(&error));CHECK(read(f.path)==original);CHECK(f.document->isDirty());
    CHECK(f.document->revision()==revision);CHECK(f.document->settings()==settings);
}

TEST_CASE(imported_material_copy_and_imported_texture_choices_remain_independent)
{
    MaterialFixture f;
    const auto imported=f.root/".ayeditor_cache/assets",sourcePath=imported/"materials/source-test/Imported.mat";
    const auto image=imported/"textures/source-test/Surface.txr";
    fs::create_directories(image.parent_path());fs::create_directories(sourcePath.parent_path());
    ayt::resource::Texture texture;texture.createSolidColor(2,2,20,100,220);
    std::vector<UInt8> imageBytes;CHECK(texture.saveToBinary(imageBytes));
    CHECK(ayt::io::File::atomicWriteUtf8(utf8(image),imageBytes.data(),imageBytes.size()));
    ayt::resource::Material source;const auto original=read(f.path);CHECK(source.loadFromBinary(original.data(),original.size()));
    source.setTexture("normalTexture","textures/source-test/Surface.txr");
    std::vector<UInt8> sourceBytes;CHECK(source.saveToBinary(sourceBytes));
    CHECK(ayt::io::File::atomicWriteUtf8(utf8(sourcePath),sourceBytes.data(),sourceBytes.size()));
    EditorMaterialDocument doc;std::string error;CHECK(doc.initialize({utf8(sourcePath)},error));
    doc.configureSourceRoot(utf8(f.assets));doc.configureImportedRoot(utf8(imported));CHECK(doc.readOnly());
    const auto revision=doc.revision();
    CHECK_MSG(doc.saveEditableCopy("materials/EditableImported.mat",&error),error.c_str());
    CHECK(doc.revision()==revision);CHECK(doc.readOnly());CHECK_FALSE(doc.isDirty());CHECK(read(sourcePath)==sourceBytes);
    EditorMaterialDocument editable;CHECK(editable.initialize({utf8(f.assets/"materials/EditableImported.mat")},error));
    editable.configureSourceRoot(utf8(f.assets));editable.configureImportedRoot(utf8(imported));CHECK_FALSE(editable.readOnly());
    CHECK(editable.settings().textures[1]=="textures/source-test/Surface.txr");
    CHECK(editable.textureAssetPath(1)=="Imported/textures/source-test/Surface.txr");
    CHECK_MSG(editable.setTextureAsset(0,"Imported/textures/source-test/Surface.txr",&error),error.c_str());
    CHECK(editable.settings().textures[0]=="textures/source-test/Surface.txr");
    CHECK(editable.undo());CHECK(editable.settings().textures[0].empty());CHECK(editable.redo());
    CHECK(editable.save(&error));CHECK(read(sourcePath)==sourceBytes);CHECK(read(image)==imageBytes);
    const auto current=editable.settings();const auto currentRevision=editable.revision();
    CHECK_FALSE(editable.setTextureAsset(0,"Imported/../escape.png",&error));
    CHECK_FALSE(editable.setTextureAsset(0,"Imported/textures/missing.txr",&error));
    CHECK(editable.settings()==current);CHECK(editable.revision()==currentRevision);
    fs::remove(image);CHECK_FALSE(editable.setTextureAsset(0,"Imported/textures/source-test/Surface.txr",&error));
    CHECK(editable.settings()==current);CHECK(read(sourcePath)==sourceBytes);
}

TEST_CASE(material_copy_publication_never_overwrites_even_with_concurrent_creators)
{
    MaterialFixture f;const auto target=f.assets/"materials/race.mat";
    const std::string a(65536,'a'),b(65536,'b');std::barrier start(2);std::atomic<int> successes{0};
    const auto publish=[&](const std::string& bytes){start.arrive_and_wait();
        try{ayt::editor::detail::publishNewMaterialCopy(f.assets,target,bytes.data(),bytes.size());++successes;}
        catch(const std::exception&){}
    };
    std::thread first([&]{publish(a);}),second([&]{publish(b);});first.join();second.join();
    CHECK(successes.load()==1);const auto bytes=read(target);
    CHECK(bytes==std::vector<UInt8>(a.begin(),a.end())||bytes==std::vector<UInt8>(b.begin(),b.end()));
    bool refused=false;try{ayt::editor::detail::publishNewMaterialCopy(f.assets,target,"replace",7);}
    catch(const std::exception&){refused=true;}
    CHECK(refused);CHECK(read(target)==bytes);
    for(const auto& entry:fs::directory_iterator(f.root))
        CHECK(entry.path().filename().string().rfind(".aymaterial-copy-",0)!=0);
}
TEST_CASE(copy_rebases_unknown_cube_texture_without_changing_its_type)
{
    MaterialFixture f;ayt::resource::Material source;source.initialise("Cube reference","pbr.phoskia");
    source.setTexture("customCube","./cube.txr");std::vector<UInt8> bytes;CHECK(source.saveToBinary(bytes));
    UInt32 name=0,shader=0;std::memcpy(&name,bytes.data()+24,4);std::memcpy(&shader,bytes.data()+28,4);
    bytes[40+name+shader+4+std::string("customCube").size()]=static_cast<UInt8>(ayt::resource::MaterialParamType::TextureCube);
    CHECK(ayt::io::File::atomicWriteUtf8(utf8(f.path),bytes.data(),bytes.size()));
    std::string error;CHECK(f.document->reload(&error));
    CHECK_MSG(f.document->saveEditableCopy("materials/CubeCopy.mat",&error),error.c_str());
    ayt::resource::Material copy;const auto saved=read(f.assets/"materials/CubeCopy.mat");
    CHECK(copy.loadFromBinary(saved.data(),saved.size()));
    CHECK(copy.getParameterType("customCube")==ayt::resource::MaterialParamType::TextureCube);
    CHECK(fs::weakly_canonical(fs::u8path(ayt::resource::resolveAssetPath(utf8(f.assets/"materials/CubeCopy.mat"),copy.getTexture("customCube"))))
        ==fs::weakly_canonical(f.assets/"cube.txr"));
}

TEST_CASE(session_copy_opens_a_new_document_and_picker_uses_source_and_imported_catalog)
{
    MaterialFixture f;ayt::app::EngineHostScope scope(ayt::app::defaultEngineHost());
    const auto imported=f.root/".ayeditor_cache/assets/textures/source-ui/Pick.txr";
    fs::create_directories(imported.parent_path());
    ayt::resource::Texture texture;texture.createSolidColor(2,2,10,100,220);std::vector<UInt8> bytes;
    CHECK(texture.saveToBinary(bytes));CHECK(ayt::io::File::atomicWriteUtf8(utf8(imported),bytes.data(),bytes.size()));
    ayt::ui::MockRenderer backend;EditorSession session;EditorSessionDesc desc;
    desc.uiBackend=&backend;desc.layoutPath=std::string(AY_EDITOR_TEST_SOURCE_DIR)+"/ui/editor_shell.ui.json";
    desc.projectRoot=utf8(f.root);CHECK(session.initialize(desc));session.setClientSize(1500,1000);CHECK(session.rescanAssetsNow());
    const auto* record=session.assetDatabase().findByLogicalPath("Assets/材质.mat");CHECK(record);
    if(!record){session.shutdown();return;}CHECK(session.openAsset(record->id));session.ui().layout();
    auto original=std::dynamic_pointer_cast<EditorMaterialDocument>(session.workspace().documents().active()->document);
    CHECK(original);if(!original){session.shutdown();return;}
    auto* roughness=dynamic_cast<ayt::ui::TextInput*>(findVisible(session.ui().root(),"material_roughness"));
    CHECK(roughness);if(!roughness){session.shutdown();return;}roughness->setText(L"0.42");
    const auto originalDisk=read(f.path);CHECK(clickMaterialWidget(session,findVisible(session.ui().root(),"material_create_copy")));
    auto* dialog=dynamic_cast<ayt::ui::ModalDialog*>(find(session.ui().getOverlayRoot(),"material_copy_dialog"));
    CHECK(dialog);if(!dialog){session.shutdown();return;}
    auto* path=dynamic_cast<ayt::ui::TextInput*>(find(dialog,"material_copy_path"));CHECK(path);
    if(!path){session.shutdown();return;}path->setText(L"materials/UIEditable.mat");
    CHECK(clickMaterialWidget(session,materialAccept(dialog)));CHECK_FALSE(dialog->isOpen());
    session.update(.25f);session.ui().layout();
    auto copied=std::dynamic_pointer_cast<EditorMaterialDocument>(session.workspace().documents().active()->document);
    CHECK(copied&&copied!=original);if(!copied||copied==original){session.shutdown();return;}
    CHECK(fs::u8path(copied->path()).filename()=="UIEditable.mat");CHECK_FALSE(copied->readOnly());
    CHECK_FLOAT_EQ(copied->settings().roughness,.42f,1e-6f);CHECK(original->isDirty());CHECK(read(f.path)==originalDisk);
    session.ui().layout();
    auto* browse=findVisible(session.ui().root(),"material_browse_baseColorTexture");CHECK(browse);
    CHECK(clickMaterialWidget(session,browse));
    auto* picker=dynamic_cast<ayt::ui::ModalDialog*>(find(session.ui().getOverlayRoot(),"material_texture_dialog"));
    CHECK(picker);if(!picker){session.shutdown();return;}
    auto* search=dynamic_cast<ayt::ui::TextInput*>(find(picker,"material_texture_search"));
    auto* choices=dynamic_cast<ayt::ui::ListView*>(find(picker,"material_texture_choices"));
    CHECK(search&&choices);if(!search||!choices){session.shutdown();return;}
    search->setText(L"Imported/");CHECK(choices->getItemCount()==2u);choices->setSelectedIndex(1);
    CHECK(clickMaterialWidget(session,materialAccept(picker)));CHECK_FALSE(picker->isOpen());
    CHECK(copied->settings().textures[0]=="textures/source-ui/Pick.txr");CHECK(original->settings().textures[0].empty());
    CHECK(copied->undo());CHECK(copied->settings().textures[0].empty());CHECK(copied->redo());
    std::string error;CHECK(copied->save(&error));CHECK(read(imported)==bytes);CHECK(read(f.path)==originalDisk);
    session.shutdown();
}
TEST_CASE(actual_material_asset_drag_assigns_one_existing_mesh_with_undo_and_scene_roundtrip)
{
    MaterialFixture f;std::string error;CHECK(f.document->saveEditableCopy("materials/Assigned.mat",&error));
    ayt::app::EngineHostScope scope(ayt::app::defaultEngineHost());ayt::ui::MockRenderer backend;
    EditorSession session;EditorSessionDesc desc;desc.uiBackend=&backend;
    desc.layoutPath=std::string(AY_EDITOR_TEST_SOURCE_DIR)+"/ui/editor_shell.ui.json";desc.projectRoot=utf8(f.root);
    CHECK(session.initialize(desc));session.setClientSize(1500,1000);CHECK(session.rescanAssetsNow());
    auto& world=session.document()->scene().world();auto* target=world.createSpatialEntity();
    auto* mesh=target->createComponent<ayt::entity::MeshComponent>();const auto identity=target->persistentId();
    const auto count=world.getAllEntities().size();
    auto* mode=dynamic_cast<ayt::ui::ComboBox*>(session.ui().findById("cmb_assets_view"));
    auto* list=dynamic_cast<ayt::ui::TileView*>(session.ui().findById("list_assets"));
    auto* viewport=session.ui().findById("panel_viewport");CHECK(mode&&list&&viewport);
    if(!mode||!list||!viewport){session.shutdown();return;}
    mode->setSelectedIndexAndNotify(1);session.update(0);session.ui().layout();
    int index=-1;for(std::size_t i=0;i<list->getItemCount();++i)
        if(list->getItem(i).find(L"materials/Assigned.mat")!=std::wstring::npos){index=static_cast<int>(i);break;}
    CHECK(index>=0);if(index<0){session.shutdown();return;}
    list->scrollToIndex(index);session.ui().layout();auto* cell=list->cellForLogicalIndex(index);CHECK(cell);
    if(!cell){session.shutdown();return;}
    const auto thumb=cell->getThumbnailRect(),bounds=viewport->getWorldBounds();
    const auto client=list->getClientRect();
    const auto top=std::max(thumb.minY,client.minY),bottom=std::min(thumb.maxY,client.maxY);
    CHECK(bottom>top);if(bottom<=top){session.shutdown();return;}
    const auto press=session.ui().logicalToPhysical({(thumb.minX+thumb.maxX)*.5f,(top+bottom)*.5f});
    const auto drop=session.ui().logicalToPhysical({(bounds.minX+bounds.maxX)*.5f,(bounds.minY+bounds.maxY)*.5f});
    session.onMouseMove(press.x,press.y);CHECK(session.onMouseButtonDown(press.x,press.y,0));
    CHECK(session.onMouseMove(drop.x,drop.y));CHECK(session.ui().isDragging());
    CHECK(session.onMouseButtonUp(drop.x,drop.y,0));CHECK_FALSE(session.ui().isDragging());
    CHECK(world.getAllEntities().size()==count);CHECK(session.selectedEntityId()==target->getId());
    CHECK(mesh->materialPath=="materials/Assigned.mat");CHECK(session.document()->canUndo());
    CHECK(session.document()->undo());CHECK(mesh->materialPath.empty());
    CHECK(session.document()->redo());CHECK(mesh->materialPath=="materials/Assigned.mat");
    const auto scene=f.assets/"scenes/Assignment.scn";
    CHECK_MSG(session.document()->saveAs(utf8(scene),&error),error.c_str());
    CHECK_MSG(session.document()->open(utf8(scene),&error),error.c_str());
    auto* reopened=world.findEntityByPersistentId(identity);CHECK(reopened);
    if(reopened){auto* component=reopened->getComponent<ayt::entity::MeshComponent>();CHECK(component);
        if(component)CHECK(component->materialPath=="materials/Assigned.mat");}
    session.shutdown();
}
TEST_SUITE_END
