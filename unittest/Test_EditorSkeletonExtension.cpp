#include "AYEditor/EditorAssetTilePresenter.h"
#include "AYEditor/EditorSkeletonDocument.h"
#include "AYEditor/EditorSkeletonExtension.h"
#include "AYEditor/EditorWorkspace.h"
#include "../src/AYEditorSkeletonCanvas.h"

#include <AYIO/File.h>
#include <AYResource/assetsImpl/Animation.h>
#include <AYResource/assetsImpl/Skeleton.h>
#include <AYUI/MockRenderer.h>
#include <AYUI/ListView.h>
#include <AYUI/TextInput.h>
#include <nlohmann/json.hpp>

#include <algorithm>
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

ayt::ui::Widget* findSkeletonWidget(ayt::ui::Widget* root,
                                    const std::string& id)
{
    if (root == nullptr) return nullptr;
    if (root->getId() == id) return root;
    for (ayt::ui::Widget* child : root->getChildren()) {
        if (auto* found = findSkeletonWidget(child, id)) return found;
    }
    return nullptr;
}

} // namespace

TEST_SUITE(AYEditor_SkeletonExtension)

TEST_CASE(skeleton_descriptor_creates_thin_document_and_workspace)
{
    auto skeletonPath = writeEditorSkeletonFixture();
    auto legacyMappingPath = skeletonPath;
    legacyMappingPath.replace_extension(".aysmap");
    auto rigProfilePath = skeletonPath;
    rigProfilePath.replace_extension(".ayrig");
    std::error_code ignored;
    std::filesystem::remove(legacyMappingPath, ignored);
    std::filesystem::remove(rigProfilePath, ignored);

    const ayt::editor::EditorDescriptor descriptor =
        ayt::editor::makeEditorSkeletonDescriptor();
    CHECK(descriptor.id == ayt::editor::kEditorSkeletonExtensionId);
    CHECK(descriptor.extensions.size() == 3u);
    CHECK(std::find(descriptor.extensions.begin(), descriptor.extensions.end(),
                    ".ayrig") != descriptor.extensions.end());

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
    auto* search = dynamic_cast<ayt::ui::TextInput*>(findSkeletonWidget(
        view->rootWidget(), "skeleton_bone_search"));
    auto* bones = dynamic_cast<ayt::ui::ListView*>(findSkeletonWidget(
        view->rootWidget(), "skeleton_bone_list"));
    auto* roles = dynamic_cast<ayt::ui::ListView*>(findSkeletonWidget(
        view->rootWidget(), "skeleton_role_list"));
    CHECK(search != nullptr);
    CHECK(bones != nullptr && bones->getItemCount() == 17u);
    CHECK(roles != nullptr
        && roles->getItemCount() == ayt::anim::kHumanoidBoneCount);
    CHECK(bones != nullptr && bones->isDraggable());
    CHECK(roles != nullptr && roles->isAcceptDrops());
    CHECK(roles != nullptr && roles->acceptsKind("SkeletonBone"));
    if (search != nullptr && bones != nullptr) {
        search->setText(L"head");
        CHECK(bones->getItemCount() == 1u);
    }
    CHECK(findSkeletonWidget(
        view->rootWidget(), "skeleton_retarget_target") != nullptr);
    CHECK(findSkeletonWidget(
        view->rootWidget(), "skeleton_retarget_platform") != nullptr);
    CHECK(findSkeletonWidget(
        view->rootWidget(), "skeleton_target_bone_picker") != nullptr);
    CHECK(findSkeletonWidget(
        view->rootWidget(), "skeleton_correction_kind") != nullptr);
    CHECK(findSkeletonWidget(
        view->rootWidget(), "skeleton_correction_quaternion") != nullptr);
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

TEST_CASE(skeleton_document_persists_explicit_profile_binding_and_discovers_templates)
{
    const auto projectRoot = skeletonExtensionFixtureRoot();
    const auto skeletonPath = writeEditorSkeletonFixture();
    const auto profiles = projectRoot / "Profiles";
    std::error_code ignored;
    std::filesystem::remove_all(projectRoot / ".ayeditor", ignored);
    std::filesystem::remove_all(profiles, ignored);
    std::filesystem::create_directories(profiles, ignored);

    std::string error;
    ayt::anim::editor::SkeletonEditorCore complete;
    CHECK(complete.open(skeletonPath.string(), &error));
    CHECK(complete.applyCanonicalNameTemplate());
    const auto completePath = profiles / "complete.ayrig";
    CHECK(complete.saveMappingAs(completePath.string(), &error));

    ayt::anim::editor::SkeletonEditorCore minimal;
    CHECK(minimal.open(skeletonPath.string(), &error));
    CHECK(minimal.bind(ayt::anim::HumanoidBone::Hips, 2));
    const auto minimalPath = profiles / "minimal.ayrig";
    CHECK(minimal.saveMappingAs(minimalPath.string(), &error));

    const auto templatePath = profiles / "canonical-names.ayrig";
    const nlohmann::json rigTemplate = {
        {"type", "RigProfile"}, {"version", 1},
        {"id", "template-editor-test"}, {"kind", "template"},
        {"name", "Canonical names"},
        {"roles", {{"spine", "spine"}, {"head", "head"}}},
    };
    CHECK(ayt::io::File::writeAllText(
        templatePath.string(), rigTemplate.dump(2) + "\n"));

    ayt::editor::EditorSkeletonDocument first;
    CHECK(first.initialize(
        ayt::editor::EditorOpenRequest{skeletonPath.string()}, error));
    first.configureProjectRoot(projectRoot.string());
    CHECK(first.mappingProfiles().size() >= 2u);
    CHECK(first.templates().size() == 1u);
    CHECK(first.switchMappingProfile(completePath.string(), &error));
    const auto bindingPath = projectRoot / ".ayeditor"
        / "skeleton-profile-bindings.json";
    CHECK(std::filesystem::exists(bindingPath));
    const auto bindingJson = nlohmann::json::parse(
        ayt::io::File::readAllText(bindingPath.string()));
    CHECK(bindingJson["bindings"]["editor_fixture.ayskel"]
        ["profileId"].is_string());

    const auto movedCompletePath = profiles / "complete-renamed.ayrig";
    ignored.clear();
    std::filesystem::rename(completePath, movedCompletePath, ignored);
    CHECK(!ignored);

    ayt::editor::EditorSkeletonDocument reopened;
    CHECK(reopened.initialize(
        ayt::editor::EditorOpenRequest{skeletonPath.string()}, error));
    reopened.configureProjectRoot(projectRoot.string());
    CHECK(std::filesystem::equivalent(
        reopened.core().mappingPath(), movedCompletePath));
    CHECK(reopened.core().validation().isValid());
    CHECK(reopened.switchMappingProfile(minimalPath.string(), &error));
    CHECK(reopened.core().mapping().getBoundCount() == 1u);

    ayt::anim::editor::SkeletonTemplateApplyReport report;
    CHECK(reopened.applyTemplate(templatePath.string(), &report, &error));
    CHECK(report.appliedCount == 2u);
    CHECK(reopened.core().mapping().getSourceBoneIndex(
        ayt::anim::HumanoidBone::Spine) == 3);
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

TEST_CASE(skeleton_canvas_batches_bones_and_retains_static_presentation)
{
    auto document = std::make_shared<ayt::editor::EditorSkeletonDocument>();
    std::string error;
    CHECK(document->initialize(
        ayt::editor::EditorOpenRequest{writeEditorSkeletonFixture().string()},
        error));

    ayt::editor::EditorSkeletonCanvas canvas(document);
    canvas.setSize({640.0f, 480.0f});
    CHECK(canvas.getDisplayListPolicy() == ayt::ui::DisplayListPolicy::Retained);

    ayt::ui::MockRenderer renderer;
    renderer.beginFrame();
    canvas.render(renderer);

    const auto& calls = renderer.getDrawCalls();
    const auto pathCalls = std::count_if(calls.begin(), calls.end(),
        [](const ayt::ui::MockRenderer::DrawCall& call) {
            return call.type == ayt::ui::MockRenderer::DrawCall::Path;
        });
    const auto rectCalls = std::count_if(calls.begin(), calls.end(),
        [](const ayt::ui::MockRenderer::DrawCall& call) {
            return call.type == ayt::ui::MockRenderer::DrawCall::Rect;
        });
    CHECK(pathCalls == 1);
    CHECK(rectCalls <= static_cast<std::ptrdiff_t>(
        document->core().bones().size() + 1u));
    CHECK(canvas.hasCachedDisplayList());
}

TEST_SUITE_END
