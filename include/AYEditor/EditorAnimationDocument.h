#pragma once

#include "AYEditor/EditorBuiltInExtensions.h"
#include "AYEditor/EditorExtension.h"

#include <AYAnimationEditor/AnimationPreviewSession.h>
#include <AYEditorCommand/EditorCommandHistory.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ayt::editor {

class EditorAnimationDocument final
    : public IEditorDocument, public IEditorTimelineSource,
      public IEditorCommandTarget {
public:
    EditorAnimationDocument();
    bool initialize(const EditorOpenRequest& request, std::string& error);
    void configureProjectRoot(const std::string& projectRoot);

    const std::string& typeId() const noexcept override { return _type; }
    const std::string& path() const noexcept override { return _path; }
    const std::string& title() const noexcept override { return _title; }
    bool isDirty() const noexcept override;
    std::uint64_t revision() const noexcept override;
    bool save(std::string* error = nullptr) override;
    bool writeRecoveryCopy(const std::string& path,
                           std::string* error = nullptr) const override;
    bool canReload() const noexcept override { return true; }
    bool reload(std::string* error = nullptr) override;

    bool handlesCommand(const std::string& commandId) const override;
    bool canExecuteCommand(const std::string& commandId) const override;
    bool executeCommand(const std::string& commandId) override;

    double timelineDurationSeconds() const noexcept override;
    double timelinePositionSeconds() const noexcept override;
    bool setTimelinePositionSeconds(double seconds) override;
    std::vector<EditorTimelineTrack> timelineTracks() const override;
    std::vector<EditorTimelineKeyframe> timelineKeyframes() const override;
    bool timelinePlaying() const noexcept override;
    void timelinePlay() override;
    void timelinePause() override;
    void timelineStop() override;
    void timelineTick(double seconds) override;
    bool timelineOwnsPlaybackTick() const noexcept override { return true; }
    bool timelineAddKeyframe(const std::string& trackId, double time,
                             double value) override;
    bool timelineMoveKeyframe(const std::string& keyframeId,
                              double time) override;
    bool timelineRemoveKeyframe(const std::string& keyframeId) override;
    bool timelineCanUndo() const noexcept override;
    bool timelineCanRedo() const noexcept override;
    bool timelineUndo() override;
    bool timelineRedo() override;

    bool addAnimationTrack(const std::string& nodeName,
                           const std::string& property,
                           ayt::resource::AnimTrackType type);
    bool removeAnimationTrack(const std::string& trackId);
    bool animationKeyframeValues(
        const std::string& keyframeId, std::vector<float>& values,
        ayt::resource::AnimTrackType* type = nullptr) const;
    bool setAnimationKeyframeValues(const std::string& keyframeId,
                                    const std::vector<float>& values);
    bool animationTrackInterpolation(
        const std::string& trackId,
        ayt::resource::AnimInterpolation& interpolation) const;
    bool setAnimationTrackInterpolation(
        const std::string& trackId,
        ayt::resource::AnimInterpolation interpolation);
    bool animationKeyframeTangents(const std::string& keyframeId,
                                   std::vector<float>& inTangents,
                                   std::vector<float>& outTangents) const;
    bool setAnimationKeyframeTangents(
        const std::string& keyframeId,
        const std::vector<float>& inTangents,
        const std::vector<float>& outTangents);
    bool autoAnimationTrackTangents(const std::string& trackId);

    ayt::anim::editor::AnimationPreviewSession& preview() noexcept {
        return _preview;
    }
    const ayt::anim::editor::AnimationPreviewSession& preview() const noexcept {
        return _preview;
    }

    bool bindSkeleton(const std::string& path, std::string* error = nullptr);
    bool bindMesh(const std::string& path, std::string* error = nullptr);
    void setMaterialPath(const std::string& path);
    void setPreviewMode(ayt::anim::editor::AnimationPreviewMode mode);
    void setLooping(bool looping);
    void setPlayRate(float rate);

    int selectedBone() const noexcept { return _selectedBone; }
    bool selectBone(int index) noexcept;

    const std::string& metadataPath() const noexcept { return _metadataPath; }
    bool persistPreviewMetadata(std::string* error = nullptr) const;

private:
    void loadPreviewMetadata();
    bool resetEditHistory(std::string* error = nullptr);
    bool commitEditedAnimation(
        std::shared_ptr<ayt::resource::Animation> animation,
        std::string* error = nullptr);
    bool applySerializedRevision(const std::vector<std::uint8_t>& bytes,
                                 std::string* error = nullptr);
    bool writeAnimationBytes(const std::string& path,
                             std::string* error) const;

    std::string _type = "ayeditor.animation.document";
    std::string _path;
    std::string _title;
    std::string _projectRoot;
    std::string _metadataPath;
    ayt::anim::editor::AnimationPreviewSession _preview;
    int _selectedBone = -1;
    std::vector<std::uint8_t> _currentBytes;
    EditorCommandHistory _history;
};

} // namespace ayt::editor
