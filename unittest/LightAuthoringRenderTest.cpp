#include "WorkshopSessionGpuFixture.h"
#include <AYEntity/components/LightComponent.h>
#include <AYUI/Button.h>
#include <AYUI/CheckBox.h>
#include <AYUI/ComboBox.h>
#include <AYUI/ModalDialog.h>
#include "../src/EditorLightVisualization.h"
namespace
{
ui::Widget* find(ui::Widget* root, const char* id)
{
    if (!root)
        return nullptr;
    if (root->getId() == id)
        return root;
    for (auto* child : root->getChildren())
        if (auto* result = find(child, id))
            return result;
    return nullptr;
}
void click(Fixture& f, ui::Widget* widget)
{
    require(widget, "Actual light widget exists");
    f.session.ui().layout();
    const auto b = widget->getWorldBounds();
    const auto p = f.session.ui().logicalToPhysical({(b.minX + b.maxX) * .5f, (b.minY + b.maxY) * .5f});
    f.session.onMouseMove(p.x, p.y);
    require(f.session.onMouseButtonDown(p.x, p.y, 0), "Light UI receives actual pointer down");
    require(f.session.onMouseButtonUp(p.x, p.y, 0), "Light UI receives actual pointer up");
}
ui::Button* accept(ui::ModalDialog* dialog)
{
    for (auto* child : dialog->getChildren())
        if (auto* button = dynamic_cast<ui::Button*>(child);
            button && button->getText() == dialog->getAcceptText())
            return button;
    return nullptr;
}
void configure(Fixture& f, int kind, float intensity, bool enabled, bool shadows)
{
    click(f, f.session.ui().findById("btn_light_settings"));
    auto* dialog =
        dynamic_cast<ui::ModalDialog*>(find(f.session.ui().getOverlayRoot(), "light_settings_dialog"));
    require(dialog, "Light dialog opens from actual Inspector button");
    const auto text = [&](const char* id, const wchar_t* value) {
        auto* input = dynamic_cast<ui::TextInput*>(find(dialog, id));
        require(input, std::string("Light field ") + id);
        input->setText(value);
    };
    auto* type = dynamic_cast<ui::ComboBox*>(find(dialog, "light_type"));
    auto* active = dynamic_cast<ui::CheckBox*>(find(dialog, "light_enabled"));
    auto* shadow = dynamic_cast<ui::CheckBox*>(find(dialog, "light_shadows"));
    require(type && active && shadow, "Light dialog exposes type and flags");
    type->setSelectedIndex(kind);
    active->setChecked(enabled);
    shadow->setChecked(shadows);
    text("light_red", L"0.3");
    text("light_green", L"0.7");
    text("light_blue", L"1");
    text("light_range", L"18");
    text("light_inner", L"45");
    text("light_outer", L"65");
    const auto value = std::to_wstring(intensity);
    text("light_intensity", value.c_str());
    click(f, accept(dialog));
    require(!dialog->isOpen(), "Valid pending light fields commit through actual Apply");
}
} // namespace
int main(int argc, char** argv)
{
    try
    {
        Fixture f(argc > 1 ? argv[1] : "out/light-authoring-render",
                  argc > 2 && std::string(argv[2]) == "d3d12");
        f.useEditorCamera = true;
        f.useSceneLighting = true;
        f.renderer.setDebugCaptureEnabled(true);
        const auto original = f.capture("light-host-default.png");
        const auto scene = f.sceneBounds();
        require(f.renderer.debugSnapshot().lights.count == 2,
                "No authored lights retains the existing Host lighting");
        click(f, f.session.ui().findById("btn_outliner_create"));
        f.capture("light-create-menu.png");
        click(f, find(f.session.ui().getOverlayRoot(), "outliner_create_directional_light"));
        f.capture("light-created.png");
        auto& world = f.session.document()->scene().world();
        auto* object = world.findEntity(f.session.selectedEntityId());
        require(object && object->getComponent<entity::LightComponent>(),
                "Menu creates and selects a serialized LightComponent");
        const auto identity = object->persistentId();
        const auto helpers =
            editor::buildEditorLightVisuals(world, f.session.sceneCamera().frame(), scene, 0);
        require(helpers.icons.size() == 1, "Authored light exposes one actual visible helper icon");
        const auto pointer = [&](math::FVector2 p) {
            p = f.session.ui().logicalToPhysical(p);
            f.session.onMouseMove(p.x, p.y);
            f.session.onMouseButtonDown(p.x, p.y, 0);
            f.session.onMouseButtonUp(p.x, p.y, 0);
        };
        pointer({scene.minX + 20, scene.minY + 30});
        f.capture("light-helper-unselected.png");
        require(f.session.selectedEntityId() != object->getId(),
                "Actual empty Scene click clears the light selection");
        pointer(helpers.icons.front().center);
        const auto helperPixels = f.capture("light-helper-selected.png");
        require(f.session.selectedEntityId() == object->getId(),
                "Actual visible helper click selects the authored light");
        auto visibility = f.session.sceneVisibility();
        visibility.lightHelpers = false;
        f.session.setSceneVisibility(visibility, false);
        const auto hiddenPixels = f.capture("light-helpers-hidden.png");
        const auto center = helpers.icons.front().center;
        const math::FRectangle iconBounds{center.x - 12, center.y - 12, center.x + 12, center.y + 12};
        require(Fixture::difference(helperPixels, hiddenPixels, iconBounds, 4) > 20,
                "Actual helper glyph changes pixels in its own pick region");
        f.openMaterial();
        require(!f.sceneVisible(), "A real material Dock hides the Scene tab");
        const auto otherTabWithoutHelpers = f.capture("light-other-tab-reference.png");
        visibility.lightHelpers = true;
        f.session.setSceneVisibility(visibility, false);
        const auto otherTabWithHelpers = f.capture("light-other-tab-helpers-enabled.png");
        require(Fixture::difference(otherTabWithoutHelpers, otherTabWithHelpers, scene, 1) == 0,
                "Scene helper visibility does not paint over another actual authoring Dock");
        f.activateScene();
        f.frame();
        pointer(helpers.icons.front().center);
        f.capture("light-tab-return-selected.png");
        require(f.session.selectedEntityId() == object->getId(),
                "Returning to Scene retains actual light helper picking");
        visibility.lightHelpers = false;
        f.session.setSceneVisibility(visibility, false);
        // Compare illumination without selection overlays; helper visibility is separately proved above.
        click(f, f.session.ui().findById("btn_light_settings"));
        f.capture("light-settings-open.png");
        auto* initial =
            dynamic_cast<ui::ModalDialog*>(find(f.session.ui().getOverlayRoot(), "light_settings_dialog"));
        require(initial, "Capture real settings UI");
        initial->rejectDialog();
        f.capture("light-settings-cancelled.png");
        configure(f, 0, 2, true, false);
        const auto directional = f.capture("light-directional.png");
        require(Fixture::difference(original, directional, scene, 8) > 1000,
                "Authored directional lighting changes actual pixels");
        require(f.renderer.debugSnapshot().lights.count == 1 &&
                    f.renderer.debugSnapshot().lights.lights[0].type == render::LightType::Directional,
                "Actual packet supplies the authored directional light");
        configure(f, 1, 12, true, false);
        const auto point = f.capture("light-point.png");
        require(Fixture::difference(directional, point, scene, 8) > 1000,
                "Actual point attenuation changes Scene lighting");
        require(f.renderer.debugSnapshot().lights.lights[0].type == render::LightType::Point,
                "Actual packet changes to Point");
        configure(f, 2, 32, true, false);
        const auto spot = f.capture("light-spot.png");
        require(Fixture::difference(point, spot, scene, 8) > 500, "Actual spot cone changes Scene lighting");
        f.renderer.setShadowsEnabled(true);
        configure(f, 2, 32, true, true);
        const auto shadowed = f.capture("light-spot-shadow.png");
        const auto snapshot = f.renderer.debugSnapshot();
        require(snapshot.lights.count == 1 && snapshot.lights.lights[0].castShadow,
                "Authored shadow flag reaches the actual renderer");
        require(!snapshot.shadowViewProjections.empty(),
                "The authored Spot produces a real shadow projection");
        const auto shadowPixels = Fixture::difference(spot, shadowed, scene, 3);
        require(shadowPixels > 20, "Actual shadow projection changes Scene pixels");
        configure(f, 2, 32, false, true);
        const auto disabled = f.capture("light-all-disabled.png");
        require(f.renderer.debugSnapshot().lights.count == 0,
                "All disabled authored lights remain an explicit empty packet");
        require(f.renderer.debugSnapshot().shadowViewProjections.empty(),
                "Disabled lights do not leave an old shadow projection");
        require(Fixture::difference(disabled, shadowed, scene, 8) > 1000,
                "Disabling lights removes actual direct illumination");
        require(f.session.document()->undo(), "Undo the disable setting");
        const auto restored = f.capture("light-disable-undone.png");
        require(Fixture::difference(shadowed, restored, scene, 1) == 0,
                "Undo restores the same lit and shadowed Scene");
        f.session.onKeyDown(ui::UIKey_Control);
        require(f.session.onKeyDown(ui::UIKey_S), "Actual Ctrl+S saves the light Scene");
        f.session.onKeyUp(ui::UIKey_S);
        f.session.onKeyUp(ui::UIKey_Control);
        require(!f.session.document()->isDirty(), "Scene light save clears the document dirty state");
        std::string error;
        const auto path = f.session.document()->path();
        require(f.session.document()->open(path, &error), error);
        object = world.findEntityByPersistentId(identity);
        require(object && object->getComponent<entity::LightComponent>()->castShadow,
                "Light component and shadow flag persist on Scene reopen");
        const auto reopened = f.capture("light-reopened.png");
        require(Fixture::difference(restored, reopened, scene, 1) == 0,
                "Reopened Scene retains actual authored lighting");
        require(f.session.document()->deleteEntity(object->getId()),
                "Remove the authored light through document history");
        const auto noLight = f.capture("light-removed-host-restored.png");
        require(f.renderer.debugSnapshot().lights.count == 2,
                "Removing all authored components restores Host policy");
        require(Fixture::difference(original, noLight, scene, 1) == 0,
                "Removing lights restores the original actual Host rendering");
        require(f.session.document()->undo(), "Undo light deletion");
        const auto recovered = f.capture("light-delete-undone.png");
        require(Fixture::difference(reopened, recovered, scene, 1) == 0,
                "Undo deletion restores the saved authored lighting");
        std::cout << "PASS real create menu -> validated Inspector settings -> Directional/Point/Spot -> "
                     "actual spot shadow -> explicit all-disabled dark -> undo -> Ctrl+S -> Scene reopen -> "
                     "remove/Host fallback -> undo; shadow pixels="
                  << shadowPixels << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
