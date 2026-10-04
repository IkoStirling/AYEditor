#pragma once
#include <AYAvatar/Creator.h>
#include <AYEditor/EditorExtensionRegistry.h>
#include <memory>

namespace ayt::editor {
inline constexpr const char *kEditorAvatarExtensionId = "ayeditor.avatar";
inline constexpr const char *kEditorAvatarToolExtensionId =
    "ayeditor.tool.avatar";
/// Isolated Avatar authoring session. Bundles remain read-only; save writes a
/// parameter-only .avp and an editor-only .avp.editor.json bundle association.
/// path() always identifies the writable .avp, including when opened from .avb,
/// so the recovery host cannot overwrite the read-only bundle with a preset.
/// Reopening an .avp requires that association or a same-stem .avb. Catalog and
/// Creator outlive the attached CreatorPanel. Navigation never changes content.
class EditorAvatarDocument final : public IEditorDocument,
                                   public IEditorCommandTarget {
public:
  bool initialize(const EditorOpenRequest &, std::string &error);
  const std::string &typeId() const noexcept override { return _type; }
  const std::string &path() const noexcept override { return _path; }
  const std::string &title() const noexcept override { return _title; }
  bool isDirty() const noexcept override;
  uint64_t revision() const noexcept override { return _revision; }
  uint64_t appearanceRevision() const noexcept { return _appearanceRevision; }
  bool save(std::string *error = nullptr) override;
  bool canSaveAs() const noexcept override { return _creator != nullptr; }
  bool saveAs(const std::string &, std::string *error = nullptr) override;
  bool canReload() const noexcept override { return !_path.empty(); }
  bool reload(std::string *error = nullptr) override;
  bool writeRecoveryCopy(const std::string &,
                         std::string *error = nullptr) const override;
  bool handlesCommand(const std::string &) const override;
  bool canExecuteCommand(const std::string &) const override;
  bool executeCommand(const std::string &) override;
  avatar::Creator *creator() noexcept { return _creator.get(); }
  const avatar::Creator *creator() const noexcept { return _creator.get(); }
  const avatar::Catalog *catalog() const noexcept { return _catalog.get(); }
  const std::string &bundlePath() const noexcept { return _bundlePath; }
  const std::string &presetPath() const noexcept { return _presetPath; }
  const std::string &diagnostic() const noexcept { return _diagnostic; }
  void contentChanged();
  bool setParameter(const std::string &, float);
  void setSavePathProvider(std::function<std::string(bool)> provider) {
    _savePath = std::move(provider);
  }

private:
  bool writePresetAndBinding(const std::string &, std::string *error) const;
  std::string _type = "ayeditor.avatar.document", _path, _bundlePath,
              _presetPath;
  std::string _title = "Avatar Creator", _saved, _observed, _diagnostic;
  std::unique_ptr<avatar::Catalog> _catalog;
  std::unique_ptr<avatar::Creator> _creator;
  uint64_t _revision = 1;
  uint64_t _appearanceRevision = 1;
  std::function<std::string(bool)> _savePath;
};
/// Register the Avatar tool and .avb/.avp authoring documents when Avatar's
/// Engine/UI adapters are linked. Uses the existing document/command host.
bool registerEditorAvatarExtension(EditorExtensionRegistry &,
                                   std::string *error = nullptr);
} // namespace ayt::editor
