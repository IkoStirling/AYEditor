#pragma once

#include "AYUI/ListView.h"

#include <functional>

namespace ayt::editor {

// ListView with editor-asset drag initiation. AYUI intentionally keeps its
// generic ListView payload-agnostic; this host widget maps a row index to the
// EditorAsset payload after pointer intent passes a small drag threshold.
class EditorAssetListView final : public ayt::ui::ListView {
public:
    using PayloadProvider =
        std::function<ayt::ui::DragPayload(int rowIndex)>;

    EditorAssetListView();

    void setPayloadProvider(PayloadProvider provider) {
        _payloadProvider = std::move(provider);
    }

    ayt::ui::Widget* hitTest(
        const ayt::math::FVector2& worldPos) override;
    bool onMouseButtonDown(const ayt::ui::UIMouseEvent& event) override;
    bool onMouseMove(const ayt::ui::UIMouseEvent& event) override;
    bool onMouseButtonUp(const ayt::ui::UIMouseEvent& event) override;
    void onMouseLeave() override;

private:
    int rowAt(const ayt::math::FVector2& worldPos) const;
    void resetDragIntent();

    PayloadProvider _payloadProvider;
    ayt::math::FVector2 _pressPosition{};
    int _pressedRow = -1;
    bool _dragArmed = false;
    bool _dragStarted = false;
};

// Must be called before UILayoutLoader sees "EditorAssetListView".
void registerEditorAssetWidgets();

} // namespace ayt::editor
