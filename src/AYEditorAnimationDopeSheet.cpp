#include "AYEditorAnimationDopeSheet.h"
#include "AYEditorAnimationCurveSource.h"
namespace ayt::editor {
EditorAnimationDopeSheet::EditorAnimationDopeSheet(
    std::shared_ptr<EditorAnimationDocument> document)
    : EditorAnimationDopeSheet(makeAnimationCurveSource(std::move(document)))
{}
EditorAnimationDopeSheet::EditorAnimationDopeSheet(
    std::shared_ptr<ayt::ui::authoring::ICurveEditorSource> source)
    : DopeSheet(std::move(source))
{
    setId("animation_dope_sheet");
}
} // namespace ayt::editor
