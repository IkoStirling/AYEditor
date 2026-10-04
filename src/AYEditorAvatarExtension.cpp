#include "AYEditorAvatarCanvas.h"
#include <AYAvatar/CreatorPanel.h>
#include <AYEditor/EditorAvatarDocument.h>
#include <AYEditor/EditorCommandButtons.h>
#include <AYUI/Box.h>
#include <AYUI/Button.h>
#include <AYUI/ComboBox.h>
#include <AYUI/ScrollView.h>
#include <AYUI/TextLabel.h>
#include <AYUI/UIManager.h>
#include <AYUI/UnicodeText.h>
#include <filesystem>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <commdlg.h>
#endif
namespace ayt::editor {
namespace {
std::string chooseFile(bool save) {
#ifdef _WIN32
  wchar_t path[32768]{};
  OPENFILENAMEW dialog{};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = GetActiveWindow();
  dialog.lpstrFile = path;
  dialog.nMaxFile = 32768;
  dialog.lpstrFilter = save ? L"Avatar parameters (*.avp)\0*.avp\0\0"
                            : L"Avatar (*.avb;*.avp)\0*.avb;*.avp\0\0";
  dialog.lpstrDefExt = save ? L"avp" : nullptr;
  dialog.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST |
                 (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
  if (!(save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog)))
    return {};
  auto text = std::filesystem::path(path).generic_u8string();
  return {text.begin(), text.end()};
#else
  return {};
#endif
}
class AvatarView final : public IEditorView, public IEditorCommandTarget {
public:
  AvatarView(std::shared_ptr<EditorAvatarDocument> document,
             IEditorHostServices &host)
      : _document(std::move(document)), _host(host),
        _commands([this] { return this; }) {
    _document->setSavePathProvider([this](bool saveAs) {
      if (auto *provider =
              dynamic_cast<IEditorDocumentSavePathProvider *>(&_host))
        return provider->chooseDocumentSavePath(*_document, saveAs);
      return chooseFile(true);
    });
    _root = new ui::VBox();
    _root->setId("avatar_creator");
    _root->setSpacing(4);
    _root->setPadding(6, 6, 6, 6);
    auto *toolbar = new ui::HBox();
    toolbar->setSpacing(4);
    auto *open = new ui::Button();
    open->setId("avatar_open");
    open->setText(
        host.localizedText("ui.editor.avatar.open", L"Open character"));
    open->setOnClicked([this] { openCharacter(); });
    toolbar->addWidget(open, 130);
    _commands.add(
        *toolbar,
        host.localizedText("ui.editor.avatar.save", L"Save parameters"),
        "file.save", 140);
    _commands.add(*toolbar,
                  host.localizedText("ui.editor.avatar.save_as", L"Save As"),
                  "file.save_as", 100);
    _commands.add(*toolbar,
                  host.localizedText("ui.editor.avatar.reload", L"Reload"),
                  "file.reload", 90);
    _lod = new ui::ComboBox();
    _lod->setId("avatar_lod");
    _lod->setOnSelectionChanged([this](int value) {
      if (value >= 0 && _canvas)
        _canvas->setLod(uint32_t(value));
    });
    toolbar->addWidget(_lod, 100);
    _root->addWidget(toolbar, 32);
    auto *help = new ui::TextLabel();
    help->setText(
        host.localizedText("ui.editor.avatar.navigation",
                           L"Right drag: look | WASD / QE: move | Shift: "
                           L"faster | Wheel: zoom toward pointer | F: focus"));
    _root->addWidget(help, 24);
    auto *content = new ui::HBox();
    content->setSpacing(6);
    _scroll = new ui::ScrollView();
    _scroll->setId("avatar_parameters");
    _scroll->setHorizontalScrollBarVisibility(
        ui::ScrollView::ScrollBarVisibility::Hidden);
    content->addWidget(_scroll, 336);
    _canvas = new EditorAvatarCanvas(_document, host.uiManager());
    content->addWidget(_canvas, 0);
    _root->addWidget(content, 0);
    _status = new ui::TextLabel();
    _status->setId("avatar_status");
    _root->addWidget(_status, 44);
    rebuildPanel();
    refresh();
  }
  ~AvatarView() override {
    auto *owned = _ownsRoot ? _root : nullptr;
    prepareForUiShutdown();
    if (owned)
      ui::destroyWidgetTree(owned);
  }
  ui::Widget *rootWidget() noexcept override { return _root; }
  ui::Widget *releaseRootWidget() noexcept override {
    _ownsRoot = false;
    return _root;
  }
  IEditorCommandTarget *commandTarget() noexcept override { return this; }
  void onDeactivated() override {
    if (_canvas)
      _canvas->cancelNavigation();
  }
  void prepareForUiShutdown() override {
    if (_closed)
      return;
    _closed = true;
    _commands.detach();
    _document->setSavePathProvider({});
    clearPanel();
    if (_canvas)
      _canvas->shutdown();
    _canvas = nullptr;
    _scroll = nullptr;
    _lod = nullptr;
    _status = nullptr;
    _root = nullptr;
  }
  void tick(float dt) override {
    if (_closed)
      return;
    _canvas->tickNavigation(dt);
    if (_revision != _document->revision())
      refresh();
  }
  bool handlesCommand(const std::string &id) const override {
    return _document->handlesCommand(id);
  }
  bool canExecuteCommand(const std::string &id) const override {
    return _document->canExecuteCommand(id);
  }
  bool executeCommand(const std::string &id) override {
    // Commit a focused numeric field before Save, matching CreatorPanel/Studio.
    if (id == "file.save" || id == "file.save_as") {
      if (auto *manager = _host.uiManager()) {
        bool numeric = manager->getFocusedWidget() &&
                       manager->getFocusedWidget()->isTextEditingWidget();
        manager->setFocus(nullptr);
        if (numeric && _panel && !_panel->lastStatus()) {
          _status->setText(ui::decodeUtf8Text(_panel->lastStatus().message));
          return false;
        }
      }
    }
    auto result = _document->executeCommand(id);
    refresh();
    return result;
  }

private:
  void clearPanel() {
    if (auto *manager = _host.uiManager())
      if (_root)
        manager->clearTransientStateForSubtree(_root);
    if (_scroll)
      _scroll->setContent(nullptr);
    _panel.reset();
  }
  void rebuildPanel() {
    clearPanel();
    std::vector<std::wstring> lods;
    if (auto *session = _document->creator()) {
      _panel = std::make_unique<avatar::CreatorPanel>(*session, [this] {
        _document->contentChanged();
        _host.requestRepaint();
      });
      _scroll->setContent(&_panel->widget());
      _scroll->setContentSize(_panel->widget().getSize());
      auto canonical =
          _document->catalog()->canonical(session->preset().canonical);
      for (size_t i = 0; i < canonical->lods.size(); ++i)
        lods.push_back(L"LOD " + std::to_wstring(i));
    }
    _lod->setItems(lods);
    _lod->setSelectedIndex(lods.empty() ? -1 : 0);
    _canvas->setLod(0);
    _canvas->focusAvatar();
  }
  void refresh() {
    if (_closed)
      return;
    _revision = _document->revision();
    if (_panel)
      _panel->refresh();
    _commands.refresh();
    auto text = _document->diagnostic();
    if (text.empty())
      text = _document->creator() ? "Parameters: " + _document->presetPath()
                                  : "Open a character to start editing.";
    _status->setText(ui::decodeUtf8Text(text));
    _canvas->markDirty();
    _host.requestRepaint();
  }
  void openCharacter() {
    if (auto *manager = _host.uiManager())
      manager->setFocus(nullptr);
    if (_document->isDirty()) {
      _status->setText(
          L"Save your parameter changes before opening another character.");
      return;
    }
    const auto chosen = chooseFile(false);
    if (chosen.empty())
      return;
    clearPanel();
    std::string error;
    if (!_document->initialize({chosen}, error))
      _host.setStatusText(ui::decodeUtf8Text(error));
    rebuildPanel();
    refresh();
    if (!error.empty())
      _status->setText(ui::decodeUtf8Text(error));
  }
  std::shared_ptr<EditorAvatarDocument> _document;
  IEditorHostServices &_host;
  EditorCommandButtons _commands;
  ui::VBox *_root = nullptr;
  ui::ScrollView *_scroll = nullptr;
  EditorAvatarCanvas *_canvas = nullptr;
  ui::ComboBox *_lod = nullptr;
  ui::TextLabel *_status = nullptr;
  std::unique_ptr<avatar::CreatorPanel> _panel;
  bool _ownsRoot = true, _closed = false;
  uint64_t _revision = 0;
};
EditorDescriptor descriptor(bool tool) {
  EditorDescriptor result;
  result.id = tool ? kEditorAvatarToolExtensionId : kEditorAvatarExtensionId;
  result.displayName = L"Avatar Creator";
  result.defaultDockSlot = EditorDockSlot::Center;
  result.priority = 100;
  result.surfaceKind =
      tool ? EditorSurfaceKind::ToolPanel : EditorSurfaceKind::Document;
  result.openPolicy =
      tool ? EditorOpenPolicy::Singleton : EditorOpenPolicy::PerResource;
  if (!tool) {
    result.extensions = {".avb", ".avp"};
    result.assetTypes = {"Avatar Bundle", "Avatar Preset",
                         "runtime.avatar-bundle", "authoring.avatar-preset"};
  }
  result.createDocument = [](const EditorOpenRequest &request,
                             std::string &error) {
    auto document = std::make_shared<EditorAvatarDocument>();
    return document->initialize(request, error)
               ? std::static_pointer_cast<IEditorDocument>(document)
               : nullptr;
  };
  result.createView =
      [](const std::shared_ptr<IEditorDocument> &document,
         IEditorHostServices &host) -> std::unique_ptr<IEditorView> {
    auto avatar = std::dynamic_pointer_cast<EditorAvatarDocument>(document);
    return avatar ? std::make_unique<AvatarView>(avatar, host) : nullptr;
  };
  return result;
}
} // namespace
bool registerEditorAvatarExtension(EditorExtensionRegistry &registry,
                                   std::string *error) {
  return registry.registerEditor(descriptor(false), error) &&
         registry.registerEditor(descriptor(true), error);
}
} // namespace ayt::editor
