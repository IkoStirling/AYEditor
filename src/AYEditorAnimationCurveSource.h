#pragma once

#include <AYUI/Authoring/TimelineModel.h>
#include <memory>

namespace ayt::editor {
class EditorAnimationDocument;
// Thin owner adapter: immutable revision cache, resource units, typed sampling
// and animation mutations. Shared controls never include the animation document.
std::shared_ptr<ayt::ui::authoring::ICurveEditorSource>
makeAnimationCurveSource(std::shared_ptr<EditorAnimationDocument> document);
} // namespace ayt::editor
