#include "AYTest.h"
#include "AYEditor/EditorAnimationDocument.h"
#include "AYEditor/EditorAnimationExtension.h"
#include "AYEditor/EditorBuiltInExtensions.h"
#include "AYEditor/EditorWorkspace.h"
#include "../src/AYEditorAnimationCanvas.h"
#include "../src/AYEditorAnimationCurveCanvas.h"
#include "../src/AYEditorAnimationCurveSource.h"
#include "../src/AYEditorAnimationDopeSheet.h"
#include "../src/AYEditorAnimationRigPanel.h"
#include "../src/AYEditorTimelinePlaybackSource.h"
#include <AYAnimationEditor/SkeletonEditorCore.h>
#include <AYAnimationEditor/SkeletonBakeJob.h>

#include <AYIO/File.h>
#include <AYResource/assetsImpl/Animation.h>
#include <AYResource/assetsImpl/Mesh.h>
#include <AYResource/assetsImpl/Skeleton.h>
#include <AYUI/MockRenderer.h>
#include <AYUI/Authoring/TimelineSelectionOps.h>
#include <AYTestFixtures.h>
#include <AYUI/TextInput.h>
#include <AYEditor/EditorCommandButtons.h>
#include <AYEditor/EditorAuthoringSelectionBridge.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>

namespace {

std::filesystem::path animationExtensionFixtureRoot()
{
    const auto root = std::filesystem::temp_directory_path()
        / "ay_editor_animation_extension";
    std::error_code error;
    std::filesystem::create_directories(root / "Assets", error);
    return root;
}

std::filesystem::path writeAnimationEditorSkeleton()
{
    ayt::resource::Skeleton skeleton;
    ayt::resource::Bone root;
    root.name = "root";
    root.parentIndex = -1;
    root.localRotation = ayt::math::FQuaternion::identity();
    root.localScale = {1, 1, 1};
    root.inverseBindMatrix = ayt::math::Float4x4::identity();
    skeleton.addBone(root);
    ayt::resource::Bone hips = root;
    hips.name = "hips";
    hips.parentIndex = 0;
    hips.localPosition = {0, 1, 0};
    skeleton.addBone(hips);
    std::vector<ayt::math::UInt8> bytes;
    CHECK(skeleton.saveToBinary(bytes));
    const auto path = animationExtensionFixtureRoot() / "Assets" / "hero.ayskel";
    CHECK(ayt::io::File::writeAllBytes(path.string(), bytes));
    return path;
}

std::filesystem::path writeAnimationEditorClip()
{
    ayt::resource::Animation animation;
    animation.setName("walk");
    animation.setDuration(2.0f);
    animation.setTicksPerSecond(2.0f);
    ayt::resource::AnimTrack track;
    track.nodeName = "hips";
    track.property = "position";
    track.valueType = ayt::resource::AnimTrackType::Vector3;
    track.times = {0.0f, 2.0f, 4.0f};
    track.values = {0, 1, 0, 0, 2, 0, 0, 1, 0};
    animation.addTrack(track);
    animation.addNotify({"footstep", 0.5f, 1.0f});
    std::vector<ayt::math::UInt8> bytes;
    CHECK(animation.saveToBinary(bytes));
    const auto path = animationExtensionFixtureRoot() / "Assets" / "walk.anm";
    CHECK(ayt::io::File::writeAllBytes(path.string(), bytes));
    return path;
}

std::filesystem::path writeAnimationEditorMesh()
{
    ayt::resource::Mesh mesh;
    mesh.createCube(1.0f);
    std::vector<ayt::resource::VertexSkinWeight> weights(mesh.getVertexCount());
    for (auto& weight : weights) {
        weight.boneIndex[0] = 0u;
        weight.boneWeight[0] = 1.0f;
    }
    mesh.debugSetSkinWeights(weights);
    std::vector<ayt::math::UInt8> bytes;
    CHECK(mesh.saveToBinary(bytes));
    const auto path = animationExtensionFixtureRoot() / "Assets" / "hero.aymesh";
    CHECK(ayt::io::File::writeAllBytes(path.string(), bytes));
    return path;
}

class AnimationExtensionHost final : public ayt::editor::IEditorHostServices {
public:
    explicit AnimationExtensionHost(std::string root) : _root(std::move(root)) {}
    ayt::editor::EditorWorkspace& workspace() noexcept override { return _workspace; }
    const std::string& projectRoot() const noexcept override { return _root; }
    void requestRepaint() override { ++repaintCount; }
    void setStatusText(const std::wstring& value) override { status = value; }

    ayt::editor::EditorWorkspace _workspace;
    std::string _root;
    int repaintCount = 0;
    std::wstring status;
};

ayt::ui::Widget* findAuthoringWidget(ayt::ui::Widget* root, const std::string& id) {
    if (!root) return nullptr;
    if (root->getId() == id) return root;
    for (auto* child : root->getChildren())
        if (auto* found = findAuthoringWidget(child, id)) return found;
    return nullptr;
}

} // namespace

TEST_SUITE(AYEditor_AnimationExtension)
TEST_CASE(control_rig_corrupt_metadata_blocks_silent_save_and_clear_is_undoable) {
    ayt::test::ScratchDirectory scratch("rig-corrupt-metadata"); const auto clip=scratch.path()/"clip.anm"; std::string error;
    CHECK(ayt::io::File::writeAllBytes(clip.string(),ayt::io::File::readAllBytes(writeAnimationEditorClip().string())));
    ayt::editor::EditorAnimationDocument document; CHECK(document.initialize({clip.string()},error)); document.configureProjectRoot(scratch.path().string());
    CHECK(document.bindSkeleton(writeAnimationEditorSkeleton().string())); CHECK(document.createControlRig()); CHECK(document.save());
    auto metadata=nlohmann::json::parse(ayt::io::File::readAllText(document.metadataPath()));
    for(auto& value:metadata["bindings"].items()) value.value()["controlRig"]["version"]=999;
    const auto poisoned=metadata.dump(); CHECK(ayt::io::File::writeAllText(document.metadataPath(),poisoned));
    CHECK(document.reload()); CHECK(!document.preview().controlRig()); CHECK(!document.controlRigLoadError().empty());
    CHECK(!document.save(&error)); CHECK(ayt::io::File::readAllText(document.metadataPath())==poisoned);
    CHECK(document.clearControlRig()); CHECK(document.controlRigLoadError().empty()); CHECK(document.timelineUndo()); CHECK(!document.controlRigLoadError().empty());
    CHECK(document.timelineRedo()); CHECK(document.save()); CHECK(document.controlRigLoadError().empty());
}
TEST_CASE(control_rig_rebind_preserves_live_edits_and_clear_binding_history_is_safe) {
    ayt::test::ScratchDirectory scratch("rig-rebind"); std::string error;
    const auto clip=scratch.path()/"clip.anm",skeletonPath=writeAnimationEditorSkeleton();
    CHECK(ayt::io::File::writeAllBytes(clip.string(),ayt::io::File::readAllBytes(writeAnimationEditorClip().string())));
    ayt::editor::EditorAnimationDocument document; CHECK(document.initialize({clip.string()},error)); document.configureProjectRoot(scratch.path().string());
    CHECK(document.bindSkeleton(skeletonPath.string())); CHECK(document.createControlRig()); CHECK(document.recordControlRigKey());
    CHECK(document.editControlRig([](auto& rig){auto p=rig.pose();p.fk.positions[1]={0,7,0};return rig.setPose(p);}));
    const auto live=document.preview().controlRig()->encode(); CHECK(document.bindSkeleton(skeletonPath.string())); CHECK(document.preview().controlRig()->encode()==live);
    CHECK(document.save());
    ayt::resource::Skeleton originalSkeleton,changed; CHECK(originalSkeleton.load(skeletonPath.string()));
    std::vector<ayt::resource::Bone> bones(originalSkeleton.getBones(),originalSkeleton.getBones()+originalSkeleton.getBoneCount()); bones[1].name="different-hips"; for(const auto& b:bones) changed.addBone(b);
    std::vector<ayt::math::UInt8> bytes; CHECK(changed.saveToBinary(bytes)); const auto different=scratch.path()/"different.ayskel";
    CHECK(ayt::io::File::writeAllBytes(different.string(),bytes)); CHECK(!document.bindSkeleton(different.string(),&error)); CHECK(document.preview().controlRig()->encode()==live);
    CHECK(document.clearControlRig()); CHECK(document.bindSkeleton(different.string())); CHECK(!document.preview().controlRig());
    CHECK(document.timelineUndo()); CHECK(document.preview().skeletonPath()==skeletonPath.generic_string());
    CHECK(document.timelineUndo()); CHECK(document.preview().controlRig()->encode()==live);
    CHECK(document.timelineRedo()); CHECK(document.timelineRedo());
    CHECK(document.reload()); CHECK(document.preview().controlRig()); CHECK(document.preview().skeletonPath()==skeletonPath.generic_string());
}
TEST_CASE(control_rig_clip_shortening_rejects_out_of_range_keys_without_mutation) {
    ayt::editor::EditorAnimationDocument document; std::string error;
    CHECK(document.initialize({writeAnimationEditorClip().string()},error)); CHECK(document.bindSkeleton(writeAnimationEditorSkeleton().string()));
    CHECK(document.createControlRig()); CHECK(document.setTimelinePositionSeconds(2)); CHECK(document.recordControlRigKey());
    // Clear the original TRS track so its end key is not the reason to reject.
    CHECK(document.removeAnimationTrack("animation.0"));
    auto properties=document.animationClipProperties(); properties.durationSeconds=1;
    CHECK(!document.setAnimationClipProperties(properties,&error)); CHECK(document.timelineDurationSeconds()==2); CHECK(!error.empty());
    CHECK(document.preview().controlRig()->keys().back().seconds==2);
}

TEST_CASE(control_rig_keys_preview_save_reopen_and_bake_are_undoable) {
    ayt::test::ScratchDirectory scratch("rig-document");
    const auto path=scratch.path()/"rig.anm";
    CHECK(ayt::io::File::writeAllBytes(path.string(),ayt::io::File::readAllBytes(writeAnimationEditorClip().string())));
    auto document=std::make_shared<ayt::editor::EditorAnimationDocument>(); std::string error;
    CHECK(document->initialize({path.string()},error)); document->configureProjectRoot(scratch.path().string());
    CHECK(document->bindSkeleton(writeAnimationEditorSkeleton().string(),&error));
    CHECK(document->createControlRig({},&error)); CHECK(document->recordControlRigKey(&error));
    CHECK(document->setTimelinePositionSeconds(1));
    CHECK(document->editControlRig([](auto& rig){auto p=rig.pose();p.fk.positions[1]={0,3,0};p.overrides[1]=true;return rig.setPose(p);}));
    CHECK(document->recordControlRigKey(&error)); CHECK(document->preview().controlRig()->keys().size()==2);
    CHECK(document->setTimelinePositionSeconds(.5));
    CHECK(std::fabs(document->preview().poseWorldMatrices()[1].transformPoint({}).y-2)<1e-4f);
    CHECK(document->save(&error)); const auto original=ayt::io::File::readAllBytes(path.string());
    CHECK(document->editControlRig([](auto& rig){auto p=rig.pose();p.fk.positions[1]={0,9,0};return rig.setPose(p);}));
    document->setLooping(false); // Transport metadata must not silently save unsaved controller edits.
    CHECK(document->reload(&error)); CHECK(document->preview().controlRig()); CHECK(document->preview().controlRig()->keys().size()==2);
    CHECK(document->setTimelinePositionSeconds(.5));
    CHECK(std::fabs(document->preview().poseWorldMatrices()[1].transformPoint({}).y-2)<1e-4f);
    CHECK(document->bakeControlRig(30,&error)); CHECK(!document->preview().controlRig()->enabled);
    CHECK(document->preview().animation()->getTrackCount()==3); CHECK(document->preview().animation()->getNotifyCount()==1);
    CHECK(document->timelineUndo()); CHECK(document->preview().controlRig()->enabled); CHECK(document->preview().animation()->getTrackCount()==1);
    CHECK(document->timelineRedo()); CHECK(!document->preview().controlRig()->enabled); CHECK(document->preview().animation()->getTrackCount()==3);
    CHECK(ayt::io::File::readAllBytes(path.string())==original);
}

TEST_CASE(control_rig_timeline_moves_atomically_and_reuses_playhead_snapshot) {
    auto document=std::make_shared<ayt::editor::EditorAnimationDocument>(); std::string error;
    CHECK(document->initialize({writeAnimationEditorClip().string()},error)); CHECK(document->bindSkeleton(writeAnimationEditorSkeleton().string()));
    CHECK(document->createControlRig()); CHECK(document->recordControlRigKey()); CHECK(document->setTimelinePositionSeconds(1)); CHECK(document->recordControlRigKey());
    const auto source=ayt::editor::makeControlRigTimelineSource(document); const auto before=source->timelineSnapshot(); CHECK(before->keys.size()==2);
    CHECK(document->setTimelinePositionSeconds(.25)); CHECK(source->timelineSnapshot()==before);
    std::vector<std::string> ids{"rig.key.1"}; CHECK(!source->transformKeys(ids,-1,0,0)); CHECK(source->timelineSnapshot()==before);
    CHECK(source->beginEdit("rig move")); CHECK(source->transformKeys(ids,-.25,0,0)); CHECK(source->endEdit(false));
    CHECK(document->preview().controlRig()->keys()[1].seconds==.75); CHECK(document->timelineUndo()); CHECK(document->preview().controlRig()->keys()[1].seconds==1);
    CHECK(source->removeKeys({"rig.key.0","rig.key.1"})); CHECK(document->preview().controlRig()->keys().empty());
    CHECK(document->timelineUndo()); CHECK(document->preview().controlRig()->keys().size()==2);
    source->selectionState()->keyIds={"rig.key.0"}; source->selectionState()->primaryKeyId="rig.key.0";
    ids={"rig.key.0"}; CHECK(source->beginEdit("reorder rig key")); CHECK(source->transformKeys(ids,1.5,0,0)); CHECK(source->endEdit(false));
    CHECK(source->selectionState()->primaryKeyId=="rig.key.1"); CHECK(document->timelineUndo()); CHECK(source->selectionState()->primaryKeyId=="rig.key.0");
    CHECK(document->timelineRedo()); CHECK(source->selectionState()->primaryKeyId=="rig.key.1");
    CHECK(source->removeKeys({"rig.key.0","rig.key.1"})); CHECK(source->selectionState()->keyIds.empty());
    CHECK(document->timelineUndo()); CHECK(source->selectionState()->primaryKeyId=="rig.key.1");
}

TEST_CASE(control_rig_canvas_drag_cancel_and_page_controls_use_one_history) {
    auto document=std::make_shared<ayt::editor::EditorAnimationDocument>(); std::string error;
    CHECK(document->initialize({writeAnimationEditorClip().string()},error)); CHECK(document->bindSkeleton(writeAnimationEditorSkeleton().string())); CHECK(document->createControlRig());
    ayt::editor::EditorAnimationCanvas canvas(document); canvas.setSize({640,320}); ayt::ui::MockRenderer renderer; renderer.beginFrame();canvas.render(renderer);
    ayt::ui::authoring::PreviewBounds bounds; for(const auto& matrix:document->preview().poseWorldMatrices()) bounds.include(matrix.transformPoint({}));
    ayt::ui::authoring::PreviewOrbit orbit; ayt::ui::authoring::PreviewProjection project(bounds,{0,0,640,320},orbit,.72f);
    const auto center=project(document->preview().poseWorldMatrices()[1].transformPoint({})); const auto original=document->preview().poseWorldMatrices()[1].transformPoint({});
    CHECK(canvas.onMouseButtonDown({{center.x,center.y},0})); CHECK(canvas.onMouseMove({{center.x+30,center.y},0}));
    CHECK((document->preview().poseWorldMatrices()[1].transformPoint({})-original).length()>.01f);
    canvas.onCaptureCancelled(); CHECK((document->preview().poseWorldMatrices()[1].transformPoint({})-original).length()<1e-4f); CHECK(!document->animationEditGestureActive());
    CHECK(canvas.onMouseButtonDown({{center.x,center.y},0})); CHECK(canvas.onMouseMove({{center.x+40,center.y},0})); CHECK(canvas.onMouseButtonUp({{center.x+40,center.y},0}));
    CHECK(document->timelineUndo()); CHECK((document->preview().poseWorldMatrices()[1].transformPoint({})-original).length()<1e-4f);
    CHECK(canvas.onMouseButtonDown({{center.x+14,center.y},0})); CHECK(canvas.onMouseMove({{center.x,center.y-14},0}));
    const auto q=document->preview().controlRig()->pose().fk.rotations[1]; CHECK(std::fabs(q.w)<.99f);
    canvas.onCaptureCancelled(); CHECK(std::fabs(document->preview().controlRig()->pose().fk.rotations[1].w-1)<1e-4f);
    AnimationExtensionHost host(animationExtensionFixtureRoot().string()); const auto view=ayt::editor::makeEditorAnimationDescriptor().createView(document,host);
    CHECK(findAuthoringWidget(view->rootWidget(),"animation_control_rig_panel")); CHECK(findAuthoringWidget(view->rootWidget(),"animation_rig_timeline"));
    CHECK(findAuthoringWidget(view->rootWidget(),"animation_rig_record")); CHECK(findAuthoringWidget(view->rootWidget(),"animation_inspector_scroll"));
}

TEST_CASE(control_rig_save_failure_stays_dirty_and_legacy_refuses_creation) {
    ayt::test::ScratchDirectory scratch("rig-save-failure"); const auto path=scratch.path()/"clip.anm";
    CHECK(ayt::io::File::writeAllBytes(path.string(),ayt::io::File::readAllBytes(writeAnimationEditorClip().string())));
    ayt::editor::EditorAnimationDocument document; std::string error;
    CHECK(document.initialize({path.string()},error)); document.configureProjectRoot(scratch.path().string()); CHECK(document.bindSkeleton(writeAnimationEditorSkeleton().string()));
    CHECK(document.createControlRig()); CHECK(document.recordControlRigKey());
    const auto original=ayt::io::File::readAllBytes(path.string());
    auto properties=document.animationClipProperties(); properties.name="unsaved-clip"; CHECK(document.setAnimationClipProperties(properties));
    CHECK(!document.writeRecoveryCopy((scratch.path()/"recovery.anm").string(),&error)); CHECK(!error.empty());
    const auto metadata=std::filesystem::path(document.metadataPath());
    // Existing metadata already has a directory after bind; a directory at the file target must be refused.
    std::error_code ec; std::filesystem::remove(metadata,ec); std::filesystem::create_directory(metadata,ec);
    CHECK(!document.save(&error)); CHECK(document.isDirty()); CHECK(!error.empty());
    CHECK(ayt::io::File::readAllBytes(path.string())==original);
    const auto legacy=scratch.path()/"readonly.ayanim"; CHECK(ayt::io::File::writeAllBytes(legacy.string(),ayt::io::File::readAllBytes(path.string())));
    ayt::editor::EditorAnimationDocument readOnly; CHECK(readOnly.initialize({legacy.string()},error)); CHECK(readOnly.bindSkeleton(writeAnimationEditorSkeleton().string())); CHECK(!readOnly.createControlRig());
}

TEST_CASE(session_clipboard_survives_source_close_and_pastes_compatible_clip) {
    ayt::test::ScratchDirectory scratch("cross-clip-authoring");
    const auto data = ayt::io::File::readAllBytes(writeAnimationEditorClip().string());
    const auto sourcePath = scratch.path() / "source.anm";
    const auto targetPath = scratch.path() / "target.anm";
    CHECK(ayt::io::File::writeAllBytes(sourcePath.string(), data));
    CHECK(ayt::io::File::writeAllBytes(targetPath.string(), data));
    const auto descriptor = ayt::editor::makeEditorAnimationDescriptor();
    std::string error;
    AnimationExtensionHost host(scratch.path().string());
    std::weak_ptr<ayt::editor::IEditorDocument> sourceLifetime;
    {
        const auto source = descriptor.createDocument({sourcePath.string()}, error);
        sourceLifetime = source;
        const auto sourceView = descriptor.createView(source, host);
        CHECK(sourceView->commandTarget()->executeCommand("edit.copy"));
    }
    CHECK(sourceLifetime.expired());
    const auto target = descriptor.createDocument({targetPath.string()}, error);
    const auto targetView = descriptor.createView(target, host);
    auto document = std::dynamic_pointer_cast<ayt::editor::EditorAnimationDocument>(target);
    CHECK(document->setTimelinePositionSeconds(.5));
    CHECK(targetView->commandTarget()->executeCommand("edit.paste"));
    CHECK(document->preview().animation()->getTrackKeyframeCount(0) == 4);
    CHECK(document->save(&error));
    CHECK(document->reload(&error));
    CHECK(document->preview().animation()->getTrackTimes(0)[1] == 1);
}

TEST_CASE(insert_key_uses_formal_cubic_and_shortest_arc_sampler) {
    ayt::editor::EditorAnimationDocument document;
    std::string error;
    CHECK(document.initialize({writeAnimationEditorClip().string()}, error));
    CHECK(document.setAnimationTrackInterpolation("animation.0", ayt::resource::AnimInterpolation::CubicHermite));
    CHECK(document.setAnimationKeyframeTangents("key.0.0", {0, 0, 0}, {0, 4, 0}));
    // Unspecified tangents are already zero; setting them again is a no-op.
    const auto source = ayt::editor::makeAnimationCurveSource(
        std::shared_ptr<ayt::editor::EditorAnimationDocument>(&document, [](auto*) {}));
    const auto old = source->curveTrack("animation.0");
    const auto expected = old->sample(1, .5);
    CHECK(document.timelineAddKeyframe("animation.0", .5, 0));
    std::vector<float> values;
    CHECK(document.animationKeyframeValues("key.0.1", values));
    CHECK(std::fabs(values[1] - expected) < 1e-5f);
    CHECK(document.addAnimationTrack("hips", "rotation", ayt::resource::AnimTrackType::Quaternion));
    CHECK(document.timelineAddKeyframe("animation.1", 2, 0));
    CHECK(document.setAnimationKeyframeValues("key.1.1", {0, 0, 0, -1}));
    CHECK(document.timelineAddKeyframe("animation.1", 1, 0));
    CHECK(document.animationKeyframeValues("key.1.1", values));
    CHECK(std::fabs(std::fabs(values[3]) - 1) < 1e-5f);
    CHECK(std::fabs(values[0]) + std::fabs(values[1]) + std::fabs(values[2]) < 1e-5f);
}

TEST_CASE(large_clip_playback_reuses_snapshots_and_old_samples_survive_edit) {
    ayt::test::ScratchDirectory scratch("large-authoring");
    ayt::resource::Animation clip;
    clip.setDuration(100); clip.setTicksPerSecond(30);
    for (int row = 0; row < 64; ++row) {
        ayt::resource::AnimTrack track;
        track.nodeName = "bone-" + std::to_string(row);
        track.property = "weight"; track.valueType = ayt::resource::AnimTrackType::Float;
        for (int key = 0; key < 500; ++key) {
            track.times.push_back(key * 3.f); track.values.push_back(static_cast<float>(key));
        }
        clip.addTrack(track);
    }
    const auto path = scratch.path() / "large.anm";
    std::vector<ayt::math::UInt8> bytes;
    CHECK(clip.saveToBinary(bytes)); CHECK(ayt::io::File::writeAllBytes(path.string(), bytes));
    auto document = std::make_shared<ayt::editor::EditorAnimationDocument>();
    std::string error;
    CHECK(document->initialize({path.string()}, error));
    CHECK(!document->preview().skeleton());
    const auto source = ayt::editor::makeAnimationCurveSource(document);
    const auto timeline = source->timelineSnapshot();
    const auto curve = source->curveTrack("animation.0");
    CHECK(timeline->keys.size() == 32000);
    const auto revision = document->revision();
    document->setLooping(false); document->setPlayRate(2);
    document->timelinePlay();
    for (int frame = 0; frame < 100; ++frame) {
        document->timelineTick(.01);
        CHECK(source->timelineSnapshot() == timeline);
        CHECK(source->curveTrack("animation.0") == curve);
    }
    CHECK(document->revision() == revision);
    CHECK(document->timelinePositionSeconds() > 1.9);
    CHECK(document->setAnimationKeyframeValues("key.0.10", {999}));
    const auto changed = source->curveTrack("animation.0");
    CHECK(changed != curve);
    CHECK(std::fabs(changed->sample(0, 1) - 999) < 1e-4f);
    CHECK(std::fabs(curve->sample(0, 1) - 10) < 1e-4f);
    CHECK(document->timelineUndo());
    CHECK(std::fabs(source->curveTrack("animation.0")->sample(0, 1) - 10) < 1e-4f);
}

TEST_CASE(batch_history_restores_selection_and_coalesces_drag_revisions) {
    auto document = std::make_shared<ayt::editor::EditorAnimationDocument>();
    std::string error;
    CHECK(document->initialize({writeAnimationEditorClip().string()}, error));
    CHECK(document->setAnimationKeyframeValues("key.0.2", {0, 3, 0}));
    const auto source = ayt::editor::makeAnimationCurveSource(document);
    using namespace ayt::ui::authoring;
    TimelineSelectionOps::keys(*source->selectionState(), "animation.0", {"key.0.0", "key.0.1", "key.0.2"});
    auto ids = source->selectionState()->keyIds;
    const auto beforeIds = ids;
    CHECK(document->reverseAnimationKeyframes(ids, &error));
    TimelineSelectionOps::remap(*source->selectionState(), beforeIds, ids);
    CHECK(source->selectionState()->primaryKeyId == "key.0.2");
    CHECK(document->timelineUndo());
    CHECK(source->selectionState()->primaryKeyId == "key.0.0");
    CHECK(document->timelineRedo());
    CHECK(source->selectionState()->primaryKeyId == "key.0.2");
    CHECK(document->timelineUndo());
    TimelineSelectionOps::keys(*source->selectionState(), "animation.0", {"key.0.1"});
    CHECK(source->beginEdit("many updates"));
    for (int update = 0; update < 100; ++update) {
        auto selected = source->selectionState()->keyIds;
        CHECK(source->transformKeys(selected, .001, 0, 0));
    }
    CHECK(source->endEdit(false));
    const auto beforeUndo = document->revision();
    CHECK(document->timelineUndo());
    CHECK(document->revision() == beforeUndo + 1); // One retained baseline, not 100 snapshots.
    CHECK(document->preview().animation()->getTrackTimes(0)[1] == 2);
    CHECK(document->timelineRedo());
    CHECK(document->preview().animation()->getTrackTimes(0)[1] > 2.19f);
}

TEST_CASE(time_transform_document_and_view_are_undoable_and_persistent) {
    const auto descriptor = ayt::editor::makeEditorAnimationDescriptor();
    std::string error;
    const auto base = descriptor.createDocument({writeAnimationEditorClip().string()}, error);
    auto document = std::dynamic_pointer_cast<ayt::editor::EditorAnimationDocument>(base);
    std::vector<std::string> ids{"key.0.0", "key.0.1", "key.0.2", "notify.0"};
    CHECK(document->reverseAnimationKeyframes(ids, &error));
    CHECK(ids.front() == "key.0.2");
    CHECK(document->preview().animation()->getNotifyTime(0) == 1.5f);
    CHECK(document->timelineUndo());
    CHECK(document->preview().animation()->getNotifyTime(0) == .5f);
    CHECK(document->retimeAnimationKeyframes(ids, 0, .5, &error));
    CHECK(document->save(&error));
    CHECK(document->reload(&error));
    CHECK(document->preview().animation()->getTrackTimes(0)[2] == 2);
    AnimationExtensionHost host(animationExtensionFixtureRoot().string());
    const auto view = descriptor.createView(base, host);
    auto* anchor = dynamic_cast<ayt::ui::TextInput*>(findAuthoringWidget(view->rootWidget(), "animation_time_anchor"));
    auto* scale = dynamic_cast<ayt::ui::TextInput*>(findAuthoringWidget(view->rootWidget(), "animation_time_scale"));
    CHECK(anchor); CHECK(scale);
    if (!anchor || !scale) return;
    anchor->setText(L"0"); scale->setText(L"0");
    CHECK(!view->commandTarget()->canExecuteCommand("animation.scale-time"));
    scale->setText(L"2");
    CHECK(view->commandTarget()->canExecuteCommand("animation.scale-time"));
    // Selected first key remains at anchor; no content change is reported.
    CHECK(!view->commandTarget()->executeCommand("animation.scale-time"));
}

TEST_CASE(clipboard_document_cut_paste_and_duplicate_are_atomic_undoable) {
    ayt::editor::EditorAnimationDocument document;
    std::string error;
    CHECK(document.initialize({writeAnimationEditorClip().string()}, error));
    ayt::editor::EditorAnimationClipboard clipboard;
    std::vector<std::string> ids{"key.0.1", "notify.0"};
    CHECK(document.copyAnimationKeyframes(ids, clipboard, &error));
    CHECK(!document.isDirty());
    CHECK(document.cutAnimationKeyframes(ids, clipboard, &error));
    CHECK(document.preview().animation()->getNotifyCount() == 0);
    CHECK(document.timelineUndo());
    CHECK(document.preview().animation()->getNotifyCount() == 1);
    std::vector<std::string> pasted{"unchanged"};
    CHECK(!document.pasteAnimationKeyframes(clipboard, .5, pasted, &error));
    CHECK(pasted == std::vector<std::string>({"unchanged"}));
    CHECK(!document.isDirty());
    CHECK(document.pasteAnimationKeyframes(clipboard, .75, pasted, &error));
    CHECK(pasted.size() == 2);
    CHECK(document.preview().animation()->getNotifyCount() == 2);
    CHECK(document.timelineUndo());
    CHECK(document.preview().animation()->getNotifyCount() == 1);
    std::vector<std::string> duplicate{"key.0.1"};
    CHECK(document.duplicateAnimationKeyframes(duplicate, &error));
    CHECK(duplicate == std::vector<std::string>({"key.0.2"}));
    CHECK(document.save(&error));
    CHECK(document.reload(&error));
    CHECK(document.preview().animation()->getTrackTimes(0)[2] == 3);
}

TEST_CASE(clipboard_binding_and_readonly_guards_never_mutate) {
    ayt::editor::EditorAnimationDocument document;
    std::string error;
    CHECK(document.initialize({writeAnimationEditorClip().string()}, error));
    ayt::editor::EditorAnimationClipboard clipboard;
    CHECK(document.copyAnimationKeyframes({"key.0.1"}, clipboard, &error));
    clipboard.skeletonPath = "different-skeleton";
    std::vector<std::string> ids{"keep"};
    CHECK(!document.pasteAnimationKeyframes(clipboard, .5, ids, &error));
    CHECK(!document.isDirty());
    const auto baked = animationExtensionFixtureRoot() / "Assets" / "readonly.baked.anm";
    CHECK(ayt::io::File::writeAllBytes(baked.string(),
        ayt::io::File::readAllBytes(document.path())));
    ayt::editor::EditorAnimationDocument readonly;
    CHECK(readonly.initialize({baked.string()}, error));
    CHECK(readonly.authoringReadOnly());
    CHECK(readonly.copyAnimationKeyframes({"key.0.1"}, clipboard, &error));
    const auto before = clipboard.data;
    CHECK(!readonly.cutAnimationKeyframes({"key.0.1"}, clipboard, &error));
    CHECK(clipboard.data.tracks.size() == before.tracks.size());
    CHECK(!readonly.setAnimationKeyframeValues("key.0.1", {1, 2, 3}));
    CHECK(!readonly.removeAnimationKeyframes({"key.0.1"}));
    CHECK(!readonly.isDirty());
}

TEST_CASE(clipboard_view_commands_route_to_shared_authoring_selection) {
    const auto descriptor = ayt::editor::makeEditorAnimationDescriptor();
    std::string error;
    const auto base = descriptor.createDocument({writeAnimationEditorClip().string()}, error);
    auto document = std::dynamic_pointer_cast<ayt::editor::EditorAnimationDocument>(base);
    AnimationExtensionHost host(animationExtensionFixtureRoot().string());
    const auto view = descriptor.createView(base, host);
    auto* target = view->commandTarget();
    CHECK(target->handlesCommand("edit.copy"));
    CHECK(target->executeCommand("edit.copy"));
    CHECK(document->setTimelinePositionSeconds(.5));
    CHECK(target->executeCommand("edit.paste"));
    CHECK(document->preview().animation()->getTrackKeyframeCount(0) == 4);
    CHECK(target->executeCommand("edit.cut"));
    CHECK(document->preview().animation()->getTrackKeyframeCount(0) == 3);
    CHECK(target->executeCommand("edit.undo"));
    CHECK(document->preview().animation()->getTrackKeyframeCount(0) == 4);
}

TEST_CASE(batch_cross_track_notify_edit_is_one_revision_and_undo) {
    auto document = std::make_shared<ayt::editor::EditorAnimationDocument>();
    std::string error;
    CHECK(document->initialize({writeAnimationEditorClip().string()}, error));
    CHECK(document->addAnimationTrack("root", "scale", ayt::resource::AnimTrackType::Vector3));
    CHECK(document->timelineAddKeyframe("animation.1", .5, 1));
    const auto before = document->preview().revision();
    std::vector<std::string> ids{"key.0.1", "key.1.1", "notify.0"};
    CHECK(document->transformAnimationKeyframes(ids, .5, 0, 0));
    CHECK(document->preview().revision() == before + 1);
    CHECK(document->preview().animation()->getTrackTimes(0)[1] == 3);
    CHECK(document->preview().animation()->getNotifyTime(0) == 1);
    CHECK(document->timelineUndo());
    CHECK(document->preview().animation()->getTrackTimes(0)[1] == 2);
    CHECK(document->preview().animation()->getNotifyTime(0) == .5f);
    CHECK(document->timelineRedo());
    CHECK(document->removeAnimationKeyframes(ids));
    CHECK(document->preview().animation()->getNotifyCount() == 0);
    CHECK(document->timelineUndo());
    CHECK(document->preview().animation()->getNotifyCount() == 1);
}

TEST_CASE(animation_descriptor_opens_full_preview_document)
{
    const auto descriptor = ayt::editor::makeEditorAnimationDescriptor();
    CHECK(descriptor.id == ayt::editor::kEditorAnimationTimelineExtensionId);
    CHECK(std::find(descriptor.extensions.begin(), descriptor.extensions.end(),
                    ".anm") != descriptor.extensions.end());
    std::string error;
    const auto base = descriptor.createDocument(
        ayt::editor::EditorOpenRequest{writeAnimationEditorClip().string()}, error);
    auto document = std::dynamic_pointer_cast<ayt::editor::EditorAnimationDocument>(base);
    CHECK(document != nullptr);
    CHECK(document->timelineDurationSeconds() == 2.0);
    CHECK(document->timelineTracks().size() == 2u);
    CHECK(document->timelineKeyframes().size() == 4u);
    CHECK_FALSE(document->isDirty());
    CHECK(document->save(&error));
    CHECK(error.empty());

    AnimationExtensionHost host(animationExtensionFixtureRoot().string());
    const auto view = descriptor.createView(base, host);
    CHECK(view != nullptr);
    CHECK(view != nullptr && view->rootWidget() != nullptr);
    CHECK(view != nullptr && view->commandTarget() != nullptr);
}

TEST_CASE(animation_tracks_and_timeline_are_editable_undoable_and_persistent)
{
    const auto path = writeAnimationEditorClip();
    ayt::editor::EditorAnimationDocument document;
    std::string error;
    CHECK(document.initialize(ayt::editor::EditorOpenRequest{path.string()}, error));
    CHECK(document.bindSkeleton(writeAnimationEditorSkeleton().string(), &error));
    CHECK(document.timelineTracks().size() == 2u);
    CHECK(document.timelineKeyframes().size() == 4u);

    CHECK(document.timelineAddKeyframe("animation.0", 0.25, 0.0));
    CHECK(document.isDirty());
    auto keys = document.timelineKeyframes();
    auto inserted = std::find_if(keys.begin(), keys.end(), [](const auto& key) {
        return key.trackId == "animation.0"
            && std::fabs(key.timeSeconds - 0.25) < 1.0e-6;
    });
    CHECK(inserted != keys.end());
    const std::string insertedId = inserted != keys.end() ? inserted->id : "";
    CHECK(document.timelineMoveKeyframe(insertedId, 0.75));
    CHECK(document.setTimelinePositionSeconds(0.75));
    CHECK(document.preview().poseWorldMatrices()[1]
        .transformPoint({0, 0, 0}).y < 1.5f);
    keys = document.timelineKeyframes();
    CHECK(std::any_of(keys.begin(), keys.end(), [](const auto& key) {
            return key.trackId == "animation.0"
                && std::fabs(key.timeSeconds - 0.75) < 1.0e-6;
        }));
    CHECK(document.timelineCanUndo());
    CHECK(document.timelineUndo());
    keys = document.timelineKeyframes();
    CHECK(std::any_of(keys.begin(), keys.end(), [](const auto& key) {
            return key.trackId == "animation.0"
                && std::fabs(key.timeSeconds - 0.25) < 1.0e-6;
        }));
    CHECK(document.timelineRedo());

    keys = document.timelineKeyframes();
    inserted = std::find_if(keys.begin(), keys.end(), [](const auto& key) {
        return key.trackId == "animation.0"
            && std::fabs(key.timeSeconds - 0.75) < 1.0e-6;
    });
    CHECK(inserted != keys.end());
    CHECK(document.timelineRemoveKeyframe(
        inserted != keys.end() ? inserted->id : ""));
    CHECK(document.addAnimationTrack("hips", "scale",
        ayt::resource::AnimTrackType::Vector3));
    CHECK(document.timelineTracks().size() == 3u);
    CHECK_FALSE(document.addAnimationTrack("hips", "scale",
        ayt::resource::AnimTrackType::Vector3));
    CHECK(document.removeAnimationTrack("animation.1"));
    CHECK(document.timelineTracks().size() == 2u);
    CHECK(document.timelineUndo());
    CHECK(document.timelineTracks().size() == 3u);

    const auto recovery = animationExtensionFixtureRoot() / "walk.recovery.anm";
    CHECK(document.writeRecoveryCopy(recovery.string(), &error));
    CHECK(std::filesystem::is_regular_file(recovery));
    CHECK(document.save(&error));
    CHECK_FALSE(document.isDirty());

    ayt::editor::EditorAnimationDocument reopened;
    CHECK(reopened.initialize(
        ayt::editor::EditorOpenRequest{path.string()}, error));
    CHECK(reopened.timelineTracks().size() == 3u);
    CHECK(reopened.timelineKeyframes().size() == 5u);
}

TEST_CASE(shared_numeric_fields_submit_to_document_and_reject_partial_input)
{
    const auto descriptor = ayt::editor::makeEditorAnimationDescriptor();
    std::string error;
    auto base = descriptor.createDocument(
        ayt::editor::EditorOpenRequest{writeAnimationEditorClip().string()}, error);
    auto document = std::dynamic_pointer_cast<ayt::editor::EditorAnimationDocument>(base);
    CHECK(document != nullptr);
    if (!document) return;
    AnimationExtensionHost host(animationExtensionFixtureRoot().string());
    auto view = descriptor.createView(base, host);
    CHECK(view != nullptr);
    if (!view) return;
    std::function<ayt::ui::Widget*(ayt::ui::Widget*, const std::string&)> findInput;
    findInput = [&](ayt::ui::Widget* root, const std::string& id) -> ayt::ui::Widget* {
        if (root->getId() == id) return root;
        for (auto* child : root->getChildren()) if (auto* found = findInput(child, id)) return found;
        return nullptr;
    };
    auto* x = dynamic_cast<ayt::ui::TextInput*>(findInput(view->rootWidget(), "animation_key_value_0"));
    CHECK(x != nullptr);
    if (!x) return;
    const auto id = document->timelineKeyframes().front().id;
    auto* undoButton = dynamic_cast<ayt::ui::Button*>(findInput(view->rootWidget(), "command_edit.undo"));
    CHECK(undoButton != nullptr);
    if (!undoButton) return;
    CHECK(!undoButton->isEnabled());
    std::vector<float> before, values;
    CHECK(document->animationKeyframeValues(id, before));
    x->setFocus(true);
    x->setText(L"3.25");
    CHECK(x->onKeyDown(13));
    CHECK(document->animationKeyframeValues(id, values));
    CHECK(values[0] == 3.25f);
    CHECK(undoButton->isEnabled());
    CHECK(view->commandTarget()->executeCommand("edit.undo"));
    view->tick(0);
    CHECK(document->animationKeyframeValues(id, values));
    CHECK(values == before);
    const auto revision = document->revision();
    x->setText(L"invalid");
    CHECK(x->onKeyDown(13));
    CHECK(document->revision() == revision);
    CHECK(document->animationKeyframeValues(id, values));
    CHECK(values == before);
    CHECK(document->setAnimationTrackInterpolation("animation.0", ayt::resource::AnimInterpolation::CubicHermite));
    view->tick(0);
    auto* tangent = dynamic_cast<ayt::ui::TextInput*>(findInput(view->rootWidget(), "animation_in_tangent_0"));
    CHECK(tangent != nullptr);
    if (!tangent) return;
    x->setFocus(false);
    tangent->setFocus(true);
    tangent->setText(L"1.75");
    CHECK(tangent->onKeyDown(13));
    std::vector<float> incoming, outgoing;
    CHECK(document->animationKeyframeTangents(id, incoming, outgoing));
    CHECK(incoming[0] == 1.75f);
    CHECK(outgoing[0] == 0.0f);
}

TEST_CASE(animation_keyframe_components_are_editable_normalized_and_undoable)
{
    const auto path = writeAnimationEditorClip();
    ayt::editor::EditorAnimationDocument document;
    std::string error;
    CHECK(document.initialize(ayt::editor::EditorOpenRequest{path.string()}, error));
    CHECK(document.bindSkeleton(writeAnimationEditorSkeleton().string(), &error));

    std::vector<float> values;
    ayt::resource::AnimTrackType type{};
    CHECK(document.animationKeyframeValues("key.0.1", values, &type));
    CHECK(type == ayt::resource::AnimTrackType::Vector3);
    const std::vector<float> original{0.0f, 2.0f, 0.0f};
    const std::vector<float> edited{3.0f, 4.0f, 5.0f};
    CHECK(values == original);
    CHECK(document.setAnimationKeyframeValues("key.0.1", edited));
    CHECK(document.animationKeyframeValues("key.0.1", values));
    CHECK(values == edited);
    CHECK(document.setTimelinePositionSeconds(1.0));
    const auto editedPosition = document.preview().poseWorldMatrices()[1]
        .transformPoint({0, 0, 0});
    CHECK(std::fabs(editedPosition.x - 3.0f) < 1.0e-5f);
    CHECK(std::fabs(editedPosition.y - 4.0f) < 1.0e-5f);
    CHECK(std::fabs(editedPosition.z - 5.0f) < 1.0e-5f);
    const std::vector<float> wrongWidth{1.0f, 2.0f};
    const std::vector<float> nonFinite{
        1.0f, std::numeric_limits<float>::infinity(), 2.0f};
    CHECK_FALSE(document.setAnimationKeyframeValues("key.0.1", wrongWidth));
    CHECK_FALSE(document.setAnimationKeyframeValues("key.0.1", nonFinite));
    CHECK(document.timelineUndo());
    CHECK(document.animationKeyframeValues("key.0.1", values));
    CHECK(values == original);
    CHECK(document.timelineRedo());
    CHECK(document.save(&error));

    ayt::editor::EditorAnimationDocument reopened;
    CHECK(reopened.initialize(
        ayt::editor::EditorOpenRequest{path.string()}, error));
    CHECK(reopened.animationKeyframeValues("key.0.1", values));
    CHECK(values == edited);

    CHECK(reopened.addAnimationTrack("hips", "rotation",
        ayt::resource::AnimTrackType::Quaternion));
    const std::vector<float> quaternionInput{0.0f, 0.0f, 2.0f, 0.0f};
    const std::vector<float> quaternionExpected{0.0f, 0.0f, 1.0f, 0.0f};
    const std::vector<float> zeroQuaternion{0.0f, 0.0f, 0.0f, 0.0f};
    CHECK(reopened.setAnimationKeyframeValues("key.1.0", quaternionInput));
    CHECK(reopened.animationKeyframeValues("key.1.0", values));
    CHECK(values == quaternionExpected);
    CHECK_FALSE(reopened.setAnimationKeyframeValues(
        "key.1.0", zeroQuaternion));
}

TEST_CASE(animation_curve_modes_and_tangents_drive_preview_and_persist)
{
    const auto path = writeAnimationEditorClip();
    ayt::editor::EditorAnimationDocument document;
    std::string error;
    CHECK(document.initialize(ayt::editor::EditorOpenRequest{path.string()}, error));
    CHECK(document.bindSkeleton(writeAnimationEditorSkeleton().string(), &error));

    ayt::resource::AnimInterpolation interpolation{};
    CHECK(document.animationTrackInterpolation("animation.0", interpolation));
    CHECK(interpolation == ayt::resource::AnimInterpolation::Linear);
    CHECK(document.setAnimationTrackInterpolation(
        "animation.0", ayt::resource::AnimInterpolation::Step));
    CHECK(document.setTimelinePositionSeconds(0.5));
    CHECK(std::fabs(document.preview().poseWorldMatrices()[1]
        .transformPoint({0, 0, 0}).y - 1.0f) < 1.0e-5f);

    CHECK(document.setAnimationTrackInterpolation(
        "animation.0", ayt::resource::AnimInterpolation::CubicHermite));
    std::vector<float> incoming;
    std::vector<float> outgoing;
    CHECK(document.animationKeyframeTangents(
        "key.0.0", incoming, outgoing));
    CHECK(incoming.size() == 3u);
    const std::vector<float> zero{0.0f, 0.0f, 0.0f};
    const std::vector<float> fastUp{0.0f, 8.0f, 0.0f};
    CHECK(document.setAnimationKeyframeTangents(
        "key.0.0", zero, fastUp));
    CHECK(document.animationKeyframeTangents("key.0.1", incoming, outgoing));
    CHECK(incoming == zero);
    CHECK(document.setTimelinePositionSeconds(0.5));
    CHECK(std::fabs(document.preview().poseWorldMatrices()[1]
        .transformPoint({0, 0, 0}).y - 2.5f) < 1.0e-5f);
    CHECK(document.autoAnimationTrackTangents("animation.0"));
    CHECK(document.timelineUndo());
    CHECK(document.setTimelinePositionSeconds(0.5));
    CHECK(std::fabs(document.preview().poseWorldMatrices()[1]
        .transformPoint({0, 0, 0}).y - 2.5f) < 1.0e-5f);
    CHECK(document.save(&error));

    ayt::editor::EditorAnimationDocument reopened;
    CHECK(reopened.initialize(
        ayt::editor::EditorOpenRequest{path.string()}, error));
    CHECK(reopened.animationTrackInterpolation("animation.0", interpolation));
    CHECK(interpolation == ayt::resource::AnimInterpolation::CubicHermite);
    CHECK(reopened.animationKeyframeTangents(
        "key.0.0", incoming, outgoing));
    CHECK(outgoing == fastUp);
    CHECK(reopened.timelineAddKeyframe("animation.0", 0.25, 0.0));
    auto keys = reopened.timelineKeyframes();
    auto inserted = std::find_if(keys.begin(), keys.end(), [](const auto& key) {
        return key.trackId == "animation.0"
            && std::fabs(key.timeSeconds - 0.25) < 1.0e-6;
    });
    CHECK(inserted != keys.end());
    std::string insertedId = inserted != keys.end() ? inserted->id : "";
    CHECK(reopened.animationKeyframeTangents(
        insertedId, incoming, outgoing));
    CHECK(incoming == zero);
    CHECK(outgoing == zero);
    CHECK(reopened.timelineMoveKeyframe(insertedId, 0.75));
    keys = reopened.timelineKeyframes();
    inserted = std::find_if(keys.begin(), keys.end(), [](const auto& key) {
        return key.trackId == "animation.0"
            && std::fabs(key.timeSeconds - 0.75) < 1.0e-6;
    });
    CHECK(inserted != keys.end());
    insertedId = inserted != keys.end() ? inserted->id : "";
    CHECK(reopened.timelineRemoveKeyframe(insertedId));
    CHECK(reopened.animationKeyframeTangents(
        "key.0.0", incoming, outgoing));
    CHECK(outgoing == fastUp);
}

TEST_CASE(animation_preview_bindings_use_project_editor_metadata)
{
    ayt::editor::EditorAnimationDocument document;
    std::string error;
    CHECK(document.initialize(
        ayt::editor::EditorOpenRequest{writeAnimationEditorClip().string()}, error));
    std::error_code cleanupError;
    std::filesystem::remove_all(
        animationExtensionFixtureRoot() / ".ayeditor", cleanupError);
    document.configureProjectRoot(animationExtensionFixtureRoot().string());
    CHECK(document.bindSkeleton(writeAnimationEditorSkeleton().string(), &error));
    CHECK(document.bindMesh(writeAnimationEditorMesh().string(), &error));
    document.setPreviewMode(
        ayt::anim::editor::AnimationPreviewMode::ModelAndSkeleton);
    CHECK(document.preview().canPreviewModel());
    CHECK(std::filesystem::is_regular_file(document.metadataPath()));
    CHECK(document.metadataPath().find(".ayeditor") != std::string::npos);
    const auto metadata = nlohmann::json::parse(
        ayt::io::File::readAllText(document.metadataPath()));
    CHECK(metadata["version"] == 2);
    CHECK(metadata["bindings"].contains("Assets/walk.anm"));
    CHECK(metadata["bindings"]["Assets/walk.anm"]["skeleton"]
        == "Assets/hero.ayskel");
    CHECK(metadata["bindings"]["Assets/walk.anm"]["mesh"]
        == "Assets/hero.aymesh");
    CHECK(document.path().find(".anm") != std::string::npos);
    CHECK_FALSE(std::filesystem::is_regular_file(document.path() + ".timeline.json"));

    ayt::editor::EditorAnimationDocument reopened;
    CHECK(reopened.initialize(
        ayt::editor::EditorOpenRequest{document.path()}, error));
    reopened.configureProjectRoot(animationExtensionFixtureRoot().string());
    CHECK(reopened.preview().canPreviewModel());
}

TEST_CASE(animation_canvas_renders_model_and_skeleton_as_one_retained_preview)
{
    auto document = std::make_shared<ayt::editor::EditorAnimationDocument>();
    std::string error;
    CHECK(document->initialize(
        ayt::editor::EditorOpenRequest{writeAnimationEditorClip().string()}, error));
    CHECK(document->bindSkeleton(writeAnimationEditorSkeleton().string(), &error));
    CHECK(document->bindMesh(writeAnimationEditorMesh().string(), &error));
    document->setPreviewMode(
        ayt::anim::editor::AnimationPreviewMode::ModelAndSkeleton);

    ayt::editor::EditorAnimationCanvas canvas(document);
    canvas.setSize({640.0f, 480.0f});
    ayt::ui::MockRenderer renderer;
    renderer.beginFrame();
    canvas.render(renderer);
    const auto pathCalls = std::count_if(renderer.getDrawCalls().begin(),
        renderer.getDrawCalls().end(), [](const auto& call) {
            return call.type == ayt::ui::MockRenderer::DrawCall::Path;
        });
    CHECK(pathCalls == 2);
    CHECK(canvas.hasCachedDisplayList());
}

TEST_CASE(animation_curve_canvas_uses_transactional_live_edits)
{
    auto document = std::make_shared<ayt::editor::EditorAnimationDocument>();
    std::string error;
    CHECK(document->initialize(
        ayt::editor::EditorOpenRequest{writeAnimationEditorClip().string()}, error));

    ayt::editor::EditorAnimationCurveTrack track;
    CHECK(document->animationCurveTrack("animation.0", track));
    CHECK(track.keys.size() == 3u);
    CHECK(track.keys[1].values == std::vector<float>({0.0f, 2.0f, 0.0f}));
    CHECK(track.ticksPerSecond == 2.0);

    CHECK(document->beginAnimationEditGesture("Drag animation key"));
    std::string keyId = "key.0.1";
    CHECK(document->updateAnimationKeyframe(
        keyId, 0.75, {1.0f, 2.5f, 3.0f}));
    CHECK(document->updateAnimationKeyframe(
        keyId, 0.5, {2.0f, 3.0f, 4.0f}));
    CHECK(document->commitAnimationEditGesture());
    CHECK(document->timelineCanUndo());
    CHECK(document->timelineUndo());
    CHECK(document->animationCurveTrack("animation.0", track));
    CHECK(std::fabs(track.keys[1].timeSeconds - 1.0) < 1.0e-6);
    CHECK(track.keys[1].values == std::vector<float>({0.0f, 2.0f, 0.0f}));
    CHECK(document->timelineRedo());
    CHECK(document->animationCurveTrack("animation.0", track));
    CHECK(std::fabs(track.keys[1].timeSeconds - 0.5) < 1.0e-6);
    CHECK(track.keys[1].values == std::vector<float>({2.0f, 3.0f, 4.0f}));

    ayt::editor::EditorAnimationCurveCanvas canvas(document);
    canvas.setTrackId("animation.0");
    canvas.setSize({640.0f, 220.0f});
    ayt::ui::MockRenderer renderer;
    renderer.beginFrame();
    canvas.render(renderer);
    CHECK(std::count_if(renderer.getDrawCalls().begin(),
        renderer.getDrawCalls().end(), [](const auto& call) {
            return call.type == ayt::ui::MockRenderer::DrawCall::Path;
        }) >= 3);
}

TEST_CASE(animation_curve_multi_key_edits_are_atomic_and_conflict_safe)
{
    auto document = std::make_shared<ayt::editor::EditorAnimationDocument>();
    std::string error;
    CHECK(document->initialize(
        ayt::editor::EditorOpenRequest{writeAnimationEditorClip().string()}, error));
    std::vector<std::string> keys{"key.0.0", "key.0.1"};
    CHECK(document->transformAnimationKeyframes(keys, 0.25, 1u, 1.0f));
    ayt::editor::EditorAnimationCurveTrack track;
    CHECK(document->animationCurveTrack("animation.0", track));
    CHECK(std::fabs(track.keys[0].timeSeconds - 0.25) < 1.0e-6);
    CHECK(std::fabs(track.keys[1].timeSeconds - 1.25) < 1.0e-6);
    CHECK(track.keys[0].values[1] == 2.0f);
    CHECK(track.keys[1].values[1] == 3.0f);
    const auto conflicting = keys;
    CHECK_FALSE(document->transformAnimationKeyframes(keys, 0.75, 0u, 0.0f));
    CHECK(keys == conflicting);
    CHECK(document->timelineUndo());
    CHECK(document->animationCurveTrack("animation.0", track));
    CHECK(std::fabs(track.keys[0].timeSeconds) < 1.0e-6);
    CHECK(std::fabs(track.keys[1].timeSeconds - 1.0) < 1.0e-6);

    ayt::editor::EditorAnimationCurveCanvas canvas(document);
    canvas.setTrackId("animation.0");
    canvas.selectAllKeys();
    CHECK(canvas.selectedKeyCount() == 3u);
    CHECK(canvas.deleteSelectedKeys());
    CHECK(document->animationCurveTrack("animation.0", track));
    CHECK(track.keys.empty());
    CHECK(document->timelineUndo());
    CHECK(document->animationCurveTrack("animation.0", track));
    CHECK(track.keys.size() == 3u);
}

TEST_CASE(animation_dope_sheet_drags_keys_with_one_undo_step)
{
    auto document = std::make_shared<ayt::editor::EditorAnimationDocument>();
    std::string error;
    CHECK(document->initialize(
        ayt::editor::EditorOpenRequest{writeAnimationEditorClip().string()}, error));
    ayt::editor::EditorAnimationDopeSheet sheet(document);
    sheet.setSize({640.0f, 150.0f});
    ayt::ui::MockRenderer renderer;
    renderer.beginFrame();
    sheet.render(renderer);
    CHECK(!renderer.getDrawCalls().empty());

    CHECK(sheet.onMouseButtonDown({{404.0f, 34.0f}, 0}));
    CHECK(sheet.onMouseMove({{522.0f, 34.0f}, 0}));
    CHECK(sheet.onMouseButtonUp({{522.0f, 34.0f}, 0}));
    ayt::editor::EditorAnimationCurveTrack track;
    CHECK(document->animationCurveTrack("animation.0", track));
    CHECK(std::fabs(track.keys[1].timeSeconds - 1.5) < 1.0e-6);
    CHECK(document->timelineUndo());
    CHECK(document->animationCurveTrack("animation.0", track));
    CHECK(std::fabs(track.keys[1].timeSeconds - 1.0) < 1.0e-6);
}

TEST_CASE(animation_notifies_are_authored_dragged_undoable_and_persistent)
{
    const auto path = writeAnimationEditorClip();
    auto document = std::make_shared<ayt::editor::EditorAnimationDocument>();
    std::string error;
    CHECK(document->initialize(
        ayt::editor::EditorOpenRequest{path.string()}, error));
    auto notifies = document->animationNotifies();
    CHECK(notifies.size() == 1u);
    CHECK(notifies[0].name == "footstep");
    CHECK(document->addAnimationNotify("land", 1.5, 0.8f));
    notifies = document->animationNotifies();
    CHECK(notifies.size() == 2u);
    std::string edited = "notify.0";
    CHECK(document->updateAnimationNotify(edited, "step", 1.75, 0.5f));
    CHECK(edited == "notify.1");
    notifies = document->animationNotifies();
    CHECK(notifies[1].name == "step");
    CHECK(notifies[1].payload == 0.5f);
    CHECK(document->removeAnimationNotify("notify.0"));
    CHECK(document->animationNotifies().size() == 1u);
    CHECK(document->timelineUndo());
    CHECK(document->animationNotifies().size() == 2u);
    ayt::editor::EditorAnimationDopeSheet sheet(document);
    sheet.setSize({640.0f, 170.0f});
    std::string selectedNotify;
    int editCallbacks = 0;
    sheet.setOnSelectionChanged([&](const std::string&,
                                    const std::string& keyId) {
        selectedNotify = keyId;
    });
    sheet.setOnEdited([&]() { ++editCallbacks; });
    ayt::ui::MockRenderer renderer;
    renderer.beginFrame();
    sheet.render(renderer);
    CHECK(sheet.onMouseButtonDown({{522.0f, 58.0f}, 0}));
    CHECK(selectedNotify == "notify.0");
    CHECK(sheet.onMouseMove({{404.0f, 58.0f}, 0}));
    CHECK(sheet.onMouseButtonUp({{404.0f, 58.0f}, 0}));
    CHECK(editCallbacks > 0);
    notifies = document->animationNotifies();
    CHECK(std::fabs(notifies[0].timeSeconds - 1.0) < 1.0e-6);
    CHECK(document->timelineUndo());
    CHECK(document->save(&error));

    ayt::editor::EditorAnimationDocument reopened;
    CHECK(reopened.initialize(
        ayt::editor::EditorOpenRequest{path.string()}, error));
    notifies = reopened.animationNotifies();
    CHECK(notifies.size() == 2u);
    CHECK(notifies[1].name == "step");
    CHECK(std::fabs(notifies[1].timeSeconds - 1.75) < 1.0e-6);
}

TEST_CASE(animation_clip_properties_preserve_seconds_and_guard_content)
{
    const auto path = writeAnimationEditorClip();
    ayt::editor::EditorAnimationDocument document;
    std::string error;
    CHECK(document.initialize(
        ayt::editor::EditorOpenRequest{path.string()}, error));
    auto properties = document.animationClipProperties();
    CHECK(properties.name == "walk");
    CHECK(properties.durationSeconds == 2.0);
    CHECK(properties.ticksPerSecond == 2.0);
    CHECK_FALSE(document.setAnimationClipProperties(
        {"walk", 1.5, 2.0}, &error));
    CHECK(!error.empty());

    CHECK(document.setAnimationClipProperties(
        {"walk_extended", 3.0, 4.0}, &error));
    CHECK(error.empty());
    properties = document.animationClipProperties();
    CHECK(properties.name == "walk_extended");
    CHECK(properties.durationSeconds == 3.0);
    CHECK(properties.ticksPerSecond == 4.0);
    ayt::editor::EditorAnimationCurveTrack track;
    CHECK(document.animationCurveTrack("animation.0", track));
    CHECK(track.ticksPerSecond == 4.0);
    CHECK(std::fabs(track.keys[0].timeSeconds) < 1.0e-6);
    CHECK(std::fabs(track.keys[1].timeSeconds - 1.0) < 1.0e-6);
    CHECK(std::fabs(track.keys[2].timeSeconds - 2.0) < 1.0e-6);
    CHECK(document.timelineDurationSeconds() == 3.0);
    CHECK(document.timelineUndo());
    CHECK(document.animationClipProperties().ticksPerSecond == 2.0);
    CHECK(document.timelineRedo());
    CHECK(document.save(&error));

    ayt::editor::EditorAnimationDocument reopened;
    CHECK(reopened.initialize(
        ayt::editor::EditorOpenRequest{path.string()}, error));
    properties = reopened.animationClipProperties();
    CHECK(properties.name == "walk_extended");
    CHECK(properties.durationSeconds == 3.0);
    CHECK(properties.ticksPerSecond == 4.0);
    CHECK(reopened.animationNotifies().size() == 1u);
    CHECK(std::fabs(reopened.animationNotifies()[0].timeSeconds - 0.5)
        < 1.0e-6);
}

TEST_CASE(common_curve_source_caches_immutable_revisions_and_samples_rotation)
{
    auto document = std::make_shared<ayt::editor::EditorAnimationDocument>();
    std::string error;
    CHECK(document->initialize({writeAnimationEditorClip().string()}, error));
    CHECK(document->bindSkeleton(writeAnimationEditorSkeleton().string(), &error));
    CHECK(document->addAnimationTrack("hips", "rotation", ayt::resource::AnimTrackType::Quaternion));
    CHECK(document->timelineAddKeyframe("animation.1", 1.0, 0.0));
    const float half = std::sqrt(0.5f);
    CHECK(document->setAnimationKeyframeValues("key.1.1", {0, 0, half, half}));
    const auto source = ayt::editor::makeAnimationCurveSource(document);
    const auto first = source->curveTrack("animation.1");
    CHECK(first != nullptr);
    CHECK(source->curveTrack("animation.1") == first);
    const auto timeline = source->timelineSnapshot();
    CHECK(source->seek(0.25));
    CHECK(source->timelineSnapshot() == timeline);
    CHECK(source->curveTrack("animation.1") == first);
    const float z = first->sample(2u, 0.5);
    const float w = first->sample(3u, 0.5);
    CHECK(std::fabs(z - std::sin(3.1415926535f / 8.0f)) < 1e-5f);
    CHECK(std::fabs(z * z + w * w - 1.0f) < 1e-5f);
    CHECK(document->setAnimationKeyframeValues("key.1.1", {0, 0, 0, 1}));
    const auto changed = source->curveTrack("animation.1");
    CHECK(changed != first);
    CHECK(source->timelineSnapshot() != timeline);
    CHECK(std::fabs(changed->sample(2u, 0.5)) < 1e-5f);
    CHECK(std::fabs(first->sample(2u, 0.5) - z) < 1e-5f);
    CHECK(document->timelineUndo());
    CHECK(std::fabs(source->curveTrack("animation.1")->sample(2u, 0.5) - z) < 1e-5f);
}

TEST_CASE(authoring_selection_bridge_uses_workspace_ids_and_recovers_after_close)
{
    ayt::editor::EditorWorkspace workspace;
    CHECK(workspace.registry().registerEditor(ayt::editor::makeEditorAnimationDescriptor()));
    const auto opened = workspace.documents().open({writeAnimationEditorClip().string()});
    CHECK(static_cast<bool>(opened));
    if (!opened) return;
    ayt::editor::EditorAuthoringSelectionBridge bridge(workspace, opened.document);
    auto* context = workspace.selections().find(opened.documentId);
    CHECK(bridge.context() == context);
    CHECK(context != nullptr);
    if (!context) return;
    int notices = 0;
    const auto listener = context->addListener([&] { ++notices; });
    ayt::ui::authoring::TimelineSelection selection{"opaque-track", "second", {"first", "second"}, 2};
    CHECK(bridge.publish(selection));
    CHECK(context->primary()->documentId == opened.documentId);
    CHECK(context->primary()->objectId == "second");
    CHECK(context->items().size() == 2u);
    CHECK(!bridge.publish(selection)); CHECK(notices == 1);
    context->removeListener(listener);
    CHECK(static_cast<bool>(workspace.documents().close(opened.documentId, ayt::editor::EditorDocumentCloseAction::Discard)));
    CHECK(bridge.context()->documentId() != opened.documentId);
    CHECK(bridge.publish("timeline.key", {}, "" ) == false);
    CHECK(bridge.context()->empty());
}

TEST_CASE(command_buttons_follow_current_target_and_disable_stale_actions)
{
    class Target final : public ayt::editor::IEditorCommandTarget {
    public:
        bool enabled = true; int executions = 0;
        bool handlesCommand(const std::string& id) const override { return id == "test.action"; }
        bool canExecuteCommand(const std::string& id) const override { return handlesCommand(id) && enabled; }
        bool executeCommand(const std::string& id) override {
            if (!canExecuteCommand(id)) return false;
            ++executions; return true;
        }
    } first, second;
    ayt::editor::IEditorCommandTarget* active = &first;
    auto* row = new ayt::ui::HBox();
    ayt::ui::Button* retained = nullptr;
    {
        ayt::editor::EditorCommandButtons commands([&] { return active; });
        retained = commands.add(*row, L"Action", "test.action", 80);
        retained->setSize({80, 28});
        CHECK(retained->isEnabled());
        CHECK(commands.invoke("test.action")); CHECK(first.executions == 1);
        first.enabled = false; commands.refresh();
        CHECK(!retained->isEnabled());
        CHECK(!commands.invoke("test.action")); CHECK(first.executions == 1);
        active = &second; commands.refresh();
        CHECK(retained->isEnabled());
        retained->onMouseMove({{10, 10}, 0});
        retained->onMouseButtonDown({{10, 10}, 0});
        retained->onMouseButtonUp({{10, 10}, 0});
        CHECK(second.executions == 1);
        CHECK(!commands.invoke("unknown"));
        active = nullptr; commands.refresh();
        CHECK(!retained->isEnabled());
        active = &second; commands.refresh();
    }
    // A hosted tree may outlive the binding object; its closure is inert.
    retained->onMouseMove({{10, 10}, 0});
    retained->onMouseButtonDown({{10, 10}, 0});
    retained->onMouseButtonUp({{10, 10}, 0});
    CHECK(second.executions == 1);
    ayt::ui::destroyWidgetTree(row);
}
TEST_SUITE_END

TEST_SUITE(AYEditor_PlaybackAdapter)
TEST_CASE(contextual_adapter_switches_owner_without_ticking_or_retaining_old_documents)
{
    struct Owner final : ayt::editor::IEditorTimelineSource {
        double position = 0;
        bool playing = false;
        int ticks = 0;
        double timelineDurationSeconds() const noexcept override { return 1.0; }
        double timelinePositionSeconds() const noexcept override { return position; }
        bool setTimelinePositionSeconds(double value) override { position = value; return true; }
        bool timelinePlaying() const noexcept override { return playing; }
        void timelinePlay() override { playing = true; }
        void timelinePause() override { playing = false; }
        void timelineTick(double) override { ++ticks; }
    };
    auto first = std::make_shared<Owner>();
    auto second = std::make_shared<Owner>();
    std::shared_ptr<ayt::editor::IEditorTimelineSource> current = first;
    ayt::editor::TimelinePlaybackSource source([&] { return current; });
    CHECK(source.playbackState().available);
    source.play();
    CHECK(first->playing);
    CHECK(source.seek(0.25));
    CHECK(first->position == 0.25);
    CHECK(first->ticks == 0);
    current = second;
    source.play();
    CHECK(second->playing);
    CHECK(source.seek(0.75));
    CHECK(second->position == 0.75);
    CHECK(first->position == 0.25);
    std::weak_ptr<Owner> previous = first;
    first.reset();
    CHECK(previous.expired());
    current.reset();
    CHECK(!source.playbackState().available);
    CHECK(!source.seek(0.5));
    source.play();
    CHECK(second->ticks == 0);
}
TEST_SUITE_END
