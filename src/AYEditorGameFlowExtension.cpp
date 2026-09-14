#include "AYEditor/EditorGameFlowExtension.h"

#include "AYEditor/EditorGameFlowDocument.h"
#include "AYEditor/EditorGameFlowPreview.h"

#include <AYUI/Box.h>
#include <AYUI/Button.h>
#include <AYUI/CheckBox.h>
#include <AYUI/ComboBox.h>
#include <AYUI/IRenderBackend.h>
#include <AYUI/ListView.h>
#include <AYUI/TextArea.h>
#include <AYUI/TextInput.h>
#include <AYUI/TextLabel.h>
#include <AYUI/UIKeyCode.h>
#include <AYUI/UnicodeText.h>
#include <AYUI/Widget.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

class EditorGameFlowCanvas final : public ayt::ui::Widget
{
public:
    EditorGameFlowCanvas(EditorGameFlowDocument& document,
                         EditorGameFlowPreview& preview,
                         std::function<void()> selected)
        : _document(document), _preview(preview),
          _selected(std::move(selected))
    {
        setId("gameflow_canvas");
        setDisplayListPolicy(ayt::ui::DisplayListPolicy::Immediate);
    }

    bool pointerDown(float x, float y, int button)
    {
        if (!contains(getWorldBounds(), x, y)) return false;
        if (button == 0) {
            for (auto it = _hits.rbegin(); it != _hits.rend(); ++it) {
                if (!contains(it->bounds, x, y)) continue;
                (void)_document.select(it->selection);
                if (_selected != nullptr) _selected();
                markDirty();
                return true;
            }
        }
        if (button == 0 || button == 1 || button == 2) {
            _panning = true;
            _lastPointer = {x, y};
            return true;
        }
        return false;
    }

    bool pointerMove(float x, float y)
    {
        if (!_panning) return false;
        _pan.x += x - _lastPointer.x;
        _pan.y += y - _lastPointer.y;
        _lastPointer = {x, y};
        markDirty();
        return true;
    }

    bool pointerUp(float, float, int)
    {
        if (!_panning) return false;
        _panning = false;
        return true;
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
        _zoom = 1.0f;
        _pan = {28.0f, 62.0f};
        markDirty();
    }

    bool captured() const noexcept { return _panning; }

protected:
    void onRender(ayt::ui::IRenderBackend& renderer) override
    {
        _hits.clear();
        const FRectangle bounds = getWorldBounds();
        renderer.drawRect(bounds, {0.045f, 0.052f, 0.066f, 1.0f});
        drawGrid(renderer, bounds);
        drawFlow(renderer, bounds);
    }

private:
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
        const auto& flow = _document.flow();
        const auto preview = _preview.snapshot();
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
                ayt::ui::decodeUtf8Text("intent: " + intent.id), font,
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
        std::unordered_map<std::string, FRectangle> stateBounds;
        for (std::size_t index = 0; index < flow.states.size(); ++index) {
            std::unordered_set<std::size_t> visiting;
            const int depth = depthOf(depthOf, index, visiting);
            const int row = rows[depth]++;
            const FVector2 at = graphPoint(bounds,
                static_cast<float>(depth) * 310.0f,
                static_cast<float>(row) * 154.0f);
            const FRectangle rect{at.x, at.y, at.x + 205.0f * _zoom,
                                  at.y + 66.0f * _zoom};
            layouts.push_back({&flow.states[index], rect});
            stateBounds.emplace(flow.states[index].id, rect);
        }

        for (std::size_t index = 0; index < flow.transitions.size(); ++index) {
            const auto& transition = flow.transitions[index];
            const auto from = stateBounds.find(transition.fromState);
            const auto to = stateBounds.find(transition.toState);
            if (from == stateBounds.end() || to == stateBounds.end()) continue;
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
            const bool selected = _document.selection()
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
                    ayt::ui::decodeUtf8Text(
                        "guard: " + transition.guard.guard), font,
                    FVector4{0.98f, 0.80f, 0.46f, 1.0f});
                _hits.push_back({guardRect,
                    {EditorGameFlowObjectKind::Guard,
                     transition.guard.guard, transition.id}});
                laneY = guardRect.maxY + 3.0f * _zoom;
            }
            for (std::size_t actionIndex = 0;
                 actionIndex < transition.actions.size(); ++actionIndex) {
                const auto& action = transition.actions[actionIndex];
                const bool known = _document.actionRegistry().findAction(
                    action.action) != nullptr;
                const bool actionActive = active
                    && preview.activeActionIndex == actionIndex;
                const FRectangle actionRect{
                    transitionRect.minX, laneY, transitionRect.maxX,
                    laneY + 23.0f * _zoom};
                renderer.drawRect(actionRect, actionActive
                    ? FVector4{0.08f, 0.28f, 0.23f, 1.0f}
                    : known ? FVector4{0.09f, 0.15f, 0.19f, 1.0f}
                            : FVector4{0.24f, 0.08f, 0.10f, 1.0f});
                drawBorder(renderer, actionRect, actionActive
                    ? FVector4{0.22f, 0.90f, 0.62f, 1.0f}
                    : known ? FVector4{0.22f, 0.48f, 0.64f, 1.0f}
                            : FVector4{0.92f, 0.28f, 0.34f, 1.0f});
                std::string label = std::to_string(actionIndex + 1u)
                    + ". " + action.action;
                if (!known) label += "  [unknown]";
                renderer.drawText({actionRect.minX + 6.0f, actionRect.minY,
                                   actionRect.maxX - 4.0f, actionRect.maxY},
                    ayt::ui::decodeUtf8Text(label), font,
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
                const auto target = stateBounds.find(stateId);
                if (target == stateBounds.end()) return;
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
                drawFallback(transition.onFailureState, -4.0f, "failure",
                    {0.94f, 0.31f, 0.34f, 1.0f});
            }
            if (!transition.onCancelState.empty()) {
                drawFallback(transition.onCancelState, 12.0f, "cancel",
                    {0.92f, 0.62f, 0.24f, 1.0f});
            }
        }

        for (const auto& layout : layouts) {
            const auto& state = *layout.state;
            const bool active = preview.currentStateId == state.id;
            const bool selected = _document.selection()
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
            std::string detail;
            if (flow.initialState == state.id) detail = "initial";
            if (!state.parent.empty()) {
                if (!detail.empty()) detail += "  |  ";
                detail += "parent: " + state.parent;
            }
            if (!state.initialChild.empty()) {
                if (!detail.empty()) detail += "  |  ";
                detail += "child: " + state.initialChild;
            }
            renderer.drawText({layout.bounds.minX + 9.0f,
                               layout.bounds.minY + 32.0f * _zoom,
                               layout.bounds.maxX - 6.0f,
                               layout.bounds.maxY - 3.0f},
                ayt::ui::decodeUtf8Text(detail),
                (std::max)(9, font - 2),
                FVector4{0.55f, 0.63f, 0.73f, 1.0f});
            _hits.push_back({layout.bounds,
                {EditorGameFlowObjectKind::State, state.id}});
        }

        if (flow.states.empty()) {
            renderer.drawText({bounds.minX + 22.0f, bounds.minY + 22.0f,
                               bounds.maxX - 22.0f, bounds.minY + 60.0f},
                L"Add a State to begin authoring the game flow.", 15,
                FVector4{0.58f, 0.63f, 0.72f, 1.0f});
        }
    }

    EditorGameFlowDocument& _document;
    EditorGameFlowPreview& _preview;
    std::function<void()> _selected;
    std::vector<Hit> _hits;
    FVector2 _pan{28.0f, 62.0f};
    FVector2 _lastPointer{};
    float _zoom = 1.0f;
    bool _panning = false;
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

    void prepareForUiShutdown() override
    {
        if (_document != nullptr) _document->setChangedHandler({});
        _canvas = nullptr;
        _outline = nullptr;
        _diagnostics = nullptr;
        _trace = nullptr;
        _status = nullptr;
        _previewStatus = nullptr;
        _intentPicker = nullptr;
        _intentFieldPicker = nullptr;
        _intentFieldValue = nullptr;
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
        if (keyCode == ayt::ui::UIKey_Delete) return deleteSelected();
        if (keyCode == ayt::ui::UIKey_Escape && _preview.isRunning()) {
            (void)_preview.cancelActive("Cancelled from editor preview.");
            syncPreviewPresentation();
            return true;
        }
        return false;
    }
    void onKeyUp(int) override {}
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
    struct PropertyRow
    {
        ayt::ui::HBox* row = nullptr;
        ayt::ui::TextLabel* label = nullptr;
        ayt::ui::TextInput* input = nullptr;
    };

    ayt::ui::Button* button(ayt::ui::HBox& parent,
                            std::wstring text,
                            float width,
                            std::function<void()> clicked)
    {
        auto* result = new ayt::ui::Button();
        result->setText(std::move(text));
        result->setPadding(6.0f, 3.0f, 6.0f, 3.0f);
        result->setOnClicked(std::move(clicked));
        parent.addWidget(result, width);
        return result;
    }

    ayt::ui::TextLabel* label(ayt::ui::VBox& parent,
                              std::wstring text,
                              float height = 20.0f)
    {
        auto* result = new ayt::ui::TextLabel();
        result->setText(std::move(text));
        result->setFontSize(11);
        result->setTextColor({0.58f, 0.64f, 0.73f, 1.0f});
        result->setVerticalAlignment(
            ayt::ui::TextLabel::VAlignment::Center);
        parent.addWidget(result, height);
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
        value.row->addWidget(value.label, 102.0f);
        value.row->addWidget(value.input, 0.0f);
        parent.addWidget(value.row, 25.0f);
        return value;
    }

    void buildWidgetTree()
    {
        auto* root = new ayt::ui::VBox();
        _root = root;
        root->setSpacing(4.0f);
        root->setPadding(5.0f, 4.0f, 5.0f, 5.0f);

        auto* toolbar = new ayt::ui::HBox();
        toolbar->setSpacing(4.0f);
        button(*toolbar, L"Save", 50.0f, [this]() { (void)save(); });
        button(*toolbar, L"Undo", 50.0f, [this]() {
            if (!_document->undo()) setStatus("Nothing to undo.", true);
        });
        button(*toolbar, L"Redo", 50.0f, [this]() {
            if (!_document->redo()) setStatus("Nothing to redo.", true);
        });
        button(*toolbar, L"+ Intent", 66.0f, [this]() {
            addObject(EditorGameFlowObjectKind::Intent);
        });
        button(*toolbar, L"+ State", 62.0f, [this]() {
            addObject(EditorGameFlowObjectKind::State);
        });
        button(*toolbar, L"+ Transition", 86.0f, [this]() {
            addObject(EditorGameFlowObjectKind::Transition);
        });
        button(*toolbar, L"Delete", 58.0f,
            [this]() { (void)deleteSelected(); });
        button(*toolbar, L"Validate", 66.0f, [this]() { validate(); });
        button(*toolbar, L"Frame", 52.0f, [this]() {
            if (_canvas != nullptr) _canvas->frameAll();
        });
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
        label(*left, L"FLOW OUTLINE");
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
        label(*left, L"ACTION / GUARD PALETTE");
        _actionPalette = new ayt::ui::ComboBox();
        left->addWidget(_actionPalette, 27.0f);
        auto* actionButtons = new ayt::ui::HBox();
        actionButtons->setSpacing(4.0f);
        button(*actionButtons, L"Add Action", 92.0f,
            [this]() { addPaletteAction(); });
        button(*actionButtons, L"Up", 44.0f,
            [this]() { moveAction(-1); });
        button(*actionButtons, L"Down", 50.0f,
            [this]() { moveAction(1); });
        left->addWidget(actionButtons, 27.0f);
        _guardPalette = new ayt::ui::ComboBox();
        left->addWidget(_guardPalette, 27.0f);
        auto* guardButtons = new ayt::ui::HBox();
        guardButtons->setSpacing(4.0f);
        button(*guardButtons, L"Set Guard", 92.0f,
            [this]() { setPaletteGuard(); });
        button(*guardButtons, L"Clear", 54.0f,
            [this]() { clearGuard(); });
        button(*guardButtons, L"+ Field", 62.0f,
            [this]() { addIntentField(); });
        left->addWidget(guardButtons, 27.0f);
        body->addWidget(left, 224.0f);

        auto* center = new ayt::ui::VBox();
        center->setSpacing(4.0f);
        _canvas = new EditorGameFlowCanvas(
            *_document, _preview, [this]() { _refreshPending = true; });
        center->addWidget(_canvas, 0.0f);
        auto* previewBar = new ayt::ui::HBox();
        previewBar->setSpacing(4.0f);
        button(*previewBar, L"Restart", 62.0f,
            [this]() { restartPreview(); });
        _intentPicker = new ayt::ui::ComboBox();
        _intentPicker->setOnSelectionChanged([this](int) {
            refreshPreviewPayloadFields();
        });
        previewBar->addWidget(_intentPicker, 156.0f);
        button(*previewBar, L"Send", 50.0f,
            [this]() { sendIntent(); });
        button(*previewBar, L"Complete", 68.0f,
            [this]() { completeAction(); });
        button(*previewBar, L"Fail", 44.0f,
            [this]() { failAction(); });
        button(*previewBar, L"Cancel", 54.0f, [this]() {
            if (!_preview.cancelActive("Cancelled from preview toolbar.")) {
                setStatus("No active transition to cancel.", true);
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
        payloadLabel->setText(L"Payload");
        payloadLabel->setFontSize(11);
        payloadLabel->setVerticalAlignment(
            ayt::ui::TextLabel::VAlignment::Center);
        payloadBar->addWidget(payloadLabel, 52.0f);
        _intentFieldPicker = new ayt::ui::ComboBox();
        _intentFieldPicker->setOnSelectionChanged([this](int) {
            syncPreviewPayloadValue();
        });
        payloadBar->addWidget(_intentFieldPicker, 184.0f);
        _intentFieldValue = new ayt::ui::TextInput();
        _intentFieldValue->setPlaceholder(L"Value");
        payloadBar->addWidget(_intentFieldValue, 0.0f);
        button(*payloadBar, L"Set", 46.0f,
            [this]() { setPreviewPayloadField(); });
        button(*payloadBar, L"Clear", 50.0f,
            [this]() { clearPreviewPayloadField(); });
        center->addWidget(payloadBar, 27.0f);
        _trace = new ayt::ui::TextArea();
        _trace->setReadOnly(true);
        _trace->setWordWrap(false);
        _trace->setLineHeight(15.0f);
        center->addWidget(_trace, 92.0f);
        body->addWidget(center, 0.0f);

        auto* inspector = new ayt::ui::VBox();
        inspector->setSpacing(3.0f);
        inspector->setPadding(4.0f, 3.0f, 4.0f, 3.0f);
        _inspectorHeading = label(*inspector, L"INSPECTOR");
        _id = propertyRow(*inspector);
        _first = propertyRow(*inspector);
        _second = propertyRow(*inspector);
        _third = propertyRow(*inspector);
        _fourth = propertyRow(*inspector);
        _fifth = propertyRow(*inspector);
        _sixth = propertyRow(*inspector);
        _number = propertyRow(*inspector);
        _integer = propertyRow(*inspector);
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
        button(*inspectorButtons, L"Apply", 62.0f,
            [this]() { applyInspector(); });
        inspector->addWidget(inspectorButtons, 27.0f);
        label(*inspector, L"PARAMETERS");
        _arguments = new ayt::ui::ListView();
        _arguments->setItemHeight(21.0f);
        _arguments->setOnSelectionChanged([this](int index) {
            selectArgument(index);
        });
        inspector->addWidget(_arguments, 100.0f);
        _argumentValue = new ayt::ui::TextInput();
        _argumentValue->setPlaceholder(L"Selected parameter value");
        inspector->addWidget(_argumentValue, 25.0f);
        auto* argumentButtons = new ayt::ui::HBox();
        argumentButtons->setSpacing(4.0f);
        button(*argumentButtons, L"Set", 50.0f,
            [this]() { setArgument(); });
        button(*argumentButtons, L"Use Default", 88.0f,
            [this]() { clearArgument(); });
        inspector->addWidget(argumentButtons, 27.0f);
        label(*inspector, L"DIAGNOSTICS");
        _diagnostics = new ayt::ui::ListView();
        _diagnostics->setItemHeight(23.0f);
        _diagnostics->setOnSelectionChanged([this](int index) {
            locateDiagnostic(index);
        });
        inspector->addWidget(_diagnostics, 0.0f);
        body->addWidget(inspector, 306.0f);

        root->addWidget(body, 0.0f);
        populatePalettes();
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
    }

    void setStatus(std::string message, bool error = false)
    {
        if (_status == nullptr) return;
        _status->setText(ayt::ui::decodeUtf8Text(message));
        _status->setTextColor(error
            ? FVector4{0.96f, 0.38f, 0.40f, 1.0f}
            : FVector4{0.42f, 0.80f, 0.58f, 1.0f});
        _host.setStatusText(ayt::ui::decodeUtf8Text(message));
        _host.requestRepaint();
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
        if (!_document->deleteSelection(&error)) {
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
                setStatus("Save failed: this host has no Save As provider.",
                    true);
                return false;
            }
            const std::string path =
                provider->chooseDocumentSavePath(*_document, true);
            if (path.empty()) {
                setStatus("Save cancelled.");
                return false;
            }
            saved = _document->saveAs(path, &error);
        } else {
            saved = _document->save(&error);
        }
        if (!saved) {
            setStatus("Save failed: " + error, true);
            return false;
        }
        setStatus("Saved " + _document->title());
        return true;
    }

    void validate()
    {
        std::vector<ayt::app::GameFlowDiagnostic> diagnostics;
        ayt::app::GameFlowPlan plan;
        const bool valid = _document->buildPlan(plan, &diagnostics);
        refreshDiagnostics(diagnostics);
        setStatus(valid ? "GameFlow validation succeeded."
                        : "GameFlow validation failed.", !valid);
    }

    void applyInspector()
    {
        EditorGameFlowProperties properties = _document->selectedProperties();
        const auto labels = _document->selectedPropertyLabels();
        properties.id = encodeUtf8(_id.input->getText());
        if (!labels.first.empty()) {
            properties.first = encodeUtf8(_first.input->getText());
        }
        if (!labels.second.empty()) {
            properties.second = encodeUtf8(_second.input->getText());
        }
        if (!labels.third.empty()) {
            properties.third = encodeUtf8(_third.input->getText());
        }
        if (!labels.fourth.empty()) {
            properties.fourth = encodeUtf8(_fourth.input->getText());
        }
        if (!labels.fifth.empty()) {
            properties.fifth = encodeUtf8(_fifth.input->getText());
        }
        if (!labels.sixth.empty()) {
            properties.sixth = encodeUtf8(_sixth.input->getText());
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
            setStatus("Inspector number or integer is invalid.", true);
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
                setStatus("Inspector value does not match its type.", true);
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
            setStatus("Select a parameter before setting its value.", true);
            return;
        }
        const auto found = std::find_if(_argumentItems.begin(),
            _argumentItems.end(), [&](const auto& value) {
                return value.id == selection.id;
            });
        if (found == _argumentItems.end()) return;
        bool valid = false;
        auto value = parseValue(_argumentValue->getText(), found->type, valid);
        std::string error;
        if (!valid || !_document->setSelectedArgument(
                selection.id, std::move(value), &error)) {
            setStatus(valid ? error : "Parameter value has the wrong type.",
                true);
        }
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

    void restartPreview()
    {
        std::string error;
        if (!_preview.rebuild(
                _document->flow(), _document->actionRegistry(), &error)) {
            setStatus("Preview failed: " + error, true);
            syncPreviewPresentation();
            return;
        }
        _previewStale = false;
        syncPreviewPresentation();
        setStatus("GameFlow preview restarted.");
    }

    void sendIntent()
    {
        const std::string intent = encodeUtf8(_intentPicker->getSelectedItem());
        if (intent.empty()) {
            setStatus("The flow has no Intent to send.", true);
            return;
        }
        if (_previewStale) {
            restartPreview();
            if (_previewStale || !_preview.isRunning()) return;
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
                               const std::string& labelText,
                               const std::string& value)
    {
        const bool visible = !labelText.empty();
        row.row->setVisible(visible);
        if (!visible) return;
        row.label->setText(ayt::ui::decodeUtf8Text(labelText));
        row.input->setText(ayt::ui::decodeUtf8Text(value));
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
                + ayt::ui::decodeUtf8Text(item.label));
            if (item.selection == _document->selection()) {
                selectedIndex = static_cast<int>(index);
            }
        }
        _outline->setItems(labels);
        _outline->setSelectedIndex(selectedIndex);

        const auto properties = _document->selectedProperties();
        const auto propertyLabels = _document->selectedPropertyLabels();
        _inspectorHeading->setText(ayt::ui::decodeUtf8Text(
            std::string("INSPECTOR  /  ")
            + EditorGameFlowDocument::kindName(
                _document->selection().kind)));
        setPropertyRow(_id, "ID", properties.id);
        setPropertyRow(_first, propertyLabels.first, properties.first);
        setPropertyRow(_second, propertyLabels.second, properties.second);
        setPropertyRow(_third, propertyLabels.third, properties.third);
        setPropertyRow(_fourth, propertyLabels.fourth, properties.fourth);
        setPropertyRow(_fifth, propertyLabels.fifth, properties.fifth);
        setPropertyRow(_sixth, propertyLabels.sixth, properties.sixth);
        setPropertyRow(_number, propertyLabels.number,
            std::to_string(properties.number));
        setPropertyRow(_integer, propertyLabels.integer,
            std::to_string(properties.integer));
        _flagRow->setVisible(!propertyLabels.flag.empty());
        _flagLabel->setText(ayt::ui::decodeUtf8Text(propertyLabels.flag));
        _flag->setChecked(properties.flag);
        setPropertyRow(_value, propertyLabels.value,
            valueText(properties.value));

        _argumentItems = _document->selectedArguments();
        std::vector<std::wstring> arguments;
        arguments.reserve(_argumentItems.size());
        int selectedArgument = -1;
        for (std::size_t index = 0; index < _argumentItems.size(); ++index) {
            const auto& item = _argumentItems[index];
            std::string text = item.id + " : "
                + ayt::app::gameFlowValueTypeName(item.type);
            text += item.authored ? "  [authored]" : "  [default]";
            if (!item.known) text += "  [unknown schema]";
            arguments.push_back(ayt::ui::decodeUtf8Text(text));
            if (_document->selection().kind
                    == EditorGameFlowObjectKind::ActionArgument
                && _document->selection().id == item.id) {
                selectedArgument = static_cast<int>(index);
                _argumentValue->setText(
                    ayt::ui::decodeUtf8Text(valueText(item.value)));
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
            setStatus("Select an Intent payload field first.", true);
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
            setStatus("Required field cleared; enter a value before Send.",
                true);
        } else {
            setStatus("Preview payload field uses its default: " + field.id);
        }
    }

    void refreshDiagnostics(
        const std::vector<ayt::app::GameFlowDiagnostic>& diagnostics)
    {
        if (_diagnostics == nullptr) return;
        _diagnosticItems = diagnostics;
        std::vector<std::wstring> items;
        if (diagnostics.empty()) {
            items.push_back(L"No diagnostics.");
            _diagnostics->setItems(items);
            return;
        }
        for (const auto& diagnostic : diagnostics) {
            std::ostringstream output;
            output << (diagnostic.severity
                    == ayt::app::GameFlowDiagnosticSeverity::Error
                ? "error" : "warning");
            if (!diagnostic.path.empty()) output << "  " << diagnostic.path;
            output << "  |  " << diagnostic.message;
            items.push_back(ayt::ui::decodeUtf8Text(output.str()));
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
        if (_previewStatus != nullptr) {
            std::ostringstream output;
            output << (_previewStale ? "STALE  |  " : "")
                   << (_lastSnapshot.currentStateId.empty()
                        ? "no state" : _lastSnapshot.currentStateId);
            if (!_lastSnapshot.activeTransitionId.empty()) {
                output << "  ->  " << _lastSnapshot.activeTransitionId;
            }
            if (!_lastSnapshot.activeActionId.empty()) {
                output << "  /  " << _lastSnapshot.activeActionId;
            }
            if (_lastSnapshot.queuedIntentCount != 0u) {
                output << "  |  queued " << _lastSnapshot.queuedIntentCount;
            }
            _previewStatus->setText(
                ayt::ui::decodeUtf8Text(output.str()));
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
            if (trace.empty()) output << "Preview trace is empty.";
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
    ayt::ui::ListView* _outline = nullptr;
    ayt::ui::ComboBox* _actionPalette = nullptr;
    ayt::ui::ComboBox* _guardPalette = nullptr;
    ayt::ui::ComboBox* _intentPicker = nullptr;
    ayt::ui::ComboBox* _intentFieldPicker = nullptr;
    ayt::ui::TextInput* _intentFieldValue = nullptr;
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
    std::vector<EditorGameFlowOutlineItem> _outlineItems;
    std::vector<EditorGameFlowArgumentView> _argumentItems;
    std::vector<ayt::app::GameFlowFieldDefinition> _previewIntentFields;
    std::unordered_map<std::string, ayt::app::GameFlowPayload>
        _previewPayloads;
    std::vector<ayt::app::GameFlowDiagnostic> _diagnosticItems;
    ayt::app::GameFlowCoordinatorSnapshot _lastSnapshot;
    std::uint64_t _contentFingerprint = 0u;
    std::size_t _lastTraceSize = 0u;
    bool _refreshPending = false;
    bool _refreshing = false;
    bool _previewStale = true;
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
