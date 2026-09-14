#include "AYTest.h"

#include "AYEditor/EditorWorkspace.h"
#include "AYEditor/EditorDockViewHost.h"
#include "AYUI/DockArea.h"
#include "AYUI/DockCard.h"
#include "AYUI/MockRenderer.h"
#include "AYUI/Panel.h"
#include "AYUI/UIManager.h"
#include "AYUI/Widget.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace ayt::editor;

namespace {

class FrameworkTestDocument final : public IEditorDocument {
public:
    explicit FrameworkTestDocument(std::string path)
        : _path(std::move(path)), _title(_path.empty() ? "Untitled" : _path) {}

    const std::string& typeId() const noexcept override { return _typeId; }
    const std::string& path() const noexcept override { return _path; }
    const std::string& title() const noexcept override { return _title; }
    bool isDirty() const noexcept override { return _dirty; }
    uint64_t revision() const noexcept override { return _revision; }

    bool save(std::string* error) override {
        ++saveCount;
        if (!saveSucceeds) {
            if (error != nullptr) *error = "expected save failure";
            return false;
        }
        _dirty = false;
        ++_revision;
        if (error != nullptr) error->clear();
        return true;
    }

    bool saveSucceeds = true;
    int saveCount = 0;

private:
    std::string _typeId = "test.document";
    std::string _path;
    std::string _title;
    bool _dirty = true;
    uint64_t _revision = 1;
};

class FrameworkNullView final : public IEditorView {
public:
    ayt::ui::Widget* rootWidget() noexcept override { return nullptr; }
    ayt::ui::Widget* releaseRootWidget() noexcept override { return nullptr; }
};

struct FrameworkViewProbe {
    int activated = 0;
    int deactivated = 0;
    int ticks = 0;
    int destroyed = 0;
};

class FrameworkPanelView final : public IEditorView {
public:
    explicit FrameworkPanelView(FrameworkViewProbe& probe) : _probe(probe) {
        _root = new ayt::ui::Panel();
    }
    ~FrameworkPanelView() override {
        if (_root != nullptr) ayt::ui::destroyWidgetTree(_root);
        ++_probe.destroyed;
    }
    ayt::ui::Widget* rootWidget() noexcept override { return _root; }
    ayt::ui::Widget* releaseRootWidget() noexcept override {
        ayt::ui::Widget* root = _root;
        _root = nullptr;
        return root;
    }
    void onActivated() override { ++_probe.activated; }
    void onDeactivated() override { ++_probe.deactivated; }
    void tick(float) override { ++_probe.ticks; }

private:
    FrameworkViewProbe& _probe;
    ayt::ui::Widget* _root = nullptr;
};

class FrameworkHostServices final : public IEditorHostServices {
public:
    explicit FrameworkHostServices(EditorWorkspace& workspace)
        : _workspace(workspace) {}
    EditorWorkspace& workspace() noexcept override { return _workspace; }
    const std::string& projectRoot() const noexcept override { return _root; }
    void requestRepaint() override { ++repaints; }
    void setStatusText(const std::wstring& text) override { status = text; }

    int repaints = 0;
    std::wstring status;

private:
    EditorWorkspace& _workspace;
    std::string _root;
};

EditorDescriptor makeFrameworkDescriptor(
    std::string id, std::vector<std::string> extensions,
    EditorOpenPolicy policy = EditorOpenPolicy::PerResource,
    int priority = 0, int* factoryCalls = nullptr)
{
    EditorDescriptor descriptor;
    descriptor.id = std::move(id);
    descriptor.displayName = L"Framework Test Editor";
    descriptor.extensions = std::move(extensions);
    descriptor.openPolicy = policy;
    descriptor.priority = priority;
    descriptor.createDocument =
        [factoryCalls](const EditorOpenRequest& request, std::string&) {
            if (factoryCalls != nullptr) ++*factoryCalls;
            return std::make_shared<FrameworkTestDocument>(
                request.resourcePath);
        };
    descriptor.createView =
        [](const std::shared_ptr<IEditorDocument>&,
           IEditorHostServices&) -> std::unique_ptr<IEditorView> {
            return std::make_unique<FrameworkNullView>();
        };
    return descriptor;
}

class SetIntegerCommand final : public IEditorCommand {
public:
    SetIntegerCommand(int& value, int after, std::string mergeKey = {})
        : _value(value), _before(value), _after(after),
          _mergeKey(std::move(mergeKey)) {}

    const std::string& label() const noexcept override { return _label; }
    bool execute() override { _value = _after; return true; }
    bool undo() override { _value = _before; return true; }
    std::string mergeKey() const override { return _mergeKey; }
    bool mergeFrom(const IEditorCommand& newer) override {
        const auto* command = dynamic_cast<const SetIntegerCommand*>(&newer);
        if (command == nullptr || command->_mergeKey != _mergeKey) return false;
        _after = command->_after;
        return true;
    }

private:
    int& _value;
    int _before = 0;
    int _after = 0;
    std::string _mergeKey;
    std::string _label = "Set integer";
};

class FrameworkCommandTarget final : public IEditorCommandTarget {
public:
    bool handlesCommand(const std::string& commandId) const override {
        return commandId == "edit.undo";
    }
    bool canExecuteCommand(const std::string& commandId) const override {
        return handlesCommand(commandId) && enabled;
    }
    bool executeCommand(const std::string& commandId) override {
        if (!canExecuteCommand(commandId)) return false;
        ++executeCount;
        return true;
    }

    bool enabled = false;
    int executeCount = 0;
};

struct FrameworkInputCommandProbe {
    int undoCount = 0;
    int pointerDownCount = 0;
};

class FrameworkInputCommandView final
    : public IEditorView,
      public IEditorCommandTarget,
      public IEditorViewInputTarget {
public:
    explicit FrameworkInputCommandView(FrameworkInputCommandProbe& probe)
        : _probe(probe), _root(new ayt::ui::Panel()) {}

    ~FrameworkInputCommandView() override {
        if (_root != nullptr) ayt::ui::destroyWidgetTree(_root);
    }

    ayt::ui::Widget* rootWidget() noexcept override { return _root; }
    ayt::ui::Widget* releaseRootWidget() noexcept override {
        ayt::ui::Widget* root = _root;
        _root = nullptr;
        return root;
    }
    IEditorCommandTarget* commandTarget() noexcept override { return this; }
    IEditorViewInputTarget* inputTarget() noexcept override { return this; }

    bool handlesCommand(const std::string& commandId) const override {
        return commandId == "edit.undo";
    }
    bool canExecuteCommand(const std::string& commandId) const override {
        return handlesCommand(commandId);
    }
    bool executeCommand(const std::string& commandId) override {
        if (!canExecuteCommand(commandId)) return false;
        ++_probe.undoCount;
        return true;
    }

    bool onPointerDown(float, float, int) override {
        ++_probe.pointerDownCount;
        return true;
    }
    bool onPointerMove(float, float) override { return false; }
    bool onPointerUp(float, float, int) override { return false; }
    bool onWheel(float, float, float) override { return false; }
    bool onKeyDown(int) override { return false; }
    void onKeyUp(int) override {}
    bool hasPointerCapture() const noexcept override { return false; }
    ayt::ui::UiCursorHint cursorHint(float, float) const override {
        return ayt::ui::UiCursorHint::Default;
    }

private:
    FrameworkInputCommandProbe& _probe;
    ayt::ui::Widget* _root = nullptr;
};

} // namespace

TEST_SUITE(AYEditor_Framework)

TEST_CASE(editor_registry_resolves_long_extensions_priority_and_preference)
{
    EditorExtensionRegistry registry;
    std::string error;
    CHECK(registry.registerEditor(
        makeFrameworkDescriptor("json", {"json"},
                                EditorOpenPolicy::PerResource, 1), &error));
    CHECK(registry.registerEditor(
        makeFrameworkDescriptor("ui", {".ui.json"},
                                EditorOpenPolicy::PerResource, 10), &error));
    CHECK_FALSE(registry.registerEditor(
        makeFrameworkDescriptor("ui", {"other"}), &error));

    EditorOpenRequest request;
    request.resourcePath = "Assets/Hud.UI.JSON";
    const EditorDescriptor* resolved = registry.resolve(request);
    CHECK(resolved != nullptr);
    CHECK(resolved != nullptr && resolved->id == "ui");

    request.preferredEditorId = "json";
    resolved = registry.resolve(request);
    CHECK(resolved != nullptr && resolved->id == "json");
}

TEST_CASE(editor_document_manager_deduplicates_and_centralizes_close_policy)
{
    EditorExtensionRegistry registry;
    int factoryCalls = 0;
    CHECK(registry.registerEditor(makeFrameworkDescriptor(
        "ui", {".ui.json"}, EditorOpenPolicy::PerResource, 0,
        &factoryCalls)));
    EditorDocumentManager documents(registry);

    int opened = 0;
    int activated = 0;
    int closed = 0;
    documents.addListener([&](const EditorDocumentEvent& event) {
        if (event.type == EditorDocumentEventType::Opened) ++opened;
        if (event.type == EditorDocumentEventType::Activated) ++activated;
        if (event.type == EditorDocumentEventType::Closed) ++closed;
    });

    EditorOpenResult first = documents.open(
        EditorOpenRequest{"Assets/UI/Hud.ui.json"});
    EditorOpenResult duplicate = documents.open(
        EditorOpenRequest{"Assets\\UI\\Hud.ui.json"});
    CHECK(first.status == EditorOpenStatus::Opened);
    CHECK(duplicate.status == EditorOpenStatus::FocusedExisting);
    CHECK(first.documentId == duplicate.documentId);
    CHECK(factoryCalls == 1);
    CHECK(documents.size() == 1u);
    CHECK(opened == 1);
    CHECK(activated == 1);

    auto document = std::dynamic_pointer_cast<FrameworkTestDocument>(
        first.document);
    CHECK(document != nullptr);
    CHECK(documents.close(first.documentId,
        EditorDocumentCloseAction::Cancel).status
        == EditorCloseStatus::Cancelled);
    CHECK(documents.size() == 1u);

    document->saveSucceeds = false;
    CHECK(documents.close(first.documentId,
        EditorDocumentCloseAction::Save).status
        == EditorCloseStatus::SaveFailed);
    CHECK(document->saveCount == 1);
    document->saveSucceeds = true;
    CHECK(documents.close(first.documentId,
        EditorDocumentCloseAction::Save).status
        == EditorCloseStatus::Closed);
    CHECK(document->saveCount == 2);
    CHECK(closed == 1);
    CHECK(documents.size() == 0u);
}

TEST_CASE(editor_dock_view_host_owns_view_activation_and_dirty_close)
{
    EditorWorkspace workspace;
    FrameworkViewProbe probe;
    EditorDescriptor descriptor = makeFrameworkDescriptor(
        "hosted", {".hosted"});
    descriptor.createView =
        [&probe](const std::shared_ptr<IEditorDocument>&,
                 IEditorHostServices&) -> std::unique_ptr<IEditorView> {
            return std::make_unique<FrameworkPanelView>(probe);
        };
    CHECK(workspace.registry().registerEditor(std::move(descriptor)));

    ayt::ui::DockArea dock;
    FrameworkHostServices services(workspace);
    EditorDockViewHost host(workspace, dock, services);
    dock.setOnCardCloseRequested(
        [&host](ayt::ui::DockCard* card) {
            return host.requestClose(card);
        });

    EditorDockViewOptions options;
    options.cardId = "card_hosted_test";
    const EditorDockOpenResult first = host.open(
        EditorOpenRequest{"Assets/Test.hosted"}, options);
    const EditorDockOpenResult duplicate = host.open(
        EditorOpenRequest{"Assets/Test.hosted"}, options);
    CHECK(first);
    CHECK(duplicate);
    CHECK(first.document.status == EditorOpenStatus::Opened);
    CHECK(duplicate.document.status == EditorOpenStatus::FocusedExisting);
    CHECK(host.count() == 1u);
    CHECK(workspace.documents().size() == 1u);
    CHECK(dock.findCard("card_hosted_test") == first.card);
    CHECK(probe.activated == 1);

    host.tick(0.016f);
    CHECK(probe.ticks == 1);
    host.refreshPresentations();
    CHECK(first.card != nullptr
          && first.card->getTitle().find(L"*") != std::wstring::npos);

    host.setCloseActionProvider(
        [](const EditorHostedView&) {
            return EditorDocumentCloseAction::Cancel;
        });
    CHECK(dock.requestCloseCard(first.card));
    CHECK(host.count() == 1u);
    CHECK(workspace.documents().size() == 1u);

    host.setCloseActionProvider(
        [](const EditorHostedView&) {
            return EditorDocumentCloseAction::Discard;
        });
    CHECK(dock.requestCloseCard(first.card));
    CHECK(host.count() == 0u);
    CHECK(workspace.documents().size() == 0u);
    CHECK(dock.findCard("card_hosted_test") == nullptr);
    CHECK(probe.deactivated == 1);
    CHECK(probe.destroyed == 1);
}

TEST_CASE(editor_dock_view_host_preserves_input_command_target_without_widget_focus)
{
    EditorWorkspace workspace;
    FrameworkInputCommandProbe probe;
    EditorDescriptor descriptor = makeFrameworkDescriptor(
        "input-command-hosted", {".input-command"});
    descriptor.createView =
        [&probe](const std::shared_ptr<IEditorDocument>&,
                 IEditorHostServices&) -> std::unique_ptr<IEditorView> {
            return std::make_unique<FrameworkInputCommandView>(probe);
        };
    CHECK(workspace.registry().registerEditor(std::move(descriptor)));

    ayt::ui::MockRenderer backend;
    ayt::ui::UIManager ui;
    ui.initialize(&backend);
    ayt::ui::DockArea dock;
    FrameworkHostServices services(workspace);
    EditorDockViewHost host(workspace, dock, services, &ui);

    const EditorDockOpenResult opened = host.open(
        EditorOpenRequest{"Assets/Test.input-command"});
    CHECK(opened);
    CHECK(ui.getFocusedWidget() == nullptr);

    // A canvas view owns an input lease but intentionally has no AYUI
    // FocusableWidget. Synchronizing focus must not discard its command target.
    host.syncCommandTargetFromFocus();
    CHECK(workspace.commands().activeTarget() != nullptr);
    CHECK(workspace.commands().execute("edit.undo"));
    CHECK(probe.undoCount == 1);

    host.shutdown();
    ui.shutdown();
}

TEST_CASE(editor_command_history_merges_transactions_and_save_cursor)
{
    int value = 0;
    EditorCommandHistory history;
    int changed = 0;
    history.setChangedCallback([&]() { ++changed; });

    CHECK(history.execute(
        std::make_unique<SetIntegerCommand>(value, 1, "value")));
    history.markSaved();
    CHECK_FALSE(history.isDirty());
    CHECK(history.execute(
        std::make_unique<SetIntegerCommand>(value, 2, "value")));
    CHECK(value == 2);
    CHECK(history.size() == 2u);
    CHECK(history.isDirty());
    CHECK(history.undo());
    CHECK(value == 1);
    CHECK_FALSE(history.isDirty());

    CHECK(history.beginTransaction("Two fields"));
    CHECK(history.execute(
        std::make_unique<SetIntegerCommand>(value, 3, "field-a")));
    CHECK(history.execute(
        std::make_unique<SetIntegerCommand>(value, 4, "field-b")));
    CHECK(history.commitTransaction());
    CHECK(value == 4);
    CHECK(history.undoLabel() == "Two fields");
    CHECK(history.undo());
    CHECK(value == 1);
    CHECK(history.redo());
    CHECK(value == 4);

    CHECK(history.beginTransaction("Single field"));
    CHECK(history.execute(
        std::make_unique<SetIntegerCommand>(value, 5, "field-b")));
    CHECK(history.commitTransaction());
    CHECK(history.undoLabel() == "Single field");
    CHECK(history.undo());
    CHECK(value == 4);

    CHECK(history.beginTransaction("Cancelled gesture"));
    CHECK(history.execute(
        std::make_unique<SetIntegerCommand>(value, 9)));
    CHECK(history.cancelTransaction());
    CHECK(value == 4);
    CHECK(changed >= 5);
}

TEST_CASE(editor_command_router_does_not_fall_through_active_disabled_target)
{
    EditorCommandRouter router;
    int globalUndo = 0;
    int globalNew = 0;
    CHECK(router.registerGlobal(EditorCommandBinding{
        "edit.undo", L"Undo", "Ctrl+Z",
        [&]() { ++globalUndo; return true; }, {}}));
    CHECK(router.registerGlobal(EditorCommandBinding{
        "file.new", L"New", "Ctrl+N",
        [&]() { ++globalNew; return true; }, {}}));

    FrameworkCommandTarget target;
    router.setActiveTarget(&target);
    CHECK(router.handles("edit.undo"));
    CHECK_FALSE(router.canExecute("edit.undo"));
    CHECK_FALSE(router.execute("edit.undo"));
    CHECK(globalUndo == 0);

    target.enabled = true;
    CHECK(router.execute("edit.undo"));
    CHECK(target.executeCount == 1);
    CHECK(globalUndo == 0);
    CHECK(router.execute("file.new"));
    CHECK(globalNew == 1);
}

TEST_CASE(editor_workspace_keeps_typed_selection_per_document)
{
    EditorWorkspace workspace;
    CHECK(workspace.registry().registerEditor(
        makeFrameworkDescriptor("ui", {".ui.json"})));

    const EditorOpenResult first = workspace.documents().open(
        EditorOpenRequest{"Assets/First.ui.json"});
    const EditorOpenResult second = workspace.documents().open(
        EditorOpenRequest{"Assets/Second.ui.json"});
    CHECK(first);
    CHECK(second);
    CHECK(workspace.selections().size() == 2u);
    CHECK(workspace.selections().activeDocumentId() == second.documentId);

    EditorSelectionContext* secondSelection = workspace.selections().active();
    CHECK(secondSelection != nullptr);
    CHECK(secondSelection != nullptr && secondSelection->select(
        EditorObjectRef{{}, "widget", "button_ok"}));
    CHECK(secondSelection != nullptr && secondSelection->select(
        EditorObjectRef{{}, "widget", "label_title"}, true));
    CHECK(secondSelection != nullptr && secondSelection->items().size() == 2u);
    CHECK(secondSelection != nullptr && secondSelection->primary() != nullptr);
    CHECK(secondSelection != nullptr && secondSelection->primary() != nullptr
          && secondSelection->primary()->objectId == "label_title");
    CHECK(secondSelection != nullptr && secondSelection->remove(
        EditorObjectRef{{}, "widget", "button_ok"}));
    CHECK(secondSelection != nullptr && secondSelection->items().size() == 1u);
    CHECK(secondSelection != nullptr && secondSelection->primary() != nullptr
          && secondSelection->primary()->objectId == "label_title");
    CHECK_FALSE(secondSelection != nullptr && secondSelection->select(
        EditorObjectRef{first.documentId, "widget", "wrong_document"}, true));

    CHECK(workspace.documents().close(second.documentId,
        EditorDocumentCloseAction::Discard));
    CHECK(workspace.selections().size() == 1u);
    CHECK(workspace.selections().activeDocumentId() == first.documentId);
}

TEST_SUITE_END
