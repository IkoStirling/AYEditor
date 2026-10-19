#ifdef AYEDITOR_HAS_SEQUENCE
#include <AYEditor/EditorAssetDatabase.h>
#include <AYEditor/EditorProjectAssetFactory.h>
#include <AYEditor/EditorSequenceDocument.h>
#include <AYEditor/EditorDslDocument.h>
#include <AYEditor/EditorDslExtension.h>
#include <AYEditor/EditorWorkspace.h>
#include <AYEditor/EditorSession.h>
#include <AYApplication.h>
#include <AYApplication/IEngineHost.h>
#include <AYUI/MockRenderer.h>
#include <AYUI/DockCard.h>
#include <AYEntity.h>
#include <AYEntity/EntityModule.h>
#include <AYEntity/components/SkeletonComponent.h>
#include <AYIO/File.h>
#include <AYResource.h>
#include <AYResource/assetsImpl/Animation.h>
#include <AYScene.h>
#include <AYSequence/SequenceAssetIO.h>
#include <AYSequence/LightShowAssetIO.h>
#include <AYTest.h>
#include <AYUI/Authoring/DopeSheet.h>
#include <AYUI/Button.h>
#include <AYUI/TextArea.h>
#include <AYUI/TextLabel.h>
#ifdef AYEDITOR_HAS_LIGHTSHOW_RENDERER
#include <AYRenderer.h>
#include <AYRenderer/RendererSubSystem.h>
#include <AYRenderer/VolumetricDensity.h>
#include <cstdlib>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif
#endif
#include <AYUI/TextInput.h>
#include <chrono>
#include <cmath>
#include <filesystem>
using namespace ayt::editor;
using namespace ayt::sequence;
namespace {
ayt::ui::Widget *find(ayt::ui::Widget *root, const std::string &id) {
    if (root->getId() == id)
        return root;
    for (auto *child : root->getChildren())
        if (auto *result = find(child, id))
            return result;
    return nullptr;
}
struct Fixture {
    std::filesystem::path root;
    std::shared_ptr<EditorSequenceDocument> document = std::make_shared<EditorSequenceDocument>();
    Fixture(bool withScene = false) {
        root = ayt::test::testTmpDir() /
               ("seq-editor-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!std::filesystem::create_directory(root))
            throw std::runtime_error("Fresh test root unavailable");
        std::filesystem::create_directories(root / "Assets" / "worlds");
        Sequence s;
        s.id = "editor-sequence";
        s.duration = 4;
        s.bindings = {{"actor", withScene ? "Actor" : ""}};
        s.transforms = {{"position",
                         "actor",
                         Channel::Position,
                         {{"key0", 0, {0, 0, 0, 0}}, {"key1", 4, {8, 0, 0, 0}}}}};
        s.eventTracks = {{"events", {{"event", 1, "marker", "payload"}}}};
        if (withScene) {
            s.scenePath = "worlds/test.scn";
            ayt::entity::registerEntityCoreComponents(ayt::entity::ComponentRegistry::instance());
            ayt::scene::Scene scene(ayt::scene::SceneMode::Edit, "saved");
            auto *e = scene.world().createEntity();
            e->setName("Actor");
            e->addComponent<ayt::entity::Transform>()->setPosition(20, 0, 0);
            if (!scene.save((root / "Assets" / s.scenePath).string()))
                throw std::runtime_error("Scene save failed");
        }
        auto path = (root / "Assets" / "test.seq").string();
        if (!saveSequence(path, s))
            throw std::runtime_error("Sequence save failed");
        std::string error;
        if (!document->initialize({path}, error))
            throw std::runtime_error(error);
        document->configureProjectRoot(root.string());
    }
    ~Fixture() {
        document->closePreview();
        auto &r = ayt::resource::ResourceManager::instance();
        for (auto name : {"rig.ayskel", "walk.ayanm", "missing.ayanm"})
            r.unloadResource((root / "Assets" / name).string());
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};
struct Host : IEditorHostServices {
    EditorWorkspace value;
    std::string root;
    EditorWorkspace &workspace() noexcept override { return value; }
    const std::string &projectRoot() const noexcept override { return root; }
    void requestRepaint() override {}
    void setStatusText(const std::wstring &) override {}
};
void click(ayt::ui::Widget *root, const char *id) {
    auto *b = dynamic_cast<ayt::ui::Button *>(find(root, id));
    if (!b)
        throw std::runtime_error(id);
    b->setSize({100, 28});
    const auto bounds = b->getWorldBounds();
    b->onMouseButtonDown({{bounds.minX + 2, bounds.minY + 2}, 0});
    b->onMouseButtonUp({{bounds.minX + 2, bounds.minY + 2}, 0});
}
} // namespace
TEST_SUITE(AYEditor_Sequence)
TEST_CASE(edit_save_reopen_and_undo_to_saved_marker) {
    Fixture f;
    CHECK_FALSE(f.document->isDirty());
    CHECK_TRUE(f.document->edit(
        "move", [](Sequence &s) { return updateSequenceKey(s, "key0", .5, {2, 3, 4, 0}); }));
    CHECK_TRUE(f.document->isDirty());
    CHECK_TRUE(f.document->save());
    CHECK_FALSE(f.document->isDirty());
    EditorSequenceDocument reopened;
    std::string error;
    CHECK_TRUE(reopened.initialize({f.document->path()}, error));
    CHECK(reopened.model().snapshot()->data() == f.document->model().snapshot()->data());
    CHECK_TRUE(f.document->executeCommand("edit.undo"));
    CHECK_TRUE(f.document->isDirty());
    CHECK_TRUE(f.document->executeCommand("edit.redo"));
    CHECK_FALSE(f.document->isDirty());
}
TEST_CASE(continuous_gesture_one_history_record_and_cancel_restores) {
    Fixture f;
    auto before = f.document->model().snapshot();
    CHECK_TRUE(f.document->beginEdit("drag"));
    CHECK_FALSE(f.document->beginEdit("nested"));
    for (int i = 1; i <= 20; ++i)
        CHECK_TRUE(f.document->edit("move", [&](Sequence &s) {
            return updateSequenceKey(s, "key0", i * .01, {0, 0, 0, 0});
        }));
    CHECK(f.document->historySize() == 0);
    CHECK_FALSE(f.document->save());
    CHECK_TRUE(f.document->endEdit(false));
    CHECK(f.document->historySize() == 1);
    CHECK_TRUE(f.document->executeCommand("edit.undo"));
    CHECK(f.document->model().snapshot()->data() == before->data());
    CHECK_FALSE(f.document->isDirty());
    CHECK_TRUE(f.document->beginEdit("cancel"));
    CHECK_TRUE(f.document->edit(
        "move", [](Sequence &s) { return updateSequenceKey(s, "key0", 1, {7, 0, 0, 0}); }));
    CHECK_TRUE(f.document->endEdit(true));
    CHECK(f.document->model().snapshot()->data() == before->data());
    CHECK_FALSE(f.document->isDirty());
}
TEST_CASE(rejected_edit_and_failed_reload_preserve_model_and_history) {
    Fixture f;
    auto before = f.document->model().snapshot();
    CHECK_FALSE(f.document->edit("bad", [](Sequence &s) {
        s.duration = -1;
        return true;
    }));
    CHECK(before == f.document->model().snapshot());
    CHECK_FALSE(f.document->isDirty());
    CHECK_TRUE(ayt::io::File::atomicWrite(f.document->path(), "bad", 3));
    CHECK_FALSE(f.document->reload());
    CHECK(before == f.document->model().snapshot());
    CHECK(f.document->historySize() == 0);
}
TEST_CASE(playhead_refresh_retains_owned_snapshot_and_does_not_dirty) {
    Fixture f;
    auto source = makeEditorSequenceSource(f.document);
    auto timeline = source->timelineSnapshot();
    auto curve = source->curveTrack("position");
    auto revision = f.document->revision();
    CHECK_TRUE(f.document->play());
    for (int i = 0; i < 30; ++i)
        f.document->tick(.02);
    CHECK(f.document->position() > .59);
    CHECK(f.document->revision() == revision);
    CHECK_FALSE(f.document->isDirty());
    CHECK(timeline == source->timelineSnapshot());
    CHECK(curve == source->curveTrack("position"));
    f.document->stop();
    CHECK(f.document->position() == 0);
}
TEST_CASE(source_atomic_group_move_and_normalized_quaternion_numeric_input) {
    Fixture f;
    auto source = makeEditorSequenceSource(f.document);
    std::vector<std::string> ids{"key0", "event"};
    CHECK_TRUE(source->transformKeys(ids, .5, 0, 0));
    CHECK(f.document->historySize() == 1);
    auto before = f.document->model().snapshot();
    CHECK_FALSE(source->transformKeys(ids, 10, 0, 0));
    CHECK(f.document->model().snapshot() == before);
    CHECK_TRUE(f.document->edit("rotation", [](Sequence &s) {
        s.transforms.push_back(
            {"rotation", "actor", Channel::Rotation, {{"quat", 0, {0, 0, 0, 1}}}});
        return true;
    }));
    std::string id = "quat";
    CHECK_TRUE(source->updateKey(id, 0, {0, 2, 0, 0}));
    CHECK(f.document->model().snapshot()->data().transforms.back().keys[0].value[1] == 1);
    CHECK_FALSE(source->updateKey(id, 0, {0, 0, 0, 0}));
}
TEST_CASE(old_curve_snapshot_survives_content_revision_and_undo) {
    Fixture f;
    auto source = makeEditorSequenceSource(f.document);
    auto old = source->curveTrack("position");
    CHECK(old->sample(0, 2) == 4);
    CHECK_TRUE(f.document->edit("value", [](Sequence &s) {
        s.transforms[0].keys[1].value[0] = 16;
        return true;
    }));
    auto next = source->curveTrack("position");
    CHECK(old != next);
    CHECK(old->sample(0, 2) == 4);
    CHECK(next->sample(0, 2) == 8);
    CHECK_TRUE(f.document->executeCommand("edit.undo"));
    CHECK(old->sample(0, 2) == 4);
}
TEST_CASE(asset_factory_and_double_click_descriptor_route_sequence) {
    Fixture f;
    auto result = createEditorProjectAsset(f.root.string(), EditorAssetType::Sequence);
    CHECK_TRUE(result.success);
    CHECK(classifyEditorAssetPath(result.absolutePath) == EditorAssetType::Sequence);
    EditorExtensionRegistry registry;
    CHECK_TRUE(registerEditorSequenceExtension(registry));
    EditorOpenRequest request{result.absolutePath};
    auto *d = registry.resolve(request);
    CHECK(d != nullptr);
    if (!d)
        throw std::runtime_error("No sequence descriptor");
    CHECK(d->id == kEditorSequenceExtensionId);
    std::string error;
    CHECK(d->createDocument(request, error) != nullptr);
}
TEST_CASE(light_show_asset_opens_in_editor_and_rejects_invalid_save) {
    Fixture f;
    const auto first=createEditorProjectAsset(f.root.string(),EditorAssetType::LightShow);
    const auto second=createEditorProjectAsset(f.root.string(),EditorAssetType::LightShow);
    CHECK(first.success);CHECK(second.success);CHECK(first.absolutePath!=second.absolutePath);
    CHECK(classifyEditorAssetPath(first.absolutePath)==EditorAssetType::LightShow);
    LightShow loaded;CHECK(loadLightShow(first.absolutePath,loaded));CHECK(loaded.fixtures.size()==1);
    EditorExtensionRegistry registry;CHECK(registerEditorDslExtension(registry));
    EditorOpenRequest request{first.absolutePath};
    const auto* descriptor=registry.resolve(request);CHECK(descriptor!=nullptr);
    CHECK(descriptor->id==kEditorDslExtensionId);
    std::string error;
    auto opened=descriptor->createDocument(request,error);
    auto document=std::dynamic_pointer_cast<EditorDslDocument>(opened);
    CHECK(document!=nullptr);CHECK(document->language()==EditorDslLanguage::LightShowJson);
    CHECK(document->compile().success);
    const auto before=ayt::io::File::readAllText(first.absolutePath);
    CHECK(before.find("\"version\": 8")!=std::string::npos);
    CHECK(before.find("\"goboTexturePath\"")!=std::string::npos);
    CHECK(before.find("\"shadowSoftness\"")!=std::string::npos);
    document->setSourceUtf8("{}");CHECK(!document->compile().success);
    CHECK(!document->save(&error));CHECK(!error.empty());
    CHECK(ayt::io::File::readAllText(first.absolutePath)==before);
    loaded.name="Edited Light Show";
    std::string edited;CHECK(encodeLightShow(loaded,edited));
    document->setSourceUtf8(edited);CHECK(document->save(&error));CHECK(!document->isDirty());
    LightShow saved;CHECK(loadLightShow(first.absolutePath,saved));
    CHECK(saved.name=="Edited Light Show");
}
TEST_CASE(isolated_scene_preview_never_changes_author_scene_or_active_world) {
    Fixture f(true);
    ayt::scene::Scene author(ayt::scene::SceneMode::Edit, "author");
    CHECK_TRUE(author.load((f.root / "Assets" / "worlds" / "test.scn").string()));
    auto *actor = author.world().getAllEntities().front();
    auto *active = ayt::entity::World::activeWorld();
    CHECK_TRUE(f.document->seek(2));
    CHECK(f.document->previewScene() != nullptr);
    CHECK(&f.document->previewScene()->world() != &author.world());
    CHECK(f.document->previewScene()
              ->world()
              .getAllEntities()
              .front()
              ->getComponent<ayt::entity::Transform>()
              ->position.x == 4);
    CHECK(actor->getComponent<ayt::entity::Transform>()->position.x == 20);
    CHECK(ayt::entity::World::activeWorld() == active);
    CHECK_FALSE(f.document->isDirty());
    f.document->stop();
    CHECK(f.document->previewScene()
              ->world()
              .getAllEntities()
              .front()
              ->getComponent<ayt::entity::Transform>()
              ->position.x == 20);
    f.document->closePreview();
    CHECK(f.document->previewScene() == nullptr);
    CHECK(actor->getComponent<ayt::entity::Transform>()->position.x == 20);
}
TEST_CASE(missing_and_duplicate_entity_locators_are_diagnostics_not_guessing) {
    Fixture f(true);
    CHECK_TRUE(f.document->edit("locator", [](Sequence &s) {
        s.bindings[0].entityName = "Missing";
        return true;
    }));
    CHECK_FALSE(f.document->seek(1));
    CHECK(f.document->diagnostic().find("Missing") != std::string::npos);
    CHECK(f.document->previewScene() == nullptr);
    CHECK_TRUE(f.document->executeCommand("edit.undo"));
    ayt::scene::Scene scene(ayt::scene::SceneMode::Edit, "duplicate");
    for (int i = 0; i < 2; ++i) {
        auto *e = scene.world().createEntity();
        e->setName("Actor");
        e->addComponent<ayt::entity::Transform>();
    }
    CHECK_TRUE(scene.save((f.root / "Assets" / "worlds" / "test.scn").string()));
    CHECK_FALSE(f.document->seek(1));
    CHECK(f.document->diagnostic().find("Ambiguous") != std::string::npos);
}
TEST_CASE(content_edit_revokes_old_preview_and_undo_rebuilds_explicitly) {
    Fixture f(true);
    CHECK_TRUE(f.document->seek(2));
    CHECK_TRUE(f.document->edit("value", [](Sequence &s) {
        s.transforms[0].keys[1].value[0] = 16;
        return true;
    }));
    CHECK(f.document->previewScene() == nullptr);
    CHECK_TRUE(f.document->seek(2));
    CHECK(f.document->previewScene()
              ->world()
              .getAllEntities()[0]
              ->getComponent<ayt::entity::Transform>()
              ->position.x == 8);
    CHECK_TRUE(f.document->executeCommand("edit.undo"));
    CHECK(f.document->previewScene() == nullptr);
    CHECK_TRUE(f.document->seek(2));
    CHECK(f.document->previewScene()
              ->world()
              .getAllEntities()[0]
              ->getComponent<ayt::entity::Transform>()
              ->position.x == 4);
}
TEST_CASE(page_close_cancels_owned_gesture_and_discards_preview) {
    Fixture f(true);
    Host host;
    host.root = f.root.string();
    auto descriptor = makeEditorSequenceDescriptor();
    auto view = descriptor.createView(f.document, host);
    CHECK(view != nullptr);
    CHECK_TRUE(f.document->seek(1));
    auto *sheet =
        dynamic_cast<ayt::ui::authoring::DopeSheet *>(find(view->rootWidget(), "dope_sheet"));
    CHECK(sheet != nullptr);
    if (!sheet)
        throw std::runtime_error("Missing sheet");
    sheet->setSize({640, 100});
    const auto b = sheet->getWorldBounds();
    CHECK_TRUE(sheet->onMouseButtonDown({{b.minX + 168, b.minY + 34}, 0}));
    CHECK_TRUE(sheet->onMouseMove({{b.minX + 250, b.minY + 34}, 0}));
    CHECK_TRUE(f.document->gestureActive());
    view->prepareForUiShutdown();
    CHECK_FALSE(f.document->gestureActive());
    CHECK_FALSE(f.document->isDirty());
    CHECK(f.document->previewScene() == nullptr);
    view.reset();
}
TEST_CASE(section_widget_drag_and_cancel_use_one_record_and_restore_trim_offset) {
    Fixture f;
    CHECK_TRUE(f.document->edit("clips", [](Sequence &s) {
        s.animations = {{"clips", "actor", {{"section", "walk.ayanm", 0, 2, 0, 1, 4}}}};
        return true;
    }));
    CHECK_TRUE(f.document->save());
    Host host;
    host.root = f.root.string();
    auto view = makeEditorSequenceDescriptor().createView(f.document, host);
    auto *widget = find(view->rootWidget(), "sequence_sections");
    CHECK(widget != nullptr);
    widget->setSize({640, 120});
    auto b = widget->getWorldBounds();
    auto history = f.document->historySize();
    CHECK_TRUE(widget->onMouseButtonDown({{b.minX + 220, b.minY + 34}, 0}));
    CHECK_TRUE(widget->onMouseMove({{b.minX + 279, b.minY + 34}, 0}));
    CHECK_TRUE(widget->onMouseButtonUp({{b.minX + 279, b.minY + 34}, 0}));
    CHECK(f.document->historySize() == history + 1);
    CHECK(std::abs(f.document->model().snapshot()->data().animations[0].sections[0].start - .5) <
          1e-9);
    CHECK_TRUE(f.document->executeCommand("edit.undo"));
    CHECK_FALSE(f.document->isDirty());
    CHECK_TRUE(widget->onMouseButtonDown({{b.minX + 168, b.minY + 34}, 0}));
    CHECK_TRUE(widget->onMouseMove({{b.minX + 227, b.minY + 34}, 0}));
    widget->onCaptureCancelled();
    CHECK(f.document->model().snapshot()->data().animations[0].sections[0].sourceOffset == 0);
    CHECK_FALSE(f.document->isDirty());
}
TEST_CASE(page_buttons_edit_binding_track_key_and_persist_no_second_history) {
    Fixture f;
    Host host;
    host.root = f.root.string();
    auto view = makeEditorSequenceDescriptor().createView(f.document, host);
    auto *root = view->rootWidget();
    auto *input = dynamic_cast<ayt::ui::TextInput *>(find(root, "sequence_entity_name"));
    input->setText(L"Actor");
    click(root, "sequence_set_target");
    CHECK(f.document->model().snapshot()->data().bindings[0].entityName == "Actor");
    click(root, "sequence_add_scale");
    CHECK(f.document->model().snapshot()->data().transforms.size() == 2);
    CHECK(f.document->historySize() == 2);
    click(root, "command_edit.undo");
    CHECK(f.document->model().snapshot()->data().transforms.size() == 1);
}
TEST_CASE(real_skeletal_preview_loads_owned_clips_and_reports_missing_source) {
    Fixture f(true);
    CHECK_TRUE(ayt::resource::initializeLoaders());
    auto &registry = ayt::entity::ComponentRegistry::instance();
    CHECK(registry.find<ayt::entity::SkeletonComponent>() != nullptr);
    ayt::resource::Skeleton skeleton;
    skeleton.setBoneCount(1);
    ayt::resource::Bone bone;
    bone.name = "root";
    bone.parentIndex = -1;
    bone.localPosition = {0, 0, 0};
    bone.localRotation = {0, 0, 0, 1};
    bone.localScale = {1, 1, 1};
    bone.inverseBindMatrix = ayt::math::Float4x4::identity();
    skeleton.setBone(0, bone);
    std::vector<ayt::math::UInt8> bytes;
    CHECK_TRUE(skeleton.saveToBinary(bytes));
    CHECK_TRUE(ayt::io::File::atomicWrite((f.root / "Assets" / "rig.ayskel").string(), bytes.data(),
                                          bytes.size()));
    ayt::resource::Animation clip;
    clip.setDuration(4);
    clip.setTicksPerSecond(1);
    ayt::resource::AnimTrack track;
    track.nodeName = "root";
    track.property = "position";
    track.valueType = ayt::resource::AnimTrackType::Vector3;
    track.times = {0, 4};
    track.values = {0, 0, 0, 8, 0, 0};
    clip.addTrack(track);
    CHECK_TRUE(clip.saveToBinary(bytes));
    CHECK_TRUE(ayt::io::File::atomicWrite((f.root / "Assets" / "walk.ayanm").string(), bytes.data(),
                                          bytes.size()));
    ayt::scene::Scene scene(ayt::scene::SceneMode::Edit, "saved");
    auto *e = scene.world().createEntity();
    e->setName("Actor");
    e->addComponent<ayt::entity::Transform>();
    e->addComponent<ayt::entity::SkeletonComponent>()->skeletonPath = "rig.ayskel";
    CHECK_TRUE(scene.save((f.root / "Assets" / "worlds" / "test.scn").string()));
    CHECK_TRUE(f.document->edit("animation", [](Sequence &s) {
        s.animations = {{"clips", "actor", {{"section", "walk.ayanm", 0, 4, 0, 1, 4}}}};
        return true;
    }));
    CHECK_TRUE(f.document->save());
    CHECK_TRUE(f.document->seek(2));
    if (!f.document->previewScene())
        throw std::runtime_error(f.document->diagnostic());
    auto *c = f.document->previewScene()
                  ->world()
                  .getAllEntities()[0]
                  ->getComponent<ayt::entity::SkeletonComponent>();
    CHECK(c->skinMatrices[0].row[0].w == 4);
    CHECK(c->externalPoseOwner != nullptr);
    CHECK_FALSE(f.document->isDirty());
    f.document->stop();
    CHECK(c->skinMatrices[0].row[0].w == 0);
    CHECK(c->externalPoseOwner == nullptr);
    CHECK_TRUE(f.document->edit("missing", [](Sequence &s) {
        s.animations[0].sections[0].source = "missing.ayanm";
        return true;
    }));
    CHECK_FALSE(f.document->seek(1));
    CHECK_FALSE(f.document->diagnostic().empty());
    CHECK(f.document->previewScene() == nullptr);
}

#ifdef AYEDITOR_HAS_LIGHTSHOW_RENDERER


TEST_CASE(light_show_session_opens_transparent_composite_hole_without_collapsing_layout) {
    Fixture f;const auto asset=createEditorProjectAsset(f.root.string(),EditorAssetType::LightShow);CHECK(asset.success);

    struct HoleProbe:ayt::ui::MockRenderer {
        bool probing=false;unsigned covers=0;ayt::math::FVector2 point;
        using ayt::ui::MockRenderer::drawRect;
        void record(const ayt::math::FRectangle& b,float alpha){const auto clip=debugClip();if(probing&&alpha>.99f&&b.contains(point)&&(!clip.valid||clip.rectangle.contains(point)))++covers;}
        void drawRect(const ayt::math::FRectangle& b,const ayt::math::FVector4& c)override{record(b,c.w);MockRenderer::drawRect(b,c);}
        void drawCard(const ayt::math::FRectangle& b,const CardStyle& style)override{record(b,style.fillColor.w);MockRenderer::drawCard(b,style);}
    };
    ayt::app::EngineHostScope scope(ayt::app::defaultEngineHost());HoleProbe renderer;EditorSessionDesc desc;desc.uiBackend=&renderer;desc.projectRoot=f.root.string();desc.layoutPath=std::string(AY_EDITOR_TEST_SOURCE_DIR)+"/ui/editor_shell.ui.json";
    EditorSession session;CHECK(session.initialize(desc));session.setClientSize(1500,1000);CHECK(session.rescanAssetsNow());
    EditorAssetId id=0;for(const auto& record:session.assetDatabase().records())if(record.type==EditorAssetType::LightShow)id=record.id;CHECK(id!=0);CHECK(session.openDslAsset(id));session.update(0);session.ui().layout();
    auto* viewport=session.ui().findById("lightshow_viewport");CHECK(viewport!=nullptr);if(!viewport){session.shutdown();return;}
    ayt::ui::DockCard* card=nullptr;for(auto* ancestor=viewport->getParent();ancestor;ancestor=ancestor->getParent())if(auto* c=dynamic_cast<ayt::ui::DockCard*>(ancestor)){card=c;break;}
    CHECK(card!=nullptr);if(!card){session.shutdown();return;}CHECK(card->isVisible());CHECK(card->isBackgroundEnabled());
    const auto before=viewport->getWorldBounds();CHECK(before.maxX-before.minX>=32);CHECK(before.maxY-before.minY>=32);
    renderer.point={(before.minX+before.maxX)*.5f,(before.minY+before.maxY)*.5f};renderer.probing=true;session.populateFrame(true);CHECK(renderer.covers==0);CHECK_FALSE(card->isBackgroundEnabled());CHECK(viewport->isVisible());const auto during=viewport->getWorldBounds();CHECK_FLOAT_EQ(before.minX,during.minX,1e-5f);CHECK_FLOAT_EQ(before.maxX,during.maxX,1e-5f);CHECK_FLOAT_EQ(before.minY,during.minY,1e-5f);CHECK_FLOAT_EQ(before.maxY,during.maxY,1e-5f);session.flushFrame();CHECK(card->isBackgroundEnabled());
    session.shutdown();
}
TEST_CASE(light_show_main_deferred_native_preview_restores_scene_on_hide_source_and_close) {
#if defined(_WIN32)
    const char* selected=std::getenv("AY_EDITOR_LIGHTSHOW_GPU");
    if(!selected){CHECK_TRUE(true);return;}
    const std::string backend=selected;CHECK(backend=="d3d11"||backend=="d3d12");if(backend!="d3d11"&&backend!="d3d12")return;
    Fixture f;const auto file=f.root/"Assets"/"native.lshow";auto data=makeLightShowExample(8,1000);
    ShowFogState authored;authored.local=true;authored.center={0,5,0};authored.halfExtent={20,10,20};data.fog.keys.push_back({0,authored});data.fog.grid.width=4;data.fog.grid.height=4;data.fog.grid.depth=4;data.fog.grid.values.assign(64,255);CHECK(saveLightShow(file.string(),data));
    HWND window=CreateWindowExW(0,L"STATIC",L"Aliyat lighting Editor native acceptance",WS_OVERLAPPEDWINDOW,0,0,640,480,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);CHECK(window!=nullptr);if(!window)return;
    using ayt::render::RendererSubSystem;RendererSubSystem::registerSubSystem();auto* sub=RendererSubSystem::findRegistered();CHECK(sub!=nullptr);if(!sub){DestroyWindow(window);return;}
    RendererSubSystem::setWindowProvider({});RendererSubSystem::setBootstrapBackend(backend=="d3d12"?ayt::render::Backend::Direct3D12:ayt::render::Backend::Direct3D11);RendererSubSystem::setBootstrapWindow(window,640,480);RendererSubSystem::setBootstrapViewport(8,12,600,420);
    CHECK(sub->initialize());if(!sub->isReady()){DestroyWindow(window);return;}
    auto& renderer=sub->renderer();renderer.setAmbientStrength(.31f);ayt::render::VolumetricSettings hostFog;hostFog.enabled=true;hostFog.density=.067f;renderer.setVolumetricSettings(hostFog);sub->setCameraLookAt({2,3,-4},{0,0,0},{0,1,0});
    std::string error;auto profile=ayt::render::VolumetricDensityField::create(2,2,2,std::vector<uint8_t>(8,200),&error);CHECK(profile!=nullptr);auto hostTexture=ayt::render::createVolumetricDensityTexture(renderer,*profile);CHECK(hostTexture.isValid());CHECK(renderer.setVolumetricDensityField({profile,hostTexture},&error));const auto hostTextureId=hostTexture.id;
    ayt::render::PerformanceLight hostLight;hostLight.id=900;hostLight.light.type=ayt::render::LightType::Spot;hostLight.light.range=20;hostLight.light.coneCosInner=.98f;hostLight.light.coneCosOuter=.95f;CHECK(renderer.setPerformanceLights(std::span<const ayt::render::PerformanceLight>(&hostLight,1)));
    int worldExtractions=0;sub->setSceneBuilder([&](ayt::render::RenderScene&){++worldExtractions;});
    auto document=std::make_shared<EditorDslDocument>();CHECK(document->open(file.string(),file.string(),&error));Host host;auto view=makeEditorDslDescriptor().createView(document,host);auto* root=view->rootWidget();root->setSize({1000,700});view->onActivated();
    click(root,"lightshow_gpu");click(root,"lightshow_readback");click(root,"lightshow_froxel");
    for(int i=0;i<70;++i){view->tick(1.f/60);sub->update(0);sub->renderCompositeFrame(true,nullptr,[](bool,ayt::render::CompositeUiPhase){});}
    CHECK(sub->hasMainPresentation());CHECK(worldExtractions==0);CHECK(renderer.pipelineDesc().path==ayt::render::RenderPath::Deferred);
    const auto stats=renderer.performanceLightStats();CHECK(stats.sourceLights>=8);CHECK(stats.surfaceDraws>0);CHECK(stats.volumeDraws>0);CHECK(renderer.volumetricDensityField().texture.id!=hostTextureId);
    const auto timings=renderer.getFrameStats();CHECK(timings.frameCount>=60);CHECK(timings.backendDrawCalls>0);CHECK(timings.volumetric.froxelCells>0);CHECK(timings.volumetric.historyUsed);
    auto* hud=dynamic_cast<ayt::ui::TextLabel*>(find(root,"lightshow_budget_hud"));CHECK(hud!=nullptr);CHECK(hud->getText().find(L"GPU sample#")!=std::wstring::npos);

    const auto beforeQuality=document->sourceUtf8();
    for(const auto& [id,value]:std::initializer_list<std::pair<const char*,const wchar_t*>>{{"lightshow_surface_budget",L"2"},{"lightshow_volume_budget",L"1"},{"lightshow_shadow_budget",L"0"},{"lightshow_point_budget",L"0"}}){auto* field=dynamic_cast<ayt::ui::TextInput*>(find(root,id));CHECK(field!=nullptr);field->setText(value);}click(root,"lightshow_quality_apply");view->tick(0);sub->renderCompositeFrame(true,nullptr,[](bool,ayt::render::CompositeUiPhase){});
    CHECK(renderer.performanceLightStats().surfaceDraws<=2);CHECK(renderer.performanceLightStats().volumeDraws<=1);CHECK(renderer.performanceLightStats().shadowLights==0);CHECK(hud->getText().find(L"0 visible / 1000")!=std::wstring::npos);CHECK(document->sourceUtf8()==beforeQuality);
    root->setVisible(false);view->tick(0);CHECK_FALSE(sub->hasMainPresentation());CHECK_FLOAT_EQ(renderer.ambientStrength(),.31f,1e-6f);CHECK_FLOAT_EQ(renderer.volumetricSettings().density,.067f,1e-6f);CHECK(renderer.volumetricDensityField().texture.id==hostTextureId);CHECK(renderer.performanceLightStats().sourceLights==1);CHECK(sub->viewportWidth()==600);CHECK(sub->viewportHeight()==420);
    sub->update(0);CHECK(worldExtractions==1);sub->renderCompositeFrame(true,nullptr,[](bool,ayt::render::CompositeUiPhase){});CHECK(renderer.volumetricDensityField().texture.id==hostTextureId);CHECK_FLOAT_EQ(renderer.mainCameraPosition().x,2.f,1e-6f);root->setVisible(true);view->tick(0);sub->renderCompositeFrame(true,nullptr,[](bool,ayt::render::CompositeUiPhase){});CHECK(sub->hasMainPresentation());
    click(root,"lightshow_source_toggle");CHECK_FALSE(sub->hasMainPresentation());CHECK(renderer.pipelineDesc().path==ayt::render::RenderPath::Forward);CHECK(renderer.volumetricDensityField().texture.id==hostTextureId);
    click(root,"lightshow_source_toggle");view->tick(0);sub->renderCompositeFrame(true,nullptr,[](bool,ayt::render::CompositeUiPhase){});view->onDeactivated();view->prepareForUiShutdown();view.reset();CHECK_FALSE(sub->hasMainPresentation());CHECK(renderer.volumetricDensityField().texture.id==hostTextureId);CHECK(renderer.performanceLightStats().sourceLights==1);
    // A new document can reuse released GPU caches on the same live device.
    auto reopened=makeEditorDslDescriptor().createView(document,host);reopened->onActivated();reopened->tick(0);sub->renderCompositeFrame(true,nullptr,[](bool,ayt::render::CompositeUiPhase){});CHECK(renderer.performanceLightStats().surfaceDraws>0);reopened->prepareForUiShutdown();reopened.reset();renderer.destroyTexture(hostTexture);sub->shutdown();DestroyWindow(window);RendererSubSystem::setBootstrapWindow(nullptr,640,480);
#else
    CHECK_TRUE(true);
#endif
}
TEST_CASE(light_show_visual_groups_keys_and_fog_share_atomic_undo_history) {
    Fixture f;const auto asset=createEditorProjectAsset(f.root.string(),EditorAssetType::LightShow);CHECK(asset.success);
    auto document=std::make_shared<EditorDslDocument>();std::string error;CHECK(document->open(asset.absolutePath,asset.absolutePath,&error));
    Host host;host.root=f.root.string();auto descriptor=makeEditorDslDescriptor();auto view=descriptor.createView(document,host);CHECK(view!=nullptr);
    auto* root=view->rootWidget();CHECK(find(root,"lightshow_timeline")!=nullptr);CHECK(find(root,"lightshow_budget_hud")!=nullptr);
    auto field=[&](const char* id,const wchar_t* value){auto* input=dynamic_cast<ayt::ui::TextInput*>(find(root,id));CHECK(input!=nullptr);input->setText(value);};
    auto decode=[&]{LightShow result;CHECK(decodeLightShow(document->sourceUtf8(),result,&error));return result;};
    const auto initial=document->sourceUtf8();
    field("lightshow_group",L"front");click(root,"lightshow_group_assign");CHECK(decode().fixtures.front().group=="front");
    CHECK(view->commandTarget()->executeCommand("edit.undo"));CHECK(document->sourceUtf8()==initial);
    CHECK(view->commandTarget()->executeCommand("edit.redo"));CHECK(decode().fixtures.front().group=="front");
    field("lightshow_target",L"group:front");field("lightshow_from",L"0");field("lightshow_to",L"2");click(root,"lightshow_key_copy");CHECK(decode().fixtures.front().keys.size()==2);
    const auto copied=document->sourceUtf8();field("lightshow_to",L"0");click(root,"lightshow_key_move");CHECK(document->sourceUtf8()==copied);
    field("lightshow_target",L"1");field("lightshow_from",L"1");field("lightshow_intensity",L"70");click(root,"lightshow_fixture_apply");CHECK(decode().fixtures.front().keys.size()==3);CHECK(decode().fixtures.front().keys[1].value.intensity==70);
    const auto keyed=document->sourceUtf8();field("lightshow_inner",L"2");click(root,"lightshow_fixture_apply");CHECK(document->sourceUtf8()==keyed);
    CHECK(view->commandTarget()->executeCommand("edit.undo"));CHECK(document->sourceUtf8()==copied);
    field("lightshow_from",L"1");field("lightshow_fog_density",L"0.06");click(root,"lightshow_fog_apply");CHECK(decode().fog.keys.size()==1);CHECK_FLOAT_EQ(decode().fog.keys[0].value.density,.06f,1e-6f);
    field("lightshow_target",L"fog");field("lightshow_from",L"1");field("lightshow_to",L"3");click(root,"lightshow_key_copy");CHECK(decode().fog.keys.size()==2);
    const auto beforeFormation=document->sourceUtf8();field("lightshow_from",L"0");field("lightshow_formation_count",L"16");click(root,"lightshow_circle");CHECK(decode().formations.front().points.size()==16);
    CHECK(view->commandTarget()->executeCommand("edit.undo"));CHECK(document->sourceUtf8()==beforeFormation);
    CHECK(view->commandTarget()->executeCommand("file.save"));CHECK_FALSE(document->isDirty());
    auto* source=dynamic_cast<ayt::ui::TextArea*>(find(root,"lightshow_source"));CHECK(source!=nullptr);source->setText(L"{}");CHECK_FALSE(view->commandTarget()->executeCommand("file.save"));
    LightShow disk;CHECK(loadLightShow(asset.absolutePath,disk));CHECK(disk.fog.keys.size()==2);
    view->onActivated();view->tick(.1f);view->onDeactivated();view->prepareForUiShutdown();
}

TEST_CASE(light_show_group_edits_preserve_offsets_and_track_identity_and_fixture_crud_undoes) {
    Fixture f;const auto asset=createEditorProjectAsset(f.root.string(),EditorAssetType::LightShow);
    LightShow show;show.name="Group edits";FixtureTrack a;a.id=1;a.group="stage";FixtureState av;av.position={-6,8,0};av.color={1,0,0};a.keys.push_back({0,av});a.strobeFrequency=2;a.goboTexturePath="first.png";
    auto b=a;b.id=2;b.keys[0].value.position={6,9,0};b.keys[0].value.color={0,0,1};b.strobeFrequency=7;b.goboTexturePath="second.png";show.fixtures={a,b};CHECK(saveLightShow(asset.absolutePath,show));
    auto document=std::make_shared<EditorDslDocument>();std::string error;CHECK(document->open(asset.absolutePath,asset.absolutePath,&error));Host host;auto view=makeEditorDslDescriptor().createView(document,host);auto* root=view->rootWidget();const auto original=document->sourceUtf8();
    auto field=[&](const char* id,const wchar_t* value){auto* input=dynamic_cast<ayt::ui::TextInput*>(find(root,id));CHECK(input!=nullptr);input->setText(value);};
    field("lightshow_target",L"group:stage");click(root,"lightshow_select");field("lightshow_intensity",L"55");click(root,"lightshow_fixture_apply");LightShow edited;CHECK(decodeLightShow(document->sourceUtf8(),edited));
    CHECK(edited.fixtures[0].keys[0].value.intensity==55);CHECK(edited.fixtures[1].keys[0].value.intensity==55);CHECK(edited.fixtures[0].keys[0].value.position[0]==-6);CHECK(edited.fixtures[1].keys[0].value.position[0]==6);CHECK(edited.fixtures[1].keys[0].value.position[1]==9);CHECK(edited.fixtures[1].keys[0].value.color[2]==1);
    field("lightshow_strobe",L"12");click(root,"lightshow_track_apply");CHECK(decodeLightShow(document->sourceUtf8(),edited));CHECK(edited.fixtures[0].strobeFrequency==12);CHECK(edited.fixtures[1].strobeFrequency==12);CHECK(edited.fixtures[0].goboTexturePath=="first.png");CHECK(edited.fixtures[1].goboTexturePath=="second.png");
    CHECK(view->commandTarget()->executeCommand("edit.undo"));CHECK(view->commandTarget()->executeCommand("edit.undo"));CHECK(document->sourceUtf8()==original);
    field("lightshow_target",L"1");click(root,"lightshow_select");click(root,"lightshow_fixture_clone");CHECK(decodeLightShow(document->sourceUtf8(),edited));CHECK(edited.fixtures.size()==3);CHECK(edited.fixtures[2].id==3);CHECK(edited.fixtures[2].goboTexturePath=="first.png");
    click(root,"lightshow_fixture_remove");CHECK(decodeLightShow(document->sourceUtf8(),edited));CHECK(edited.fixtures.size()==2);CHECK(view->commandTarget()->executeCommand("edit.undo"));CHECK(decodeLightShow(document->sourceUtf8(),edited));CHECK(edited.fixtures.size()==3);
    click(root,"lightshow_fixture_add");CHECK(decodeLightShow(document->sourceUtf8(),edited));CHECK(edited.fixtures.size()==4);CHECK(edited.fixtures.back().id==4);CHECK(edited.fixtures.back().keys.front().value.position[1]==8);
    const auto beforeQuality=document->sourceUtf8();field("lightshow_surface_budget",L"2");field("lightshow_exposure",L"-2");click(root,"lightshow_quality_apply");CHECK(document->sourceUtf8()==beforeQuality);field("lightshow_surface_budget",L"2.5");click(root,"lightshow_quality_apply");CHECK(document->sourceUtf8()==beforeQuality);
}
TEST_CASE(light_show_timeline_drag_commits_once_and_capture_cancel_preserves_source) {
    Fixture f;const auto asset=createEditorProjectAsset(f.root.string(),EditorAssetType::LightShow);auto document=std::make_shared<EditorDslDocument>();std::string error;CHECK(document->open(asset.absolutePath,asset.absolutePath,&error));Host host;auto view=makeEditorDslDescriptor().createView(document,host);auto* root=view->rootWidget();auto* timeline=find(root,"lightshow_timeline");CHECK(timeline!=nullptr);timeline->setSize({664,190});const auto b=timeline->getWorldBounds();const auto original=document->sourceUtf8();
    CHECK(timeline->onMouseButtonDown({{b.minX+160,b.minY+34},0}));CHECK(timeline->onMouseMove({{b.minX+260,b.minY+34},0}));CHECK(document->sourceUtf8()==original);timeline->onCaptureCancelled();CHECK(document->sourceUtf8()==original);
    CHECK(timeline->onMouseButtonDown({{b.minX+160,b.minY+34},0}));CHECK(timeline->onMouseMove({{b.minX+260,b.minY+34},0}));CHECK(timeline->onMouseButtonUp({{b.minX+260,b.minY+34},0}));LightShow edited;CHECK(decodeLightShow(document->sourceUtf8(),edited));CHECK_FLOAT_EQ(float(edited.fixtures.front().keys.front().time),2.f,1e-6f);
    CHECK(view->commandTarget()->executeCommand("edit.undo"));CHECK(document->sourceUtf8()==original);
    CHECK(view->commandTarget()->executeCommand("edit.redo"));CHECK(decodeLightShow(document->sourceUtf8(),edited));CHECK_FLOAT_EQ(float(edited.fixtures.front().keys.front().time),2.f,1e-6f);
    CHECK(timeline->onMouseButtonDown({{b.minX+260,b.minY+34},0}));CHECK(timeline->onMouseMove({{b.minX+310,b.minY+34},0}));
    edited.fixtures.front().keys.front().time=4;std::string external;CHECK(encodeLightShow(edited,external));document->setSourceUtf8(external);view->tick(0);CHECK_FALSE(timeline->onMouseButtonUp({{b.minX+310,b.minY+34},0}));CHECK(document->sourceUtf8()==external);

}
#endif
TEST_SUITE_END
TEST_SUITE(AYEditor_SequencePreflight)
TEST_CASE(bad_skeleton_is_rejected_before_player_initialization_or_clip_load) {
    Fixture f(true);
    CHECK_TRUE(ayt::resource::initializeLoaders());
    ayt::resource::Skeleton skeleton;
    skeleton.setBoneCount(1);
    ayt::resource::Bone bone;
    bone.name = "root";
    bone.parentIndex = 0;
    bone.localPosition = {0, 0, 0};
    bone.localRotation = {0, 0, 0, 1};
    bone.localScale = {1, 1, 1};
    bone.inverseBindMatrix = ayt::math::Float4x4::identity();
    skeleton.setBone(0, bone);
    std::vector<ayt::math::UInt8> bytes;
    CHECK_TRUE(skeleton.saveToBinary(bytes));
    CHECK_TRUE(ayt::io::File::atomicWrite((f.root / "Assets" / "rig.ayskel").string(), bytes.data(),
                                          bytes.size()));
    ayt::scene::Scene scene(ayt::scene::SceneMode::Edit, "bad-rig");
    auto *e = scene.world().createEntity();
    e->setName("Actor");
    e->addComponent<ayt::entity::Transform>();
    e->addComponent<ayt::entity::SkeletonComponent>()->skeletonPath = "rig.ayskel";
    CHECK_TRUE(scene.save((f.root / "Assets" / "worlds" / "test.scn").string()));
    CHECK_TRUE(f.document->edit("bad-rig", [](Sequence &s) {
        s.animations = {{"clips", "actor", {{"section", "walk.ayanm", 0, 4, 0, 1, 4}}}};
        return true;
    }));
    CHECK_FALSE(f.document->seek(1));
    CHECK_FALSE(f.document->diagnostic().empty());
    CHECK(f.document->previewScene() == nullptr);
    CHECK(ayt::resource::ResourceManager::instance().getLoadState(
              (f.root / "Assets" / "walk.ayanm").string()) ==
          ayt::resource::ResourceLoadState::NotLoaded);
}
TEST_SUITE_END
#endif
