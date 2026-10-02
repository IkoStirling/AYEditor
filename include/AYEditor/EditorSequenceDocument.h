#pragma once
#include <AYEditor/EditorExtensionRegistry.h>
#include <AYEditorCommand/EditorCommandHistory.h>
#include <AYSequence/SceneSequenceSession.h>
#include <AYSequence/SequenceAuthoring.h>
#include <AYUI/Authoring/PlaybackControls.h>
#include <AYUI/Authoring/TimelineModel.h>

namespace ayt::scene {
class Scene;
}
namespace ayt::editor {
inline constexpr const char *kEditorSequenceExtensionId = "ayeditor.sequence";
/// Thin editor owner: independent SequenceAuthoring + existing command history.
/// Preview loads a separate saved Scene, never makes it the active World or ticks
/// gameplay/scripts. Content edits revoke preview; seek/play never dirty content.
class EditorSequenceDocument final : public IEditorDocument, public IEditorCommandTarget {
  public:
    EditorSequenceDocument();
    ~EditorSequenceDocument() override;
    bool initialize(const EditorOpenRequest &, std::string &error);
    void configureProjectRoot(std::string root);
    const std::string &typeId() const noexcept override { return _type; }
    const std::string &path() const noexcept override { return _path; }
    const std::string &title() const noexcept override { return _title; }
    bool isDirty() const noexcept override;
    std::uint64_t revision() const noexcept override { return _model->revision(); }
    bool save(std::string *error = nullptr) override;
    bool writeRecoveryCopy(const std::string &, std::string *error = nullptr) const override;
    bool canReload() const noexcept override { return true; }
    bool reload(std::string *error = nullptr) override;
    bool handlesCommand(const std::string &) const override;
    bool canExecuteCommand(const std::string &) const override;
    bool executeCommand(const std::string &) override;
    const ayt::sequence::SequenceAuthoring &model() const noexcept { return *_model; }
    bool edit(const std::string &label, const std::function<bool(ayt::sequence::Sequence &)> &);
    bool beginEdit(const std::string &label);
    bool endEdit(bool cancel);
    bool gestureActive() const noexcept { return bool(_gesture); }
    std::size_t historySize() const noexcept { return _history.size(); }
    bool seek(double seconds);
    bool play();
    void pause();
    void stop();
    void tick(double seconds);
    double position() const noexcept;
    bool playing() const noexcept;
    const std::string &diagnostic() const noexcept { return _diagnostic; }
    const ayt::scene::Scene *previewScene() const noexcept { return _scene.get(); }
    void closePreview();

  private:
    bool preparePreview();
    void invalidatePreview();
    std::shared_ptr<ayt::sequence::SequenceAuthoring> _model;
    EditorCommandHistory _history;
    std::shared_ptr<const ayt::sequence::CompiledSequence> _gesture;
    std::string _gestureLabel, _type = "ayeditor.sequence.document", _path, _title, _projectRoot,
                               _diagnostic;
    ayt::sequence::SequencePlayer _player;
    std::unique_ptr<ayt::scene::Scene> _scene;
    std::shared_ptr<ayt::sequence::SceneSequenceSession> _session;
    std::uint64_t _previewRevision = ~std::uint64_t{};
};
std::shared_ptr<ayt::ui::authoring::ICurveEditorSource>
    makeEditorSequenceSource(std::shared_ptr<EditorSequenceDocument>);
EditorDescriptor makeEditorSequenceDescriptor();
/// Register loose .seq authoring (normally automatic in built-in Editor setup).
/// Requires Sequence AssetIO/Animation integration; no Resource loader or cook/pak.
bool registerEditorSequenceExtension(EditorExtensionRegistry &, std::string *error = nullptr);
} // namespace ayt::editor
