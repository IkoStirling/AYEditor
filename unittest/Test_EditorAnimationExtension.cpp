#include "AYEditor/EditorAnimationDocument.h"
#include "AYEditor/EditorAnimationExtension.h"
#include "AYEditor/EditorBuiltInExtensions.h"
#include "AYEditor/EditorWorkspace.h"
#include "../src/AYEditorAnimationCanvas.h"

#include <AYIO/File.h>
#include <AYResource/assetsImpl/Animation.h>
#include <AYResource/assetsImpl/Mesh.h>
#include <AYResource/assetsImpl/Skeleton.h>
#include <AYUI/MockRenderer.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
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

TEST_SUITE_END
