#pragma once
#include <AYUI/Authoring/DopeSheet.h>
#include <memory>
namespace ayt::editor {
class EditorAnimationDocument;
class EditorAnimationDopeSheet final : public ayt::ui::authoring::DopeSheet {
public:
    explicit EditorAnimationDopeSheet(std::shared_ptr<EditorAnimationDocument> document);
    explicit EditorAnimationDopeSheet(std::shared_ptr<ayt::ui::authoring::ICurveEditorSource> source);
};
} // namespace ayt::editor
