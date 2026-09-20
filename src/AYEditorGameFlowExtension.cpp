#if defined(_WIN32) && !defined(NOMINMAX)
#  define NOMINMAX
#endif

#include "AYEditor/EditorGameFlowExtension.h"

#include "AYEditor/EditorGameFlowDocument.h"
#include "AYEditor/EditorGameFlowPreview.h"
#include "AYEditor/EditorProjectDescriptor.h"
#include "AYEditor/EditorUiFlowDocument.h"
#include "AYEditor/EditorWorkspace.h"

#include <AYApplication/GameFlowProgram.h>
#include <AYIO/File.h>

#include <AYUI/Box.h>
#include <AYUI/Button.h>
#include <AYUI/CheckBox.h>
#include <AYUI/ComboBox.h>
#include <AYUI/IRenderBackend.h>
#include <AYUI/ListView.h>
#include <AYUI/Menu.h>
#include <AYUI/TextArea.h>
#include <AYUI/TextInput.h>
#include <AYUI/TextLabel.h>
#include <AYUI/UIManager.h>
#include <AYUI/UIKeyCode.h>
#include <AYUI/UnicodeText.h>
#include <AYUI/Widget.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(min)
#  undef min
#endif
#if defined(max)
#  undef max
#endif

namespace ayt::editor
{
namespace
{

using ayt::math::FRectangle;
using ayt::math::FVector2;
using ayt::math::FVector4;

std::string encodeUtf8(const std::wstring& text)
{
    std::string output;
    output.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        std::uint32_t codePoint = static_cast<std::uint32_t>(text[index]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (codePoint >= 0xd800u && codePoint <= 0xdbffu
                && index + 1u < text.size()) {
                const auto low = static_cast<std::uint32_t>(text[index + 1u]);
                if (low >= 0xdc00u && low <= 0xdfffu) {
                    codePoint = 0x10000u + ((codePoint - 0xd800u) << 10u)
                        + (low - 0xdc00u);
                    ++index;
                }
            }
        }
        if (codePoint <= 0x7fu) {
            output.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7ffu) {
            output.push_back(static_cast<char>(0xc0u | (codePoint >> 6u)));
            output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
        } else if (codePoint <= 0xffffu) {
            output.push_back(static_cast<char>(0xe0u | (codePoint >> 12u)));
            output.push_back(static_cast<char>(
                0x80u | ((codePoint >> 6u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
        } else {
            output.push_back(static_cast<char>(0xf0u | (codePoint >> 18u)));
            output.push_back(static_cast<char>(
                0x80u | ((codePoint >> 12u) & 0x3fu)));
            output.push_back(static_cast<char>(
                0x80u | ((codePoint >> 6u) & 0x3fu)));
            output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
        }
    }
    return output;
}

bool contains(const FRectangle& rect, float x, float y) noexcept
{
    return x >= rect.minX && x <= rect.maxX
        && y >= rect.minY && y <= rect.maxY;
}

void drawBorder(ayt::ui::IRenderBackend& renderer,
                const FRectangle& rect,
                const FVector4& color,
                float width = 1.0f)
{
    renderer.drawRect({rect.minX, rect.minY, rect.maxX,
                       rect.minY + width}, color);
    renderer.drawRect({rect.minX, rect.maxY - width, rect.maxX,
                       rect.maxY}, color);
    renderer.drawRect({rect.minX, rect.minY, rect.minX + width,
                       rect.maxY}, color);
    renderer.drawRect({rect.maxX - width, rect.minY, rect.maxX,
                       rect.maxY}, color);
}

void drawConnector(ayt::ui::IRenderBackend& renderer,
                   FVector2 from,
                   FVector2 to,
                   const FVector4& color,
                   float width = 2.0f)
{
    const auto path = renderer.createPath();
    if (path.id < 0) return;
    const float reach = (std::max)(44.0f, std::abs(to.x - from.x) * 0.45f);
    renderer.addPathBezier(path, from, {from.x + reach, from.y},
        {to.x - reach, to.y}, to);
    renderer.setPathStrokeColor(path, color);
    renderer.setPathStrokeWidth(path, width);
    renderer.setPathStrokeStyle(path, ayt::ui::PathStrokeCap::Round,
        ayt::ui::PathStrokeJoin::Round);
    renderer.drawPath(path, ayt::ui::PathFillMode::Stroke);
    renderer.releasePath(path);
}

std::string valueText(const ayt::app::GameFlowValue& value)
{
    if (std::holds_alternative<std::monostate>(value.data)) return {};
    if (const auto* boolean = std::get_if<bool>(&value.data)) {
        return *boolean ? "true" : "false";
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value.data)) {
        return std::to_string(*integer);
    }
    if (const auto* number = std::get_if<double>(&value.data)) {
        std::ostringstream output;
        output << std::setprecision(12) << *number;
        return output.str();
    }
    if (const auto* string = std::get_if<std::string>(&value.data)) {
        return *string;
    }
    if (const auto* array =
            std::get_if<ayt::app::GameFlowValue::Array>(&value.data)) {
        return "<array: " + std::to_string(array->size()) + ">";
    }
    const auto* object =
        std::get_if<ayt::app::GameFlowValue::Object>(&value.data);
    return "<object: " + std::to_string(object == nullptr ? 0u
                                                           : object->size())
        + ">";
}

ayt::app::GameFlowValue parseValue(
    const std::wstring& text, ayt::app::GameFlowValueType type,
    bool& valid)
{
    valid = true;
    const std::string encoded = encodeUtf8(text);
    if (encoded.empty()) return {};
    try {
        switch (type) {
        case ayt::app::GameFlowValueType::Boolean:
            if (encoded == "true" || encoded == "1") return true;
            if (encoded == "false" || encoded == "0") return false;
            valid = false;
            return {};
        case ayt::app::GameFlowValueType::Integer: {
            std::size_t consumed = 0u;
            const std::int64_t value = std::stoll(encoded, &consumed);
            valid = consumed == encoded.size();
            return valid ? ayt::app::GameFlowValue(value)
                         : ayt::app::GameFlowValue{};
        }
        case ayt::app::GameFlowValueType::Number: {
            std::size_t consumed = 0u;
            const double value = std::stod(encoded, &consumed);
            valid = consumed == encoded.size() && std::isfinite(value);
            return valid ? ayt::app::GameFlowValue(value)
                         : ayt::app::GameFlowValue{};
        }
        case ayt::app::GameFlowValueType::String:
            return encoded;
        }
    } catch (...) {
        valid = false;
    }
    return {};
}

void hashBytes(std::uint64_t& hash, const void* data, std::size_t size)
{
    constexpr std::uint64_t kPrime = 1099511628211ull;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= kPrime;
    }
}

template<class T>
void hashScalar(std::uint64_t& hash, const T& value)
{
    hashBytes(hash, &value, sizeof(value));
}

void hashString(std::uint64_t& hash, const std::string& value)
{
    hashScalar(hash, value.size());
    hashBytes(hash, value.data(), value.size());
}

void hashValue(std::uint64_t& hash, const ayt::app::GameFlowValue& value)
{
    const std::size_t type = value.data.index();
    hashScalar(hash, type);
    if (const auto* boolean = std::get_if<bool>(&value.data)) {
        hashScalar(hash, *boolean);
    } else if (const auto* integer =
                   std::get_if<std::int64_t>(&value.data)) {
        hashScalar(hash, *integer);
    } else if (const auto* number = std::get_if<double>(&value.data)) {
        std::uint64_t bits = 0u;
        static_assert(sizeof(bits) == sizeof(*number));
        std::memcpy(&bits, number, sizeof(bits));
        hashScalar(hash, bits);
    } else if (const auto* string =
                   std::get_if<std::string>(&value.data)) {
        hashString(hash, *string);
    } else if (const auto* array =
                   std::get_if<ayt::app::GameFlowValue::Array>(&value.data)) {
        hashScalar(hash, array->size());
        for (const auto& element : *array) hashValue(hash, element);
    } else if (const auto* object =
                   std::get_if<ayt::app::GameFlowValue::Object>(&value.data)) {
        hashScalar(hash, object->size());
        for (const auto& [id, element] : *object) {
            hashString(hash, id);
            hashValue(hash, element);
        }
    }
}

void hashPayload(std::uint64_t& hash,
                 const ayt::app::GameFlowPayload& payload)
{
    hashScalar(hash, payload.size());
    for (const auto& [id, value] : payload) {
        hashString(hash, id);
        hashValue(hash, value);
    }
}

std::uint64_t flowFingerprint(const ayt::app::GameFlowDocument& flow)
{
    std::uint64_t hash = 1469598103934665603ull;
    hashScalar(hash, flow.schemaVersion);
    hashString(hash, flow.id);
    hashString(hash, flow.initialState);
    hashScalar(hash, flow.intents.size());
    for (const auto& intent : flow.intents) {
        hashString(hash, intent.id);
        hashScalar(hash, intent.payload.size());
        for (const auto& field : intent.payload) {
            hashString(hash, field.id);
            hashScalar(hash, field.type);
            hashScalar(hash, field.required);
            hashValue(hash, field.defaultValue);
        }
    }
    hashScalar(hash, flow.states.size());
    for (const auto& state : flow.states) {
        hashString(hash, state.id);
        hashString(hash, state.parent);
        hashString(hash, state.initialChild);
    }
    hashScalar(hash, flow.transitions.size());
    for (const auto& transition : flow.transitions) {
        hashString(hash, transition.id);
        hashString(hash, transition.fromState);
        hashString(hash, transition.triggerIntent);
        hashString(hash, transition.toState);
        hashString(hash, transition.guard.guard);
        hashPayload(hash, transition.guard.arguments);
        hashScalar(hash, transition.actions.size());
        for (const auto& action : transition.actions) {
            hashString(hash, action.action);
            hashPayload(hash, action.arguments);
        }
        hashString(hash, transition.onFailureState);
        hashString(hash, transition.onCancelState);
        hashScalar(hash, transition.timeoutSeconds);
        hashScalar(hash, transition.priority);
    }
    return hash;
}

class EditorGameFlowActionPalette final : public ayt::ui::ListView
{
public:
    EditorGameFlowActionPalette()
    {
        setItemHeight(22.0f);
        setDraggable(true);
        setOnDragEnd([this](bool) { _dragging = false; });
    }

    ayt::ui::Widget* hitTest(const FVector2& worldPos) override
    {
        ayt::ui::Widget* hit = ayt::ui::ListView::hitTest(worldPos);
        if (hit == nullptr || hit == getVerticalScrollBar()) return hit;
        // Route row presses through the palette itself so it can distinguish
        // click selection from a threshold-crossing Action drag gesture.
        return this;
    }

    bool onMouseButtonDown(const ayt::ui::UIMouseEvent& event) override
    {
        if (event.mouseButton == 0) {
            const FRectangle bounds = getWorldBounds();
            const float localY = event.mousePos.y - bounds.minY
                + getScrollOffset().y;
            const int index = static_cast<int>(std::floor(
                localY / (std::max)(1.0f, getItemHeight())));
            if (index >= 0 && static_cast<std::size_t>(index) < getItemCount()) {
                setSelectedIndex(index);
                _pressed = true;
                _pressPoint = event.mousePos;
            }
        }
        return ayt::ui::ListView::onMouseButtonDown(event) || _pressed;
    }

    bool onMouseMove(const ayt::ui::UIMouseEvent& event) override
    {
        if (!_pressed || _dragging || getSelectedIndex() < 0) return false;
        const FVector2 delta = event.mousePos - _pressPoint;
        if (delta.x * delta.x + delta.y * delta.y < 25.0f) return false;
        ayt::ui::DragPayload payload;
        payload.kind = "GameFlowAction";
        payload.text = getSelectedItem();
        setDragPayload(payload);
        if (auto* manager = ayt::ui::UIManager::tryGet();
            manager != nullptr && manager->beginDrag(this)) {
            _dragging = true;
            return true;
        }
        return false;
    }

    bool onMouseButtonUp(const ayt::ui::UIMouseEvent& event) override
    {
        _pressed = false;
        if (_dragging) return true;
        return event.mouseButton == 0;
    }

    void onCaptureCancelled() override
    {
        _pressed = false;
        _dragging = false;
    }

private:
    FVector2 _pressPoint{};
    bool _pressed = false;
    bool _dragging = false;
};

struct EditorGameFlowCanvasModel
{
    ayt::app::GameFlowDocument flow;
    EditorGameFlowSelection selection;
    ayt::app::GameFlowCoordinatorSnapshot preview;
    std::unordered_set<std::string> knownActions;
};

using EditorGameFlowLayout =
    std::unordered_map<std::string, FVector2>;

class EditorGameFlowCanvas final : public ayt::ui::Widget
{
public:
    EditorGameFlowCanvas(
                         std::function<EditorGameFlowCanvasModel()> model,
                         std::function<void(EditorGameFlowSelection)> select,
                         std::function<void(std::string, std::string)> addAction,
                         std::function<void(std::string, std::string, FVector2)>
                             transitionRequested,
                         std::function<void(const EditorGameFlowLayout&)>
                             layoutChanged,
                         std::function<void()> selected,
                         std::function<void(FVector2)> contextRequested,
                         std::function<std::wstring(
                             std::string_view, std::wstring_view)> localize)
        : _model(std::move(model)), _select(std::move(select)),
          _addAction(std::move(addAction)),
          _transitionRequested(std::move(transitionRequested)),
          _layoutChanged(std::move(layoutChanged)),
          _selected(std::move(selected)),
          _contextRequested(std::move(contextRequested)),
          _localize(std::move(localize))
    {
        setId("gameflow_canvas");
        setDisplayListPolicy(ayt::ui::DisplayListPolicy::Immediate);
        setAcceptDrops(true);
        setAcceptDropKinds({"GameFlowAction"});
        setOnDrop([this](const ayt::ui::DragPayload& payload) {
            if (payload.kind != "GameFlowAction") return;
            const auto* manager = ayt::ui::UIManager::tryGet();
            const FVector2 point = manager == nullptr
                ? FVector2{} : manager->getDragLastMousePos();
            std::string transitionId;
            for (auto it = _hits.rbegin(); it != _hits.rend(); ++it) {
                if (it->selection.kind == EditorGameFlowObjectKind::Transition
                    && contains(it->bounds, point.x, point.y)) {
                    transitionId = it->selection.id;
                    break;
                }
            }
            if (!transitionId.empty()) {
                if (_addAction != nullptr) {
                    _addAction(transitionId, encodeUtf8(payload.text));
                }
                if (_selected != nullptr) _selected();
            }
        });
    }

    bool pointerDown(float x, float y, int button)
    {
        if (!contains(getWorldBounds(), x, y)) return false;
        if (button == 1) {
            if (_contextRequested != nullptr) _contextRequested({x, y});
            return true;
        }
        if (button == 0) {
            for (auto it = _portHits.rbegin(); it != _portHits.rend(); ++it) {
                if (!it->output || !contains(it->bounds, x, y)) continue;
                _linking = true;
                _linkFrom = it->stateId;
                _linkPointer = {x, y};
                markDirty();
                return true;
            }
            for (auto it = _hits.rbegin(); it != _hits.rend(); ++it) {
                if (!contains(it->bounds, x, y)) continue;
                if (_select != nullptr) _select(it->selection);
                if (it->selection.kind == EditorGameFlowObjectKind::State) {
                    if (!_controlDown) _selectedStates.clear();
                    if (_controlDown && _selectedStates.contains(
                            it->selection.id)) {
                        _selectedStates.erase(it->selection.id);
                    } else {
                        _selectedStates.insert(it->selection.id);
                    }
                    _draggingState = it->selection.id;
                    const FRectangle canvas = getWorldBounds();
                    _dragStateOrigin = {
                        (it->bounds.minX - canvas.minX - _pan.x) / _zoom,
                        (it->bounds.minY - canvas.minY - _pan.y) / _zoom};
                    _dragPointerOrigin = {x, y};
                } else if (!_controlDown) {
                    _selectedStates.clear();
                }
                if (_selected != nullptr) _selected();
                markDirty();
                return true;
            }
            _selectedStates.clear();
            _boxSelecting = true;
            _boxStart = _boxEnd = {x, y};
            markDirty();
            return true;
        }
        if (button == 2) {
            _panning = true;
            _lastPointer = {x, y};
            return true;
        }
        return false;
    }

    bool pointerMove(float x, float y)
    {
        if (_linking) {
            _linkPointer = {x, y};
            markDirty();
            return true;
        }
        if (!_draggingState.empty()) {
            _manualPositions[_draggingState] = {
                _dragStateOrigin.x + (x - _dragPointerOrigin.x) / _zoom,
                _dragStateOrigin.y + (y - _dragPointerOrigin.y) / _zoom};
            markDirty();
            return true;
        }
        if (_boxSelecting) {
            _boxEnd = {x, y};
            markDirty();
            return true;
        }
        if (!_panning) return false;
        _pan.x += x - _lastPointer.x;
        _pan.y += y - _lastPointer.y;
        _lastPointer = {x, y};
        markDirty();
        return true;
    }

    bool pointerUp(float x, float y, int)
    {
        if (_linking) {
            std::string target;
            for (auto it = _portHits.rbegin(); it != _portHits.rend(); ++it) {
                if (!it->output && contains(it->bounds, x, y)) {
                    target = it->stateId;
                    break;
                }
            }
            if (!target.empty() && _transitionRequested != nullptr) {
                _transitionRequested(_linkFrom, target, {x, y});
            }
            _linking = false;
            _linkFrom.clear();
            markDirty();
            return true;
        }
        if (!_draggingState.empty()) {
            _draggingState.clear();
            if (_layoutChanged != nullptr) {
                _layoutChanged(_manualPositions);
            }
            return true;
        }
        if (_boxSelecting) {
            const FRectangle box{
                (std::min)(_boxStart.x, _boxEnd.x),
                (std::min)(_boxStart.y, _boxEnd.y),
                (std::max)(_boxStart.x, _boxEnd.x),
                (std::max)(_boxStart.y, _boxEnd.y)};
            _selectedStates.clear();
            for (const auto& [id, bounds] : _stateBounds) {
                if (bounds.maxX >= box.minX && bounds.minX <= box.maxX
                    && bounds.maxY >= box.minY && bounds.minY <= box.maxY) {
                    _selectedStates.insert(id);
                }
            }
            if (!_selectedStates.empty()) {
                if (_select != nullptr) _select({
                    EditorGameFlowObjectKind::State,
                    *_selectedStates.begin()});
                if (_selected != nullptr) _selected();
            }
            _boxSelecting = false;
            markDirty();
            return true;
        }
        if (_panning) {
            _panning = false;
            return true;
        }
        return false;
    }

    bool wheel(float x, float y, float delta)
    {
        if (!contains(getWorldBounds(), x, y)) return false;
        const FRectangle bounds = getWorldBounds();
        const float localX = x - bounds.minX;
        const float localY = y - bounds.minY;
        const float graphX = (localX - _pan.x) / _zoom;
        const float graphY = (localY - _pan.y) / _zoom;
        const float factor = delta > 0.0f ? 0.88f : 1.14f;
        _zoom = std::clamp(_zoom * factor, 0.42f, 2.2f);
        _pan.x = localX - graphX * _zoom;
        _pan.y = localY - graphY * _zoom;
        markDirty();
        return true;
    }

    void frameAll()
    {
        if (_stateBounds.empty()) {
            _zoom = 1.0f;
            _pan = {28.0f, 62.0f};
            markDirty();
            return;
        }
        const FRectangle canvas = getWorldBounds();
        float minX = (std::numeric_limits<float>::max)();
        float minY = (std::numeric_limits<float>::max)();
        float maxX = (std::numeric_limits<float>::lowest)();
        float maxY = (std::numeric_limits<float>::lowest)();
        for (const auto& [id, bounds] : _stateBounds) {
            (void)id;
            minX = (std::min)(minX,
                (bounds.minX - canvas.minX - _pan.x) / _zoom);
            minY = (std::min)(minY,
                (bounds.minY - canvas.minY - _pan.y) / _zoom);
            maxX = (std::max)(maxX,
                (bounds.maxX - canvas.minX - _pan.x) / _zoom);
            maxY = (std::max)(maxY,
                (bounds.maxY - canvas.minY - _pan.y) / _zoom);
        }
        constexpr float margin = 64.0f;
        const float graphWidth = (std::max)(1.0f, maxX - minX);
        const float graphHeight = (std::max)(1.0f, maxY - minY);
        const float width = (std::max)(1.0f,
            canvas.maxX - canvas.minX - margin * 2.0f);
        const float height = (std::max)(1.0f,
            canvas.maxY - canvas.minY - margin * 2.0f);
        _zoom = std::clamp((std::min)(width / graphWidth,
                                      height / graphHeight), 0.42f, 2.2f);
        _pan = {
            (canvas.maxX - canvas.minX - (minX + maxX) * _zoom) * 0.5f,
            (canvas.maxY - canvas.minY - (minY + maxY) * _zoom) * 0.5f};
        markDirty();
    }

    void autoLayout()
    {
        _manualPositions.clear();
        if (_layoutChanged != nullptr) _layoutChanged(_manualPositions);
        frameAll();
    }

    void setManualPositions(EditorGameFlowLayout positions)
    {
        _manualPositions = std::move(positions);
        markDirty();
    }

    const EditorGameFlowLayout& manualPositions() const noexcept
    {
        return _manualPositions;
    }

    void setModifierState(bool controlDown) noexcept
    {
        _controlDown = controlDown;
    }

    void selectAllStates()
    {
        _selectedStates.clear();
        const auto model = _model();
        for (const auto& state : model.flow.states) {
            _selectedStates.insert(state.id);
        }
        if (!model.flow.states.empty() && _select != nullptr) {
            _select({EditorGameFlowObjectKind::State,
                model.flow.states.front().id});
        }
        markDirty();
    }

    std::vector<EditorGameFlowSelection> selectedObjects() const
    {
        std::vector<EditorGameFlowSelection> result;
        result.reserve(_selectedStates.size());
        for (const auto& id : _selectedStates) {
            result.push_back({EditorGameFlowObjectKind::State, id});
        }
        const auto model = _model();
        if (result.empty()
            && model.selection.kind != EditorGameFlowObjectKind::Document) {
            result.push_back(model.selection);
        }
        return result;
    }

    bool captured() const noexcept
    {
        return _panning || _boxSelecting || _linking
            || !_draggingState.empty();
    }

protected:
    // Keep the canvas interactive after its DockCard moves to a promoted
    // child window. The primary editor also has a document-input bridge, but
    // child windows dispatch through their own UIManager, so the Widget must
    // implement the ordinary AYUI pointer contract as well.
    bool onMouseButtonDown(const ayt::ui::UIMouseEvent& event) override
    {
        return pointerDown(event.mousePos.x, event.mousePos.y,
                           event.mouseButton);
    }

    bool onMouseMove(const ayt::ui::UIMouseEvent& event) override
    {
        return pointerMove(event.mousePos.x, event.mousePos.y);
    }

    bool onMouseButtonUp(const ayt::ui::UIMouseEvent& event) override
    {
        return pointerUp(event.mousePos.x, event.mousePos.y,
                         event.mouseButton);
    }

    bool onMouseWheel(const ayt::ui::UIMouseWheelEvent& event) override
    {
        return wheel(event.mousePos.x, event.mousePos.y, event.deltaY);
    }

    void onCaptureCancelled() override
    {
        _panning = false;
        _boxSelecting = false;
        _linking = false;
        _draggingState.clear();
    }

    ayt::ui::UiCursorHint getCursorHint() const override
    {
        return _panning ? ayt::ui::UiCursorHint::Move
                        : ayt::ui::UiCursorHint::Hand;
    }

    void onRender(ayt::ui::IRenderBackend& renderer) override
    {
        _hits.clear();
        const FRectangle bounds = getWorldBounds();
        renderer.drawRect(bounds, {0.045f, 0.052f, 0.066f, 1.0f});
        drawGrid(renderer, bounds);
        drawFlow(renderer, bounds);
        if (_boxSelecting) {
            const FRectangle box{
                (std::min)(_boxStart.x, _boxEnd.x),
                (std::min)(_boxStart.y, _boxEnd.y),
                (std::max)(_boxStart.x, _boxEnd.x),
                (std::max)(_boxStart.y, _boxEnd.y)};
            renderer.drawRect(box, {0.12f, 0.42f, 0.78f, 0.18f});
            drawBorder(renderer, box, {0.30f, 0.68f, 1.0f, 0.9f});
        }
    }

private:
    std::wstring localized(std::string_view key,
                           std::wstring_view fallback) const
    {
        return _localize != nullptr ? _localize(key, fallback)
                                    : std::wstring(fallback);
    }

    struct Hit
    {
        FRectangle bounds;
        EditorGameFlowSelection selection;
    };

    struct StateLayout
    {
        const ayt::app::GameFlowStateDefinition* state = nullptr;
        FRectangle bounds;
    };

    struct PortHit
    {
        FRectangle bounds;
        std::string stateId;
        bool output = false;
    };

    FVector2 graphPoint(const FRectangle& bounds, float x, float y) const
    {
        return {bounds.minX + _pan.x + x * _zoom,
                bounds.minY + _pan.y + y * _zoom};
    }

    void drawGrid(ayt::ui::IRenderBackend& renderer,
                  const FRectangle& bounds) const
    {
        const float spacing = 32.0f * _zoom;
        const float startX = bounds.minX
            + std::fmod(_pan.x, spacing);
        const float startY = bounds.minY
            + std::fmod(_pan.y, spacing);
        const FVector4 fine{0.075f, 0.085f, 0.105f, 1.0f};
        for (float x = startX; x < bounds.maxX; x += spacing) {
            renderer.drawRect({x, bounds.minY, x + 1.0f, bounds.maxY}, fine);
        }
        for (float y = startY; y < bounds.maxY; y += spacing) {
            renderer.drawRect({bounds.minX, y, bounds.maxX, y + 1.0f}, fine);
        }
    }

    void drawFlow(ayt::ui::IRenderBackend& renderer,
                  const FRectangle& bounds)
    {
        _portHits.clear();
        _stateBounds.clear();
        const auto model = _model();
        const auto& flow = model.flow;
        const auto& preview = model.preview;
        const int font = std::clamp(
            static_cast<int>(std::lround(12.0f * _zoom)), 9, 18);

        float intentX = 0.0f;
        for (const auto& intent : flow.intents) {
            const FVector2 at = graphPoint(bounds, intentX, -42.0f);
            const FRectangle chip{at.x, at.y, at.x + 150.0f * _zoom,
                                  at.y + 28.0f * _zoom};
            renderer.drawRect(chip, {0.10f, 0.16f, 0.24f, 1.0f});
            drawBorder(renderer, chip, {0.20f, 0.48f, 0.78f, 1.0f});
            renderer.drawText({chip.minX + 7.0f, chip.minY,
                               chip.maxX - 5.0f, chip.maxY},
                localized("ui.editor.game_flow.canvas.intent", L"Intent")
                    + L": " + ayt::ui::decodeUtf8Text(intent.id), font,
                FVector4{0.72f, 0.85f, 1.0f, 1.0f});
            _hits.push_back({chip,
                {EditorGameFlowObjectKind::Intent, intent.id}});
            intentX += 166.0f;
        }

        std::unordered_map<std::string, std::size_t> indices;
        for (std::size_t index = 0; index < flow.states.size(); ++index) {
            indices.emplace(flow.states[index].id, index);
        }
        std::vector<int> depths(flow.states.size(), -1);
        const auto depthOf = [&](auto&& self, std::size_t index,
                                 std::unordered_set<std::size_t>& visiting)
            -> int {
            if (depths[index] >= 0) return depths[index];
            if (!visiting.insert(index).second) return 0;
            const auto parent = indices.find(flow.states[index].parent);
            const int depth = parent == indices.end()
                ? 0 : self(self, parent->second, visiting) + 1;
            visiting.erase(index);
            return depths[index] = (std::min)(depth, 8);
        };

        std::unordered_map<int, int> rows;
        std::vector<StateLayout> layouts;
        layouts.reserve(flow.states.size());
        for (std::size_t index = 0; index < flow.states.size(); ++index) {
            std::unordered_set<std::size_t> visiting;
            const int depth = depthOf(depthOf, index, visiting);
            const int row = rows[depth]++;
            FVector2 graphPosition{
                static_cast<float>(depth) * 310.0f,
                static_cast<float>(row) * 154.0f};
            if (const auto manual = _manualPositions.find(
                    flow.states[index].id); manual != _manualPositions.end()) {
                graphPosition = manual->second;
            }
            const FVector2 at = graphPoint(
                bounds, graphPosition.x, graphPosition.y);
            const FRectangle rect{at.x, at.y, at.x + 205.0f * _zoom,
                                  at.y + 66.0f * _zoom};
            layouts.push_back({&flow.states[index], rect});
            _stateBounds.emplace(flow.states[index].id, rect);
        }

        for (std::size_t index = 0; index < flow.transitions.size(); ++index) {
            const auto& transition = flow.transitions[index];
            const auto from = _stateBounds.find(transition.fromState);
            const auto to = _stateBounds.find(transition.toState);
            if (from == _stateBounds.end() || to == _stateBounds.end()) continue;
            const FVector2 start{from->second.maxX,
                (from->second.minY + from->second.maxY) * 0.5f};
            const FVector2 end{to->second.minX,
                (to->second.minY + to->second.maxY) * 0.5f};
            const bool active = preview.activeTransitionId == transition.id;
            drawConnector(renderer, start, end,
                active ? FVector4{0.20f, 0.84f, 0.63f, 1.0f}
                       : FVector4{0.30f, 0.42f, 0.58f, 1.0f},
                active ? 3.0f : 2.0f);

            const float laneOffset = static_cast<float>(index % 4u)
                * 34.0f * _zoom;
            const float midX = (start.x + end.x) * 0.5f;
            const float midY = (start.y + end.y) * 0.5f + laneOffset;
            const FRectangle transitionRect{
                midX - 78.0f * _zoom, midY - 16.0f * _zoom,
                midX + 78.0f * _zoom, midY + 16.0f * _zoom};
            renderer.drawRect(transitionRect, active
                ? FVector4{0.08f, 0.27f, 0.23f, 1.0f}
                : FVector4{0.105f, 0.12f, 0.15f, 1.0f});
            const bool selected = model.selection
                == EditorGameFlowSelection{
                    EditorGameFlowObjectKind::Transition, transition.id};
            drawBorder(renderer, transitionRect, selected
                ? FVector4{0.26f, 0.66f, 1.0f, 1.0f}
                : FVector4{0.24f, 0.30f, 0.38f, 1.0f},
                selected ? 2.0f : 1.0f);
            renderer.drawText({transitionRect.minX + 6.0f,
                               transitionRect.minY,
                               transitionRect.maxX - 5.0f,
                               transitionRect.maxY},
                ayt::ui::decodeUtf8Text(
                    transition.id + "  [" + transition.triggerIntent + "]"),
                font, FVector4{0.88f, 0.91f, 0.96f, 1.0f});
            _hits.push_back({transitionRect,
                {EditorGameFlowObjectKind::Transition, transition.id}});

            float laneY = transitionRect.maxY + 5.0f * _zoom;
            if (!transition.guard.guard.empty()) {
                const FRectangle guardRect{
                    transitionRect.minX, laneY, transitionRect.maxX,
                    laneY + 23.0f * _zoom};
                renderer.drawRect(guardRect,
                    {0.23f, 0.16f, 0.08f, 1.0f});
                drawBorder(renderer, guardRect,
                    {0.76f, 0.51f, 0.18f, 1.0f});
                renderer.drawText({guardRect.minX + 6.0f, guardRect.minY,
                                   guardRect.maxX - 4.0f, guardRect.maxY},
                    localized("ui.editor.game_flow.canvas.guard", L"Guard")
                        + L": "
                        + ayt::ui::decodeUtf8Text(transition.guard.guard), font,
                    FVector4{0.98f, 0.80f, 0.46f, 1.0f});
                _hits.push_back({guardRect,
                    {EditorGameFlowObjectKind::Guard,
                     transition.guard.guard, transition.id}});
                laneY = guardRect.maxY + 3.0f * _zoom;
            }
            for (std::size_t actionIndex = 0;
                 actionIndex < transition.actions.size(); ++actionIndex) {
                const auto& action = transition.actions[actionIndex];
                const bool subflow = action.action
                    == ayt::app::kGameFlowActionEnter;
                const bool known = subflow
                    || model.knownActions.contains(action.action);
                const bool actionActive = active
                    && preview.activeActionIndex == actionIndex;
                const FRectangle actionRect{
                    transitionRect.minX, laneY, transitionRect.maxX,
                    laneY + 23.0f * _zoom};
                renderer.drawRect(actionRect, actionActive
                    ? FVector4{0.08f, 0.28f, 0.23f, 1.0f}
                    : subflow ? FVector4{0.20f, 0.12f, 0.28f, 1.0f}
                    : known ? FVector4{0.09f, 0.15f, 0.19f, 1.0f}
                            : FVector4{0.24f, 0.08f, 0.10f, 1.0f});
                drawBorder(renderer, actionRect, actionActive
                    ? FVector4{0.22f, 0.90f, 0.62f, 1.0f}
                    : subflow ? FVector4{0.68f, 0.38f, 0.92f, 1.0f}
                    : known ? FVector4{0.22f, 0.48f, 0.64f, 1.0f}
                            : FVector4{0.92f, 0.28f, 0.34f, 1.0f});
                std::string label = std::to_string(actionIndex + 1u)
                    + ". " + action.action;
                if (subflow) {
                    const auto target = action.arguments.find(
                        ayt::app::kGameFlowSubflowIdArgument);
                    if (target != action.arguments.end()) {
                        if (const auto* id = std::get_if<std::string>(
                                &target->second.data)) {
                            label = std::to_string(actionIndex + 1u)
                                + ". Subflow: " + *id;
                        }
                    }
                }
                std::wstring displayLabel = ayt::ui::decodeUtf8Text(label);
                if (!known) {
                    displayLabel += L"  ["
                        + localized("ui.editor.game_flow.unknown", L"unknown")
                        + L"]";
                }
                renderer.drawText({actionRect.minX + 6.0f, actionRect.minY,
                                   actionRect.maxX - 4.0f, actionRect.maxY},
                    displayLabel, font,
                    known ? FVector4{0.75f, 0.87f, 0.96f, 1.0f}
                          : FVector4{1.0f, 0.60f, 0.64f, 1.0f});
                _hits.push_back({actionRect,
                    {EditorGameFlowObjectKind::Action, action.action,
                     transition.id, actionIndex}});
                laneY = actionRect.maxY + 3.0f * _zoom;
            }

            const auto drawFallback = [&](const std::string& stateId,
                                          float yOffset,
                                          const char* label,
                                          const FVector4& color) {
                const auto target = _stateBounds.find(stateId);
                if (target == _stateBounds.end()) return;
                const FVector2 fallbackStart{transitionRect.maxX,
                    transitionRect.minY + yOffset * _zoom};
                const FVector2 fallbackEnd{target->second.minX,
                    target->second.maxY - 8.0f * _zoom};
                drawConnector(renderer, fallbackStart, fallbackEnd, color,
                    1.5f);
                renderer.drawText({fallbackStart.x + 4.0f,
                                   fallbackStart.y - 13.0f,
                                   fallbackStart.x + 74.0f,
                                   fallbackStart.y + 4.0f},
                    ayt::ui::decodeUtf8Text(label),
                    (std::max)(9, font - 2), color);
            };
            if (!transition.onFailureState.empty()) {
                const std::string failure = encodeUtf8(localized(
                    "ui.editor.game_flow.canvas.failure", L"Failure"));
                drawFallback(transition.onFailureState, -4.0f,
                    failure.c_str(),
                    {0.94f, 0.31f, 0.34f, 1.0f});
            }
            if (!transition.onCancelState.empty()) {
                const std::string cancel = encodeUtf8(localized(
                    "ui.editor.game_flow.canvas.cancel", L"Cancel"));
                drawFallback(transition.onCancelState, 12.0f,
                    cancel.c_str(),
                    {0.92f, 0.62f, 0.24f, 1.0f});
            }
        }

        for (const auto& layout : layouts) {
            const auto& state = *layout.state;
            const bool active = preview.currentStateId == state.id;
            const bool selected = _selectedStates.contains(state.id)
                || model.selection
                == EditorGameFlowSelection{
                    EditorGameFlowObjectKind::State, state.id};
            renderer.drawRect(layout.bounds, active
                ? FVector4{0.07f, 0.25f, 0.22f, 1.0f}
                : FVector4{0.105f, 0.12f, 0.15f, 1.0f});
            drawBorder(renderer, layout.bounds, selected
                ? FVector4{0.25f, 0.67f, 1.0f, 1.0f}
                : active ? FVector4{0.18f, 0.88f, 0.60f, 1.0f}
                         : FVector4{0.23f, 0.28f, 0.35f, 1.0f},
                selected || active ? 2.0f : 1.0f);
            renderer.drawText({layout.bounds.minX + 9.0f,
                               layout.bounds.minY + 4.0f,
                               layout.bounds.maxX - 6.0f,
                               layout.bounds.minY + 31.0f * _zoom},
                ayt::ui::decodeUtf8Text(state.id), font,
                FVector4{0.92f, 0.95f, 0.98f, 1.0f});
            std::wstring detail;
            if (flow.initialState == state.id) {
                detail = localized(
                    "ui.editor.game_flow.canvas.initial", L"Initial");
            }
            if (!state.parent.empty()) {
                if (!detail.empty()) detail += L"  |  ";
                detail += localized(
                    "ui.editor.game_flow.canvas.parent", L"Parent")
                    + L": " + ayt::ui::decodeUtf8Text(state.parent);
            }
            if (!state.initialChild.empty()) {
                if (!detail.empty()) detail += L"  |  ";
                detail += localized(
                    "ui.editor.game_flow.canvas.child", L"Child")
                    + L": " + ayt::ui::decodeUtf8Text(state.initialChild);
            }
            renderer.drawText({layout.bounds.minX + 9.0f,
                               layout.bounds.minY + 32.0f * _zoom,
                               layout.bounds.maxX - 6.0f,
                               layout.bounds.maxY - 3.0f},
                detail,
                (std::max)(9, font - 2),
                FVector4{0.55f, 0.63f, 0.73f, 1.0f});
            _hits.push_back({layout.bounds,
                {EditorGameFlowObjectKind::State, state.id}});
            const float portWidth = (std::max)(7.0f, 9.0f * _zoom);
            const float portHalfHeight = (std::max)(7.0f, 9.0f * _zoom);
            const float centerY = (layout.bounds.minY + layout.bounds.maxY)
                * 0.5f;
            const FRectangle inputPort{layout.bounds.minX - portWidth,
                centerY - portHalfHeight, layout.bounds.minX + 2.0f,
                centerY + portHalfHeight};
            const FRectangle outputPort{layout.bounds.maxX - 2.0f,
                centerY - portHalfHeight, layout.bounds.maxX + portWidth,
                centerY + portHalfHeight};
            renderer.drawRect(inputPort, {0.18f, 0.48f, 0.72f, 1.0f});
            renderer.drawRect(outputPort, {0.24f, 0.72f, 0.52f, 1.0f});
            _portHits.push_back({inputPort, state.id, false});
            _portHits.push_back({outputPort, state.id, true});
        }

        if (_linking) {
            const auto source = _stateBounds.find(_linkFrom);
            if (source != _stateBounds.end()) {
                drawConnector(renderer,
                    {source->second.maxX,
                     (source->second.minY + source->second.maxY) * 0.5f},
                    _linkPointer, {0.28f, 0.78f, 1.0f, 1.0f}, 2.0f);
            }
        }

        if (flow.states.empty()) {
            renderer.drawText({bounds.minX + 22.0f, bounds.minY + 22.0f,
                               bounds.maxX - 22.0f, bounds.minY + 60.0f},
                localized("ui.editor.game_flow.canvas.empty",
                    L"Add a State to begin authoring the game flow."), 15,
                FVector4{0.58f, 0.63f, 0.72f, 1.0f});
        }
    }

    std::function<EditorGameFlowCanvasModel()> _model;
    std::function<void(EditorGameFlowSelection)> _select;
    std::function<void(std::string, std::string)> _addAction;
    std::function<void(std::string, std::string, FVector2)>
        _transitionRequested;
    std::function<void(const EditorGameFlowLayout&)> _layoutChanged;
    std::function<void()> _selected;
    std::function<void(FVector2)> _contextRequested;
    std::function<std::wstring(std::string_view, std::wstring_view)> _localize;
    std::vector<Hit> _hits;
    std::vector<PortHit> _portHits;
    std::unordered_map<std::string, FRectangle> _stateBounds;
    std::unordered_map<std::string, FVector2> _manualPositions;
    std::unordered_set<std::string> _selectedStates;
    FVector2 _pan{28.0f, 62.0f};
    FVector2 _lastPointer{};
    FVector2 _boxStart{};
    FVector2 _boxEnd{};
    FVector2 _linkPointer{};
    FVector2 _dragPointerOrigin{};
    FVector2 _dragStateOrigin{};
    std::string _linkFrom;
    std::string _draggingState;
    float _zoom = 1.0f;
    bool _panning = false;
    bool _boxSelecting = false;
    bool _linking = false;
    bool _controlDown = false;
};

class EditorGameFlowWorkspaceView final
    : public IEditorView,
      public IEditorCommandTarget,
      public IEditorViewInputTarget
{
public:
    EditorGameFlowWorkspaceView(
        std::shared_ptr<EditorGameFlowDocument> document,
        IEditorHostServices& host)
        : _document(std::move(document)), _host(host)
    {
        buildWidgetTree();
        _contentFingerprint = flowFingerprint(_document->flow());
        _document->setChangedHandler([this]() {
            _refreshPending = true;
            const std::uint64_t fingerprint =
                flowFingerprint(_document->flow());
            if (fingerprint != _contentFingerprint) {
                _contentFingerprint = fingerprint;
                _previewStale = true;
            }
            _host.requestRepaint();
        });
        refresh();
        restartPreview();
    }

    ~EditorGameFlowWorkspaceView() override
    {
        if (_document != nullptr) _document->setChangedHandler({});
        if (_root != nullptr) ayt::ui::destroyWidgetTree(_root);
    }

    ayt::ui::Widget* rootWidget() noexcept override { return _root; }
    ayt::ui::Widget* releaseRootWidget() noexcept override
    {
        ayt::ui::Widget* result = _root;
        _root = nullptr;
        return result;
    }
    IEditorCommandTarget* commandTarget() noexcept override { return this; }
    IEditorViewInputTarget* inputTarget() noexcept override { return this; }

    void onLanguageChanged(const std::string&) override
    {
        applyLocalization();
        rebuildCanvasMenu();
        refresh();
        syncPreviewPresentation();
    }

    void prepareForUiShutdown() override
    {
        if (_document != nullptr) _document->setChangedHandler({});
        _canvas = nullptr;
        _canvasMenu = nullptr;
        _transitionMenu = nullptr;
        _outline = nullptr;
        _diagnostics = nullptr;
        _trace = nullptr;
        _status = nullptr;
        _previewStatus = nullptr;
        _completePreview = nullptr;
        _failPreview = nullptr;
        _cancelPreview = nullptr;
        _intentPicker = nullptr;
        _intentFieldPicker = nullptr;
        _intentFieldValue = nullptr;
        _previewGuardPicker = nullptr;
        _previewGuardAccepted = nullptr;
        _subflowPicker = nullptr;
        _argumentChoice = nullptr;
        _argumentBool = nullptr;
        _localizedBindings.clear();
    }

    void tick(float dt) override
    {
        if (_preview.isRunning()) _preview.update(dt);
        const auto snapshot = _preview.snapshot();
        if (!(snapshot.currentStateId == _lastSnapshot.currentStateId
              && snapshot.activeTransitionId
                    == _lastSnapshot.activeTransitionId
              && snapshot.activeActionIndex == _lastSnapshot.activeActionIndex
              && snapshot.status == _lastSnapshot.status
              && snapshot.queuedIntentCount
                    == _lastSnapshot.queuedIntentCount)
            || _preview.trace().size() != _lastTraceSize) {
            syncPreviewPresentation();
        }
        if (_refreshPending) refresh();
    }

    bool handlesCommand(const std::string& commandId) const override
    {
        return commandId == "file.save" || commandId == "edit.undo"
            || commandId == "edit.redo"
            || commandId == kEditorGameFlowValidateCommand
            || commandId == kEditorGameFlowDeleteCommand;
    }

    bool canExecuteCommand(const std::string& commandId) const override
    {
        if (commandId == "edit.undo") return _document->canUndo();
        if (commandId == "edit.redo") return _document->canRedo();
        if (commandId == "file.save") return _document->isDirty();
        if (commandId == kEditorGameFlowDeleteCommand) {
            return _document->selection().kind
                != EditorGameFlowObjectKind::Document;
        }
        return handlesCommand(commandId);
    }

    bool executeCommand(const std::string& commandId) override
    {
        if (!canExecuteCommand(commandId)) return false;
        if (commandId == "file.save") return save();
        if (commandId == "edit.undo") return _document->undo();
        if (commandId == "edit.redo") return _document->redo();
        if (commandId == kEditorGameFlowDeleteCommand) return deleteSelected();
        validate();
        return true;
    }

    bool onPointerDown(float x, float y, int button) override
    {
        return _canvas != nullptr && _canvas->pointerDown(x, y, button);
    }
    bool onPointerMove(float x, float y) override
    {
        return _canvas != nullptr && _canvas->pointerMove(x, y);
    }
    bool onPointerUp(float x, float y, int button) override
    {
        return _canvas != nullptr && _canvas->pointerUp(x, y, button);
    }
    bool onWheel(float x, float y, float deltaY) override
    {
        return _canvas != nullptr && _canvas->wheel(x, y, deltaY);
    }
    bool onKeyDown(int keyCode) override
    {
        if (keyCode == ayt::ui::UIKey_Control) {
            _controlDown = true;
            if (_canvas != nullptr) _canvas->setModifierState(true);
            return false;
        }
        if (_controlDown && keyCode == ayt::ui::UIKey_C) {
            return copySelection();
        }
        if (_controlDown && keyCode == ayt::ui::UIKey_V) {
            return pasteSelection();
        }
        if (_controlDown && keyCode == ayt::ui::UIKey_A) {
            if (_canvas != nullptr) _canvas->selectAllStates();
            return true;
        }
        if (keyCode == ayt::ui::UIKey_Delete) return deleteSelected();
        if (keyCode == ayt::ui::UIKey_Escape && _preview.isRunning()) {
            (void)_preview.cancelActive("Cancelled from editor preview.");
            syncPreviewPresentation();
            return true;
        }
        return false;
    }
    void onKeyUp(int keyCode) override
    {
        if (keyCode == ayt::ui::UIKey_Control) {
            _controlDown = false;
            if (_canvas != nullptr) _canvas->setModifierState(false);
        }
    }
    bool hasPointerCapture() const noexcept override
    {
        return _canvas != nullptr && _canvas->captured();
    }
    ayt::ui::UiCursorHint cursorHint(float x, float y) const override
    {
        return _canvas != nullptr
                && contains(_canvas->getWorldBounds(), x, y)
            ? (_canvas->captured() ? ayt::ui::UiCursorHint::Move
                                   : ayt::ui::UiCursorHint::Hand)
            : ayt::ui::UiCursorHint::Default;
    }

private:
    enum class LocalizedTarget : std::uint8_t
    {
        Text,
        Placeholder,
    };

    struct LocalizedBinding
    {
        ayt::ui::Widget* widget = nullptr;
        std::string key;
        std::wstring fallback;
        LocalizedTarget target = LocalizedTarget::Text;
    };

    struct PropertyRow
    {
        ayt::ui::HBox* row = nullptr;
        ayt::ui::TextLabel* label = nullptr;
        ayt::ui::TextInput* input = nullptr;
        ayt::ui::ComboBox* choice = nullptr;
    };

    std::wstring text(std::string_view key,
                      std::wstring_view fallback) const
    {
        return _host.localizedText(key, fallback);
    }

    void bindLocalized(ayt::ui::Widget* widget,
                       std::string key,
                       std::wstring fallback,
                       LocalizedTarget target = LocalizedTarget::Text)
    {
        if (widget == nullptr) return;
        _localizedBindings.push_back(
            {widget, std::move(key), std::move(fallback), target});
    }

    void applyLocalization()
    {
        for (const auto& binding : _localizedBindings) {
            if (binding.widget == nullptr) continue;
            const std::wstring value = text(binding.key, binding.fallback);
            if (binding.target == LocalizedTarget::Placeholder) {
                if (auto* input = dynamic_cast<ayt::ui::TextInput*>(
                        binding.widget)) {
                    input->setPlaceholder(value);
                }
            } else if (auto* button = dynamic_cast<ayt::ui::Button*>(
                           binding.widget)) {
                button->setText(value);
            } else if (auto* label = dynamic_cast<ayt::ui::TextLabel*>(
                           binding.widget)) {
                label->setText(value);
            }
        }
        if (_canvas != nullptr) _canvas->markDirty();
        _host.requestRepaint();
    }

    ayt::ui::Button* button(ayt::ui::HBox& parent,
                            std::string key,
                            std::wstring fallback,
                            float width,
                            std::function<void()> clicked)
    {
        auto* result = new ayt::ui::Button();
        result->setText(text(key, fallback));
        result->setPadding(6.0f, 3.0f, 6.0f, 3.0f);
        result->setOnClicked(std::move(clicked));
        parent.addWidget(result, width);
        bindLocalized(result, std::move(key), std::move(fallback));
        return result;
    }

    ayt::ui::TextLabel* label(ayt::ui::VBox& parent,
                              std::string key,
                              std::wstring fallback,
                              float height = 20.0f)
    {
        auto* result = new ayt::ui::TextLabel();
        result->setText(text(key, fallback));
        result->setFontSize(11);
        result->setTextColor({0.58f, 0.64f, 0.73f, 1.0f});
        result->setVerticalAlignment(
            ayt::ui::TextLabel::VAlignment::Center);
        parent.addWidget(result, height);
        bindLocalized(result, std::move(key), std::move(fallback));
        return result;
    }

    PropertyRow propertyRow(ayt::ui::VBox& parent)
    {
        PropertyRow value;
        value.row = new ayt::ui::HBox();
        value.row->setSpacing(4.0f);
        value.label = new ayt::ui::TextLabel();
        value.label->setFontSize(11);
        value.label->setTextColor({0.67f, 0.71f, 0.78f, 1.0f});
        value.label->setVerticalAlignment(
            ayt::ui::TextLabel::VAlignment::Center);
        value.input = new ayt::ui::TextInput();
        value.choice = new ayt::ui::ComboBox();
        value.choice->setVisible(false);
        value.row->addWidget(value.label, 102.0f);
        value.row->addWidget(value.input, 0.0f);
        value.row->addWidget(value.choice, 0.0f);
        parent.addWidget(value.row, 25.0f);
        return value;
    }

    std::wstring localizedKind(EditorGameFlowObjectKind kind) const
    {
        switch (kind) {
        case EditorGameFlowObjectKind::Document:
            return text("ui.editor.game_flow.kind.document", L"Document");
        case EditorGameFlowObjectKind::Intent:
            return text("ui.editor.game_flow.kind.intent", L"Intent");
        case EditorGameFlowObjectKind::IntentField:
            return text("ui.editor.game_flow.kind.intent_field", L"Intent Field");
        case EditorGameFlowObjectKind::State:
            return text("ui.editor.game_flow.kind.state", L"State");
        case EditorGameFlowObjectKind::Transition:
            return text("ui.editor.game_flow.kind.transition", L"Transition");
        case EditorGameFlowObjectKind::Guard:
            return text("ui.editor.game_flow.kind.guard", L"Guard");
        case EditorGameFlowObjectKind::Action:
            return text("ui.editor.game_flow.kind.action", L"Action");
        case EditorGameFlowObjectKind::ActionArgument:
            return text("ui.editor.game_flow.kind.argument", L"Argument");
        }
        return {};
    }

    std::wstring localizedProperty(std::string_view value) const
    {
        if (value.empty()) return {};
        if (value == "ID") return text("ui.editor.game_flow.property.id", L"ID");
        if (value == "Initial State") return text(
            "ui.editor.game_flow.property.initial_state", L"Initial State");
        if (value == "Value Type") return text(
            "ui.editor.game_flow.property.value_type", L"Value Type");
        if (value == "Required") return text(
            "ui.editor.game_flow.property.required", L"Required");
        if (value == "Default Value") return text(
            "ui.editor.game_flow.property.default_value", L"Default Value");
        if (value == "Parent") return text(
            "ui.editor.game_flow.property.parent", L"Parent");
        if (value == "Initial Child") return text(
            "ui.editor.game_flow.property.initial_child", L"Initial Child");
        if (value == "From State") return text(
            "ui.editor.game_flow.property.from_state", L"From State");
        if (value == "Trigger Intent") return text(
            "ui.editor.game_flow.property.trigger_intent", L"Trigger Intent");
        if (value == "To State") return text(
            "ui.editor.game_flow.property.to_state", L"To State");
        if (value == "Guard") return text(
            "ui.editor.game_flow.property.guard", L"Guard");
        if (value == "Failure State") return text(
            "ui.editor.game_flow.property.failure_state", L"Failure State");
        if (value == "Cancel State") return text(
            "ui.editor.game_flow.property.cancel_state", L"Cancel State");
        if (value == "Timeout Seconds") return text(
            "ui.editor.game_flow.property.timeout_seconds", L"Timeout Seconds");
        if (value == "Priority") return text(
            "ui.editor.game_flow.property.priority", L"Priority");
        if (value == "Registered Type") return text(
            "ui.editor.game_flow.property.registered_type", L"Registered Type");
        if (value == "Authored") return text(
            "ui.editor.game_flow.property.authored", L"Authored");
        if (value == "Effective Value") return text(
            "ui.editor.game_flow.property.effective_value", L"Effective Value");
        return ayt::ui::decodeUtf8Text(std::string(value));
    }

    std::wstring localizedOutlineLabel(
        const EditorGameFlowOutlineItem& item) const
    {
        if (item.selection.kind == EditorGameFlowObjectKind::Document) {
            return ayt::ui::decodeUtf8Text(item.label);
        }
        if (item.selection.kind == EditorGameFlowObjectKind::IntentField) {
            const auto* intent = _document->flow().findIntent(
                item.selection.ownerId);
            if (intent != nullptr) {
                const auto field = std::find_if(intent->payload.begin(),
                    intent->payload.end(), [&](const auto& candidate) {
                        return candidate.id == item.selection.id;
                    });
                if (field != intent->payload.end()) {
                    return ayt::ui::decodeUtf8Text(field->id + " : "
                        + ayt::app::gameFlowValueTypeName(field->type));
                }
            }
        }
        std::wstring prefix = localizedKind(item.selection.kind);
        if (item.selection.kind == EditorGameFlowObjectKind::Action) {
            prefix += L" " + std::to_wstring(item.selection.index + 1u);
        }
        return prefix + L": " + ayt::ui::decodeUtf8Text(item.selection.id);
    }

    void buildWidgetTree()
    {
        auto* root = new ayt::ui::VBox();
        _root = root;
        root->setSpacing(4.0f);
        root->setPadding(5.0f, 4.0f, 5.0f, 5.0f);

        auto* toolbar = new ayt::ui::HBox();
        toolbar->setSpacing(4.0f);
        button(*toolbar, "ui.editor.game_flow.save", L"Save", 50.0f,
            [this]() { (void)save(); });
        button(*toolbar, "ui.editor.game_flow.undo", L"Undo", 50.0f, [this]() {
            if (!_document->undo()) setLocalizedStatus(
                "ui.editor.game_flow.status.nothing_to_undo",
                L"Nothing to undo.", true);
        });
        button(*toolbar, "ui.editor.game_flow.redo", L"Redo", 50.0f, [this]() {
            if (!_document->redo()) setLocalizedStatus(
                "ui.editor.game_flow.status.nothing_to_redo",
                L"Nothing to redo.", true);
        });
        button(*toolbar, "ui.editor.game_flow.add_intent", L"+ Intent", 76.0f, [this]() {
            addObject(EditorGameFlowObjectKind::Intent);
        });
        button(*toolbar, "ui.editor.game_flow.add_state", L"+ State", 70.0f, [this]() {
            addObject(EditorGameFlowObjectKind::State);
        });
        button(*toolbar, "ui.editor.game_flow.delete", L"Delete", 58.0f,
            [this]() { (void)deleteSelected(); });
        button(*toolbar, "ui.editor.game_flow.validate", L"Validate", 68.0f,
            [this]() { validate(); });
        button(*toolbar, "ui.editor.game_flow.frame", L"Frame", 54.0f, [this]() {
            if (_canvas != nullptr) _canvas->frameAll();
        });
        button(*toolbar, "ui.editor.game_flow.auto_layout", L"Auto Layout", 86.0f,
            [this]() { if (_canvas != nullptr) _canvas->autoLayout(); });
        button(*toolbar, "ui.editor.game_flow.template", L"Template", 72.0f,
            [this]() { applyStarterTemplate(); });
        _status = new ayt::ui::TextLabel();
        _status->setFontSize(11);
        _status->setVerticalAlignment(
            ayt::ui::TextLabel::VAlignment::Center);
        toolbar->addWidget(_status, 0.0f);
        root->addWidget(toolbar, 29.0f);

        auto* body = new ayt::ui::HBox();
        body->setSpacing(5.0f);

        auto* left = new ayt::ui::VBox();
        left->setSpacing(4.0f);
        left->setPadding(4.0f, 3.0f, 4.0f, 3.0f);
        label(*left, "ui.editor.game_flow.flow_outline", L"FLOW OUTLINE");
        _outline = new ayt::ui::ListView();
        _outline->setItemHeight(22.0f);
        _outline->setOnSelectionChanged([this](int index) {
            if (_refreshing || index < 0
                || static_cast<std::size_t>(index) >= _outlineItems.size()) {
                return;
            }
            (void)_document->select(_outlineItems[
                static_cast<std::size_t>(index)].selection);
        });
        left->addWidget(_outline, 0.0f);
        label(*left, "ui.editor.game_flow.action_guard_palette",
              L"ACTION / GUARD PALETTE");
        _actionPalette = new EditorGameFlowActionPalette();
        _actionPalette->setId("gameflow_action_palette");
        left->addWidget(_actionPalette, 88.0f);
        auto* actionButtons = new ayt::ui::HBox();
        actionButtons->setSpacing(4.0f);
        button(*actionButtons, "ui.editor.game_flow.add_action",
            L"Add Action", 92.0f,
            [this]() { addPaletteAction(); });
        button(*actionButtons, "ui.editor.game_flow.move_up", L"Up", 44.0f,
            [this]() { moveAction(-1); });
        button(*actionButtons, "ui.editor.game_flow.move_down", L"Down", 50.0f,
            [this]() { moveAction(1); });
        left->addWidget(actionButtons, 27.0f);
        _subflowPicker = new ayt::ui::ComboBox();
        _subflowPicker->setId("gameflow_subflow_picker");
        left->addWidget(_subflowPicker, 27.0f);
        auto* subflowButtons = new ayt::ui::HBox();
        subflowButtons->setSpacing(4.0f);
        button(*subflowButtons, "ui.editor.game_flow.add_subflow",
            L"Add Subflow", 112.0f, [this]() { addSubflow(); });
        left->addWidget(subflowButtons, 27.0f);
        _guardPalette = new ayt::ui::ComboBox();
        left->addWidget(_guardPalette, 27.0f);
        auto* guardButtons = new ayt::ui::HBox();
        guardButtons->setSpacing(4.0f);
        button(*guardButtons, "ui.editor.game_flow.set_guard",
            L"Set Guard", 92.0f,
            [this]() { setPaletteGuard(); });
        button(*guardButtons, "ui.editor.game_flow.clear", L"Clear", 54.0f,
            [this]() { clearGuard(); });
        button(*guardButtons, "ui.editor.game_flow.add_field", L"+ Field", 66.0f,
            [this]() { addIntentField(); });
        left->addWidget(guardButtons, 27.0f);
        body->addWidget(left, 224.0f);

        auto* center = new ayt::ui::VBox();
        center->setSpacing(4.0f);
        _canvas = new EditorGameFlowCanvas(
            [this]() { return canvasModel(); },
            [this](EditorGameFlowSelection selection) {
                (void)_document->select(std::move(selection));
            },
            [this](std::string transition, std::string action) {
                std::string error;
                if (!_document->addAction(
                        std::move(transition), std::move(action), &error)) {
                    setStatus(error, true);
                }
            },
            [this](std::string from, std::string to, FVector2 point) {
                showTransitionIntentMenu(
                    std::move(from), std::move(to), point);
            },
            [this](const EditorGameFlowLayout& positions) {
                saveCanvasLayout(positions);
            },
            [this]() { _refreshPending = true; },
            [this](FVector2 point) { showCanvasMenu(point); },
            [this](std::string_view key, std::wstring_view fallback) {
                return text(key, fallback);
            });
        buildCanvasMenu();
        loadCanvasLayout();
        center->addWidget(_canvas, 0.0f);
        auto* previewBar = new ayt::ui::HBox();
        previewBar->setSpacing(4.0f);
        button(*previewBar, "ui.editor.game_flow.restart", L"Restart", 66.0f,
            [this]() { restartPreview(); });
        _intentPicker = new ayt::ui::ComboBox();
        _intentPicker->setOnSelectionChanged([this](int) {
            refreshPreviewPayloadFields();
        });
        previewBar->addWidget(_intentPicker, 156.0f);
        button(*previewBar, "ui.editor.game_flow.send", L"Send", 50.0f,
            [this]() { sendIntent(); });
        _completePreview = button(*previewBar, "ui.editor.game_flow.complete", L"Complete", 72.0f,
            [this]() { completeAction(); });
        _failPreview = button(*previewBar, "ui.editor.game_flow.fail", L"Fail", 44.0f,
            [this]() { failAction(); });
        _cancelPreview = button(*previewBar, "ui.editor.game_flow.cancel", L"Cancel", 56.0f, [this]() {
            if (!_preview.cancelActive("Cancelled from preview toolbar.")) {
                setLocalizedStatus(
                    "ui.editor.game_flow.status.no_active_transition",
                    L"No active transition to cancel.", true);
            }
            syncPreviewPresentation();
        });
        _previewStatus = new ayt::ui::TextLabel();
        _previewStatus->setFontSize(11);
        _previewStatus->setVerticalAlignment(
            ayt::ui::TextLabel::VAlignment::Center);
        previewBar->addWidget(_previewStatus, 0.0f);
        center->addWidget(previewBar, 28.0f);
        auto* payloadBar = new ayt::ui::HBox();
        payloadBar->setSpacing(4.0f);
        auto* payloadLabel = new ayt::ui::TextLabel();
        payloadLabel->setText(text("ui.editor.game_flow.payload", L"Payload"));
        payloadLabel->setFontSize(11);
        payloadLabel->setVerticalAlignment(
            ayt::ui::TextLabel::VAlignment::Center);
        payloadBar->addWidget(payloadLabel, 52.0f);
        bindLocalized(payloadLabel, "ui.editor.game_flow.payload", L"Payload");
        _intentFieldPicker = new ayt::ui::ComboBox();
        _intentFieldPicker->setOnSelectionChanged([this](int) {
            syncPreviewPayloadValue();
        });
        payloadBar->addWidget(_intentFieldPicker, 184.0f);
        _intentFieldValue = new ayt::ui::TextInput();
        _intentFieldValue->setPlaceholder(
            text("ui.editor.game_flow.value", L"Value"));
        payloadBar->addWidget(_intentFieldValue, 0.0f);
        bindLocalized(_intentFieldValue, "ui.editor.game_flow.value", L"Value",
                      LocalizedTarget::Placeholder);
        button(*payloadBar, "ui.editor.game_flow.set", L"Set", 46.0f,
            [this]() { setPreviewPayloadField(); });
        button(*payloadBar, "ui.editor.game_flow.clear", L"Clear", 50.0f,
            [this]() { clearPreviewPayloadField(); });
        center->addWidget(payloadBar, 27.0f);
        auto* guardBar = new ayt::ui::HBox();
        guardBar->setSpacing(4.0f);
        auto* guardLabel = new ayt::ui::TextLabel();
        guardLabel->setText(text("ui.editor.game_flow.guard", L"Guard"));
        guardLabel->setFontSize(11);
        guardLabel->setVerticalAlignment(
            ayt::ui::TextLabel::VAlignment::Center);
        guardBar->addWidget(guardLabel, 52.0f);
        _previewGuardPicker = new ayt::ui::ComboBox();
        guardBar->addWidget(_previewGuardPicker, 184.0f);
        _previewGuardAccepted = new ayt::ui::CheckBox();
        _previewGuardAccepted->setChecked(true);
        guardBar->addWidget(_previewGuardAccepted, 28.0f);
        button(*guardBar, "ui.editor.game_flow.apply_guard", L"Apply Guard", 86.0f,
            [this]() { applyPreviewGuard(); });
        center->addWidget(guardBar, 27.0f);
        _trace = new ayt::ui::TextArea();
        _trace->setReadOnly(true);
        _trace->setWordWrap(false);
        _trace->setLineHeight(15.0f);
        center->addWidget(_trace, 92.0f);
        body->addWidget(center, 0.0f);

        auto* inspector = new ayt::ui::VBox();
        inspector->setSpacing(3.0f);
        inspector->setPadding(4.0f, 3.0f, 4.0f, 3.0f);
        _inspectorHeading = label(*inspector,
            "ui.editor.game_flow.inspector", L"INSPECTOR");
        _id = propertyRow(*inspector);
        _first = propertyRow(*inspector);
        _second = propertyRow(*inspector);
        _third = propertyRow(*inspector);
        _fourth = propertyRow(*inspector);
        _fifth = propertyRow(*inspector);
        _sixth = propertyRow(*inspector);
        _number = propertyRow(*inspector);
        _integer = propertyRow(*inspector);
        _first.choice->setId("gameflow_property_first_choice");
        _second.choice->setId("gameflow_property_second_choice");
        _third.choice->setId("gameflow_property_third_choice");
        _fourth.choice->setId("gameflow_property_fourth_choice");
        _fifth.choice->setId("gameflow_property_fifth_choice");
        _sixth.choice->setId("gameflow_property_sixth_choice");
        _flagRow = new ayt::ui::HBox();
        _flagLabel = new ayt::ui::TextLabel();
        _flagLabel->setFontSize(11);
        _flagLabel->setVerticalAlignment(
            ayt::ui::TextLabel::VAlignment::Center);
        _flag = new ayt::ui::CheckBox();
        _flagRow->addWidget(_flagLabel, 102.0f);
        _flagRow->addWidget(_flag, 0.0f);
        inspector->addWidget(_flagRow, 25.0f);
        _value = propertyRow(*inspector);
        auto* inspectorButtons = new ayt::ui::HBox();
        inspectorButtons->setSpacing(4.0f);
        button(*inspectorButtons, "ui.editor.game_flow.apply", L"Apply", 62.0f,
            [this]() { applyInspector(); });
        inspector->addWidget(inspectorButtons, 27.0f);
        label(*inspector, "ui.editor.game_flow.parameters", L"PARAMETERS");
        _arguments = new ayt::ui::ListView();
        _arguments->setItemHeight(21.0f);
        _arguments->setOnSelectionChanged([this](int index) {
            selectArgument(index);
        });
        inspector->addWidget(_arguments, 100.0f);
        _argumentValue = new ayt::ui::TextInput();
        _argumentValue->setId("gameflow_argument_value");
        _argumentValue->setPlaceholder(text(
            "ui.editor.game_flow.selected_parameter_value",
            L"Selected parameter value"));
        inspector->addWidget(_argumentValue, 25.0f);
        bindLocalized(_argumentValue,
            "ui.editor.game_flow.selected_parameter_value",
            L"Selected parameter value", LocalizedTarget::Placeholder);
        _argumentChoice = new ayt::ui::ComboBox();
        _argumentChoice->setId("gameflow_argument_choice");
        _argumentChoice->setVisible(false);
        inspector->addWidget(_argumentChoice, 25.0f);
        _argumentBool = new ayt::ui::CheckBox();
        _argumentBool->setId("gameflow_argument_boolean");
        _argumentBool->setVisible(false);
        inspector->addWidget(_argumentBool, 25.0f);
        auto* argumentButtons = new ayt::ui::HBox();
        argumentButtons->setSpacing(4.0f);
        button(*argumentButtons, "ui.editor.game_flow.set", L"Set", 50.0f,
            [this]() { setArgument(); });
        button(*argumentButtons, "ui.editor.game_flow.use_default",
            L"Use Default", 92.0f,
            [this]() { clearArgument(); });
        inspector->addWidget(argumentButtons, 27.0f);
        label(*inspector, "ui.editor.game_flow.diagnostics", L"DIAGNOSTICS");
        _diagnostics = new ayt::ui::ListView();
        _diagnostics->setItemHeight(23.0f);
        _diagnostics->setOnSelectionChanged([this](int index) {
            locateDiagnostic(index);
        });
        inspector->addWidget(_diagnostics, 0.0f);
        body->addWidget(inspector, 306.0f);

        root->addWidget(body, 0.0f);
        populatePalettes();
        populateSubflowPicker();
    }

    void populatePalettes()
    {
        std::vector<std::wstring> actions;
        for (const auto& type : _document->actionRegistry().actionTypes()) {
            actions.push_back(ayt::ui::decodeUtf8Text(type.id));
        }
        _actionPalette->setItems(actions);
        if (!actions.empty()) _actionPalette->setSelectedIndex(0);
        std::vector<std::wstring> guards;
        for (const auto& type : _document->actionRegistry().guardTypes()) {
            guards.push_back(ayt::ui::decodeUtf8Text(type.id));
        }
        _guardPalette->setItems(guards);
        if (!guards.empty()) _guardPalette->setSelectedIndex(0);
        if (_previewGuardPicker != nullptr) {
            _previewGuardPicker->setItems(guards);
            _previewGuardPicker->setSelectedIndex(guards.empty() ? -1 : 0);
        }
    }

    void buildCanvasMenu()
    {
        if (_canvas == nullptr) return;
        _canvasMenu = new ayt::ui::Menu();
        _canvasMenu->setSize({260.0f, 320.0f});
        auto add = [this](const std::wstring& title,
                          std::function<void()> action) {
            auto* item = _canvasMenu->addItem(title);
            item->setOnActivate(std::move(action));
        };
        add(text("ui.editor.game_flow.add_state", L"Add State"), [this]() {
            addObject(EditorGameFlowObjectKind::State);
        });
        add(text("ui.editor.game_flow.add_intent", L"Add Intent"), [this]() {
            addObject(EditorGameFlowObjectKind::Intent);
        });
        add(text("ui.editor.game_flow.add_subflow", L"Add Subflow"),
            [this]() { addSubflow(); });
        _canvasMenu->addSeparator();
        for (const auto& type : _document->actionRegistry().actionTypes()) {
            add(text("ui.editor.game_flow.add_action", L"Add Action")
                    + L": " + ayt::ui::decodeUtf8Text(type.id),
                [this, id = type.id]() {
                    const std::string transition = selectedTransition();
                    std::string error;
                    if (transition.empty()
                        || !_document->addAction(transition, id, &error)) {
                        setStatus(transition.empty()
                            ? "Select a Transition before adding an Action."
                            : error, true);
                    }
                });
        }
        _canvasMenu->addSeparator();
        add(text("ui.editor.game_flow.template", L"Template")
                + L": Main Menu → Result", [this]() {
            applyStarterTemplate();
        });
        add(text("ui.editor.game_flow.auto_layout", L"Auto Layout"), [this]() {
            if (_canvas != nullptr) _canvas->autoLayout();
        });
        _canvas->addChild(_canvasMenu);
    }

    void rebuildCanvasMenu()
    {
        if (_canvas == nullptr) return;
        if (_canvasMenu != nullptr) {
            _canvasMenu->close();
            _canvas->removeChild(_canvasMenu);
            ayt::ui::destroyWidgetTree(_canvasMenu);
            _canvasMenu = nullptr;
        }
        buildCanvasMenu();
    }

    void showCanvasMenu(FVector2 point)
    {
        if (_canvasMenu != nullptr && _canvas != nullptr) {
            _canvasMenu->open(_canvas, point);
        }
    }

    EditorGameFlowCanvasModel canvasModel() const
    {
        EditorGameFlowCanvasModel model;
        model.flow = _document->flow();
        model.selection = _document->selection();
        model.preview = _preview.snapshot();
        for (const auto& type : _document->actionRegistry().actionTypes()) {
            model.knownActions.insert(type.id);
        }
        return model;
    }

    std::filesystem::path canvasLayoutPath() const
    {
        const std::string identity = _document->path().empty()
            ? _document->flow().id
            : EditorDocumentManager::normalizeResourceKey(_document->path());
        std::uint64_t hash = 1469598103934665603ull;
        for (const unsigned char value : identity) {
            hash ^= value;
            hash *= 1099511628211ull;
        }
        std::ostringstream name;
        name << std::hex << std::setfill('0') << std::setw(16) << hash
             << ".layout";
        return std::filesystem::path(_host.projectRoot())
            / ".ayeditor" / "gameflow-layouts" / name.str();
    }

    void loadCanvasLayout()
    {
        if (_canvas == nullptr) return;
        const auto path = canvasLayoutPath();
        if (!ayt::io::File::exists(path.string())) return;
        std::istringstream source(ayt::io::File::readAllText(path.string()));
        std::string version;
        std::getline(source, version);
        if (version != "AYEDITOR_GAMEFLOW_LAYOUT 1") return;
        EditorGameFlowLayout positions;
        std::string id;
        FVector2 point{};
        while (source >> std::quoted(id) >> point.x >> point.y) {
            if (_document->flow().findState(id) != nullptr) {
                positions.insert_or_assign(id, point);
            }
        }
        _canvas->setManualPositions(std::move(positions));
    }

    void saveCanvasLayout(const EditorGameFlowLayout& positions) const
    {
        const auto path = canvasLayoutPath();
        std::error_code filesystemError;
        std::filesystem::create_directories(
            path.parent_path(), filesystemError);
        if (filesystemError) return;
        std::ostringstream output;
        output << "AYEDITOR_GAMEFLOW_LAYOUT 1\n";
        for (const auto& [id, point] : positions) {
            output << std::quoted(id) << ' ' << point.x << ' '
                   << point.y << '\n';
        }
        const std::string content = output.str();
        (void)ayt::io::File::atomicWrite(
            path.string(), content.data(), content.size());
    }

    void destroyTransitionMenu()
    {
        if (_transitionMenu == nullptr || _canvas == nullptr) return;
        _transitionMenu->close();
        _canvas->removeChild(_transitionMenu);
        ayt::ui::destroyWidgetTree(_transitionMenu);
        _transitionMenu = nullptr;
    }

    void showTransitionIntentMenu(
        std::string from, std::string to, FVector2 point)
    {
        destroyTransitionMenu();
        if (_canvas == nullptr) return;
        const auto& intents = _document->flow().intents;
        if (intents.empty()) {
            setLocalizedStatus(
                "ui.editor.game_flow.status.create_intent_before_connect",
                L"Create an Intent before connecting states.", true);
            return;
        }
        _transitionMenu = new ayt::ui::Menu();
        _transitionMenu->setSize({260.0f,
            10.0f + static_cast<float>(intents.size()) * 28.0f});
        for (const auto& intent : intents) {
            auto* item = _transitionMenu->addItem(
                ayt::ui::decodeUtf8Text(intent.id));
            item->setOnActivate([this, from, to, intentId = intent.id]() {
                std::string error;
                if (!_document->addTransition(
                        from, intentId, to, &error)) {
                    setStatus(error, true);
                }
                destroyTransitionMenu();
            });
        }
        _canvas->addChild(_transitionMenu);
        _transitionMenu->open(_canvas, point);
    }

    void applyStarterTemplate()
    {
        std::string error;
        if (!_document->applyTemplate(
                EditorGameFlowTemplate::MainMenuToResult, &error)) {
            setStatus(error, true);
            return;
        }
        if (_canvas != nullptr) _canvas->autoLayout();
    }

    bool copySelection()
    {
        if (_canvas == nullptr) return false;
        _clipboard = _document->copyObjects(_canvas->selectedObjects());
        if (_clipboard.empty()) {
            setStatus("Nothing copyable is selected.", true);
            return false;
        }
        setStatus("GameFlow selection copied.");
        return true;
    }

    bool pasteSelection()
    {
        std::string error;
        if (!_document->pasteObjects(_clipboard, &error)) {
            setStatus(error, true);
            return false;
        }
        return true;
    }

    std::vector<std::string> projectAssetsWithSuffix(
        std::string_view suffix) const
    {
        std::vector<std::string> result;
        std::string descriptorError;
        const EditorProjectDescriptor descriptor =
            EditorProjectDescriptor::load(_host.projectRoot(), &descriptorError);
        const std::filesystem::path root = std::filesystem::path(
            _host.projectRoot()) / (descriptor ? descriptor.assetRoot : "Assets");
        std::error_code error;
        if (!std::filesystem::is_directory(root, error)) return result;
        for (std::filesystem::recursive_directory_iterator iterator(
                 root, std::filesystem::directory_options::skip_permission_denied,
                 error), end;
             !error && iterator != end; iterator.increment(error)) {
            if (!iterator->is_regular_file(error)) continue;
            const std::string path = iterator->path().generic_string();
            if (path.size() < suffix.size()
                || path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) {
                continue;
            }
            result.push_back(std::filesystem::relative(
                iterator->path(), root, error).generic_string());
            error.clear();
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    void populateSubflowPicker()
    {
        if (_subflowPicker == nullptr) return;
        const auto assets = projectAssetsWithSuffix(".gameflow.json");
        std::vector<std::wstring> items;
        for (const auto& asset : assets) {
            if (_document->path().empty()
                || std::filesystem::path(_document->path()).filename()
                    != std::filesystem::path(asset).filename()) {
                items.push_back(ayt::ui::decodeUtf8Text(asset));
            }
        }
        _subflowPicker->setItems(items);
        _subflowPicker->setSelectedIndex(items.empty() ? -1 : 0);
    }

    const ayt::app::GameFlowActionTypeDefinition*
    selectedActionDefinition() const
    {
        const auto& selection = _document->selection();
        if (selection.kind != EditorGameFlowObjectKind::Action
            && selection.kind != EditorGameFlowObjectKind::ActionArgument) {
            return nullptr;
        }
        const auto transition = std::find_if(
            _document->flow().transitions.begin(),
            _document->flow().transitions.end(), [&](const auto& value) {
                return value.id == selection.ownerId;
            });
        if (transition == _document->flow().transitions.end()
            || selection.index >= transition->actions.size()) {
            return nullptr;
        }
        return _document->actionRegistry().findAction(
            transition->actions[selection.index].action);
    }

    std::optional<ayt::app::GameFlowReferenceKind>
    selectedArgumentReference(std::string_view argumentId) const
    {
        const auto* definition = selectedActionDefinition();
        if (definition == nullptr) return std::nullopt;
        const auto found = std::find_if(definition->references.begin(),
            definition->references.end(), [&](const auto& reference) {
                return reference.argumentId == argumentId;
            });
        return found == definition->references.end()
            ? std::nullopt : std::optional(found->kind);
    }

    std::vector<std::string> uiFlowReferenceChoices(
        ayt::app::GameFlowReferenceKind kind) const
    {
        std::vector<std::string> result;
        std::string error;
        const EditorProjectDescriptor descriptor =
            EditorProjectDescriptor::load(_host.projectRoot(), &error);
        if (!descriptor || descriptor.ui.flow.empty()) return result;
        const std::filesystem::path path = std::filesystem::path(
            _host.projectRoot()) / descriptor.assetRoot / descriptor.ui.flow;
        const std::string key = EditorDocumentManager::normalizeResourceKey(
            path.string());
        const ayt::ui::UIFlowDocument* flow = nullptr;
        std::shared_ptr<EditorUiFlowDocument> openDocument;
        for (const auto& record : _host.workspace().documents().records()) {
            auto candidate = std::dynamic_pointer_cast<EditorUiFlowDocument>(
                record.document);
            if (candidate == nullptr) continue;
            const std::string candidateKey =
                EditorDocumentManager::normalizeResourceKey(
                    candidate->path().empty()
                        ? record.resourceKey : candidate->path());
            if (candidateKey == key) {
                openDocument = std::move(candidate);
                flow = &openDocument->flow();
                break;
            }
        }
        EditorUiFlowDocument diskDocument;
        if (flow == nullptr) {
            if (!diskDocument.initialize(
                    path.string(), path.string(), &error)) {
                return result;
            }
            flow = &diskDocument.flow();
        }
        if (kind == ayt::app::GameFlowReferenceKind::UIFlowEntry) {
            for (const auto& value : flow->entries) {
                result.push_back(value.id);
            }
        } else if (kind == ayt::app::GameFlowReferenceKind::UIContext) {
            for (const auto& value : flow->contexts) {
                result.push_back(value.id);
            }
        } else if (kind == ayt::app::GameFlowReferenceKind::UISignal) {
            for (const auto& value : flow->signals) {
                result.push_back(value.id);
            }
        }
        return result;
    }

    std::vector<std::string> argumentChoices(
        const EditorGameFlowArgumentView& argument) const
    {
        const auto* definition = selectedActionDefinition();
        if (definition != nullptr && definition->id == "ui.context.activate"
            && argument.id == "scope") {
            return {"application", "world", "entity"};
        }
        const auto reference = selectedArgumentReference(argument.id);
        if (!reference.has_value()) return {};
        if (*reference == ayt::app::GameFlowReferenceKind::WorldId) {
            std::string error;
            const EditorProjectDescriptor descriptor =
                EditorProjectDescriptor::load(_host.projectRoot(), &error);
            std::vector<std::string> result;
            for (const auto& world : descriptor.worlds) result.push_back(world.id);
            return result;
        }
        if (*reference == ayt::app::GameFlowReferenceKind::AssetPath) {
            return projectAssetsWithSuffix({});
        }
        return uiFlowReferenceChoices(*reference);
    }

    void setStatus(std::wstring message, bool error = false)
    {
        if (_status == nullptr) return;
        _status->setText(message);
        _status->setTextColor(error
            ? FVector4{0.96f, 0.38f, 0.40f, 1.0f}
            : FVector4{0.42f, 0.80f, 0.58f, 1.0f});
        _host.setStatusText(message);
        _host.requestRepaint();
    }

    void setStatus(std::string message, bool error = false)
    {
        setStatus(ayt::ui::decodeUtf8Text(message), error);
    }

    void setLocalizedStatus(std::string_view key,
                            std::wstring_view fallback,
                            bool error = false)
    {
        setStatus(text(key, fallback), error);
    }

    std::string selectedTransition() const
    {
        const auto& selection = _document->selection();
        if (selection.kind == EditorGameFlowObjectKind::Transition) {
            return selection.id;
        }
        if (selection.kind == EditorGameFlowObjectKind::Guard
            || selection.kind == EditorGameFlowObjectKind::Action
            || selection.kind == EditorGameFlowObjectKind::ActionArgument) {
            return selection.ownerId;
        }
        return {};
    }

    void addObject(EditorGameFlowObjectKind kind)
    {
        std::string error;
        if (!_document->addObject(kind, {}, &error)) setStatus(error, true);
    }

    bool deleteSelected()
    {
        std::string error;
        const auto selected = _canvas == nullptr
            ? std::vector<EditorGameFlowSelection>{}
            : _canvas->selectedObjects();
        const bool deleted = selected.size() > 1u
            ? _document->deleteObjects(selected, &error)
            : _document->deleteSelection(&error);
        if (!deleted) {
            setStatus(error, true);
            return false;
        }
        return true;
    }

    void addIntentField()
    {
        const auto& selection = _document->selection();
        const std::string intent = selection.kind
                == EditorGameFlowObjectKind::Intent
            ? selection.id
            : selection.kind == EditorGameFlowObjectKind::IntentField
                ? selection.ownerId : std::string{};
        std::string error;
        if (intent.empty()
            || !_document->addObject(
                EditorGameFlowObjectKind::IntentField, intent, &error)) {
            setStatus(intent.empty() ? "Select an Intent before adding a field."
                                     : error, true);
        }
    }

    void addPaletteAction()
    {
        const std::string transition = selectedTransition();
        const std::string type = encodeUtf8(_actionPalette->getSelectedItem());
        std::string error;
        if (transition.empty() || type.empty()
            || !_document->addAction(transition, type, &error)) {
            setStatus(transition.empty()
                ? "Select a Transition before adding an Action."
                : type.empty() ? "The Action palette is empty." : error, true);
        }
    }

    void addSubflow()
    {
        const std::string transition = selectedTransition();
        const std::string subflow = _subflowPicker == nullptr
            ? std::string{}
            : encodeUtf8(_subflowPicker->getSelectedItem());
        std::string error;
        if (transition.empty() || subflow.empty()
            || !_document->addSubflowCall(transition, subflow, &error)) {
            setStatus(transition.empty()
                ? "Select a Transition before adding a Subflow."
                : subflow.empty() ? "No GameFlow asset is available."
                                  : error, true);
        }
    }

    void setPaletteGuard()
    {
        const std::string transition = selectedTransition();
        const std::string type = encodeUtf8(_guardPalette->getSelectedItem());
        std::string error;
        if (transition.empty() || type.empty()
            || !_document->setTransitionGuard(transition, type, &error)) {
            setStatus(transition.empty()
                ? "Select a Transition before setting a Guard."
                : type.empty() ? "The Guard palette is empty." : error, true);
        }
    }

    void clearGuard()
    {
        const std::string transition = selectedTransition();
        std::string error;
        if (transition.empty()
            || !_document->setTransitionGuard(transition, {}, &error)) {
            setStatus(transition.empty()
                ? "Select a Transition before clearing its Guard." : error,
                true);
        }
    }

    void moveAction(std::ptrdiff_t offset)
    {
        std::string error;
        if (!_document->moveSelectedAction(offset, &error)) {
            setStatus(error, true);
        }
    }

    bool save()
    {
        std::string error;
        bool saved = false;
        if (_document->path().empty()) {
            auto* provider =
                dynamic_cast<IEditorDocumentSavePathProvider*>(&_host);
            if (provider == nullptr) {
                setLocalizedStatus(
                    "ui.editor.game_flow.status.no_save_provider",
                    L"Save failed: this host has no Save As provider.", true);
                return false;
            }
            const std::string path =
                provider->chooseDocumentSavePath(*_document, true);
            if (path.empty()) {
                setLocalizedStatus(
                    "ui.editor.game_flow.status.save_cancelled",
                    L"Save cancelled.");
                return false;
            }
            saved = _document->saveAs(path, &error);
        } else {
            saved = _document->save(&error);
        }
        if (!saved) {
            setStatus(text("ui.editor.game_flow.status.save_failed",
                L"Save failed: ") + ayt::ui::decodeUtf8Text(error), true);
            return false;
        }
        if (_canvas != nullptr) {
            saveCanvasLayout(_canvas->manualPositions());
        }
        setStatus(text("ui.editor.game_flow.status.saved", L"Saved ")
            + ayt::ui::decodeUtf8Text(_document->title()));
        return true;
    }

    void validate()
    {
        std::vector<ayt::app::GameFlowDiagnostic> diagnostics;
        ayt::app::GameFlowPlan plan;
        const bool valid = _document->buildPlan(plan, &diagnostics);
        refreshDiagnostics(diagnostics);
        setLocalizedStatus(valid
                ? "ui.editor.game_flow.status.validation_succeeded"
                : "ui.editor.game_flow.status.validation_failed",
            valid ? L"GameFlow validation succeeded."
                  : L"GameFlow validation failed.", !valid);
    }

    void applyInspector()
    {
        EditorGameFlowProperties properties = _document->selectedProperties();
        const auto labels = _document->selectedPropertyLabels();
        const auto kind = _document->selection().kind;
        const bool editableId = kind == EditorGameFlowObjectKind::Document
            || kind == EditorGameFlowObjectKind::Intent
            || kind == EditorGameFlowObjectKind::IntentField
            || kind == EditorGameFlowObjectKind::State
            || kind == EditorGameFlowObjectKind::Transition;
        if (editableId) properties.id = encodeUtf8(_id.input->getText());
        if (!labels.first.empty()) {
            properties.first = propertyValue(_first);
        }
        if (!labels.second.empty()) {
            properties.second = propertyValue(_second);
        }
        if (!labels.third.empty()) {
            properties.third = propertyValue(_third);
        }
        if (!labels.fourth.empty()) {
            properties.fourth = propertyValue(_fourth);
        }
        if (!labels.fifth.empty()) {
            properties.fifth = propertyValue(_fifth);
        }
        if (!labels.sixth.empty()) {
            properties.sixth = propertyValue(_sixth);
        }
        if (!labels.flag.empty()) properties.flag = _flag->isChecked();
        try {
            if (!labels.number.empty()) {
                properties.number = std::stod(
                    encodeUtf8(_number.input->getText()));
            }
            if (!labels.integer.empty()) {
                properties.integer = static_cast<std::int32_t>(
                    std::stoll(encodeUtf8(_integer.input->getText())));
            }
        } catch (...) {
            setLocalizedStatus(
                "ui.editor.game_flow.status.invalid_number",
                L"Inspector number or integer is invalid.", true);
            return;
        }
        if (_document->selection().kind
            == EditorGameFlowObjectKind::IntentField) {
            ayt::app::GameFlowValueType type =
                ayt::app::GameFlowValueType::String;
            const std::string typeText = properties.first;
            if (typeText == "boolean") type = ayt::app::GameFlowValueType::Boolean;
            else if (typeText == "integer") type = ayt::app::GameFlowValueType::Integer;
            else if (typeText == "number") type = ayt::app::GameFlowValueType::Number;
            bool valid = false;
            properties.value = parseValue(_value.input->getText(), type, valid);
            if (!valid) {
                setLocalizedStatus(
                    "ui.editor.game_flow.status.value_type_mismatch",
                    L"Inspector value does not match its type.", true);
                return;
            }
        }
        std::string error;
        if (!_document->applySelectedProperties(properties, &error)) {
            setStatus(error, true);
        }
    }

    void selectArgument(int index)
    {
        if (_refreshing || index < 0
            || static_cast<std::size_t>(index) >= _argumentItems.size()) {
            return;
        }
        const auto current = _document->selection();
        const auto& argument = _argumentItems[static_cast<std::size_t>(index)];
        if (current.kind == EditorGameFlowObjectKind::Guard
            || current.kind == EditorGameFlowObjectKind::Action
            || current.kind == EditorGameFlowObjectKind::ActionArgument) {
            (void)_document->select({EditorGameFlowObjectKind::ActionArgument,
                argument.id, current.ownerId, current.index});
        }
    }

    void setArgument()
    {
        const auto& selection = _document->selection();
        if (selection.kind != EditorGameFlowObjectKind::ActionArgument) {
            setLocalizedStatus(
                "ui.editor.game_flow.status.select_parameter",
                L"Select a parameter before setting its value.", true);
            return;
        }
        const auto found = std::find_if(_argumentItems.begin(),
            _argumentItems.end(), [&](const auto& value) {
                return value.id == selection.id;
            });
        if (found == _argumentItems.end()) return;
        bool valid = true;
        ayt::app::GameFlowValue value;
        if (found->type == ayt::app::GameFlowValueType::Boolean
            && _argumentBool != nullptr && _argumentBool->isVisible()) {
            value = ayt::app::GameFlowValue(_argumentBool->isChecked());
        } else if (_argumentChoice != nullptr
                   && _argumentChoice->isVisible()) {
            value = ayt::app::GameFlowValue(
                encodeUtf8(_argumentChoice->getSelectedItem()));
        } else {
            value = parseValue(_argumentValue->getText(), found->type, valid);
        }
        std::string error;
        if (!valid || !_document->setSelectedArgument(
                selection.id, std::move(value), &error)) {
            setStatus(valid ? error : "Parameter value has the wrong type.",
                true);
        }
    }

    void configureArgumentEditor(const EditorGameFlowArgumentView* argument)
    {
        if (_argumentValue == nullptr || _argumentChoice == nullptr
            || _argumentBool == nullptr) return;
        _argumentValue->setVisible(false);
        _argumentChoice->setVisible(false);
        _argumentBool->setVisible(false);
        if (argument == nullptr) return;
        if (argument->type == ayt::app::GameFlowValueType::Boolean) {
            _argumentBool->setVisible(true);
            const bool* value = std::get_if<bool>(&argument->value.data);
            _argumentBool->setChecked(value != nullptr && *value);
            return;
        }
        const auto choices = argumentChoices(*argument);
        if (!choices.empty()) {
            std::vector<std::wstring> items;
            items.reserve(choices.size());
            for (const auto& choice : choices) {
                items.push_back(ayt::ui::decodeUtf8Text(choice));
            }
            const std::wstring current = ayt::ui::decodeUtf8Text(
                valueText(argument->value));
            if (!current.empty()
                && std::find(items.begin(), items.end(), current)
                    == items.end()) {
                items.push_back(current);
            }
            _argumentChoice->setItems(items);
            const auto found = std::find(items.begin(), items.end(), current);
            _argumentChoice->setSelectedIndex(found == items.end()
                ? (items.empty() ? -1 : 0)
                : static_cast<int>(std::distance(items.begin(), found)));
            _argumentChoice->setVisible(true);
            return;
        }
        _argumentValue->setText(
            ayt::ui::decodeUtf8Text(valueText(argument->value)));
        _argumentValue->setVisible(true);
    }

    void clearArgument()
    {
        const auto& selection = _document->selection();
        std::string error;
        if (selection.kind != EditorGameFlowObjectKind::ActionArgument
            || !_document->clearSelectedArgument(selection.id, &error)) {
            setStatus(selection.kind != EditorGameFlowObjectKind::ActionArgument
                ? "Select an authored parameter before using its default."
                : error, true);
        }
    }

    bool resolveSubflow(std::string_view flowId,
                        ayt::app::GameFlowDocument& result,
                        std::string& error) const
    {
        namespace fs = std::filesystem;
        if (flowId.empty()) {
            error = "Subflow id is empty.";
            return false;
        }
        std::string descriptorError;
        const EditorProjectDescriptor descriptor =
            EditorProjectDescriptor::load(_host.projectRoot(),
                                          &descriptorError);
        const fs::path assetRoot = fs::path(_host.projectRoot())
            / (descriptor ? descriptor.assetRoot : "Assets");
        const fs::path candidate = (assetRoot / fs::path(flowId)).lexically_normal();
        const fs::path relative = candidate.lexically_relative(
            assetRoot.lexically_normal());
        if (relative.empty() || relative.is_absolute()
            || (!relative.empty() && *relative.begin() == "..")) {
            error = "Subflow path escapes the project asset root.";
            return false;
        }

        const std::string key = EditorDocumentManager::normalizeResourceKey(
            candidate.string());
        for (const auto& record : _host.workspace().documents().records()) {
            auto open = std::dynamic_pointer_cast<EditorGameFlowDocument>(
                record.document);
            if (open == nullptr) continue;
            const std::string openKey = EditorDocumentManager::normalizeResourceKey(
                open->path().empty() ? record.resourceKey : open->path());
            if (openKey == key) {
                result = open->flow();
                error.clear();
                return true;
            }
        }

        if (!ayt::io::File::exists(candidate.string())) {
            error = "Subflow asset does not exist: " + candidate.string();
            return false;
        }
        const std::string source = ayt::io::File::readAllText(candidate.string());
        std::vector<ayt::app::GameFlowDiagnostic> diagnostics;
        if (!ayt::app::GameFlowSerializer::deserialize(
                source, result, &diagnostics)) {
            error = diagnostics.empty()
                ? "Subflow asset could not be parsed: " + candidate.string()
                : diagnostics.front().path + ": "
                    + diagnostics.front().message;
            return false;
        }
        error.clear();
        return true;
    }

    void restartPreview()
    {
        std::string error;
        if (!_preview.rebuild(
                _document->flow(), _document->actionRegistry(),
                [this](std::string_view flowId,
                       ayt::app::GameFlowDocument& document,
                       std::string& message) {
                    return resolveSubflow(flowId, document, message);
                }, &error)) {
            setStatus(text("ui.editor.game_flow.status.preview_failed",
                L"Preview failed: ") + ayt::ui::decodeUtf8Text(error), true);
            syncPreviewPresentation();
            return;
        }
        _previewStale = false;
        syncPreviewPresentation();
        setLocalizedStatus(
            "ui.editor.game_flow.status.preview_restarted",
            L"GameFlow preview restarted.");
    }

    void sendIntent()
    {
        const std::string intent = encodeUtf8(_intentPicker->getSelectedItem());
        if (intent.empty()) {
            setLocalizedStatus(
                "ui.editor.game_flow.status.no_intent",
                L"The flow has no Intent to send.", true);
            return;
        }
        if (_previewStale) {
            setLocalizedStatus("ui.editor.game_flow.status.preview_stale",
                L"Preview is stale. Restart it before sending an Intent.",
                true);
            return;
        }
        ayt::app::GameFlowPayload payload;
        if (const auto* definition = _document->flow().findIntent(intent)) {
            for (const auto& field : definition->payload) {
                const auto draft = _previewPayloads.find(intent);
                const auto authored = draft == _previewPayloads.end()
                    ? ayt::app::GameFlowPayload::const_iterator{}
                    : draft->second.find(field.id);
                if (draft != _previewPayloads.end()
                    && authored != draft->second.end()) {
                    payload.emplace(field.id, authored->second);
                } else if (!std::holds_alternative<std::monostate>(
                               field.defaultValue.data)) {
                    payload.emplace(field.id, field.defaultValue);
                } else if (field.required) {
                    setStatus("Required payload field '" + field.id
                        + "' has no value.", true);
                    return;
                }
            }
        }
        const auto result = _preview.request(intent, std::move(payload));
        if (!result) setStatus(result.message, true);
        _preview.update(0.0);
        syncPreviewPresentation();
    }

    void completeAction()
    {
        std::string error;
        if (!_preview.completePending(&error)) setStatus(error, true);
        _preview.update(0.0);
        syncPreviewPresentation();
    }

    void failAction()
    {
        std::string error;
        if (!_preview.failPending("Failed from preview toolbar.", &error)) {
            setStatus(error, true);
        }
        _preview.update(0.0);
        syncPreviewPresentation();
    }

    static void setPropertyRow(PropertyRow& row,
                               const std::wstring& labelText,
                               const std::string& value,
                               bool readOnly = false)
    {
        const bool visible = !labelText.empty();
        row.row->setVisible(visible);
        if (!visible) return;
        row.label->setText(labelText);
        row.input->setVisible(true);
        row.input->setReadOnly(readOnly);
        row.choice->setVisible(false);
        row.input->setText(ayt::ui::decodeUtf8Text(value));
    }

    static void setPropertyChoice(PropertyRow& row,
                                  const std::wstring& labelText,
                                  const std::string& value,
                                  const std::vector<std::string>& choices,
                                  bool allowEmpty = false)
    {
        row.row->setVisible(!labelText.empty());
        if (labelText.empty()) return;
        row.label->setText(labelText);
        row.input->setVisible(false);
        row.choice->setVisible(true);
        std::vector<std::wstring> items;
        items.reserve(choices.size() + (allowEmpty ? 1u : 0u));
        if (allowEmpty) items.emplace_back();
        for (const auto& choice : choices) {
            items.push_back(ayt::ui::decodeUtf8Text(choice));
        }
        const std::wstring selected = ayt::ui::decodeUtf8Text(value);
        if (!selected.empty()
            && std::find(items.begin(), items.end(), selected) == items.end()) {
            items.push_back(selected);
        }
        row.choice->setItems(items);
        const auto found = std::find(items.begin(), items.end(), selected);
        row.choice->setSelectedIndex(found == items.end()
            ? (items.empty() ? -1 : 0)
            : static_cast<int>(std::distance(items.begin(), found)));
    }

    static std::string propertyValue(const PropertyRow& row)
    {
        return row.choice != nullptr && row.choice->isVisible()
            ? encodeUtf8(row.choice->getSelectedItem())
            : encodeUtf8(row.input->getText());
    }

    void refresh()
    {
        if (_outline == nullptr) return;
        _refreshPending = false;
        _refreshing = true;
        _outlineItems = _document->outline();
        std::vector<std::wstring> labels;
        labels.reserve(_outlineItems.size());
        int selectedIndex = -1;
        for (std::size_t index = 0; index < _outlineItems.size(); ++index) {
            const auto& item = _outlineItems[index];
            labels.push_back(std::wstring(static_cast<std::size_t>(
                    (std::max)(0, item.depth)) * 2u, L' ')
                + localizedOutlineLabel(item));
            if (item.selection == _document->selection()) {
                selectedIndex = static_cast<int>(index);
            }
        }
        _outline->setItems(labels);
        _outline->setSelectedIndex(selectedIndex);

        const auto properties = _document->selectedProperties();
        const auto propertyLabels = _document->selectedPropertyLabels();
        _inspectorHeading->setText(
            text("ui.editor.game_flow.inspector", L"INSPECTOR")
            + L"  /  " + localizedKind(_document->selection().kind));
        const auto kind = _document->selection().kind;
        const bool editableId = kind == EditorGameFlowObjectKind::Document
            || kind == EditorGameFlowObjectKind::Intent
            || kind == EditorGameFlowObjectKind::IntentField
            || kind == EditorGameFlowObjectKind::State
            || kind == EditorGameFlowObjectKind::Transition;
        setPropertyRow(_id, editableId ? localizedProperty("ID")
                                      : std::wstring{}, properties.id);
        std::vector<std::string> stateIds;
        std::vector<std::string> intentIds;
        std::vector<std::string> guardIds;
        stateIds.reserve(_document->flow().states.size());
        intentIds.reserve(_document->flow().intents.size());
        for (const auto& state : _document->flow().states) {
            stateIds.push_back(state.id);
        }
        for (const auto& intent : _document->flow().intents) {
            intentIds.push_back(intent.id);
        }
        for (const auto& guard : _document->actionRegistry().guardTypes()) {
            guardIds.push_back(guard.id);
        }
        setPropertyRow(_first, localizedProperty(propertyLabels.first), properties.first);
        setPropertyRow(_second, localizedProperty(propertyLabels.second), properties.second);
        setPropertyRow(_third, localizedProperty(propertyLabels.third), properties.third);
        setPropertyRow(_fourth, localizedProperty(propertyLabels.fourth), properties.fourth);
        setPropertyRow(_fifth, localizedProperty(propertyLabels.fifth), properties.fifth);
        setPropertyRow(_sixth, localizedProperty(propertyLabels.sixth), properties.sixth);
        if (kind == EditorGameFlowObjectKind::Document) {
            setPropertyChoice(_first, localizedProperty(propertyLabels.first),
                properties.first, stateIds);
        } else if (kind == EditorGameFlowObjectKind::IntentField) {
            setPropertyChoice(_first, localizedProperty(propertyLabels.first),
                properties.first, {"boolean", "integer", "number", "string"});
        } else if (kind == EditorGameFlowObjectKind::State) {
            std::vector<std::string> parentChoices;
            std::vector<std::string> childChoices;
            for (const auto& state : _document->flow().states) {
                if (state.parent == properties.id) {
                    childChoices.push_back(state.id);
                }
                if (state.id == properties.id) continue;
                bool descendant = false;
                const auto* cursor = &state;
                std::unordered_set<std::string> visited;
                while (cursor != nullptr && !cursor->parent.empty()
                       && visited.insert(cursor->id).second) {
                    if (cursor->parent == properties.id) {
                        descendant = true;
                        break;
                    }
                    cursor = _document->flow().findState(cursor->parent);
                }
                if (!descendant) parentChoices.push_back(state.id);
            }
            setPropertyChoice(_first, localizedProperty(propertyLabels.first),
                properties.first, parentChoices, true);
            setPropertyChoice(_second, localizedProperty(propertyLabels.second),
                properties.second, childChoices, true);
        } else if (kind == EditorGameFlowObjectKind::Transition) {
            setPropertyChoice(_first, localizedProperty(propertyLabels.first),
                properties.first, stateIds);
            setPropertyChoice(_second, localizedProperty(propertyLabels.second),
                properties.second, intentIds);
            setPropertyChoice(_third, localizedProperty(propertyLabels.third),
                properties.third, stateIds);
            setPropertyChoice(_fourth, localizedProperty(propertyLabels.fourth),
                properties.fourth, guardIds, true);
            setPropertyChoice(_fifth, localizedProperty(propertyLabels.fifth),
                properties.fifth, stateIds, true);
            setPropertyChoice(_sixth, localizedProperty(propertyLabels.sixth),
                properties.sixth, stateIds, true);
        } else if (kind == EditorGameFlowObjectKind::Guard
                   || kind == EditorGameFlowObjectKind::Action
                   || kind == EditorGameFlowObjectKind::ActionArgument) {
            setPropertyRow(_first, localizedProperty(propertyLabels.first),
                properties.first, true);
        }
        setPropertyRow(_number, localizedProperty(propertyLabels.number),
            std::to_string(properties.number));
        setPropertyRow(_integer, localizedProperty(propertyLabels.integer),
            std::to_string(properties.integer));
        _flagRow->setVisible(!propertyLabels.flag.empty());
        _flagLabel->setText(localizedProperty(propertyLabels.flag));
        _flag->setChecked(properties.flag);
        setPropertyRow(_value, localizedProperty(propertyLabels.value),
            valueText(properties.value));

        _argumentItems = _document->selectedArguments();
        configureArgumentEditor(nullptr);
        std::vector<std::wstring> arguments;
        arguments.reserve(_argumentItems.size());
        int selectedArgument = -1;
        for (std::size_t index = 0; index < _argumentItems.size(); ++index) {
            const auto& item = _argumentItems[index];
            std::string text = item.id + " : "
                + ayt::app::gameFlowValueTypeName(item.type);
            std::wstring display = ayt::ui::decodeUtf8Text(text);
            display += L"  [" + (item.authored
                ? this->text("ui.editor.game_flow.authored", L"authored")
                : this->text("ui.editor.game_flow.default_value", L"default"))
                + L"]";
            if (!item.known) {
                display += L"  [" + this->text(
                    "ui.editor.game_flow.unknown_schema", L"unknown schema")
                    + L"]";
            }
            arguments.push_back(std::move(display));
            if (_document->selection().kind
                    == EditorGameFlowObjectKind::ActionArgument
                && _document->selection().id == item.id) {
                selectedArgument = static_cast<int>(index);
                configureArgumentEditor(&item);
            }
        }
        _arguments->setItems(arguments);
        _arguments->setSelectedIndex(selectedArgument);
        refreshDiagnostics(_document->diagnostics());
        populateIntentPicker();
        if (_canvas != nullptr) _canvas->markDirty();
        _refreshing = false;
        _host.requestRepaint();
    }

    void populateIntentPicker()
    {
        const std::wstring previous = _intentPicker->getSelectedItem();
        std::vector<std::wstring> intents;
        for (const auto& intent : _document->flow().intents) {
            intents.push_back(ayt::ui::decodeUtf8Text(intent.id));
        }
        _intentPicker->setItems(intents);
        const auto found = std::find(intents.begin(), intents.end(), previous);
        _intentPicker->setSelectedIndex(found == intents.end()
            ? (intents.empty() ? -1 : 0)
            : static_cast<int>(std::distance(intents.begin(), found)));
        refreshPreviewPayloadFields();
    }

    void refreshPreviewPayloadFields()
    {
        if (_intentPicker == nullptr || _intentFieldPicker == nullptr) return;
        const std::wstring previous = _intentFieldPicker->getSelectedItem();
        _previewIntentFields.clear();
        std::vector<std::wstring> fields;
        const std::string intent = encodeUtf8(_intentPicker->getSelectedItem());
        if (const auto* definition = _document->flow().findIntent(intent)) {
            _previewIntentFields = definition->payload;
            fields.reserve(_previewIntentFields.size());
            for (const auto& field : _previewIntentFields) {
                std::string text = field.id + " : "
                    + ayt::app::gameFlowValueTypeName(field.type);
                if (field.required) text += " *";
                fields.push_back(ayt::ui::decodeUtf8Text(text));
            }
        }
        _intentFieldPicker->setItems(fields);
        const auto found = std::find(fields.begin(), fields.end(), previous);
        _intentFieldPicker->setSelectedIndex(found == fields.end()
            ? (fields.empty() ? -1 : 0)
            : static_cast<int>(std::distance(fields.begin(), found)));
        syncPreviewPayloadValue();
    }

    void syncPreviewPayloadValue()
    {
        if (_intentFieldValue == nullptr || _intentFieldPicker == nullptr
            || _intentPicker == nullptr) {
            return;
        }
        const int index = _intentFieldPicker->getSelectedIndex();
        if (index < 0
            || static_cast<std::size_t>(index) >= _previewIntentFields.size()) {
            _intentFieldValue->setText({});
            return;
        }
        const auto& field = _previewIntentFields[static_cast<std::size_t>(index)];
        const std::string intent = encodeUtf8(_intentPicker->getSelectedItem());
        const auto draft = _previewPayloads.find(intent);
        if (draft != _previewPayloads.end()) {
            const auto value = draft->second.find(field.id);
            if (value != draft->second.end()) {
                _intentFieldValue->setText(
                    ayt::ui::decodeUtf8Text(valueText(value->second)));
                return;
            }
        }
        _intentFieldValue->setText(
            ayt::ui::decodeUtf8Text(valueText(field.defaultValue)));
    }

    void setPreviewPayloadField()
    {
        const int index = _intentFieldPicker->getSelectedIndex();
        if (index < 0
            || static_cast<std::size_t>(index) >= _previewIntentFields.size()) {
            setLocalizedStatus(
                "ui.editor.game_flow.status.select_payload_field",
                L"Select an Intent payload field first.", true);
            return;
        }
        const auto& field = _previewIntentFields[static_cast<std::size_t>(index)];
        bool valid = false;
        auto value = parseValue(_intentFieldValue->getText(), field.type, valid);
        if (!valid || (field.required
                       && std::holds_alternative<std::monostate>(value.data))) {
            setStatus("Payload field '" + field.id
                + "' has an invalid value.", true);
            return;
        }
        const std::string intent = encodeUtf8(_intentPicker->getSelectedItem());
        if (std::holds_alternative<std::monostate>(value.data)) {
            _previewPayloads[intent].erase(field.id);
        } else {
            _previewPayloads[intent].insert_or_assign(
                field.id, std::move(value));
        }
        setStatus("Preview payload field set: " + field.id);
    }

    void clearPreviewPayloadField()
    {
        const int index = _intentFieldPicker->getSelectedIndex();
        if (index < 0
            || static_cast<std::size_t>(index) >= _previewIntentFields.size()) {
            return;
        }
        const auto& field = _previewIntentFields[static_cast<std::size_t>(index)];
        const std::string intent = encodeUtf8(_intentPicker->getSelectedItem());
        const auto draft = _previewPayloads.find(intent);
        if (draft != _previewPayloads.end()) draft->second.erase(field.id);
        syncPreviewPayloadValue();
        if (field.required
            && std::holds_alternative<std::monostate>(field.defaultValue.data)) {
            setLocalizedStatus(
                "ui.editor.game_flow.status.required_field_cleared",
                L"Required field cleared; enter a value before Send.", true);
        } else {
            setStatus("Preview payload field uses its default: " + field.id);
        }
    }

    void applyPreviewGuard()
    {
        if (_previewGuardPicker == nullptr
            || _previewGuardAccepted == nullptr) return;
        const std::string guard = encodeUtf8(
            _previewGuardPicker->getSelectedItem());
        if (guard.empty()) {
            setLocalizedStatus(
                "ui.editor.game_flow.status.no_registered_guard",
                L"No registered Guard is available.", true);
            return;
        }
        _preview.setGuardResult(guard, _previewGuardAccepted->isChecked());
        setStatus(text("ui.editor.game_flow.status.guard_override_updated",
            L"Preview Guard override updated: ")
            + ayt::ui::decodeUtf8Text(guard));
    }

    void refreshDiagnostics(
        const std::vector<ayt::app::GameFlowDiagnostic>& diagnostics)
    {
        if (_diagnostics == nullptr) return;
        _diagnosticItems = diagnostics;
        std::vector<std::wstring> items;
        if (diagnostics.empty()) {
            items.push_back(text(
                "ui.editor.game_flow.no_diagnostics", L"No diagnostics."));
            _diagnostics->setItems(items);
            return;
        }
        for (const auto& diagnostic : diagnostics) {
            std::wstring output = diagnostic.severity
                    == ayt::app::GameFlowDiagnosticSeverity::Error
                ? text("ui.editor.game_flow.error", L"Error")
                : text("ui.editor.game_flow.warning", L"Warning");
            if (!diagnostic.path.empty()) {
                output += L"  " + ayt::ui::decodeUtf8Text(diagnostic.path);
            }
            output += L"  |  "
                + ayt::ui::decodeUtf8Text(diagnostic.message);
            items.push_back(std::move(output));
        }
        _diagnostics->setItems(items);
        _diagnostics->setSelectedIndex(-1);
    }

    static std::size_t diagnosticIndex(
        const std::string& path, const std::string& token)
    {
        const std::size_t start = path.find(token);
        if (start == std::string::npos) return kEditorGameFlowNoIndex;
        const std::size_t digits = start + token.size();
        const std::size_t close = path.find(']', digits);
        if (close == std::string::npos || close == digits) {
            return kEditorGameFlowNoIndex;
        }
        try {
            return static_cast<std::size_t>(
                std::stoull(path.substr(digits, close - digits)));
        } catch (...) {
            return kEditorGameFlowNoIndex;
        }
    }

    void locateDiagnostic(int selected)
    {
        if (_refreshing || selected < 0
            || static_cast<std::size_t>(selected)
                >= _diagnosticItems.size()) {
            return;
        }
        const auto& diagnostic =
            _diagnosticItems[static_cast<std::size_t>(selected)];
        const auto& flow = _document->flow();
        EditorGameFlowSelection target;
        bool resolved = false;

        const std::size_t transitionIndex =
            diagnosticIndex(diagnostic.path, "$.transitions[");
        if (transitionIndex < flow.transitions.size()) {
            const auto& transition = flow.transitions[transitionIndex];
            const std::size_t actionIndex =
                diagnosticIndex(diagnostic.path, ".actions[");
            if (actionIndex < transition.actions.size()) {
                target = {EditorGameFlowObjectKind::Action,
                    transition.actions[actionIndex].action,
                    transition.id, actionIndex};
            } else if (diagnostic.path.find(".guard")
                       != std::string::npos
                       && !transition.guard.guard.empty()) {
                target = {EditorGameFlowObjectKind::Guard,
                    transition.guard.guard, transition.id};
            } else {
                target = {EditorGameFlowObjectKind::Transition,
                    transition.id};
            }
            resolved = true;
        }

        const std::size_t stateIndex =
            diagnosticIndex(diagnostic.path, "$.states[");
        if (!resolved && stateIndex < flow.states.size()) {
            target = {EditorGameFlowObjectKind::State,
                flow.states[stateIndex].id};
            resolved = true;
        }

        const std::size_t intentIndex =
            diagnosticIndex(diagnostic.path, "$.intents[");
        if (!resolved && intentIndex < flow.intents.size()) {
            const auto& intent = flow.intents[intentIndex];
            const std::size_t fieldIndex =
                diagnosticIndex(diagnostic.path, ".payload[");
            if (fieldIndex < intent.payload.size()) {
                target = {EditorGameFlowObjectKind::IntentField,
                    intent.payload[fieldIndex].id, intent.id};
            } else {
                target = {EditorGameFlowObjectKind::Intent, intent.id};
            }
            resolved = true;
        }

        if (!resolved && (diagnostic.path.empty()
                          || diagnostic.path == "$")) {
            target = {};
            resolved = true;
        }
        if (resolved) (void)_document->select(std::move(target));
        setStatus((resolved ? "Located diagnostic: " : "Diagnostic: ")
            + diagnostic.message,
            diagnostic.severity
                == ayt::app::GameFlowDiagnosticSeverity::Error);
    }

    void syncPreviewPresentation()
    {
        _lastSnapshot = _preview.snapshot();
        _lastTraceSize = _preview.trace().size();
        const bool waitingForAction = _lastSnapshot.executionId != 0u;
        if (_completePreview != nullptr) {
            _completePreview->setEnabled(waitingForAction);
        }
        if (_failPreview != nullptr) {
            _failPreview->setEnabled(waitingForAction);
        }
        if (_cancelPreview != nullptr) {
            _cancelPreview->setEnabled(_lastSnapshot.busy);
        }
        if (_previewStatus != nullptr) {
            std::wstring output;
            if (_previewStale) {
                output += text("ui.editor.game_flow.stale", L"STALE")
                    + L"  |  ";
            }
            output += _lastSnapshot.currentStateId.empty()
                ? text("ui.editor.game_flow.no_state", L"no state")
                : ayt::ui::decodeUtf8Text(_lastSnapshot.currentStateId);
            if (!_lastSnapshot.activeTransitionId.empty()) {
                output += L"  ->  "
                    + ayt::ui::decodeUtf8Text(
                        _lastSnapshot.activeTransitionId);
            }
            if (!_lastSnapshot.activeActionId.empty()) {
                output += L"  /  "
                    + ayt::ui::decodeUtf8Text(_lastSnapshot.activeActionId);
            }
            if (_lastSnapshot.queuedIntentCount != 0u) {
                output += L"  |  "
                    + text("ui.editor.game_flow.queued", L"queued") + L" "
                    + std::to_wstring(_lastSnapshot.queuedIntentCount);
            }
            _previewStatus->setText(output);
            _previewStatus->setTextColor(_previewStale
                ? FVector4{0.94f, 0.67f, 0.28f, 1.0f}
                : FVector4{0.53f, 0.79f, 0.69f, 1.0f});
        }
        if (_trace != nullptr) {
            std::ostringstream output;
            const auto& trace = _preview.trace();
            const std::size_t first = trace.size() > 12u
                ? trace.size() - 12u : 0u;
            for (std::size_t index = first; index < trace.size(); ++index) {
                const auto& entry = trace[index];
                output << '#' << entry.serial;
                if (!entry.state.empty()) output << "  " << entry.state;
                if (!entry.transition.empty()) {
                    output << "  [" << entry.transition << ']';
                }
                output << "  " << entry.detail << '\n';
            }
            if (trace.empty()) {
                output << encodeUtf8(text(
                    "ui.editor.game_flow.empty_trace",
                    L"Preview trace is empty."));
            }
            _trace->setText(ayt::ui::decodeUtf8Text(output.str()));
        }
        if (_canvas != nullptr) _canvas->markDirty();
        _host.requestRepaint();
    }

    std::shared_ptr<EditorGameFlowDocument> _document;
    IEditorHostServices& _host;
    EditorGameFlowPreview _preview;
    ayt::ui::Widget* _root = nullptr;
    EditorGameFlowCanvas* _canvas = nullptr;
    ayt::ui::Menu* _canvasMenu = nullptr;
    ayt::ui::Menu* _transitionMenu = nullptr;
    ayt::ui::Button* _completePreview = nullptr;
    ayt::ui::Button* _failPreview = nullptr;
    ayt::ui::Button* _cancelPreview = nullptr;
    ayt::ui::ListView* _outline = nullptr;
    EditorGameFlowActionPalette* _actionPalette = nullptr;
    ayt::ui::ComboBox* _guardPalette = nullptr;
    ayt::ui::ComboBox* _subflowPicker = nullptr;
    ayt::ui::ComboBox* _intentPicker = nullptr;
    ayt::ui::ComboBox* _intentFieldPicker = nullptr;
    ayt::ui::TextInput* _intentFieldValue = nullptr;
    ayt::ui::ComboBox* _previewGuardPicker = nullptr;
    ayt::ui::CheckBox* _previewGuardAccepted = nullptr;
    ayt::ui::TextLabel* _status = nullptr;
    ayt::ui::TextLabel* _previewStatus = nullptr;
    ayt::ui::TextLabel* _inspectorHeading = nullptr;
    ayt::ui::TextArea* _trace = nullptr;
    ayt::ui::ListView* _diagnostics = nullptr;
    PropertyRow _id;
    PropertyRow _first;
    PropertyRow _second;
    PropertyRow _third;
    PropertyRow _fourth;
    PropertyRow _fifth;
    PropertyRow _sixth;
    PropertyRow _number;
    PropertyRow _integer;
    PropertyRow _value;
    ayt::ui::HBox* _flagRow = nullptr;
    ayt::ui::TextLabel* _flagLabel = nullptr;
    ayt::ui::CheckBox* _flag = nullptr;
    ayt::ui::ListView* _arguments = nullptr;
    ayt::ui::TextInput* _argumentValue = nullptr;
    ayt::ui::ComboBox* _argumentChoice = nullptr;
    ayt::ui::CheckBox* _argumentBool = nullptr;
    std::vector<LocalizedBinding> _localizedBindings;
    std::vector<EditorGameFlowOutlineItem> _outlineItems;
    std::vector<EditorGameFlowArgumentView> _argumentItems;
    std::vector<ayt::app::GameFlowFieldDefinition> _previewIntentFields;
    std::unordered_map<std::string, ayt::app::GameFlowPayload>
        _previewPayloads;
    std::vector<ayt::app::GameFlowDiagnostic> _diagnosticItems;
    EditorGameFlowClipboard _clipboard;
    ayt::app::GameFlowCoordinatorSnapshot _lastSnapshot;
    std::uint64_t _contentFingerprint = 0u;
    std::size_t _lastTraceSize = 0u;
    bool _refreshPending = false;
    bool _refreshing = false;
    bool _previewStale = true;
    bool _controlDown = false;
};

} // namespace

EditorDescriptor makeEditorGameFlowDescriptor(
    EditorGameFlowExtensionConfig config)
{
    EditorDescriptor descriptor;
    descriptor.id = kEditorGameFlowExtensionId;
    descriptor.displayName = L"Game Flow Editor";
    descriptor.iconPath = "icons/outline/route.svg";
    descriptor.surfaceKind = EditorSurfaceKind::Document;
    descriptor.openPolicy = EditorOpenPolicy::PerResource;
    descriptor.defaultDockSlot = EditorDockSlot::Center;
    descriptor.priority = 145;
    descriptor.extensions = {".gameflow.json"};
    descriptor.assetTypes = {"game-flow"};
    descriptor.createDocument =
        [configure = std::move(config.configureRegistry)](
            const EditorOpenRequest& request,
            std::string& error) -> std::shared_ptr<IEditorDocument> {
            auto document = std::make_shared<EditorGameFlowDocument>();
            const std::string displayPath = request.displayPath.empty()
                ? request.resourcePath : request.displayPath;
            if (!document->initialize(
                    request.resourcePath, displayPath, &error)) {
                return nullptr;
            }
            if (configure != nullptr) {
                try {
                    configure(document->actionRegistry());
                } catch (const std::exception& exception) {
                    error = std::string("GameFlow registry setup failed: ")
                        + exception.what();
                    return nullptr;
                } catch (...) {
                    error = "GameFlow registry setup failed.";
                    return nullptr;
                }
                document->actionRegistryChanged();
            }
            error.clear();
            return document;
        };
    descriptor.createView = [](
        const std::shared_ptr<IEditorDocument>& document,
        IEditorHostServices& host) -> std::unique_ptr<IEditorView> {
        auto gameFlow =
            std::dynamic_pointer_cast<EditorGameFlowDocument>(document);
        if (gameFlow == nullptr) return nullptr;
        return std::make_unique<EditorGameFlowWorkspaceView>(
            std::move(gameFlow), host);
    };
    return descriptor;
}

bool registerEditorGameFlowExtension(
    EditorExtensionRegistry& registry,
    EditorGameFlowExtensionConfig config,
    std::string* error)
{
    return registry.registerEditor(
        makeEditorGameFlowDescriptor(std::move(config)), error);
}

} // namespace ayt::editor
