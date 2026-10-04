#ifdef AYEDITOR_HAS_AVATAR
#include "../../AYAvatar/demo/Fixture.h"
#include "../src/AYEditorAvatarCanvas.h"
#include "fixtures/EditorShellLayout.h"
#include <AYEditor/EditorAssetDatabase.h>
#include <AYEditor/EditorAvatarDocument.h>
#include <AYEditor/EditorBuiltInExtensions.h>
#include <AYEditor/EditorRecoveryStore.h>
#include <AYEditor/EditorSession.h>
#include <AYEditor/EditorWorkspace.h>
#include <AYTest.h>
#include <AYUI/DeviceInputBridge.h>
#include <AYUI/MockRenderer.h>
#include <AYUI/TextInput.h>
#include <AYUI/UIKeyCode.h>
#include <AYUI/UIManager.h>
#include <chrono>
#include <fstream>
#include <limits>
using namespace ayt;
using namespace ayt::editor;
namespace {
std::string utf8(const std::filesystem::path &p) {
  auto s = p.generic_u8string();
  return {s.begin(), s.end()};
}
std::string bytes(const std::filesystem::path &p) {
  std::ifstream i(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(i), {}};
}
struct Fixture {
  std::filesystem::path root =
      test::testTmpDir() /
      ("avatar-editor-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::path bundle = root / "character.avb";
  std::shared_ptr<EditorAvatarDocument> document =
      std::make_shared<EditorAvatarDocument>();
  Fixture() {
    std::filesystem::create_directories(root);
    avatar::Catalog catalog;
    avatar::fixture::populate(catalog);
    auto status =
        avatar::cookBundle(catalog, avatar::fixture::preset(), bundle);
    if (!status)
      throw std::runtime_error(status.message);
    std::string error;
    if (!document->initialize({utf8(bundle)}, error))
      throw std::runtime_error(error);
  }
};
struct Host : IEditorHostServices, IEditorDocumentSavePathProvider {
  EditorWorkspace value;
  std::string root;
  ui::UIManager *manager = nullptr;
  EditorWorkspace &workspace() noexcept override { return value; }
  const std::string &projectRoot() const noexcept override { return root; }
  ui::UIManager *uiManager() noexcept override { return manager; }
  void requestRepaint() override {}
  void setStatusText(const std::wstring &) override {}
  std::string chooseDocumentSavePath(const IEditorDocument &, bool) override {
    return root + "/exported.avp";
  }
};
} // namespace
TEST_SUITE(AYEditor_Avatar)
TEST_CASE(editor_registration_routes_bundles_presets_and_tool) {
  EditorExtensionRegistry registry;
  CHECK(registerEditorAvatarExtension(registry));
  const auto *bundle = registry.resolve({"hero.avb"});
  const auto *preset = registry.resolve({"hero.avp"});
  CHECK_NOT_NULL(bundle);
  CHECK_NOT_NULL(preset);
  CHECK(bundle && bundle->id == kEditorAvatarExtensionId);
  CHECK(preset && preset->id == kEditorAvatarExtensionId);
  CHECK(registry.find(kEditorAvatarToolExtensionId)->openPolicy ==
        EditorOpenPolicy::Singleton);
  CHECK(classifyEditorAssetPath("hero.avb") == EditorAssetType::AvatarBundle);
  CHECK(classifyEditorAssetPath("hero.AVP") == EditorAssetType::AvatarPreset);
  EditorExtensionRegistry builtins;
  CHECK(registerEditorBuiltInExtensions(builtins));
  CHECK_NOT_NULL(builtins.find(kEditorAvatarExtensionId));
}
TEST_CASE(parameter_save_reopen_history_and_readonly_bundle) {
  Fixture f;
  auto original = bytes(f.bundle);
  auto &d = *f.document;
  CHECK_FALSE(d.isDirty());
  CHECK_FALSE(d.canExecuteCommand("edit.undo"));
  CHECK(d.setParameter("height", .4f));
  CHECK(d.isDirty());
  CHECK(d.canExecuteCommand("edit.undo"));
  const auto appearance = d.appearanceRevision();
  CHECK(d.save());
  CHECK_FALSE(d.isDirty());
  CHECK(d.appearanceRevision() == appearance);
  CHECK(bytes(f.bundle) == original);
  CHECK(std::filesystem::file_size(f.root / "character.avp") < 2048);
  EditorAvatarDocument reopened;
  std::string error;
  CHECK(reopened.initialize({utf8(f.root / "character.avp")}, error));
  CHECK(reopened.creator()->preset().parameters.at("height") == .4f);
  CHECK_FALSE(reopened.canExecuteCommand("edit.undo"));
  CHECK(d.executeCommand("edit.undo"));
  CHECK(d.isDirty());
  CHECK(d.executeCommand("edit.redo"));
  CHECK_FALSE(d.isDirty());
  d.creator()->begin();
  CHECK(d.setParameter("height", .6f));
  CHECK_FALSE(d.canExecuteCommand("file.save"));
  CHECK_FALSE(d.canExecuteCommand("edit.undo"));
  d.creator()->cancel();
  d.contentChanged();
  CHECK_FALSE(d.isDirty());
  const auto exported = f.root / std::filesystem::u8path("角色 🌙 参数.avp");
  CHECK(d.saveAs(utf8(exported)));
  EditorAvatarDocument unicode;
  CHECK(unicode.initialize({utf8(exported)}, error));
  CHECK(unicode.creator()->preset().parameters.at("height") == .4f);
  CHECK_FALSE(d.saveAs(utf8(f.root / "wrong.fbx")));
  CHECK_FALSE(std::filesystem::exists(f.root / "wrong.fbx"));
  CHECK(d.setParameter("height", .1f));
  CHECK(d.reload());
  CHECK(d.creator()->preset().parameters.at("height") == .4f);
  CHECK_FALSE(d.isDirty());
}
TEST_CASE(failure_preserves_document_and_recovery_is_parameter_only) {
  Fixture f;
  auto &d = *f.document;
  CHECK(d.setParameter("height", .25f));
  auto value = avatar::encodePreset(d.creator()->preset());
  const auto revision = d.revision();
  auto *session = d.creator();
  CHECK_FALSE(d.setParameter("missing", .5f));
  CHECK_FALSE(
      d.setParameter("height", std::numeric_limits<float>::quiet_NaN()));
  CHECK(d.revision() == revision);
  CHECK(avatar::encodePreset(d.creator()->preset()) == value);
  std::string error;
  std::ofstream(f.root / "broken.avb", std::ios::binary) << "broken";
  CHECK_FALSE(d.initialize({utf8(f.root / "broken.avb")}, error));
  CHECK(d.creator() == session);
  CHECK(d.revision() == revision);
  CHECK_FALSE(d.saveAs(utf8(f.root / "absent" / "save.avp"), &error));
  CHECK(d.isDirty());
  auto recovery = f.root / "recover.avp";
  CHECK(d.writeRecoveryCopy(utf8(recovery)));
  CHECK(d.path() == utf8(f.root / "character.avp"));
  CHECK(d.isDirty());
  EditorAvatarDocument copy;
  CHECK(copy.initialize({utf8(recovery)}, error));
  CHECK(avatar::encodePreset(copy.creator()->preset()) == value);
  std::ofstream(recovery.string() + ".editor.json")
      << "{\"format\":\"AYEditorAvatarBinding\",\"version\":1,\"version\":1,"
         "\"bundle\":\"character.avb\"}";
  CHECK_FALSE(copy.initialize({utf8(recovery)}, error));
  CHECK(avatar::encodePreset(copy.creator()->preset()) == value);
}
TEST_CASE(editor_input_routes_fly_dolly_focus_without_editing_parameters) {
  Fixture f;
  Host host;
  host.root = utf8(f.root);
  ui::UIManager manager;
  manager.initialize(nullptr);
  manager.setClientSize(1000, 720);
  host.manager = &manager;
  EditorExtensionRegistry registry;
  CHECK(registerEditorAvatarExtension(registry));
  auto *descriptor = registry.find(kEditorAvatarExtensionId);
  auto view = descriptor->createView(f.document, host);
  auto *root = view->releaseRootWidget();
  root->setSize({1000, 720});
  manager.root()->addChildExternal(root);
  root->performLayout();
  manager.layout();
  auto *canvas =
      dynamic_cast<EditorAvatarCanvas *>(manager.findById("avatar_viewport"));
  CHECK_NOT_NULL(canvas);
  if (!canvas)
    return;
  const auto b = canvas->getWorldBounds();
  CHECK(b.maxX - b.minX > 300);
  CHECK(b.maxY - b.minY > 300);
  const auto preset = avatar::encodePreset(f.document->creator()->preset());
  const auto revision = f.document->revision();
  ui::DeviceInputBridge input(manager);
  auto pointer = [&](device::DeviceInputEventType type, float x, float y,
                     bool pressed = false) {
    device::DeviceInputEvent e;
    e.type = type;
    e.x = x;
    e.y = y;
    e.pressed = pressed;
    e.mouseButton = device::MouseButton::Right;
    input.dispatch(e);
  };
  float x = (b.minX + b.maxX) * .5f, y = (b.minY + b.maxY) * .5f;
  pointer(device::DeviceInputEventType::MouseButton, x, y, true);
  CHECK(manager.isCapturing());
  CHECK(canvas->camera().threeD().isLooking());
  const auto yaw = canvas->camera().threeD().yawRadians();
  pointer(device::DeviceInputEventType::MouseMove, x + 20, y - 10);
  CHECK(canvas->camera().threeD().yawRadians() != yaw);
  device::DeviceInputEvent key;
  key.type = device::DeviceInputEventType::Key;
  key.key = device::KeyCode::W;
  key.pressed = true;
  input.dispatch(key);
  auto eye = canvas->camera().threeD().eye();
  view->tick(.1f);
  CHECK((canvas->camera().threeD().eye() - eye).lengthSq() > 0);
  pointer(device::DeviceInputEventType::MouseButton, b.maxX + 100,
          b.maxY + 100);
  CHECK_FALSE(canvas->camera().threeD().isLooking());
  CHECK_FALSE(manager.isCapturing());
  eye = canvas->camera().threeD().eye();
  view->tick(.1f);
  CHECK((canvas->camera().threeD().eye() - eye).lengthSq() == 0);
  device::DeviceInputEvent wheel;
  wheel.type = device::DeviceInputEventType::MouseWheel;
  wheel.x = x;
  wheel.y = y;
  wheel.deltaY = 1;
  input.dispatch(wheel);
  CHECK((canvas->camera().threeD().eye() - eye).lengthSq() > 0);
  key.key = device::KeyCode::F;
  input.dispatch(key);
  CHECK_FALSE(canvas->camera().threeD().isLooking());
  pointer(device::DeviceInputEventType::MouseButton, x, y, true);
  key.key = device::KeyCode::W;
  input.dispatch(key);
  manager.setFocus(nullptr);
  eye = canvas->camera().threeD().eye();
  view->tick(.1f);
  CHECK_FALSE(canvas->camera().threeD().isLooking());
  CHECK((canvas->camera().threeD().eye() - eye).lengthSq() == 0);
  manager.cancelCapture();
  CHECK(avatar::encodePreset(f.document->creator()->preset()) == preset);
  CHECK(f.document->revision() == revision);
  CHECK_FALSE(f.document->isDirty());
  // Numeric controls and the editor's Save command share the focused-field
  // commit path.
  auto *field =
      dynamic_cast<ui::TextInput *>(manager.findById("parameter-value:height"));
  CHECK_NOT_NULL(field);
  manager.setFocus(field);
  field->setText(L"0.375");
  CHECK(view->commandTarget()->executeCommand("file.save"));
  avatar::Preset saved;
  CHECK(avatar::readPreset(f.root / "character.avp", saved));
  CHECK(saved.parameters.at("height") == .375f);
  auto savedBytes = bytes(f.root / "character.avp");
  manager.setFocus(field);
  field->setText(L"nan");
  CHECK_FALSE(view->commandTarget()->executeCommand("file.save"));
  CHECK(bytes(f.root / "character.avp") == savedBytes);
  view->prepareForUiShutdown();
  root->detachFromParent();
  ui::destroyWidgetTree(root);
  view.reset();
  manager.shutdown();
}
TEST_CASE(asset_browser_opens_avatar_dock_and_routes_save_shortcut) {
  Fixture f;
  auto assets = f.root / "Assets";
  std::filesystem::create_directories(assets);
  std::filesystem::copy_file(f.bundle, assets / "hero.avb");
  ui::MockRenderer backend;
  EditorSessionDesc desc;
  desc.uiBackend = &backend;
  desc.layoutPath = testfixtures::resolveEditorShellLayoutPath();
  desc.projectRoot = utf8(f.root);
  CHECK(!desc.layoutPath.empty());
  EditorSession session;
  CHECK(session.initialize(desc));
  session.setClientSize(1280, 720);
  CHECK(session.rescanAssetsNow());
  EditorAssetId id = 0;
  for (const auto &record : session.assetDatabase().records())
    if (record.type == EditorAssetType::AvatarBundle)
      id = record.id;
  CHECK(id != 0);
  CHECK(session.openAsset(id));
  session.update(.016f);
  auto *active = session.workspace().documents().active();
  CHECK_NOT_NULL(active);
  auto document =
      active ? std::dynamic_pointer_cast<EditorAvatarDocument>(active->document)
             : nullptr;
  CHECK_NOT_NULL(document);
  CHECK(active && active->editorId == kEditorAvatarExtensionId);
  auto *canvas = dynamic_cast<EditorAvatarCanvas *>(
      session.ui().findById("avatar_viewport"));
  auto *field = dynamic_cast<ui::TextInput *>(
      session.ui().findById("parameter-value:height"));
  CHECK_NOT_NULL(canvas);
  CHECK_NOT_NULL(field);
  if (document && canvas && field) {
    auto bounds = canvas->getWorldBounds();
    float x = (bounds.minX + bounds.maxX) * .5f;
    float y = (bounds.minY + bounds.maxY) * .5f;
    const auto mainEye = session.freecam().eye();
    const auto mainYaw = session.freecam().yawRadians();
    const auto yaw = canvas->camera().threeD().yawRadians();
    CHECK(session.onMouseButtonDown(x, y, 1));
    CHECK(session.onMouseMove(x + 20, y));
    session.onKeyDown(ui::UIKey_W);
    session.update(.05f);
    session.onKeyUp(ui::UIKey_W);
    session.onMouseButtonUp(x + 20, y, 1);
    CHECK(canvas->camera().threeD().yawRadians() != yaw);
    CHECK(session.freecam().yawRadians() == mainYaw);
    CHECK((session.freecam().eye() - mainEye).lengthSq() == 0);
    CHECK_FALSE(document->isDirty());
    session.ui().setFocus(field);
    field->setText(L"0.25");
    session.onKeyDown(ui::UIKey_Control);
    CHECK(session.onKeyDown(ui::UIKey_S));
    session.onKeyUp(ui::UIKey_S);
    session.onKeyUp(ui::UIKey_Control);
    avatar::Preset saved;
    CHECK(avatar::readPreset(assets / "hero.avp", saved));
    CHECK(saved.parameters.at("height") == .25f);
    CHECK_FALSE(document->isDirty());
  }
  session.shutdown();
}
TEST_CASE(crash_recovery_restores_parameter_file_and_preserves_source_bundle) {
  Fixture f;
  const auto bundleBytes = bytes(f.bundle);
  CHECK(f.document->path() == utf8(f.root / "character.avp"));
  CHECK(f.document->setParameter("height", .55f));
  {
    EditorRecoveryStore crashed(utf8(f.root));
    std::string error;
    CHECK(crashed.beginSession(&error));
    CHECK(crashed.autosave({f.document.get()}).documents == 1);
  }
  EditorRecoveryStore restarted(utf8(f.root));
  std::string error;
  CHECK(restarted.beginSession(&error));
  CHECK(restarted.hasRecoverableSession());
  auto candidates = restarted.recoverableDocuments();
  CHECK(candidates.size() == 1);
  CHECK(restarted.restorePrevious().documents == 1);
  CHECK(bytes(f.bundle) == bundleBytes);
  EditorAvatarDocument recovered;
  CHECK(recovered.initialize({utf8(f.root / "character.avp")}, error));
  CHECK(recovered.creator()->preset().parameters.at("height") == .55f);
  CHECK_FALSE(recovered.isDirty());
  restarted.markCleanShutdown();
}
TEST_SUITE_END
#endif
