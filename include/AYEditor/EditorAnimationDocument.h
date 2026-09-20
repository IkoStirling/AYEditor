#pragma once

#include "AYEditor/EditorBuiltInExtensions.h"
#include "AYEditor/EditorExtension.h"

#include <AYAnimationEditor/AnimationPreviewSession.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ayt::editor {

class EditorAnimationDocument final
    : public IEditorDocument, public IEditorTimelineSource {
public:
    bool initialize(const EditorOpenRequest& request, std::string& error);
    void configureProjectRoot(const std::string& projectRoot);

    const std::string& typeId() const noexcept override { return _type; }
    const std::string& path() const noexcept override { return _path; }
    const std::string& title() const noexcept override { return _title; }
    bool isDirty() const noexcept override { return false; }
    std::uint64_t revision() const noexcept override;
    bool save(std::string* error = nullptr) override;
    bool canReload() const noexcept override { return true; }
    bool reload(std::string* error = nullptr) override;

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

    std::string _type = "ayeditor.animation.document";
    std::string _path;
    std::string _title;
    std::string _projectRoot;
    std::string _metadataPath;
    ayt::anim::editor::AnimationPreviewSession _preview;
    int _selectedBone = -1;
};

} // namespace ayt::editor
