#ifdef AYEDITOR_HAS_SEQUENCE
#include <AYEditor/EditorAssetDatabase.h>
#include <AYEditor/EditorProjectAssetFactory.h>
#include <AYEditor/EditorSequenceDocument.h>
#include <AYEditor/EditorWorkspace.h>
#include <AYEntity.h>
#include <AYEntity/EntityModule.h>
#include <AYEntity/components/SkeletonComponent.h>
#include <AYIO/File.h>
#include <AYResource.h>
#include <AYResource/assetsImpl/Animation.h>
#include <AYScene.h>
#include <AYSequence/SequenceAssetIO.h>
#include <AYTest.h>
#include <AYUI/Authoring/DopeSheet.h>
#include <AYUI/Button.h>
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
