#include "AYTest.h"
#include "AYEditor/EditorAnimationDocument.h"
#include "AYEditor/EditorAnimationExtension.h"
#include "AYEditor/EditorBuiltInExtensions.h"
#include "AYEditor/EditorWorkspace.h"
#include "../src/AYEditorAnimationCanvas.h"
#include "../src/AYEditorAnimationCurveCanvas.h"
#include "../src/AYEditorAnimationCurveSource.h"
#include "../src/AYEditorAnimationDopeSheet.h"
#include "../src/AYEditorTimelinePlaybackSource.h"

#include <AYIO/File.h>
#include <AYResource/assetsImpl/Animation.h>
#include <AYResource/assetsImpl/Mesh.h>
#include <AYResource/assetsImpl/Skeleton.h>
#include <AYUI/MockRenderer.h>
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
    const auto path = animationExtensionFixtureRoot() / "Assets" / "walk.ayanm";
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

} // namespace

TEST_SUITE(AYEditor_AnimationExtension)

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
    const auto baked = animationExtensionFixtureRoot() / "Assets" / "readonly.baked.ayanm";
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
                    ".ayanm") != descriptor.extensions.end());
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

    const auto recovery = animationExtensionFixtureRoot() / "walk.recovery.ayanm";
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
    CHECK(metadata["bindings"].contains("Assets/walk.ayanm"));
    CHECK(metadata["bindings"]["Assets/walk.ayanm"]["skeleton"]
        == "Assets/hero.ayskel");
    CHECK(metadata["bindings"]["Assets/walk.ayanm"]["mesh"]
        == "Assets/hero.aymesh");
    CHECK(document.path().find(".ayanm") != std::string::npos);
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
