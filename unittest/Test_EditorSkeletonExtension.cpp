#include "AYEditor/EditorAssetTilePresenter.h"
#include "AYEditor/EditorSkeletonDocument.h"
#include "AYEditor/EditorSkeletonExtension.h"
#include "AYEditor/EditorWorkspace.h"

#include <AYIO/File.h>
#include <AYResource/assetsImpl/Animation.h>
#include <AYResource/assetsImpl/Skeleton.h>

#include <filesystem>
#include <memory>

namespace {

std::filesystem::path skeletonExtensionFixtureRoot()
{
    const auto root = std::filesystem::temp_directory_path()
        / "ay_editor_skeleton_extension";
    std::error_code ignored;
    std::filesystem::create_directories(root, ignored);
    return root;
}

std::filesystem::path writeEditorSkeletonFixture()
{
    ayt::resource::Skeleton skeleton;
    const struct BoneDef { const char* name; int parent; float x; float y; } defs[] = {
        {"sceneRoot", -1, 0, 0}, {"motionRoot", 0, 0, 0},
        {"hips", 1, 0, 1}, {"spine", 2, 0, 1}, {"head", 3, 0, 1},
        {"leftUpperArm", 3, -1, 0}, {"leftLowerArm", 5, -1, 0},
        {"leftHand", 6, -1, 0}, {"rightUpperArm", 3, 1, 0},
        {"rightLowerArm", 8, 1, 0}, {"rightHand", 9, 1, 0},
        {"leftUpperLeg", 2, -0.4f, -1}, {"leftLowerLeg", 11, 0, -1},
        {"leftFoot", 12, 0, -1}, {"rightUpperLeg", 2, 0.4f, -1},
        {"rightLowerLeg", 14, 0, -1}, {"rightFoot", 15, 0, -1},
    };
    for (const BoneDef& def : defs) {
        ayt::resource::Bone bone;
        bone.name = def.name;
        bone.parentIndex = def.parent;
        bone.localPosition = {def.x, def.y, 0.0f};
        bone.localRotation = ayt::math::FQuaternion::identity();
        bone.localScale = {1, 1, 1};
        bone.inverseBindMatrix = ayt::math::Float4x4::identity();
        skeleton.addBone(bone);
    }
    std::vector<ayt::math::UInt8> bytes;
    CHECK(skeleton.saveToBinary(bytes));
    const auto path = skeletonExtensionFixtureRoot() / "editor_fixture.ayskel";
    CHECK(ayt::io::File::writeAllBytes(path.string(), bytes));
    return path;
}

std::filesystem::path writeEditorAnimationFixture()
{
    ayt::resource::Animation animation;
    animation.setName("editor_fixture");
    animation.setDuration(1.0f);
    animation.setTicksPerSecond(1.0f);
    ayt::resource::AnimTrack track;
    track.nodeName = "hips";
    track.property = "position";
    track.valueType = ayt::resource::AnimTrackType::Vector3;
    track.times = {0.0f, 1.0f};
    track.values = {0, 1, 0, 0, 2, 0};
    animation.addTrack(track);
    std::vector<ayt::math::UInt8> bytes;
    CHECK(animation.saveToBinary(bytes));
    const auto path = skeletonExtensionFixtureRoot() / "editor_fixture.ayanm";
    CHECK(ayt::io::File::writeAllBytes(path.string(), bytes));
    return path;
}

class SkeletonExtensionHost final : public ayt::editor::IEditorHostServices {
public:
    ayt::editor::EditorWorkspace& workspace() noexcept override {
        return _workspace;
    }
    const std::string& projectRoot() const noexcept override { return _root; }
    void requestRepaint() override { ++repaintCount; }
    void setStatusText(const std::wstring& text) override { status = text; }

    int repaintCount = 0;
    std::wstring status;

private:
    ayt::editor::EditorWorkspace _workspace;
    std::string _root;
};

} // namespace

TEST_SUITE(AYEditor_SkeletonExtension)

TEST_CASE(skeleton_descriptor_creates_thin_document_and_workspace)
{
    auto skeletonPath = writeEditorSkeletonFixture();
    auto mappingPath = skeletonPath;
    mappingPath.replace_extension(".aysmap");
    std::error_code ignored;
    std::filesystem::remove(mappingPath, ignored);

    const ayt::editor::EditorDescriptor descriptor =
        ayt::editor::makeEditorSkeletonDescriptor();
    CHECK(descriptor.id == ayt::editor::kEditorSkeletonExtensionId);
    CHECK(descriptor.extensions.size() == 2u);

    std::string error;
    const auto document = descriptor.createDocument(
        ayt::editor::EditorOpenRequest{skeletonPath.string()}, error);
    CHECK(document != nullptr);
    auto skeleton = std::dynamic_pointer_cast<ayt::editor::EditorSkeletonDocument>(
        document);
    CHECK(skeleton != nullptr);
    CHECK(skeleton->core().bones().size() == 17u);
    CHECK(skeleton->core().applyCanonicalNameTemplate());
    CHECK(skeleton->core().validation().isValid());
    CHECK(skeleton->isDirty());
    CHECK(skeleton->executeCommand("edit.undo"));
    CHECK(skeleton->executeCommand("edit.redo"));

    SkeletonExtensionHost host;
    auto view = descriptor.createView(document, host);
    CHECK(view != nullptr);
    CHECK(view != nullptr && view->rootWidget() != nullptr);
    CHECK(view != nullptr && view->commandTarget() == skeleton.get());
}

TEST_CASE(skeleton_document_exposes_animation_through_shared_timeline)
{
    ayt::editor::EditorSkeletonDocument document;
    std::string error;
    CHECK(document.initialize(
        ayt::editor::EditorOpenRequest{writeEditorSkeletonFixture().string()}, error));
    CHECK(document.core().attachAnimation(
        writeEditorAnimationFixture().string(), &error));
    CHECK(document.timelineDurationSeconds() == 1.0);
    CHECK(document.timelineTracks().size() == 1u);
    CHECK(document.timelineKeyframes().size() == 2u);
    CHECK(document.setTimelinePositionSeconds(0.5));
    document.timelinePlay();
    CHECK(document.timelinePlaying());
    document.timelinePause();
    CHECK_FALSE(document.timelinePlaying());
}

TEST_CASE(skeleton_asset_tile_keeps_mapping_and_bake_status_separate)
{
    const auto skeletonPath = writeEditorSkeletonFixture();
    ayt::editor::EditorAssetRecord record;
    record.id = 7;
    record.name = skeletonPath.filename().string();
    record.logicalPath = "Assets/" + record.name;
    record.absolutePath = skeletonPath.string();
    record.runtimePath = record.absolutePath;
    record.type = ayt::editor::EditorAssetType::Skeleton;

    const auto presentation = ayt::editor::EditorAssetTilePresenter().present(record);
    CHECK_FALSE(presentation.adaptationBadge.empty());
    CHECK_FALSE(presentation.bakeBadge.empty());
    CHECK(presentation.adaptationBadge != presentation.bakeBadge);
    CHECK(presentation.typeAbbreviation.find(presentation.adaptationBadge)
        != std::wstring::npos);
    CHECK(presentation.typeAbbreviation.find(presentation.bakeBadge)
        == std::wstring::npos);
}

TEST_SUITE_END
