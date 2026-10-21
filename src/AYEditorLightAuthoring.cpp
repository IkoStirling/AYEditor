#include <AYEditor/EditorSession.h>
#include <AYEditor/EditorSceneDocument.h>
#include <AYEntity.h>
#include <AYEntity/components/LightComponent.h>
#include <AYScene.h>
#include <AYUI/Authoring/PropertyField.h>
#include <AYUI/Authoring/AuthoringPrimitives.h>
#include <AYUI/Box.h>
#include <AYUI/Button.h>
#include <AYUI/ComboBox.h>
#include <AYUI/CheckBox.h>
#include <AYUI/ModalDialog.h>
#include <AYUI/TextInput.h>
#include <AYUI/TextLabel.h>
#include <AYUI/UnicodeText.h>
#include <AYUI/IRenderBackend.h>
#include "EditorLightVisualization.h"
#include <array>
#include <charconv>

namespace ayt::editor
{
namespace
{
class LightOverlay final : public ui::Widget
{
public:
    std::function<EditorLightVisuals()> visuals;
    LightOverlay()
    {
        setId("scene_light_helpers");
        setDisplayListPolicy(ui::DisplayListPolicy::Immediate);
    }
    ui::Widget* hitTest(const math::FVector2&) override
    {
        return nullptr;
    }

protected:
    void onRender(ui::IRenderBackend& renderer) override
    {
        if (!visuals)
            return;
        const auto data = visuals();
        if (data.icons.empty() && data.selectedLines.empty())
            return;
        renderer.pushClip(data.bounds);
        const auto stroke = [&](const auto& lines, const math::FVector4& color, float width) {
            const auto path = renderer.createPath();
            if (path.id < 0)
                return;
            for (const auto& line : lines)
                renderer.addPathLine(path, line.from, line.to);
            renderer.setPathStrokeColor(path, color);
            renderer.setPathStrokeWidth(path, width);
            renderer.drawPath(path, ui::PathFillMode::Stroke);
            renderer.releasePath(path);
        };
        stroke(data.selectedLines, {.2f, .85f, 1, .65f}, 1);
        for (const auto& icon : data.icons)
        {
            std::vector<EditorLightLine> lines;
            const auto c = icon.center;
            const auto line = [&](float x, float y, float a, float b) {
                lines.push_back({{c.x + x, c.y + y}, {c.x + a, c.y + b}});
            };
            if (icon.type == int32_t(entity::SceneLightType::Spot))
            {
                line(-6, -6, 6, -6);
                line(6, -6, 0, 7);
                line(0, 7, -6, -6);
                line(-3, -2, 3, -2);
            }
            else
            {
                for (int i = 0; i < 12; ++i)
                {
                    const float a = i * 6.28318530718f / 12, b = (i + 1) * 6.28318530718f / 12;
                    line(4 * std::cos(a), 4 * std::sin(a), 4 * std::cos(b), 4 * std::sin(b));
                }
                const int rays = icon.type == int32_t(entity::SceneLightType::Directional) ? 8 : 4;
                for (int i = 0; i < rays; ++i)
                {
                    const float a = i * 6.28318530718f / rays;
                    line(6 * std::cos(a), 6 * std::sin(a), 8 * std::cos(a), 8 * std::sin(a));
                }
            }
            // Dark outline keeps helpers legible against both bright and dark materials.
            stroke(lines, {.03f, .04f, .05f, 1}, 4);
            stroke(lines,
                   icon.selected  ? math::FVector4{.2f, .9f, 1, 1}
                   : icon.enabled ? math::FVector4{1, .8f, .25f, 1}
                                  : math::FVector4{.55f, .57f, .6f, 1},
                   2);
        }
        renderer.popClip();
    }
};
bool sameLight(const entity::LightComponent& a, const entity::LightComponent& b)
{
    return a.type == b.type && a.enabled == b.enabled && a.color.x == b.color.x && a.color.y == b.color.y &&
           a.color.z == b.color.z && a.intensity == b.intensity && a.range == b.range &&
           a.innerAngleDegrees == b.innerAngleDegrees && a.outerAngleDegrees == b.outerAngleDegrees &&
           a.castShadow == b.castShadow && a.shadowBias == b.shadowBias && a.priority == b.priority;
}
void assignLight(entity::LightComponent& to, const entity::LightComponent& from)
{
    to.type = from.type;
    to.enabled = from.enabled;
    to.color = from.color;
    to.intensity = from.intensity;
    to.range = from.range;
    to.innerAngleDegrees = from.innerAngleDegrees;
    to.outerAngleDegrees = from.outerAngleDegrees;
    to.castShadow = from.castShadow;
    to.shadowBias = from.shadowBias;
    to.priority = from.priority;
}
} // namespace
uint32_t EditorSession::createLightEntity(entity::SceneLightType type)
{
    const auto value = int32_t(type);
    if (!_document || _gameView.mode() != EditorMode::Edit || _transformGizmo.active() || _ui.isDragging() ||
        value < 0 || value > 2)
        return 0;
    auto& world = _document->scene().world();
    std::string name = type == entity::SceneLightType::Directional ? "Directional Light"
                       : type == entity::SceneLightType::Point     ? "Point Light"
                                                                   : "Spot Light";
    const auto base = name;
    for (int suffix = 2; world.findEntity(name.c_str()); ++suffix)
        name = base + " " + std::to_string(suffix);
    uint32_t id = 0;
    if (!_document->createEntity(
            "Create Light",
            [&](entity::Entity& object) {
                object.setName(name.c_str());
                object.transform()->setPosition(0, 3, 0);
                object.transform()->rotation =
                    math::FQuaternion::lookRotation({.35f, -.85f, -.4f}, {0, 1, 0});
                auto* light = object.createComponent<entity::LightComponent>();
                if (!light)
                    return false;
                light->type = value;
                light->intensity = type == entity::SceneLightType::Directional ? 1.f : 5.f;
                return true;
            },
            &id, entity::EntityKind::Spatial))
        return 0;
    setSelectedEntity(&world, world.findEntity(id));
    _outlinerRefreshPending = true;
    _inspectorRefreshPending = true;
    refreshLightAuthoring();
    refreshUnsavedIndicator();
    if (_repaintCallback)
        _repaintCallback();
    return id;
}
void EditorSession::bindLightAuthoring()
{
    auto* overlayRoot = _ui.getOverlayRoot();
    bool hasOverlay = false;
    if (overlayRoot)
        for (auto* child : overlayRoot->getChildren())
            if (child->getId() == "scene_light_helpers")
                hasOverlay = true;
    if (overlayRoot && !hasOverlay)
    {
        auto* overlay = new LightOverlay();
        overlay->setLayoutSizeManaged(false);
        // Native composition hides panel_viewport while flushing UI. Its
        // helpers live in the overlay root, clipped to the active Scene bounds.
        ui::AnchorLayout anchors;
        anchors.anchorMax = {1, 1};
        overlay->setAnchorLayout(anchors);
        overlayRoot->addChild(overlay);
        overlay->visuals = [this] {
            if (!_document || _gameView.mode() != EditorMode::Edit || _sceneCamera.isTwoD() ||
                !_sceneVisibility.lightHelpers)
                return EditorLightVisuals{};
            auto* viewport = _ui.findById("panel_viewport");
            if (!viewport)
                return EditorLightVisuals{};
            // The composite hole hides only this panel for one populate/flush
            // pair. A hidden Scene tab or ancestor must still suppress helpers.
            const bool visible =
                viewport == _panelViewportForFrame ? _panelViewportWasVisibleForFrame : viewport->isVisible();
            if (!visible)
                return EditorLightVisuals{};
            for (auto* ancestor = viewport->getParent(); ancestor; ancestor = ancestor->getParent())
                if (!ancestor->isVisible())
                    return EditorLightVisuals{};
            const auto bounds = viewport->getWorldBounds();
            return buildEditorLightVisuals(_document->scene().world(), _sceneCamera.frame(), bounds,
                                           selectedEntityId());
        };
    }
    if (auto* button = dynamic_cast<ui::Button*>(_ui.findById("btn_light_settings")))
        button->setOnClicked([this] { (void)openSelectedLightSettings(); });
}
void EditorSession::refreshLightAuthoring()
{
    auto* button = dynamic_cast<ui::Button*>(_ui.findById("btn_light_settings"));
    if (!button)
        return;
    auto* world = hierarchyWorldMutable();
    auto* selected = _selection.resolve(world);
    const bool hasLight = selected && selected->getComponent<entity::LightComponent>();
    button->setVisible(hasLight);
    button->setEnabled(hasLight && _gameView.mode() == EditorMode::Edit);
}
bool EditorSession::openSelectedLightSettings()
{
    if (!_document || _gameView.mode() != EditorMode::Edit || _transformGizmo.active() || _ui.isDragging())
        return false;
    auto& world = _document->scene().world();
    auto* selected = _selection.resolve(&world);
    auto* light = selected ? selected->getComponent<entity::LightComponent>() : nullptr;
    if (!light)
        return false;
    auto* manager = ui::UIManager::tryGet();
    if (!manager)
        manager = &_ui;
    if (!manager->root())
        return false;
    const auto identity = selected->persistentId(), componentId = selected->componentInstance(light)->id;
    const auto revision = _document->revision();
    const auto* document = _document.get();
    _lightSettingsDialog = std::make_unique<ui::ModalDialog>();
    auto* dialog = _lightSettingsDialog.get();
    dialog->setId("light_settings_dialog");
    dialog->setSize({650, 605});
    dialog->setAcceptText(localizedText("ui.editor.light.apply", "Apply Light"));
    dialog->setRejectText(localizedText("ui.editor.import.cancel", "Cancel"));
    auto* body = new ui::VBox();
    body->setSpacing(4);
    body->setSize({618, 533});
    auto* kind = new ui::ComboBox();
    kind->setId("light_type");
    kind->setItems({localizedText("ui.editor.light.directional", "Directional"),
                    localizedText("ui.editor.light.point", "Point"),
                    localizedText("ui.editor.light.spot", "Spot")});
    kind->setSelectedIndex(light->type);
    body->addWidget(kind, 30);
    auto* enabled = new ui::CheckBox();
    enabled->setId("light_enabled");
    enabled->setText(localizedText("ui.editor.light.enabled", "Enabled"));
    enabled->setChecked(light->enabled);
    body->addWidget(enabled, 26);
    const char* ids[] = {"red", "green", "blue", "intensity", "range", "inner", "outer", "bias", "priority"};
    const char* keys[] = {"red", "green", "blue", "intensity", "range", "inner", "outer", "bias", "priority"};
    const char* labels[] = {
        "Linear red (0..64)",         "Linear green (0..64)",  "Linear blue (0..64)",
        "Intensity (0..100000)",      "Range (meters)",        "Inner half angle (degrees)",
        "Outer half angle (degrees)", "Shadow bias (0..0.05)", "Priority (higher first)"};
    const float values[] = {light->color.x,           light->color.y,    light->color.z,
                            light->intensity,         light->range,      light->innerAngleDegrees,
                            light->outerAngleDegrees, light->shadowBias, float(light->priority)};
    std::array<ui::authoring::PropertyField*, 9> fields{};
    for (std::size_t i = 0; i < fields.size(); ++i)
    {
        ui::authoring::PropertyFieldOptions options;
        options.inputId = std::string("light_") + ids[i];
        options.labelWidth = 280;
        auto* field = new ui::authoring::PropertyField(options);
        fields[i] = field;
        field->setTextValue(localizedText((std::string("ui.editor.light.") + keys[i]).c_str(), labels[i]),
                            i == 8 ? ui::decodeUtf8Text(std::to_string(light->priority))
                                   : ui::authoring::formatNumber(values[i], 9));
        body->addWidget(field, 30);
    }
    auto* shadows = new ui::CheckBox();
    shadows->setId("light_shadows");
    shadows->setText(localizedText("ui.editor.light.shadows", "Cast shadows (Directional / Spot only)"));
    shadows->setChecked(light->castShadow);
    body->addWidget(shadows, 26);
    auto* hint = new ui::TextLabel();
    hint->setWordWrap(true);
    hint->setText(localizedText("ui.editor.light.hint",
                                "Move and rotate the light in the Scene. Local +Z emits light; range ignores "
                                "scale. Up to 8 lights contribute, with higher priority selected first."));
    body->addWidget(hint, 52);
    auto* error = new ui::TextLabel();
    error->setId("light_error");
    error->setWordWrap(true);
    error->setTextColor({1, .4f, .3f, 1});
    body->addWidget(error, 48);
    dialog->setBodyContentOwned(body);
    dialog->setAcceptValidator([this, document, identity, componentId, revision, kind, enabled, fields,
                                shadows, error] {
        const auto fail = [&](const std::wstring& reason) {
            error->setText(reason);
            return false;
        };
        if (_document.get() != document || _document->revision() != revision)
            return fail(localizedText("ui.editor.light.conflict", "Scene changed; reopen light settings."));
        auto* object = _document->scene().world().findEntityByPersistentId(identity);
        auto* existing =
            object ? dynamic_cast<entity::LightComponent*>(object->findComponentById(componentId)) : nullptr;
        if (!existing)
            return fail(localizedText("ui.editor.light.conflict", "Scene changed; reopen light settings."));
        std::array<float, 8> numbers{};
        for (std::size_t i = 0; i < numbers.size(); ++i)
        {
            const auto value = ui::authoring::parseFiniteFloat(fields[i]->value());
            if (!value)
                return fail(localizedText("ui.editor.light.invalid",
                                          "Use finite values in the displayed ranges; inner angle must not "
                                          "exceed outer. Point shadows are unsupported."));
            numbers[i] = *value;
        }
        const auto priorityText = ui::encodeUtf8Text(fields[8]->value());
        int32_t priority = 0;
        const auto parsed =
            std::from_chars(priorityText.data(), priorityText.data() + priorityText.size(), priority);
        if (parsed.ec != std::errc{} || parsed.ptr != priorityText.data() + priorityText.size())
            return fail(localizedText("ui.editor.light.priority_invalid",
                                      "Priority must be a signed 32-bit integer."));
        entity::LightComponent candidate;
        candidate.type = kind->getSelectedIndex();
        candidate.enabled = enabled->isChecked();
        candidate.color = {numbers[0], numbers[1], numbers[2]};
        candidate.intensity = numbers[3];
        candidate.range = numbers[4];
        candidate.innerAngleDegrees = numbers[5];
        candidate.outerAngleDegrees = numbers[6];
        candidate.shadowBias = numbers[7];
        candidate.castShadow = shadows->isChecked();
        candidate.priority = priority;
        if (!candidate.valid())
            return fail(localizedText("ui.editor.light.invalid",
                                      "Use finite values in the displayed ranges; inner angle must not "
                                      "exceed outer. Point shadows are unsupported."));
        if (sameLight(*existing, candidate))
            return true;
        // The document serializes before/after and validates the complete component.
        const bool changed = _document->mutateComponentById(
            object->getId(), componentId, "Edit Light", {}, [&](entity::IComponent& base) {
                auto* target = dynamic_cast<entity::LightComponent*>(&base);
                if (!target)
                    return false;
                assignLight(*target, candidate);
                return true;
            });
        if (!changed)
            return fail(localizedText("ui.editor.light.apply_failed",
                                      "Cannot apply light settings; Scene state was retained."));
        _inspectorRefreshPending = true;
        refreshUnsavedIndicator();
        if (_repaintCallback)
            _repaintCallback();
        return true;
    });
    dialog->fitAndCenterInViewport(manager->root()->getSize(), 12);
    dialog->openModal();
    if (_repaintCallback)
        _repaintCallback();
    return true;
}
} // namespace ayt::editor
