#pragma once
#include <AYUI/Authoring/CurveCanvas.h>
#include <memory>
namespace ayt::editor {
class EditorAnimationDocument;
class EditorAnimationCurveCanvas final : public ayt::ui::authoring::CurveCanvas {
public:
    explicit EditorAnimationCurveCanvas(std::shared_ptr<EditorAnimationDocument> document);
    explicit EditorAnimationCurveCanvas(std::shared_ptr<ayt::ui::authoring::ICurveEditorSource> source);
};
} // namespace ayt::editor
