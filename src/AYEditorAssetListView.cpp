#include "AYEditor/EditorAssetListView.h"

#include "AYUI/UIManager.h"
#include "AYUI/WidgetFactory.h"

#include <cmath>

namespace ayt::editor {

namespace {
constexpr float kAssetDragSlop = 6.0f;
}

EditorAssetListView::EditorAssetListView()
{
    setDraggable(true);
    setOnDragEnd([this](bool) { resetDragIntent(); });
}

ayt::ui::Widget* EditorAssetListView::hitTest(
    const ayt::math::FVector2& worldPos)
{
    if (!isVisible() || !getWorldBounds().contains(worldPos)) return nullptr;
    // Keep the native scrollbar fully interactive. Rows otherwise collapse to
    // this list so mouse-move reaches the drag-intent state machine instead of
    // a pooled ListView::Row.
    if (ayt::ui::ScrollBar* bar = getVerticalScrollBar()) {
        if (bar->isVisible() && bar->getWorldBounds().contains(worldPos)) {
            return bar->hitTest(worldPos);
        }
    }
    return this;
}

int EditorAssetListView::rowAt(
    const ayt::math::FVector2& worldPos) const
{
    const ayt::math::FRectangle bounds = getWorldBounds();
    if (!bounds.contains(worldPos) || getItemHeight() <= 0.0f) return -1;
    const float y = worldPos.y - bounds.minY + getScrollOffset().y;
    const int row = static_cast<int>(std::floor(y / getItemHeight()));
    return row >= 0 && row < static_cast<int>(getItemCount()) ? row : -1;
}

bool EditorAssetListView::onMouseButtonDown(
    const ayt::ui::UIMouseEvent& event)
{
    (void)ayt::ui::ListView::onMouseButtonDown(event);
    if (event.mouseButton != 0) return false;
    const int row = rowAt(event.mousePos);
    if (row < 0) {
        resetDragIntent();
        return false;
    }
    const ayt::ui::DragPayload payload = _payloadProvider
        ? _payloadProvider(row) : ayt::ui::DragPayload{};
    if (payload.isEmpty()) {
        // Folders and non-draggable asset types keep ListView's normal
        // mouse-up selection semantics. In particular, a folder callback may
        // schedule navigation; selecting it on mouse-down would let the next
        // editor update replace rows before this click's mouse-up arrives and
        // accidentally select row 0 in the new folder.
        resetDragIntent();
        return false;
    }

    // Draggable rows select on press so both a short click and a drag expose
    // the same current asset to the Inspector.
    setSelectedIndex(row);
    _pressPosition = event.mousePos;
    _pressedRow = row;
    _dragArmed = true;
    _dragStarted = false;
    setDragPayload(payload);
    // Capture until the pointer crosses the drag threshold. This guarantees
    // the source receives the first move even when a touchpad or low-rate
    // mouse jumps straight from the list into the viewport. UIManager::
    // beginDrag transfers same-source capture into the cross-widget session.
    return true;
}

bool EditorAssetListView::onMouseMove(const ayt::ui::UIMouseEvent& event)
{
    if (!_dragArmed || _dragStarted) return false;
    const float dx = event.mousePos.x - _pressPosition.x;
    const float dy = event.mousePos.y - _pressPosition.y;
    if (dx * dx + dy * dy < kAssetDragSlop * kAssetDragSlop) return true;
    if (ayt::ui::UIManager* ui = ayt::ui::UIManager::tryGet()) {
        _dragStarted = ui->beginDrag(this);
    }
    if (!_dragStarted) resetDragIntent();
    return _dragStarted;
}

bool EditorAssetListView::onMouseButtonUp(
    const ayt::ui::UIMouseEvent& event)
{
    if (_dragStarted) {
        resetDragIntent();
        return true;
    }
    const bool handled = ayt::ui::ListView::onMouseButtonUp(event);
    resetDragIntent();
    return handled;
}

void EditorAssetListView::onMouseLeave()
{
    ayt::ui::ListView::onMouseLeave();
    if (ayt::ui::UIManager* ui = ayt::ui::UIManager::tryGet();
        ui == nullptr || !ui->isDragging()) {
        resetDragIntent();
    }
}

void EditorAssetListView::resetDragIntent()
{
    _pressedRow = -1;
    _dragArmed = false;
    _dragStarted = false;
    setDragPayload({});
}

void registerEditorAssetWidgets()
{
    ayt::ui::WidgetFactory::get().registerCreator(
        "EditorAssetListView", []() -> ayt::ui::Widget* {
            return new EditorAssetListView();
        });
}

} // namespace ayt::editor
