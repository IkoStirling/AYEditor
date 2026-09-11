#include "AYEditor/EditorUiFlowExtension.h"

#include "AYEditor/EditorUiFlowDocument.h"
#include "AYEditor/EditorUiFlowPreview.h"

#include <AYApplication/UIFlowAssetValidation.h>
#include <AYUI/Button.h>
#include <AYUI/CheckBox.h>
#include <AYUI/ComboBox.h>
#include <AYUI/IRenderBackend.h>
#include <AYUI/ListView.h>
#include <AYUI/Panel.h>
#include <AYUI/TextInput.h>
#include <AYUI/TextLabel.h>
#include <AYUI/UIManager.h>
#include <AYUI/UnicodeText.h>
#include <AYUI/Widget.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ayt::editor {
namespace {

using ayt::math::FRectangle;
using ayt::math::FVector2;
using ayt::math::FVector4;

void appendUtf8(std::string& output, std::uint32_t codePoint)
{
    if (codePoint <= 0x7fu) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ffu) {
        output.push_back(static_cast<char>(0xc0u | (codePoint >> 6u)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    } else if (codePoint <= 0xffffu) {
        output.push_back(static_cast<char>(0xe0u | (codePoint >> 12u)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    } else {
        output.push_back(static_cast<char>(0xf0u | (codePoint >> 18u)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 12u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    }
}

std::string encodeUtf8(const std::wstring& text)
{
    std::string output;
    output.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        std::uint32_t codePoint = static_cast<std::uint32_t>(text[index]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (codePoint >= 0xd800u && codePoint <= 0xdbffu
                && index + 1 < text.size()) {
                const auto low = static_cast<std::uint32_t>(text[index + 1]);
                if (low >= 0xdc00u && low <= 0xdfffu) {
                    codePoint = 0x10000u + ((codePoint - 0xd800u) << 10u)
                        + (low - 0xdc00u);
                    ++index;
                }
            }
        }
        if (codePoint >= 0xd800u && codePoint <= 0xdfffu) {
            codePoint = 0xfffdu;
        }
        appendUtf8(output, (std::min)(codePoint, std::uint32_t{0x10ffffu}));
    }
    return output;
}

bool inside(const FRectangle& rect, const FVector2& point)
{
    return point.x >= rect.minX && point.x <= rect.maxX
        && point.y >= rect.minY && point.y <= rect.maxY;
}

void border(ayt::ui::IRenderBackend& renderer,
            const FRectangle& rect,
            const FVector4& color,
            float width = 1.0f)
{
    renderer.drawRect({rect.minX, rect.minY, rect.maxX, rect.minY + width}, color);
    renderer.drawRect({rect.minX, rect.maxY - width, rect.maxX, rect.maxY}, color);
    renderer.drawRect({rect.minX, rect.minY, rect.minX + width, rect.maxY}, color);
    renderer.drawRect({rect.maxX - width, rect.minY, rect.maxX, rect.maxY}, color);
}

void connector(ayt::ui::IRenderBackend& renderer,
               FVector2 from,
               FVector2 to,
               const FVector4& color)
{
    const auto path = renderer.createPath();
    if (path.id >= 0) {
        const float reach = std::max(42.0f, std::abs(to.x - from.x) * 0.5f);
        renderer.addPathBezier(path, from,
            {from.x + reach, from.y}, {to.x - reach, to.y}, to);
        renderer.setPathStrokeColor(path, color);
        renderer.setPathStrokeWidth(path, 2.0f);
        renderer.setPathStrokeStyle(path,
            ayt::ui::PathStrokeCap::Round,
            ayt::ui::PathStrokeJoin::Round);
        renderer.drawPath(path, ayt::ui::PathFillMode::Stroke);
        renderer.releasePath(path);
        return;
    }
    const float middle = std::floor((from.x + to.x) * 0.5f + 0.5f);
    renderer.drawRect({std::min(from.x, middle), from.y - 1.0f,
                       std::max(from.x, middle), from.y + 1.0f}, color);
    renderer.drawRect({middle - 1.0f, std::min(from.y, to.y),
                       middle + 1.0f, std::max(from.y, to.y)}, color);
    renderer.drawRect({std::min(middle, to.x), to.y - 1.0f,
                       std::max(middle, to.x), to.y + 1.0f}, color);
}

FVector4 graphPinColor(const ayt::ui::UIFlowGraphPinTypeDefinition& pin)
{
    if (pin.kind == ayt::ui::UIFlowGraphPinKind::Execution) {
        return {0.78f, 0.84f, 0.92f, 1.0f};
    }
    switch (pin.valueType) {
    case ayt::ui::UIFlowValueType::Boolean:
        return {0.92f, 0.28f, 0.34f, 1.0f};
    case ayt::ui::UIFlowValueType::Integer:
        return {0.22f, 0.76f, 0.82f, 1.0f};
    case ayt::ui::UIFlowValueType::Number:
        return {0.28f, 0.82f, 0.48f, 1.0f};
    case ayt::ui::UIFlowValueType::Entity:
        return {0.94f, 0.54f, 0.23f, 1.0f};
    case ayt::ui::UIFlowValueType::Asset:
        return {0.93f, 0.72f, 0.22f, 1.0f};
    case ayt::ui::UIFlowValueType::String:
    default:
        return {0.76f, 0.39f, 0.88f, 1.0f};
    }
}

class EditorUiFlowCanvas final : public ayt::ui::Widget {
public:
    EditorUiFlowCanvas(EditorUiFlowDocument& document,
                       EditorUiFlowPreview& preview,
                       const ayt::ui::UIFlowGraphNodeRegistry& registry,
                       std::function<void()> changed)
        : _document(document), _preview(preview), _registry(registry),
          _changed(std::move(changed))
    {
        setId("flow_graph_canvas");
        setLayoutPositionManaged(false);
        setLayoutSizeManaged(false);
        setDisplayListPolicy(ayt::ui::DisplayListPolicy::Immediate);
    }

    bool onMouseButtonUp(const ayt::ui::UIMouseEvent& event) override
    {
        if (event.mouseButton != 0) return false;
        for (auto it = _hits.rbegin(); it != _hits.rend(); ++it) {
            if (!inside(it->bounds, event.mousePos)) continue;
            if (_document.select(it->selection)) {
                if (_changed != nullptr) _changed();
                markDirty();
            }
            return true;
        }
        return false;
    }

protected:
    void onRender(ayt::ui::IRenderBackend& renderer) override
    {
        _hits.clear();
        const FRectangle bounds = getWorldBounds();
        renderer.drawRect(bounds, {0.055f, 0.063f, 0.078f, 1.0f});
        drawGrid(renderer, bounds);
        const auto& flow = _document.flow();
        if (_document.selection().kind == EditorUiFlowObjectKind::Graph) {
            const auto* graph = flow.findGraph(_document.selection().id);
            if (graph != nullptr) drawGraph(renderer, bounds, *graph);
        } else {
            drawStateMachines(renderer, bounds);
        }
        drawMountedScreens(renderer, bounds);
    }

private:
    struct Hit {
        FRectangle bounds;
        EditorUiFlowSelection selection;
    };

    void drawGrid(ayt::ui::IRenderBackend& renderer,
                  const FRectangle& bounds)
    {
        const FVector4 fine{0.085f, 0.095f, 0.115f, 1.0f};
        for (float x = bounds.minX; x < bounds.maxX; x += 24.0f) {
            renderer.drawRect({x, bounds.minY, x + 1.0f, bounds.maxY}, fine);
        }
        for (float y = bounds.minY; y < bounds.maxY; y += 24.0f) {
            renderer.drawRect({bounds.minX, y, bounds.maxX, y + 1.0f}, fine);
        }
    }

    void drawStateMachines(ayt::ui::IRenderBackend& renderer,
                           const FRectangle& bounds)
    {
        const auto& flow = _document.flow();
        if (flow.regions.empty()) {
            renderer.drawText({bounds.minX + 24.0f, bounds.minY + 24.0f,
                               bounds.maxX - 24.0f, bounds.minY + 60.0f},
                L"Add a Region to author parallel UI state", 16,
                FVector4{0.56f, 0.60f, 0.68f, 1.0f});
            return;
        }
        const float regionWidth = std::max(220.0f,
            (bounds.maxX - bounds.minX - 36.0f)
                / static_cast<float>(flow.regions.size()));
        std::unordered_map<std::string, FRectangle> stateRects;
        float regionX = bounds.minX + 18.0f;
        for (const auto& region : flow.regions) {
            const float right = std::min(bounds.maxX - 18.0f,
                regionX + regionWidth - 12.0f);
            const FRectangle header{regionX, bounds.minY + 18.0f,
                                    right, bounds.minY + 54.0f};
            renderer.drawRect(header, {0.105f, 0.125f, 0.16f, 1.0f});
            border(renderer, header,
                _document.selection() == EditorUiFlowSelection{
                    EditorUiFlowObjectKind::Region, region.id, {}}
                    ? FVector4{0.20f, 0.62f, 1.0f, 1.0f}
                    : FVector4{0.18f, 0.22f, 0.29f, 1.0f});
            renderer.drawText({header.minX + 10.0f, header.minY,
                               header.maxX - 8.0f, header.maxY},
                ayt::ui::decodeUtf8Text(region.id), 14,
                FVector4{0.88f, 0.91f, 0.96f, 1.0f});
            _hits.push_back({header,
                {EditorUiFlowObjectKind::Region, region.id, {}}});

            float y = header.maxY + 18.0f;
            for (const auto& state : region.states) {
                const FRectangle card{regionX + 14.0f, y,
                    right - 14.0f, y + 58.0f};
                const bool active = [&]() {
                    const auto found = _preview.activeStates().find(region.id);
                    return found != _preview.activeStates().end()
                        && found->second == state.id;
                }();
                const bool selected = _document.selection()
                    == EditorUiFlowSelection{EditorUiFlowObjectKind::State,
                                              state.id, region.id};
                renderer.drawRect(card, active
                    ? FVector4{0.08f, 0.25f, 0.36f, 1.0f}
                    : FVector4{0.115f, 0.13f, 0.16f, 1.0f});
                border(renderer, card, selected
                    ? FVector4{0.24f, 0.68f, 1.0f, 1.0f}
                    : active ? FVector4{0.12f, 0.82f, 0.67f, 1.0f}
                             : FVector4{0.22f, 0.25f, 0.31f, 1.0f},
                    selected ? 2.0f : 1.0f);
                renderer.drawText({card.minX + 10.0f, card.minY + 4.0f,
                                   card.maxX - 8.0f, card.minY + 30.0f},
                    ayt::ui::decodeUtf8Text(state.id), 14,
                    FVector4{0.93f, 0.95f, 0.98f, 1.0f});
                const std::string detail = state.contexts.empty()
                    ? "No Context" : joinContexts(state.contexts);
                renderer.drawText({card.minX + 10.0f, card.minY + 29.0f,
                                   card.maxX - 8.0f, card.maxY - 4.0f},
                    ayt::ui::decodeUtf8Text(detail), 11,
                    FVector4{0.54f, 0.60f, 0.69f, 1.0f});
                _hits.push_back({card,
                    {EditorUiFlowObjectKind::State, state.id, region.id}});
                stateRects[region.id + "\n" + state.id] = card;
                y += 76.0f;
            }
            regionX += regionWidth;
        }

        for (const auto& transition : flow.transitions) {
            const auto from = stateRects.find(
                transition.region + "\n" + transition.fromState);
            const auto to = stateRects.find(
                transition.region + "\n" + transition.toState);
            if (from == stateRects.end() || to == stateRects.end()) continue;
            connector(renderer,
                {from->second.maxX, (from->second.minY + from->second.maxY) * 0.5f},
                {to->second.minX, (to->second.minY + to->second.maxY) * 0.5f},
                {0.28f, 0.55f, 0.82f, 0.8f});
        }
    }

    static std::string joinContexts(const std::vector<std::string>& values)
    {
        std::string result;
        for (const auto& value : values) {
            if (!result.empty()) result += ", ";
            result += value;
        }
        return result;
    }

    void drawGraph(ayt::ui::IRenderBackend& renderer,
                   const FRectangle& bounds,
                   const ayt::ui::UIFlowGraphDefinition& graph)
    {
        renderer.drawText({bounds.minX + 18.0f, bounds.minY + 12.0f,
                           bounds.maxX - 18.0f, bounds.minY + 44.0f},
            ayt::ui::decodeUtf8Text("Action Graph — " + graph.id), 15,
            FVector4{0.84f, 0.88f, 0.94f, 1.0f});
        struct NodeVisual {
            FRectangle bounds;
            const ayt::ui::UIFlowGraphNodeTypeDefinition* type = nullptr;
            std::unordered_map<std::string, FVector2> inputs;
            std::unordered_map<std::string, FVector2> outputs;
        };
        std::unordered_map<std::string, NodeVisual> nodes;
        const float nodeWidth = 228.0f;
        for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
            const float x = bounds.minX + 24.0f
                + static_cast<float>(index % 3u) * 260.0f;
            const float y = bounds.minY + 62.0f
                + static_cast<float>(index / 3u) * 154.0f;
            const auto* type = _registry.find(graph.nodes[index].type);
            std::size_t inputCount = 0;
            std::size_t outputCount = 0;
            if (type != nullptr) {
                for (const auto& pin : type->pins) {
                    if (pin.direction
                        == ayt::ui::UIFlowGraphPinDirection::Input) {
                        ++inputCount;
                    } else {
                        ++outputCount;
                    }
                }
            }
            const float rows = static_cast<float>(
                std::max(inputCount, outputCount));
            const float height = std::max(78.0f, 42.0f + rows * 20.0f);
            NodeVisual visual;
            visual.bounds = {x, y, x + nodeWidth, y + height};
            visual.type = type;
            std::size_t inputIndex = 0;
            std::size_t outputIndex = 0;
            if (type != nullptr) {
                for (const auto& pin : type->pins) {
                    if (pin.direction
                        == ayt::ui::UIFlowGraphPinDirection::Input) {
                        visual.inputs[pin.id] = {x,
                            y + 38.0f + 20.0f
                                * static_cast<float>(inputIndex++)};
                    } else {
                        visual.outputs[pin.id] = {x + nodeWidth,
                            y + 38.0f + 20.0f
                                * static_cast<float>(outputIndex++)};
                    }
                }
            }
            nodes[graph.nodes[index].id] = std::move(visual);
        }
        for (const auto& link : graph.links) {
            const auto from = nodes.find(link.fromNode);
            const auto to = nodes.find(link.toNode);
            if (from == nodes.end() || to == nodes.end()) continue;
            const auto fromPin = from->second.outputs.find(link.fromPin);
            const auto toPin = to->second.inputs.find(link.toPin);
            if (fromPin == from->second.outputs.end()
                || toPin == to->second.inputs.end()) continue;
            FVector4 color{0.68f, 0.43f, 0.95f, 0.9f};
            if (from->second.type != nullptr) {
                if (const auto* pin = ayt::ui::findUIFlowGraphPin(
                        *from->second.type, link.fromPin,
                        ayt::ui::UIFlowGraphPinDirection::Output)) {
                    color = graphPinColor(*pin);
                }
            }
            connector(renderer, fromPin->second, toPin->second, color);
        }
        for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
            const auto found = nodes.find(graph.nodes[index].id);
            if (found == nodes.end()) continue;
            const NodeVisual& visual = found->second;
            const FRectangle& card = visual.bounds;
            const EditorUiFlowDebugPause* pause = _preview.debugPause();
            const bool paused = pause != nullptr
                && pause->graphId == graph.id
                && pause->nodeId == graph.nodes[index].id;
            renderer.drawRect(card, {0.12f, 0.14f, 0.18f, 1.0f});
            border(renderer, card,
                paused ? FVector4{1.0f, 0.68f, 0.18f, 1.0f}
                       : FVector4{0.30f, 0.44f, 0.62f, 1.0f},
                paused ? 3.0f : 1.0f);
            renderer.drawRect({card.minX, card.minY, card.maxX,
                               card.minY + 22.0f},
                              {0.11f, 0.32f, 0.47f, 1.0f});
            renderer.drawText({card.minX + 8.0f, card.minY,
                               card.maxX - 8.0f, card.minY + 22.0f},
                ayt::ui::decodeUtf8Text(graph.nodes[index].type), 12,
                FVector4{0.94f, 0.96f, 1.0f, 1.0f});
            renderer.drawText({card.minX + 8.0f, card.minY + 22.0f,
                               card.maxX - 8.0f, card.minY + 39.0f},
                ayt::ui::decodeUtf8Text(graph.nodes[index].id), 13,
                FVector4{0.72f, 0.77f, 0.84f, 1.0f});
            if (visual.type == nullptr) continue;
            for (const auto& pin : visual.type->pins) {
                const auto& endpoint = pin.direction
                        == ayt::ui::UIFlowGraphPinDirection::Input
                    ? visual.inputs.at(pin.id) : visual.outputs.at(pin.id);
                const FVector4 color = graphPinColor(pin);
                renderer.drawRect({endpoint.x - 4.0f, endpoint.y - 4.0f,
                                   endpoint.x + 4.0f, endpoint.y + 4.0f},
                                  color);
                const bool input = pin.direction
                    == ayt::ui::UIFlowGraphPinDirection::Input;
                renderer.drawText(
                    input
                        ? FRectangle{endpoint.x + 8.0f, endpoint.y - 9.0f,
                                     card.minX + 106.0f, endpoint.y + 10.0f}
                        : FRectangle{card.maxX - 106.0f, endpoint.y - 9.0f,
                                     endpoint.x - 8.0f, endpoint.y + 10.0f},
                    ayt::ui::decodeUtf8Text(pin.id), 10,
                    FVector4{0.72f, 0.77f, 0.84f, 1.0f});
            }
        }
    }

    void drawMountedScreens(ayt::ui::IRenderBackend& renderer,
                            const FRectangle& bounds)
    {
        const auto& screens = _preview.mountedScreens();
        if (screens.empty()) return;
        float x = bounds.minX + 18.0f;
        const float y = bounds.maxY - 42.0f;
        for (const auto& screen : screens) {
            const float width = std::min(180.0f,
                58.0f + static_cast<float>(screen.screenId.size()) * 7.0f);
            const FRectangle chip{x, y, x + width, bounds.maxY - 12.0f};
            renderer.drawRect(chip, {0.08f, 0.31f, 0.27f, 0.95f});
            border(renderer, chip, {0.12f, 0.72f, 0.58f, 1.0f});
            renderer.drawText({chip.minX + 8.0f, chip.minY,
                               chip.maxX - 6.0f, chip.maxY},
                ayt::ui::decodeUtf8Text(screen.screenId), 12,
                FVector4{0.91f, 1.0f, 0.97f, 1.0f});
            x += width + 8.0f;
            if (x > bounds.maxX - 180.0f) break;
        }
    }

    EditorUiFlowDocument& _document;
    EditorUiFlowPreview& _preview;
    const ayt::ui::UIFlowGraphNodeRegistry& _registry;
    std::function<void()> _changed;
    std::vector<Hit> _hits;
};

// Runtime layouts are authored against a full UI viewport. Keep that contract
// inside the editor while clipping both rendering and picking to the preview
// pane, so an oversized Screen can never leak into the graph or inspector.
class EditorUiFlowPreviewViewport final : public ayt::ui::CompoundWidget {
public:
    EditorUiFlowPreviewViewport()
    {
        setId("__ay_ui_flow_visual_preview");
        setLayoutPositionManaged(false);
        setLayoutSizeManaged(false);
    }

    ayt::ui::Widget* hitTest(const FVector2& worldPos) override
    {
        return ayt::ui::compoundDescendHitTestClipped(this, worldPos);
    }

protected:
    void renderChildren(ayt::ui::IRenderBackend& renderer) override
    {
        ayt::ui::compoundDescendClippedRender(this, renderer);
    }
};

template<class T>
T* widgetAs(ayt::ui::UIManager& ui, const char* id)
{
    return dynamic_cast<T*>(ui.findById(id));
}

std::wstring wide(std::string_view value)
{
    return ayt::ui::decodeUtf8Text(std::string(value));
}

EditorUiFlowObjectKind kindAt(int index)
{
    static constexpr std::array kinds{
        EditorUiFlowObjectKind::Layer,
        EditorUiFlowObjectKind::Slot,
        EditorUiFlowObjectKind::Screen,
        EditorUiFlowObjectKind::Context,
        EditorUiFlowObjectKind::Entry,
        EditorUiFlowObjectKind::Signal,
        EditorUiFlowObjectKind::Action,
        EditorUiFlowObjectKind::Region,
        EditorUiFlowObjectKind::State,
        EditorUiFlowObjectKind::Transition,
        EditorUiFlowObjectKind::Graph,
    };
    return index >= 0 && static_cast<std::size_t>(index) < kinds.size()
        ? kinds[static_cast<std::size_t>(index)]
        : EditorUiFlowObjectKind::Layer;
}

bool splitEndpoint(const std::wstring& text,
                   std::string& node,
                   std::string& pin)
{
    const std::string encoded = encodeUtf8(text);
    const std::size_t separator = encoded.rfind('.');
    if (separator == std::string::npos) return false;
    node = encoded.substr(0, separator);
    pin = encoded.substr(separator + 1);
    return !node.empty() && !pin.empty();
}

ayt::ui::UIFlowGraphNodeTypeDefinition makeCommandNodeType(
    std::string type,
    std::string displayName,
    std::string category,
    bool canFail = false)
{
    using Direction = ayt::ui::UIFlowGraphPinDirection;
    using Kind = ayt::ui::UIFlowGraphPinKind;
    ayt::ui::UIFlowGraphNodeTypeDefinition result;
    result.type = std::move(type);
    result.displayName = std::move(displayName);
    result.category = std::move(category);
    result.pins = {
        {"execute", Direction::Input, Kind::Execution},
        {"completed", Direction::Output, Kind::Execution},
    };
    if (canFail) {
        result.pins.push_back({"failed", Direction::Output, Kind::Execution});
    }
    return result;
}

std::vector<ayt::ui::UIFlowGraphNodeTypeDefinition> builtInGraphNodeTypes()
{
    using Direction = ayt::ui::UIFlowGraphPinDirection;
    using Kind = ayt::ui::UIFlowGraphPinKind;
    using ValueType = ayt::ui::UIFlowValueType;
    auto types = std::vector<ayt::ui::UIFlowGraphNodeTypeDefinition>{
        makeCommandNodeType("host.action", "Invoke Host Action", "Host", true),
        makeCommandNodeType("ui.present", "Present Screen", "UI"),
        makeCommandNodeType("ui.playAnimation", "Play UI Animation", "UI"),
        makeCommandNodeType("game.world.load", "Load World", "Game", true),
        makeCommandNodeType("flow.emitSignal", "Emit Signal", "Flow"),
        makeCommandNodeType("flow.invokeAction", "Invoke Action", "Flow", true),
    };
    const auto addInput = [](auto& type, const char* id, ValueType valueType) {
        type.pins.push_back({id, Direction::Input, Kind::Value, valueType});
        type.properties.push_back({id, valueType, false, {}});
    };
    addInput(types[0], "action", ValueType::String);
    addInput(types[1], "screen", ValueType::String);
    addInput(types[2], "animation", ValueType::String);
    addInput(types[3], "world", ValueType::Asset);
    addInput(types[4], "signal", ValueType::String);
    addInput(types[5], "action", ValueType::String);
    types[0].pins.push_back(
        {"accepted", Direction::Output, Kind::Value, ValueType::Boolean});
    types[5].pins.push_back(
        {"accepted", Direction::Output, Kind::Value, ValueType::Boolean});
    return types;
}

void selectComboItem(ayt::ui::ComboBox& combo,
                     const std::vector<std::wstring>& items,
                     const std::wstring& preferred)
{
    combo.setItems(items);
    const auto found = std::find(items.begin(), items.end(), preferred);
    combo.setSelectedIndex(found != items.end()
        ? static_cast<int>(std::distance(items.begin(), found))
        : items.empty() ? -1 : 0);
}

// EditorExtensionRegistry requires every document editor to expose a view
// factory. Desktop AYEditor opens this editor in an AYDevice child window;
// hosts without child-window support receive an explicit launch surface
// instead of making registration fail and rendering the document unopenable.
class EditorUiFlowLaunchView final : public IEditorView {
public:
    EditorUiFlowLaunchView()
    {
        auto* root = new ayt::ui::Panel();
        root->setId("ui_flow_launch_surface");
        root->setSize({720.0f, 420.0f});
        auto* title = new ayt::ui::TextLabel();
        title->setId("ui_flow_launch_title");
        title->setText(L"UI Flow Editor opens in a dedicated tool window.");
        title->setPosition({28.0f, 28.0f});
        title->setSize({640.0f, 32.0f});
        title->setLayoutPositionManaged(false);
        title->setLayoutSizeManaged(false);
        root->addChild(title);
        _root = root;
    }

    ~EditorUiFlowLaunchView() override { delete _root; }

    ayt::ui::Widget* rootWidget() noexcept override { return _root; }
    ayt::ui::Widget* releaseRootWidget() noexcept override {
        ayt::ui::Widget* result = _root;
        _root = nullptr;
        return result;
    }

private:
    ayt::ui::Widget* _root = nullptr;
};

} // namespace

class EditorUiFlowController::Impl {
public:
    Impl(std::shared_ptr<EditorUiFlowDocument> value,
         EditorUiFlowExtensionConfig cfg)
        : document(std::move(value)), config(std::move(cfg))
    {
        for (auto& type : builtInGraphNodeTypes()) {
            (void)graphRegistry.registerType(std::move(type));
        }
        for (auto& type : config.graphNodeTypes) {
            (void)graphRegistry.unregisterType(type.type);
            (void)graphRegistry.registerType(std::move(type));
        }
        preview.setGraphNodeTypes(graphRegistry.types());
        if (document != nullptr) {
            document->setChangedHandler([this]() {
                refreshPending = true;
                if (stateChanged != nullptr) stateChanged();
            });
        }
    }

    ~Impl()
    {
        detach();
        if (document != nullptr) document->setChangedHandler({});
    }

    void detach()
    {
        preview.clearVisualHost();
        if (graphFrom != nullptr) graphFrom->setOnSelectionChanged({});
        if (visualViewport != nullptr) visualViewport->detachFromParent();
        delete visualViewport;
        visualViewport = nullptr;
        if (canvas != nullptr) canvas->detachFromParent();
        delete canvas;
        canvas = nullptr;
        ui = nullptr;
        outline = nullptr;
        diagnostics = nullptr;
        trace = nullptr;
        mounted = nullptr;
        signal = nullptr;
        action = nullptr;
        entry = nullptr;
        addKind = nullptr;
        propId = nullptr;
        propA = nullptr;
        propB = nullptr;
        propC = nullptr;
        propD = nullptr;
        propE = nullptr;
        propF = nullptr;
        propG = nullptr;
        propNumber = nullptr;
        propFlag = nullptr;
        graphNodeType = nullptr;
        graphFrom = nullptr;
        graphTo = nullptr;
        debugNode = nullptr;
        debugState = nullptr;
        status = nullptr;
        canvasHost = nullptr;
        visualPreviewHost = nullptr;
        attached = false;
    }

    void setStatus(std::string value, bool error = false)
    {
        if (status != nullptr) {
            status->setText(wide((error ? "Error: " : "") + value));
        }
        if (stateChanged != nullptr) stateChanged();
    }

    void refresh()
    {
        if (!attached || document == nullptr || refreshing) return;
        refreshing = true;
        outlineItems = document->outline();
        std::vector<std::wstring> rows;
        rows.reserve(outlineItems.size());
        int selected = -1;
        for (std::size_t index = 0; index < outlineItems.size(); ++index) {
            rows.push_back(wide(outlineItems[index].label));
            if (outlineItems[index].selection == document->selection()) {
                selected = static_cast<int>(index);
            }
        }
        outline->setItems(rows);
        outline->setSelectedIndex(selected);

        if (!config.assetRoot.empty() && document->isValid()
            && assetValidationRevision != document->revision()) {
            assetValidation = ayt::app::validateUIFlowAssets(
                document->flow(), config.assetRoot);
            assetValidationRevision = document->revision();
        } else if (!document->isValid()) {
            assetValidation = {};
            assetValidationRevision = document->revision();
        }

        std::vector<std::wstring> diagnosticRows;
        for (const auto& value : document->diagnostics()) {
            diagnosticRows.push_back(wide(
                std::string(value.severity == ayt::ui::UIFlowDiagnosticSeverity::Error
                    ? "ERROR  " : "WARN   ")
                + value.path + "  " + value.message));
        }
        for (const auto& value : assetValidation.diagnostics) {
            diagnosticRows.push_back(wide(
                std::string(value.severity == ayt::ui::UIFlowDiagnosticSeverity::Error
                    ? "ASSET ERROR  " : "ASSET WARN   ")
                + value.path + "  " + value.message));
        }
        std::vector<ayt::ui::UIFlowDiagnostic> graphDiagnostics;
        (void)ayt::ui::validateUIFlowGraphNodes(
            document->flow(), graphRegistry, &graphDiagnostics);
        for (const auto& value : graphDiagnostics) {
            diagnosticRows.push_back(wide(
                std::string("NODE ERROR  ") + value.path + "  "
                + value.message));
        }
        if (diagnosticRows.empty()) diagnosticRows.push_back(L"No diagnostics");
        diagnostics->setItems(diagnosticRows);
        if (auto* title = widgetAs<ayt::ui::TextLabel>(
                *ui, "flow_diag_title")) {
            title->setText(wide("DIAGNOSTICS  ·  "
                + std::to_string(assetValidation.dependencies.size())
                + " LAYOUT ASSET(S)"));
        }

        const EditorUiFlowProperties properties = document->selectedProperties();
        const EditorUiFlowPropertyLabels labels = document->selectedPropertyLabels();
        propId->setText(wide(properties.id));
        propA->setText(wide(properties.first));
        propB->setText(wide(properties.second));
        propC->setText(wide(properties.third));
        propD->setText(wide(properties.fourth));
        propE->setText(wide(properties.fifth));
        propF->setText(wide(properties.sixth));
        propG->setText(wide(properties.seventh));
        propNumber->setText(std::to_wstring(properties.number));
        propFlag->setChecked(properties.flag);
        setFieldLabel("flow_lbl_a", labels.first, propA);
        setFieldLabel("flow_lbl_b", labels.second, propB);
        setFieldLabel("flow_lbl_c", labels.third, propC);
        setFieldLabel("flow_lbl_d", labels.fourth, propD);
        setFieldLabel("flow_lbl_e", labels.fifth, propE);
        setFieldLabel("flow_lbl_f", labels.sixth, propF);
        setFieldLabel("flow_lbl_g", labels.seventh, propG);
        setFieldLabel("flow_lbl_number", labels.number, propNumber);
        if (auto* label = widgetAs<ayt::ui::TextLabel>(*ui, "flow_lbl_flag")) {
            label->setText(wide(labels.flag));
            label->setVisible(!labels.flag.empty());
        }
        propFlag->setVisible(!labels.flag.empty());
        if (auto* kind = widgetAs<ayt::ui::TextLabel>(*ui, "flow_selection_kind")) {
            kind->setText(wide(std::string("Inspector — ")
                + EditorUiFlowDocument::kindName(document->selection().kind)));
        }

        std::vector<std::wstring> signals;
        for (const auto& value : document->flow().signals) signals.push_back(wide(value.id));
        signal->setItems(signals);
        if (!signals.empty() && signal->getSelectedIndex() < 0) signal->setSelectedIndex(0);
        std::vector<std::wstring> actions;
        for (const auto& value : document->flow().actions) actions.push_back(wide(value.id));
        action->setItems(actions);
        if (!actions.empty() && action->getSelectedIndex() < 0) action->setSelectedIndex(0);
        std::vector<std::wstring> entries;
        for (const auto& value : document->flow().entries) entries.push_back(wide(value.id));
        entry->setItems(entries);
        const auto selectedEntry = std::find_if(document->flow().entries.begin(),
            document->flow().entries.end(), [&](const auto& value) {
                return value.id == document->flow().defaultEntry;
            });
        if (selectedEntry != document->flow().entries.end()) {
            entry->setSelectedIndex(static_cast<int>(
                std::distance(document->flow().entries.begin(), selectedEntry)));
        }
        refreshGraphChoices();
        refreshPreviewLists();
        if (canvas != nullptr) canvas->markDirty();
        ui->invalidateLayout();
        refreshing = false;
        refreshPending = false;
    }

    void setFieldLabel(const char* id, const std::string& text,
                       ayt::ui::Widget* input)
    {
        if (auto* label = widgetAs<ayt::ui::TextLabel>(*ui, id)) {
            label->setText(wide(text));
            label->setVisible(!text.empty());
        }
        input->setVisible(!text.empty());
    }

    void refreshPreviewLists()
    {
        std::vector<std::wstring> screenRows;
        for (const auto& value : preview.mountedScreens()) {
            screenRows.push_back(wide(value.layerId + " / " + value.slotId
                + "  →  " + value.screenId));
        }
        if (screenRows.empty()) screenRows.push_back(L"No mounted Screens");
        mounted->setItems(screenRows);
        std::vector<std::wstring> traceRows;
        for (const auto& value : preview.trace()) {
            traceRows.push_back(wide(value.category + "  " + value.id
                + "  —  " + value.detail));
        }
        if (traceRows.empty()) traceRows.push_back(L"No preview events");
        trace->setItems(traceRows);
        if (debugState != nullptr) {
            const EditorUiFlowDebugPause* pause = preview.debugPause();
            if (pause == nullptr) {
                debugState->setText(L"Running");
            } else {
                std::string values;
                for (const auto& [id, value] : pause->inputs) {
                    if (!values.empty()) values += ", ";
                    values += id + "=" + value;
                }
                debugState->setText(wide("Paused " + pause->graphId + "/"
                    + pause->nodeId + " (" + pause->reason + ")"
                    + (values.empty() ? std::string{}
                                      : "  inputs: " + values)));
            }
        }
    }

    void refreshGraphChoices()
    {
        const std::wstring selectedType = graphNodeType->getSelectedItem();
        std::vector<std::wstring> nodeTypes;
        nodeTypes.reserve(graphRegistry.types().size());
        for (const auto& type : graphRegistry.types()) {
            nodeTypes.push_back(wide(type.type));
        }
        selectComboItem(*graphNodeType, nodeTypes, selectedType);

        struct Endpoint {
            std::wstring label;
            std::string nodeId;
            const ayt::ui::UIFlowGraphPinTypeDefinition* pin = nullptr;
        };
        std::vector<Endpoint> outputs;
        std::vector<Endpoint> inputs;
        if (document->selection().kind == EditorUiFlowObjectKind::Graph) {
            if (const auto* graph = document->flow().findGraph(
                    document->selection().id)) {
                for (const auto& node : graph->nodes) {
                    const auto* type = graphRegistry.find(node.type);
                    if (type == nullptr) continue;
                    for (const auto& pin : type->pins) {
                        auto& choices = pin.direction
                                == ayt::ui::UIFlowGraphPinDirection::Output
                            ? outputs : inputs;
                        choices.push_back({wide(node.id + "." + pin.id),
                                           node.id, &pin});
                    }
                }
            }
        }
        const std::wstring selectedOutput = graphFrom->getSelectedItem();
        const std::wstring selectedInput = graphTo->getSelectedItem();
        std::vector<std::wstring> outputLabels;
        outputLabels.reserve(outputs.size());
        for (const auto& endpoint : outputs) {
            outputLabels.push_back(endpoint.label);
        }
        selectComboItem(*graphFrom, outputLabels, selectedOutput);

        const auto selectedSource = std::find_if(outputs.begin(), outputs.end(),
            [this](const Endpoint& endpoint) {
                return endpoint.label == graphFrom->getSelectedItem();
            });
        std::vector<std::wstring> compatibleInputs;
        if (selectedSource != outputs.end()) {
            for (const auto& endpoint : inputs) {
                if (endpoint.nodeId == selectedSource->nodeId) continue;
                if (ayt::ui::areUIFlowGraphPinsCompatible(
                        *selectedSource->pin, *endpoint.pin)) {
                    compatibleInputs.push_back(endpoint.label);
                }
            }
        }
        selectComboItem(*graphTo, compatibleInputs, selectedInput);

        const std::wstring selectedDebugNode = debugNode != nullptr
            ? debugNode->getSelectedItem() : std::wstring{};
        std::vector<std::wstring> debugNodes;
        if (document->selection().kind == EditorUiFlowObjectKind::Graph) {
            if (const auto* graph = document->flow().findGraph(
                    document->selection().id)) {
                debugNodes.reserve(graph->nodes.size());
                for (const auto& node : graph->nodes) {
                    debugNodes.push_back(wide(node.id));
                }
            }
        }
        if (debugNode != nullptr) {
            selectComboItem(*debugNode, debugNodes, selectedDebugNode);
        }
    }

    bool restart(std::string* output)
    {
        const int index = entry != nullptr ? entry->getSelectedIndex() : -1;
        const std::string entryId = index >= 0
            && static_cast<std::size_t>(index) < document->flow().entries.size()
            ? document->flow().entries[static_cast<std::size_t>(index)].id
            : document->flow().defaultEntry;
        std::string error;
        const bool ok = preview.rebuild(document->flow(), entryId, &error);
        setStatus(ok ? "Preview running" : error, !ok);
        refreshPreviewLists();
        if (canvas != nullptr) canvas->markDirty();
        if (output != nullptr) *output = error;
        return ok;
    }

    bool save(bool saveAs)
    {
        std::string error;
        bool result = false;
        if (saveAs || document->path().empty()) {
            const std::string path = config.savePathPicker != nullptr
                ? config.savePathPicker() : std::string{};
            if (path.empty()) return false;
            result = document->saveAs(path, &error);
        } else {
            result = document->save(&error);
        }
        setStatus(result ? "Saved " + document->title() : error, !result);
        return result;
    }

    void bindButton(const char* id, std::function<void()> callback)
    {
        if (auto* button = widgetAs<ayt::ui::Button>(*ui, id)) {
            button->setOnClicked(std::move(callback));
        }
    }

    std::shared_ptr<EditorUiFlowDocument> document;
    EditorUiFlowExtensionConfig config;
    EditorUiFlowPreview preview;
    ayt::ui::UIFlowGraphNodeRegistry graphRegistry;
    StateChanged stateChanged;
    ayt::ui::UIManager* ui = nullptr;
    ayt::ui::ListView* outline = nullptr;
    ayt::ui::ListView* diagnostics = nullptr;
    ayt::ui::ListView* trace = nullptr;
    ayt::ui::ListView* mounted = nullptr;
    ayt::ui::ComboBox* signal = nullptr;
    ayt::ui::ComboBox* action = nullptr;
    ayt::ui::ComboBox* entry = nullptr;
    ayt::ui::ComboBox* addKind = nullptr;
    ayt::ui::TextInput* propId = nullptr;
    ayt::ui::TextInput* propA = nullptr;
    ayt::ui::TextInput* propB = nullptr;
    ayt::ui::TextInput* propC = nullptr;
    ayt::ui::TextInput* propD = nullptr;
    ayt::ui::TextInput* propE = nullptr;
    ayt::ui::TextInput* propF = nullptr;
    ayt::ui::TextInput* propG = nullptr;
    ayt::ui::TextInput* propNumber = nullptr;
    ayt::ui::CheckBox* propFlag = nullptr;
    ayt::ui::ComboBox* graphNodeType = nullptr;
    ayt::ui::ComboBox* graphFrom = nullptr;
    ayt::ui::ComboBox* graphTo = nullptr;
    ayt::ui::ComboBox* debugNode = nullptr;
    ayt::ui::TextLabel* debugState = nullptr;
    ayt::ui::TextLabel* status = nullptr;
    ayt::ui::Panel* canvasHost = nullptr;
    ayt::ui::Panel* visualPreviewHost = nullptr;
    EditorUiFlowCanvas* canvas = nullptr;
    EditorUiFlowPreviewViewport* visualViewport = nullptr;
    std::vector<EditorUiFlowOutlineItem> outlineItems;
    ayt::app::UIFlowAssetValidationResult assetValidation;
    std::uint64_t assetValidationRevision = 0;
    std::unordered_set<std::string> debugBreakpoints;
    bool attached = false;
    bool refreshing = false;
    bool refreshPending = false;
};

EditorUiFlowController::EditorUiFlowController(
    std::shared_ptr<EditorUiFlowDocument> document,
    EditorUiFlowExtensionConfig config)
    : _impl(std::make_unique<Impl>(std::move(document), std::move(config)))
{
}

EditorUiFlowController::~EditorUiFlowController() = default;

bool EditorUiFlowController::attach(ayt::ui::UIManager& ui)
{
    if (_impl == nullptr || _impl->document == nullptr) return false;
    _impl->ui = &ui;
    _impl->outline = widgetAs<ayt::ui::ListView>(ui, "flow_outline");
    _impl->diagnostics = widgetAs<ayt::ui::ListView>(ui, "flow_diagnostics");
    _impl->trace = widgetAs<ayt::ui::ListView>(ui, "flow_trace");
    _impl->mounted = widgetAs<ayt::ui::ListView>(ui, "flow_mounted");
    _impl->signal = widgetAs<ayt::ui::ComboBox>(ui, "flow_signal");
    _impl->action = widgetAs<ayt::ui::ComboBox>(ui, "flow_action");
    _impl->entry = widgetAs<ayt::ui::ComboBox>(ui, "flow_entry");
    _impl->addKind = widgetAs<ayt::ui::ComboBox>(ui, "flow_add_kind");
    _impl->propId = widgetAs<ayt::ui::TextInput>(ui, "flow_prop_id");
    _impl->propA = widgetAs<ayt::ui::TextInput>(ui, "flow_prop_a");
    _impl->propB = widgetAs<ayt::ui::TextInput>(ui, "flow_prop_b");
    _impl->propC = widgetAs<ayt::ui::TextInput>(ui, "flow_prop_c");
    _impl->propD = widgetAs<ayt::ui::TextInput>(ui, "flow_prop_d");
    _impl->propE = widgetAs<ayt::ui::TextInput>(ui, "flow_prop_e");
    _impl->propF = widgetAs<ayt::ui::TextInput>(ui, "flow_prop_f");
    _impl->propG = widgetAs<ayt::ui::TextInput>(ui, "flow_prop_g");
    _impl->propNumber = widgetAs<ayt::ui::TextInput>(ui, "flow_prop_number");
    _impl->propFlag = widgetAs<ayt::ui::CheckBox>(ui, "flow_prop_flag");
    _impl->graphNodeType = widgetAs<ayt::ui::ComboBox>(ui, "flow_graph_node_type");
    _impl->graphFrom = widgetAs<ayt::ui::ComboBox>(ui, "flow_graph_from");
    _impl->graphTo = widgetAs<ayt::ui::ComboBox>(ui, "flow_graph_to");
    _impl->debugNode = widgetAs<ayt::ui::ComboBox>(ui, "flow_debug_node");
    _impl->debugState = widgetAs<ayt::ui::TextLabel>(ui, "flow_debug_state");
    _impl->status = widgetAs<ayt::ui::TextLabel>(ui, "flow_status");
    _impl->canvasHost = widgetAs<ayt::ui::Panel>(ui, "flow_canvas_host");
    _impl->visualPreviewHost =
        widgetAs<ayt::ui::Panel>(ui, "flow_visual_preview_host");
    if (_impl->outline == nullptr || _impl->diagnostics == nullptr
        || _impl->trace == nullptr || _impl->mounted == nullptr
        || _impl->signal == nullptr || _impl->action == nullptr
        || _impl->entry == nullptr || _impl->addKind == nullptr
        || _impl->propId == nullptr || _impl->propA == nullptr
        || _impl->propB == nullptr || _impl->propC == nullptr
        || _impl->propD == nullptr || _impl->propE == nullptr
        || _impl->propF == nullptr || _impl->propG == nullptr
        || _impl->propNumber == nullptr
        || _impl->propFlag == nullptr || _impl->graphNodeType == nullptr
        || _impl->graphFrom == nullptr || _impl->graphTo == nullptr
        || _impl->debugNode == nullptr || _impl->debugState == nullptr
        || _impl->status == nullptr
        || _impl->canvasHost == nullptr
        || _impl->visualPreviewHost == nullptr) {
        _impl->detach();
        return false;
    }
    _impl->attached = true;
    _impl->addKind->setItems({L"Layer", L"Slot", L"Screen", L"Context",
        L"Entry", L"Signal", L"Action", L"Region", L"State",
        L"Transition", L"Graph"});
    _impl->addKind->setSelectedIndex(0);
    _impl->outline->setOnSelectionChanged([impl = _impl.get()](int index) {
        if (impl->refreshing || index < 0
            || static_cast<std::size_t>(index) >= impl->outlineItems.size()) return;
        (void)impl->document->select(impl->outlineItems[static_cast<std::size_t>(index)].selection);
    });
    _impl->graphFrom->setOnSelectionChanged([impl = _impl.get()](int) {
        if (!impl->refreshing) impl->refreshGraphChoices();
    });
    _impl->bindButton("flow_btn_add", [impl = _impl.get()]() {
        std::string error;
        if (!impl->document->addObject(
                kindAt(impl->addKind->getSelectedIndex()), {}, &error)) {
            impl->setStatus(error, true);
        }
    });
    _impl->bindButton("flow_btn_delete", [impl = _impl.get()]() {
        std::string error;
        if (!impl->document->deleteSelection(&error)) impl->setStatus(error, true);
    });
    _impl->bindButton("flow_btn_apply", [impl = _impl.get()]() {
        EditorUiFlowProperties value;
        value.id = encodeUtf8(impl->propId->getText());
        value.first = encodeUtf8(impl->propA->getText());
        value.second = encodeUtf8(impl->propB->getText());
        value.third = encodeUtf8(impl->propC->getText());
        value.fourth = encodeUtf8(impl->propD->getText());
        value.fifth = encodeUtf8(impl->propE->getText());
        value.sixth = encodeUtf8(impl->propF->getText());
        value.seventh = encodeUtf8(impl->propG->getText());
        try { value.number = std::stoi(impl->propNumber->getText()); }
        catch (...) { value.number = 0; }
        value.flag = impl->propFlag->isChecked();
        std::string error;
        if (!impl->document->applySelectedProperties(value, &error)) {
            impl->setStatus(error, true);
        }
    });
    _impl->bindButton("flow_btn_undo", [impl = _impl.get()]() {
        (void)impl->document->undo();
    });
    _impl->bindButton("flow_btn_redo", [impl = _impl.get()]() {
        (void)impl->document->redo();
    });
    _impl->bindButton("flow_btn_save", [impl = _impl.get()]() {
        (void)impl->save(false);
    });
    _impl->bindButton("flow_btn_save_as", [impl = _impl.get()]() {
        (void)impl->save(true);
    });
    _impl->bindButton("flow_btn_open", [impl = _impl.get()]() {
        if (impl->config.openPathPicker == nullptr) return;
        const std::string path = impl->config.openPathPicker();
        if (path.empty()) return;
        std::string error;
        if (!impl->document->initialize(path, path, &error)) {
            impl->setStatus(error, true);
        }
    });
    _impl->bindButton("flow_btn_preview", [impl = _impl.get()]() {
        (void)impl->restart(nullptr);
    });
    _impl->bindButton("flow_btn_emit", [impl = _impl.get()]() {
        const int index = impl->signal->getSelectedIndex();
        if (index < 0 || static_cast<std::size_t>(index)
                >= impl->document->flow().signals.size()) return;
        std::string error;
        if (!impl->preview.emitSignal(
                impl->document->flow().signals[static_cast<std::size_t>(index)].id,
                &error)) {
            impl->setStatus(error, true);
        } else {
            impl->setStatus("Signal simulated");
            impl->refreshPreviewLists();
            impl->canvas->markDirty();
        }
    });
    _impl->bindButton("flow_btn_action", [impl = _impl.get()]() {
        const int index = impl->action->getSelectedIndex();
        if (index < 0 || static_cast<std::size_t>(index)
                >= impl->document->flow().actions.size()) return;
        std::string error;
        if (!impl->preview.invokeAction(
                impl->document->flow().actions[static_cast<std::size_t>(index)].id,
                &error)) impl->setStatus(error, true);
        else {
            impl->setStatus("Mock Action executed");
            impl->refreshPreviewLists();
        }
    });
    _impl->bindButton("flow_btn_breakpoint", [impl = _impl.get()]() {
        if (impl->document->selection().kind
                != EditorUiFlowObjectKind::Graph
            || impl->debugNode->getSelectedIndex() < 0) {
            impl->setStatus("Select a Graph node for a breakpoint.", true);
            return;
        }
        const std::string graphId = impl->document->selection().id;
        const std::string nodeId = encodeUtf8(
            impl->debugNode->getSelectedItem());
        const std::string key = graphId + "\n" + nodeId;
        const bool enabled = !impl->debugBreakpoints.contains(key);
        if (!impl->preview.setBreakpoint(graphId, nodeId, enabled)) {
            impl->setStatus("Cannot update breakpoint.", true);
            return;
        }
        if (enabled) impl->debugBreakpoints.insert(key);
        else impl->debugBreakpoints.erase(key);
        impl->setStatus(std::string(enabled ? "Breakpoint set: "
                                            : "Breakpoint cleared: ")
            + graphId + "/" + nodeId);
    });
    _impl->bindButton("flow_btn_pause_next", [impl = _impl.get()]() {
        impl->preview.requestPause();
        impl->setStatus("Debugger will pause before the next Graph node.");
    });
    _impl->bindButton("flow_btn_step", [impl = _impl.get()]() {
        std::string error;
        if (!impl->preview.stepExecution(&error)) {
            impl->setStatus(error, true);
            return;
        }
        impl->refreshPreviewLists();
        impl->canvas->markDirty();
        impl->setStatus(impl->preview.isPaused()
            ? "Stepped to next node" : "Graph completed");
    });
    _impl->bindButton("flow_btn_continue", [impl = _impl.get()]() {
        std::string error;
        if (!impl->preview.continueExecution(&error)) {
            impl->setStatus(error, true);
            return;
        }
        impl->refreshPreviewLists();
        impl->canvas->markDirty();
        impl->setStatus(impl->preview.isPaused()
            ? "Paused at breakpoint" : "Graph continued");
    });
    _impl->bindButton("flow_btn_add_node", [impl = _impl.get()]() {
        if (impl->document->selection().kind != EditorUiFlowObjectKind::Graph) {
            impl->setStatus("Select a Graph before adding nodes.", true);
            return;
        }
        std::string error;
        if (!impl->document->addGraphNode(
                impl->document->selection().id,
                encodeUtf8(impl->graphNodeType->getSelectedItem()), &error)) {
            impl->setStatus(error, true);
        }
    });
    _impl->bindButton("flow_btn_connect", [impl = _impl.get()]() {
        if (impl->document->selection().kind != EditorUiFlowObjectKind::Graph) {
            impl->setStatus("Select a Graph before connecting nodes.", true);
            return;
        }
        std::string fromNode;
        std::string fromPin;
        std::string toNode;
        std::string toPin;
        if (!splitEndpoint(impl->graphFrom->getSelectedItem(), fromNode, fromPin)
            || !splitEndpoint(impl->graphTo->getSelectedItem(), toNode, toPin)) {
            impl->setStatus("Select compatible graph endpoints.", true);
            return;
        }
        std::string error;
        if (!impl->document->connectGraphNodes(
                impl->document->selection().id,
                std::move(fromNode), std::move(fromPin),
                std::move(toNode), std::move(toPin), &error)) {
            impl->setStatus(error, true);
        }
    });
    _impl->canvas = new EditorUiFlowCanvas(
        *_impl->document, _impl->preview, _impl->graphRegistry,
        [impl = _impl.get()]() {
            impl->refreshPending = true;
        });
    _impl->canvasHost->addChild(_impl->canvas);
    _impl->visualViewport = new EditorUiFlowPreviewViewport();
    _impl->visualPreviewHost->addChild(_impl->visualViewport);
    _impl->preview.configureVisualHost(
        ui, *_impl->visualViewport, _impl->config.assetRoot);
    _impl->refresh();
    (void)_impl->restart(nullptr);
    return true;
}

void EditorUiFlowController::detach()
{
    if (_impl != nullptr) _impl->detach();
}

bool EditorUiFlowController::isAttached() const noexcept
{
    return _impl != nullptr && _impl->attached;
}

void EditorUiFlowController::tick(float deltaSeconds)
{
    if (!isAttached()) return;
    if (_impl->canvas != nullptr && _impl->canvasHost != nullptr) {
        const FVector2 size = _impl->canvasHost->getSize();
        _impl->canvas->setPosition({0.0f, 0.0f});
        _impl->canvas->setSize(size);
    }
    if (_impl->visualViewport != nullptr
        && _impl->visualPreviewHost != nullptr) {
        _impl->visualViewport->setPosition({0.0f, 0.0f});
        _impl->visualViewport->setSize(
            _impl->visualPreviewHost->getSize());
        _impl->preview.tick(deltaSeconds);
    }
    if (_impl->refreshPending) _impl->refresh();
}

bool EditorUiFlowController::restartPreview(std::string* error)
{
    return isAttached() && _impl->restart(error);
}

void EditorUiFlowController::setStateChanged(StateChanged changed)
{
    if (_impl != nullptr) _impl->stateChanged = std::move(changed);
}

const std::shared_ptr<EditorUiFlowDocument>&
EditorUiFlowController::document() const noexcept
{
    static const std::shared_ptr<EditorUiFlowDocument> empty;
    return _impl != nullptr ? _impl->document : empty;
}

EditorDescriptor makeEditorUiFlowDescriptor(EditorUiFlowExtensionConfig)
{
    EditorDescriptor descriptor;
    descriptor.id = kEditorUiFlowExtensionId;
    descriptor.displayName = L"UI Flow Editor";
    descriptor.iconPath = "icons/outline/route.svg";
    descriptor.surfaceKind = EditorSurfaceKind::Document;
    descriptor.openPolicy = EditorOpenPolicy::PerResource;
    descriptor.defaultDockSlot = EditorDockSlot::Center;
    descriptor.priority = 130;
    descriptor.extensions = {".uiflow.json"};
    descriptor.assetTypes = {"ui-flow"};
    descriptor.createDocument = [](const EditorOpenRequest& request,
        std::string& error) -> std::shared_ptr<IEditorDocument> {
        auto document = std::make_shared<EditorUiFlowDocument>();
        if (!document->initialize(
                request.resourcePath, request.displayPath, &error)) return nullptr;
        return document;
    };
    descriptor.createView = [](
        const std::shared_ptr<IEditorDocument>& document,
        IEditorHostServices&) -> std::unique_ptr<IEditorView> {
        if (std::dynamic_pointer_cast<EditorUiFlowDocument>(document) == nullptr) {
            return nullptr;
        }
        return std::make_unique<EditorUiFlowLaunchView>();
    };
    return descriptor;
}

bool registerEditorUiFlowExtension(
    EditorExtensionRegistry& registry,
    EditorUiFlowExtensionConfig config,
    std::string* error)
{
    return registry.registerEditor(
        makeEditorUiFlowDescriptor(std::move(config)), error);
}

} // namespace ayt::editor
