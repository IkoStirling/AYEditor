#include "AYEditorAnimationCurveCanvas.h"
#include "AYEditorAnimationCurveSource.h"
namespace ayt::editor {
EditorAnimationCurveCanvas::EditorAnimationCurveCanvas(
    std::shared_ptr<EditorAnimationDocument> document)
    : EditorAnimationCurveCanvas(makeAnimationCurveSource(std::move(document)))
{}
EditorAnimationCurveCanvas::EditorAnimationCurveCanvas(
    std::shared_ptr<ayt::ui::authoring::ICurveEditorSource> source)
    : CurveCanvas(std::move(source))
{
    setId("animation_curve_canvas");
}
} // namespace ayt::editor
