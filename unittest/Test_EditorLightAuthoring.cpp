#include <AYTest.h>
#include "fixtures/EditorShellLayout.h"
#include <AYEditor/EditorSession.h>
#include <AYEditor/EditorSceneDocument.h>
#include <AYApplication/IEngineHost.h>
#include <AYEntity.h>
#include <AYEntity/components/LightComponent.h>
#include <AYEntity/SceneLighting.h>
#include <AYScene.h>
#include <AYRenderer/RenderScene.h>
#include <AYUI/MockRenderer.h>
#include <AYUI/Button.h>
#include <AYUI/CheckBox.h>
#include <AYUI/ComboBox.h>
#include <AYUI/ModalDialog.h>
#include <AYUI/TextInput.h>
#include <AYUI/TextLabel.h>
#include <AYUI/DockCard.h>
#include <AYUI/DockTabGroup.h>
#include "../src/EditorLightVisualization.h"
#include <fstream>
#include <nlohmann/json.hpp>
using namespace ayt;
using namespace ayt::editor;
namespace
{
ui::Widget* find(ui::Widget* root, const char* id)
{
    if (!root)
        return nullptr;
    if (root->getId() == id)
        return root;
    for (auto* child : root->getChildren())
        if (auto* found = find(child, id))
            return found;
    return nullptr;
}
bool click(EditorSession& session, ui::Widget* widget)
{
    if (!widget)
        return false;
    session.ui().layout();
    const auto b = widget->getWorldBounds();
    const auto p = session.ui().logicalToPhysical({(b.minX + b.maxX) * .5f, (b.minY + b.maxY) * .5f});
    session.onMouseMove(p.x, p.y);
    const bool down = session.onMouseButtonDown(p.x, p.y, 0), up = session.onMouseButtonUp(p.x, p.y, 0);
    return down && up;
}
ui::Button* accept(ui::ModalDialog* dialog)
{
    for (auto* child : dialog->getChildren())
        if (auto* button = dynamic_cast<ui::Button*>(child);
            button && button->getText() == dialog->getAcceptText())
            return button;
    return nullptr;
}
} // namespace
TEST_SUITE(AYEditor_LightAuthoring)

TEST_CASE(light_overlay_tracks_scene_tab_visibility_and_preserves_the_native_composite_hole)
{
    app::EngineHostScope scope(app::defaultEngineHost());
    ui::MockRenderer backend;
    EditorSession session;
    EditorSessionDesc desc;
    desc.uiBackend = &backend;
    desc.layoutPath = testfixtures::resolveEditorShellLayoutPath();
    CHECK(session.initialize(desc));
    session.setClientSize(1500, 1000);
    const auto id = session.createLightEntity(entity::SceneLightType::Point);
    auto* light = session.document()->scene().world().findEntity(id);
    CHECK(light != nullptr);
    if (!light)
    {
        session.shutdown();
        return;
    }
    light->transform()->position = {0, 0, 0};
    session.update(.1f);
    session.ui().layout();
    auto* overlay = find(session.ui().getOverlayRoot(), "scene_light_helpers");
    auto* card = dynamic_cast<ui::DockCard*>(session.ui().findById("card_viewport"));
    auto* tabs = card ? dynamic_cast<ui::DockTabGroup*>(card->getParent()) : nullptr;
    CHECK(overlay && card && tabs);
    if (!overlay || !card || !tabs)
    {
        session.shutdown();
        return;
    }
    const auto overlayDraws = [&] {
        backend.beginFrame();
        overlay->render(backend);
        return backend.getDrawCalls().size();
    };
    CHECK(overlayDraws() > 0u);
    auto* other = new ui::DockCard();
    other->setId("light_visibility_other_tool");
    other->setTitle(L"Another authoring view");
    tabs->addTab(other);
    tabs->activateTabById(other->getId());
    session.ui().invalidateLayout();
    session.ui().layout();
    CHECK_FALSE(card->isVisible());
    CHECK(overlayDraws() == 0u);
    tabs->activateTabById("card_viewport");
    session.ui().invalidateLayout();
    session.ui().layout();
    CHECK(card->isVisible());
    CHECK(overlayDraws() > 0u);
    session.populateFrame(true);
    auto* panel = session.ui().findById("panel_viewport");
    CHECK(panel && !panel->isVisible());
    CHECK(overlayDraws() > 0u);
    session.flushFrame();
    CHECK(panel && panel->isVisible());
    session.shutdown();
}
TEST_CASE(light_helpers_use_fixed_screen_icons_and_scale_free_clipped_range_wires)
{
    app::EngineHostScope scope(app::defaultEngineHost());
    ui::MockRenderer backend;
    EditorSession session;
    EditorSessionDesc desc;
    desc.uiBackend = &backend;
    desc.layoutPath = testfixtures::resolveEditorShellLayoutPath();
    CHECK(session.initialize(desc));
    auto& world = session.document()->scene().world();
    auto* object = world.createSpatialEntity();
    auto* light = object->createComponent<entity::LightComponent>();
    light->type = 1;
    light->range = .2f;
    object->transform()->position = {0, 0, .5f};
    EditorSceneCameraFrame frame;
    const math::FRectangle bounds{0, 0, 800, 600};
    auto visuals = buildEditorLightVisuals(world, frame, bounds, object->getId());
    CHECK(visuals.icons.size() == 1);
    CHECK(visuals.selectedLines.size() == 96);
    CHECK(pickEditorLightVisual(visuals, {407, 307}) == object);
    CHECK(pickEditorLightVisual(visuals, {409, 300}) == nullptr);
    object->transform()->scale = {90, 2, 11};
    auto scaled = buildEditorLightVisuals(world, frame, bounds, object->getId());
    CHECK(scaled.selectedLines.size() == visuals.selectedLines.size());
    for (size_t i = 0; i < scaled.selectedLines.size(); ++i)
    {
        CHECK_FLOAT_EQ(scaled.selectedLines[i].from.x, visuals.selectedLines[i].from.x, 1e-5f);
        CHECK_FLOAT_EQ(scaled.selectedLines[i].from.y, visuals.selectedLines[i].from.y, 1e-5f);
    }
    light->enabled = false;
    scaled = buildEditorLightVisuals(world, frame, bounds, object->getId());
    CHECK(scaled.icons.size() == 1 && !scaled.icons.front().enabled);
    light->type = 2;
    light->range = 5;
    auto clipped = buildEditorLightVisuals(world, frame, bounds, object->getId());
    for (const auto& line : clipped.selectedLines)
    {
        CHECK(std::isfinite(line.from.x) && std::isfinite(line.to.y));
        CHECK(line.from.x >= -.01f && line.from.x <= 800.01f);
        CHECK(line.to.y >= -.01f && line.to.y <= 600.01f);
    }
    object->transform()->position = {0, 0, -.5f};
    CHECK(buildEditorLightVisuals(world, frame, bounds, 0).icons.empty());
    object->transform()->position = {2, 0, .5f};
    CHECK(buildEditorLightVisuals(world, frame, bounds, 0).icons.empty());
    session.shutdown();
}
TEST_CASE(light_helper_visibility_persists_with_legacy_default_and_does_not_change_scene_history)
{
    const auto root = test::testTmpDir() / "light-helper-workspace";
    const auto scene = (root / "Assets/main.scn").string();
    EditorSceneViewWorkspace workspace;
    std::string error;
    CHECK(workspace.open(root.string(), &error));
    workspace.set(scene, {});
    EditorSceneVisibility visibility;
    visibility.lightHelpers = false;
    workspace.setVisibility(scene, visibility);
    CHECK(workspace.save(&error));
    EditorSceneViewWorkspace loaded;
    CHECK(loaded.open(root.string(), &error));
    auto* saved = loaded.findVisibility(scene);
    CHECK(saved && !saved->lightHelpers);
    nlohmann::json json;
    {
        std::ifstream input(workspace.path());
        input >> json;
    }
    for (auto& row : json["scenes"])
        row["visibility"].erase("lightHelpers");
    {
        std::ofstream output(workspace.path());
        output << json.dump(2);
    }
    CHECK(loaded.open(root.string(), &error));
    saved = loaded.findVisibility(scene);
    CHECK(saved && saved->lightHelpers);
}
TEST_CASE(actual_visible_light_icon_selects_and_hidden_helper_does_not_steal_scene_clicks)
{
    app::EngineHostScope scope(app::defaultEngineHost());
    ui::MockRenderer backend;
    EditorSession session;
    EditorSessionDesc desc;
    desc.uiBackend = &backend;
    desc.layoutPath = testfixtures::resolveEditorShellLayoutPath();
    CHECK(session.initialize(desc));
    session.setClientSize(1500, 1000);
    session.ui().layout();
    const auto id = session.createLightEntity(entity::SceneLightType::Point);
    auto& world = session.document()->scene().world();
    auto* object = world.findEntity(id);
    object->transform()->position = {0, 0, 0};
    session.update(.1f);
    math::FRectangle bounds;
    CHECK(session.getViewportBounds(bounds));
    auto visuals = buildEditorLightVisuals(world, session.sceneCamera().frame(), bounds, 0);
    CHECK(visuals.icons.size() == 1);
    if (visuals.icons.empty())
    {
        session.shutdown();
        return;
    }
    const auto center = visuals.icons.front().center;
    const auto pointer = [&](math::FVector2 p) {
        p = session.ui().logicalToPhysical(p);
        session.onMouseMove(p.x, p.y);
        session.onMouseButtonDown(p.x, p.y, 0);
        session.onMouseButtonUp(p.x, p.y, 0);
        session.update(.1f);
    };
    pointer({bounds.minX + 20, bounds.minY + 30});
    CHECK(session.selectedEntityId() != id);
    pointer(center);
    CHECK(session.selectedEntityId() == id);
    pointer({bounds.minX + 20, bounds.minY + 30});
    auto* backdrop = world.createSpatialEntity();
    backdrop->transform()->position = {0, 0, 0};
    const auto revision = session.document()->revision();
    auto visibility = session.sceneVisibility();
    visibility.lightHelpers = false;
    session.setSceneVisibility(visibility, false);
    CHECK(session.document()->revision() == revision);
    pointer(center);
    CHECK(session.selectedEntityId() == backdrop->getId());
    CHECK(find(session.ui().getOverlayRoot(), "scene_light_helpers"));
    session.shutdown();
}
TEST_CASE(actual_create_menu_and_settings_validate_pending_fields_with_one_undo_and_scene_roundtrip)
{
    app::EngineHostScope scope(app::defaultEngineHost());
    ui::MockRenderer backend;
    EditorSession session;
    EditorSessionDesc desc;
    desc.uiBackend = &backend;
    desc.layoutPath = testfixtures::resolveEditorShellLayoutPath();
    CHECK(session.initialize(desc));
    session.setClientSize(1500, 1000);
    auto& world = session.document()->scene().world();
    const auto before = world.getAllEntities().size();
    CHECK(click(session, session.ui().findById("btn_outliner_create")));
    auto* create = find(session.ui().getOverlayRoot(), "outliner_create_point_light");
    CHECK(create);
    CHECK(click(session, create));
    session.update(.25f);
    session.ui().layout();
    auto* object = world.findEntity(session.selectedEntityId());
    CHECK(object);
    auto* light = object ? object->getComponent<entity::LightComponent>() : nullptr;
    CHECK(light);
    if (!light)
    {
        session.shutdown();
        return;
    }
    CHECK(light->type == 1);
    CHECK(world.getAllEntities().size() == before + 1);
    const auto identity = object->persistentId();
    CHECK(click(session, session.ui().findById("btn_light_settings")));
    auto* dialog =
        dynamic_cast<ui::ModalDialog*>(find(session.ui().getOverlayRoot(), "light_settings_dialog"));
    CHECK(dialog);
    if (!dialog)
    {
        session.shutdown();
        return;
    }
    auto* range = dynamic_cast<ui::TextInput*>(find(dialog, "light_range"));
    auto* priority = dynamic_cast<ui::TextInput*>(find(dialog, "light_priority"));
    CHECK(range && priority);
    if (!range || !priority)
    {
        session.shutdown();
        return;
    }
    const auto revision = session.document()->revision();
    range->setText(L"nan");
    dialog->acceptDialog();
    CHECK(dialog->isOpen());
    CHECK(light->range == 10);
    CHECK(session.document()->revision() == revision);
    range->setText(L"17");
    priority->setText(L"2147483648");
    dialog->acceptDialog();
    CHECK(dialog->isOpen());
    CHECK(light->range == 10);
    priority->setText(L"2147483647");
    CHECK(click(session, accept(dialog)));
    auto* message = dynamic_cast<ui::TextLabel*>(find(dialog, "light_error"));
    const auto diagnostic =
        message ? ui::encodeUtf8Text(message->getText()) : std::string("No light error label");
    CHECK_MSG(!dialog->isOpen(), diagnostic.c_str());
    if (dialog->isOpen())
    {
        session.shutdown();
        return;
    }
    CHECK(light->range == 17 && light->priority == 2147483647);
    CHECK(session.document()->undo());
    CHECK(light->range == 10 && light->priority == 0);
    CHECK(session.document()->undo());
    CHECK(world.getAllEntities().size() == before);
    CHECK(session.document()->redo());
    CHECK(session.document()->redo());
    object = world.findEntityByPersistentId(identity);
    CHECK(object);
    if (!object)
    {
        session.shutdown();
        return;
    }
    light = object->getComponent<entity::LightComponent>();
    CHECK(light && light->range == 17);
    std::string error;
    const auto path = test::testTmpDir() / "light-authoring.scn";
    CHECK_MSG(session.document()->saveAs(path.string(), &error), error.c_str());
    CHECK_MSG(session.document()->open(path.string(), &error), error.c_str());
    object = world.findEntityByPersistentId(identity);
    CHECK(object);
    if (object)
    {
        light = object->getComponent<entity::LightComponent>();
        CHECK(light && light->range == 17 && light->priority == 2147483647);
    }
    session.shutdown();
}
TEST_CASE(light_settings_cancel_conflict_point_shadow_and_generic_invalid_mutation_preserve_scene)
{
    app::EngineHostScope scope(app::defaultEngineHost());
    ui::MockRenderer backend;
    EditorSession session;
    EditorSessionDesc desc;
    desc.uiBackend = &backend;
    desc.layoutPath = testfixtures::resolveEditorShellLayoutPath();
    CHECK(session.initialize(desc));
    session.setClientSize(1500, 1000);
    const auto id = session.createLightEntity(entity::SceneLightType::Point);
    CHECK(id);
    session.update(0);
    auto* object = session.document()->scene().world().findEntity(id);
    CHECK(object);
    if (!object)
    {
        session.shutdown();
        return;
    }
    auto* light = object->getComponent<entity::LightComponent>();
    const auto componentId = object->componentInstance(light)->id;
    const auto revision = session.document()->revision();
    CHECK_FALSE(session.document()->mutateComponentById(
        id, componentId, "Invalid Light", {}, [](entity::IComponent& base) {
            static_cast<entity::LightComponent&>(base).range = -1;
            return true;
        }));
    CHECK(light->range == 10);
    CHECK(session.document()->revision() == revision);
    CHECK(session.openSelectedLightSettings());
    auto* dialog =
        dynamic_cast<ui::ModalDialog*>(find(session.ui().getOverlayRoot(), "light_settings_dialog"));
    CHECK(dialog);
    if (!dialog)
    {
        session.shutdown();
        return;
    }
    auto* shadows = dynamic_cast<ui::CheckBox*>(find(dialog, "light_shadows"));
    CHECK(shadows);
    if (shadows)
        shadows->setChecked(true);
    dialog->acceptDialog();
    CHECK(dialog->isOpen());
    CHECK_FALSE(light->castShadow);
    dialog->rejectDialog();
    CHECK_FALSE(dialog->isOpen());
    CHECK(session.document()->revision() == revision);
    session.update(.25f);
    CHECK(session.openSelectedLightSettings());
    dialog = dynamic_cast<ui::ModalDialog*>(find(session.ui().getOverlayRoot(), "light_settings_dialog"));
    CHECK(session.document()->renameEntity(id, "Renamed Light"));
    const auto changed = session.document()->revision();
    dialog->acceptDialog();
    CHECK(dialog->isOpen());
    CHECK(session.document()->revision() == changed);
    dialog->rejectDialog();
    session.shutdown();
}
TEST_SUITE_END
