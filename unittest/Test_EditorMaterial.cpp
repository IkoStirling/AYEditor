#include <AYEditor/EditorMaterialDocument.h>
#include <AYEditor/EditorProjectDescriptor.h>
#include <AYEditor/EditorWorkspace.h>
#include <AYEditor/EditorSession.h>
#include <AYEditor/EditorProjectAssetFactory.h>
#include <AYResource/assetsImpl/Material.h>
#include <AYResource/Loader/MaterialFile.h>
#include <AYIO/File.h>
#include <AYUI/TextInput.h>
#include <AYUI/UIKeyCode.h>
#include <AYUI/Button.h>
#include <AYUI/ComboBox.h>
#include <AYUI/MockRenderer.h>
#include <AYApplication/IEngineHost.h>
#include <AYTest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>

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
TEST_SUITE_END
