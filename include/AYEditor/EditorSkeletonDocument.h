#pragma once

#include "AYEditor/EditorBuiltInExtensions.h"
#include "AYEditor/EditorExtension.h"

#include <AYAnimationEditor/SkeletonEditorCore.h>

#include <memory>
#include <string>
#include <vector>

namespace ayt::editor {

// Thin AYEditor document adapter. All skeleton/mapping/animation semantics
// remain in AYAnimationEditorCore; this class only translates editor lifecycle,
// commands and timeline capability calls.
class EditorSkeletonDocument final : public IEditorDocument,
                                     public IEditorCommandTarget,
                                     public IEditorTimelineSource {
public:
    bool initialize(const EditorOpenRequest& request, std::string& error);

    const std::string& typeId() const noexcept override { return _typeId; }
    const std::string& path() const noexcept override { return _path; }
    const std::string& title() const noexcept override { return _title; }
    bool isDirty() const noexcept override { return _core.isDirty(); }
    std::uint64_t revision() const noexcept override { return _core.revision(); }
    bool save(std::string* error = nullptr) override;
    bool canReload() const noexcept override { return true; }
    bool reload(std::string* error = nullptr) override;

    ayt::anim::editor::SkeletonEditorCore& core() noexcept { return _core; }
    const ayt::anim::editor::SkeletonEditorCore& core() const noexcept {
        return _core;
    }

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
    bool timelineCanUndo() const noexcept override;
    bool timelineCanRedo() const noexcept override;
    bool timelineUndo() override;
    bool timelineRedo() override;

private:
    std::string _typeId = "ayeditor.skeleton.document";
    std::string _path;
    std::string _title;
    ayt::anim::editor::SkeletonEditorCore _core;
};

} // namespace ayt::editor
