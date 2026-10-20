#include "WorkshopSessionGpuFixture.h"
int main(int argc, char **argv)
{
    try
    {
        Fixture f(argc > 1 ? argv[1] : "out/material-workshop-session",
                  argc > 2 && std::string(argv[2]) == "d3d12");
        const auto original = f.capture("session-scene-open.png");
        const auto bounds = f.sceneBounds();
        size_t visible = 0;
        for (int y = int(bounds.minY) + 6; y < int(bounds.maxY) - 6; ++y)
            for (int x = int(bounds.minX) + 6; x < int(bounds.maxX) - 6; ++x)
            {
                const auto p = (y * Fixture::width + x) * 4;
                if (original.rgba8[p] > 80 || original.rgba8[p + 1] > 100 || original.rgba8[p + 2] > 100)
                    ++visible;
            }
        require(visible > 5000, "Actual EditorSession Scene initially renders visible geometry");
        f.openMaterial();
        const auto preview = f.capture("session-material-open.png");
        auto *field = dynamic_cast<ui::TextInput *>(f.session.ui().findById("material_r"));
        require(field != nullptr, "Actual Dock contains the material color field");
        const auto revision = f.material->revision();
        f.session.ui().setFocus(field);
        field->setText(L"0.1");
        f.session.onKeyDown(ui::UIKey_Enter);
        require(f.material->revision() > revision && f.material->isDirty(),
                "Enter commits actual material field");
        require(f.material->settings().baseColor[0] == .1f, "Actual field reaches the document");
        const auto editedPreview = f.capture("session-material-unsaved.png");
        require(Fixture::difference(preview, editedPreview,
                                    {bounds.minX, bounds.minY, bounds.maxX - 450, bounds.maxY}, 8) > 1000,
                "Actual EditorSession preview changes after Enter");
        f.activateScene();
        const auto unsaved = f.capture("session-scene-unsaved.png");
        require(Fixture::difference(original, unsaved, bounds, 1) == 0,
                "Switching back to Scene retains the cached material after an unsaved Dock edit");
        f.openMaterial();
        auto *pending = dynamic_cast<ui::TextInput *>(f.session.ui().findById("material_g"));
        require(pending != nullptr, "Actual Dock contains the pending color field");
        f.session.ui().setFocus(pending);
        pending->setText(L"0.4");
        f.session.onKeyDown(ui::UIKey_Control);
        f.session.onKeyDown(ui::UIKey_S);
        f.session.onKeyUp(ui::UIKey_S);
        f.session.onKeyUp(ui::UIKey_Control);
        require(!f.material->isDirty() && f.material->settings().baseColor[1] == .4f,
                "Actual Ctrl+S commits pending fields and saves the material");
        f.activateScene();
        const auto materialPath = resource::resolveAssetPath({}, utf8(f.assets / "materials/ceramic.mat"));
        auto &cached = f.materials.at(materialPath);
        f.renderer.destroyMaterial(cached);
        f.materials.erase(materialPath);
        resource::ResourceManager::instance().reloadResource(materialPath);
        const auto saved = f.capture("session-scene-saved-reloaded.png");
        const auto changes = Fixture::difference(original, saved, bounds, 8);
        require(changes > 500, "Actual Session Scene changes after saved material reload");

        // Exercise the actual New Material action as well as the pre-authored
        // sample. Its companion shader must work without any source textures.
        require(f.session.createProjectAsset(editor::EditorAssetType::Material),
                "Actual Session creates and opens a standalone PBR material");
        const auto *created = f.session.workspace().documents().active();
        require(created != nullptr, "New material becomes the active document");
        auto authored = std::dynamic_pointer_cast<editor::EditorMaterialDocument>(created->document);
        require(authored && fs::u8path(authored->path()).filename() == "NewMaterial.mat",
                "Actual new action opens its created native material");
        require(fs::is_regular_file(
                    fs::u8path(resource::resolveAssetPath(authored->path(), authored->shaderReference()))),
                "New material has its own existing shader companion");
        require(authored->textureReferences().empty(),
                "New surface starts without external texture dependencies");
        const auto freshPreview = f.capture("session-new-material.png");
        require(Fixture::difference(editedPreview, freshPreview,
                                    {bounds.minX, bounds.minY, bounds.maxX - 450, bounds.maxY}, 8) > 1000,
                "New texture-free material renders its actual studio preview");
        const auto *record = f.session.assetDatabase().findByLogicalPath("Assets/materials/NewMaterial.mat");
        require(record != nullptr, "New native material is revealed in the asset database");
        auto *dock = dynamic_cast<ui::DockArea *>(f.session.ui().findById("main_dock"));
        require(dock != nullptr, "Actual shell owns the main DockArea");
        // DockCard's stable docking id is separate from Widget's lookup id.
        auto *card = dock->findCard("card_material_" + std::to_string(record->id));
        require(card != nullptr, "New material has its own Dock card");
        const auto findField = [](auto &&self, ui::Widget *root, const std::string &id) -> ui::TextInput * {
            if (root->getId() == id)
                return dynamic_cast<ui::TextInput *>(root);
            for (auto *child : root->getChildren())
                if (auto *found = self(self, child, id))
                    return found;
            return nullptr;
        };
        auto *newField = findField(findField, card, "material_g");
        require(newField != nullptr,
                "Resolve a field inside the new card, independently of hidden old cards");
        f.session.ui().setFocus(newField);
        newField->setText(L"0.2");
        f.session.onKeyDown(ui::UIKey_Enter);
        require(authored->isDirty() && authored->settings().baseColor[1] == .2f,
                "New card Enter edits its own material document");
        require(f.material->settings().baseColor[1] == .4f && !f.material->isDirty(),
                "Editing the new card leaves the previously saved material unchanged");
        const auto authoredPreview = f.capture("session-new-material-edited.png");
        require(Fixture::difference(freshPreview, authoredPreview,
                                    {bounds.minX, bounds.minY, bounds.maxX - 450, bounds.maxY}, 8) > 1000,
                "New material preview responds to its actual field");
        auto *roughness = findField(findField, card, "material_roughness");
        require(roughness != nullptr, "Resolve the new material's pending roughness field");
        f.session.ui().setFocus(roughness);
        roughness->setText(L"0.6");
        f.session.onKeyDown(ui::UIKey_Control);
        f.session.onKeyDown(ui::UIKey_S);
        f.session.onKeyUp(ui::UIKey_S);
        f.session.onKeyUp(ui::UIKey_Control);
        require(!authored->isDirty() && authored->settings().roughness == .6f,
                "Ctrl+S commits and saves the new active material's pending field");
        editor::EditorMaterialDocument reopened;
        std::string error;
        require(reopened.initialize({authored->path()}, error), error);
        require(reopened.settings() == authored->settings(), "New material's actual UI edits survive reopen");
        f.activateScene();
        const auto afterCreation = f.capture("session-scene-after-material-creation.png");
        require(Fixture::difference(saved, afterCreation, bounds, 1) == 0,
                "Creating/editing a separate material preserves the saved Scene's GPU output");
        std::cout
            << "PASS actual EditorSession: startup Scene, asset/Dock routing, Enter edit, preview change, "
               "tab-return Scene isolation, Ctrl+S pending fields, new standalone shader material, saved "
               "Scene changes="
            << changes << '\n';
        return 0;
    }
    catch (const std::exception &ex)
    {
        std::cerr << "FAIL: " << ex.what() << '\n';
        return 1;
    }
}
