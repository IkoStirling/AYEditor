#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "AYEditor/EditorUiLayoutExtension.h"

#include "AYEditor/EditorUiLayoutDocument.h"
#include "AYIO/File.h"
#include "AYUI/LayoutLoader.h"
#include "AYUI/UIManager.h"
#include "AYUI/Widget.h"
#include "AYUI/WidgetFactory.h"
#include "LayoutEditorSession.h"

#include <memory>
#include <utility>

namespace ayt::editor {

struct EditorUiLayoutController::Impl {
    Impl(std::shared_ptr<EditorUiLayoutDocument> value,
         EditorUiLayoutExtensionConfig cfg)
        : document(std::move(value)), config(std::move(cfg))
    {
        session.setOpenPathPicker(config.openPathPicker);
        session.setSavePathPicker(config.savePathPicker);
        session.setDocumentStateUpdater(
            [this](const std::string& path, bool dirty) {
                if (document != nullptr) document->updateViewState(path, dirty);
                if (stateChanged != nullptr) stateChanged();
            });
        if (document != nullptr) {
            document->bindView(
                this,
                [this](const std::string& path, bool saveAs,
                       std::string& error) {
                    if (!attached) {
                        error = "UI Layout controller is not attached.";
                        return false;
                    }
                    const bool saved = saveAs
                        ? session.saveAs(path) : session.save();
                    if (!saved) error = "UI Layout session could not save.";
                    return saved;
                });
        }
    }

    ~Impl()
    {
        session.detach();
        attached = false;
        if (document != nullptr) document->unbindView(this);
    }

    std::shared_ptr<EditorUiLayoutDocument> document;
    EditorUiLayoutExtensionConfig config;
    ayt::ui::LayoutEditorSession session;
    StateChanged stateChanged;
    bool attached = false;
};

EditorUiLayoutController::EditorUiLayoutController(
    std::shared_ptr<EditorUiLayoutDocument> document,
    EditorUiLayoutExtensionConfig config)
    : _impl(std::make_unique<Impl>(std::move(document), std::move(config)))
{
}

EditorUiLayoutController::~EditorUiLayoutController() = default;

bool EditorUiLayoutController::attach(
    ayt::ui::UIManager& ui, ayt::ui::Widget* chromeRoot)
{
    if (_impl == nullptr) return false;
    _impl->attached = _impl->session.attach(ui, chromeRoot);
    return _impl->attached;
}

void EditorUiLayoutController::detach()
{
    if (_impl == nullptr || !_impl->attached) return;
    _impl->session.detach();
    _impl->attached = false;
}

bool EditorUiLayoutController::isAttached() const noexcept
{
    return _impl != nullptr && _impl->attached;
}

void EditorUiLayoutController::pumpDeferred()
{
    if (isAttached()) _impl->session.pumpDeferred();
}

bool EditorUiLayoutController::openDocument(const std::string& path)
{
    return isAttached() && _impl->session.open(path);
}

bool EditorUiLayoutController::saveDocument(std::string* error)
{
    if (_impl == nullptr || _impl->document == nullptr) {
        if (error != nullptr) *error = "UI Layout document is unavailable.";
        return false;
    }
    return _impl->document->save(error);
}

void EditorUiLayoutController::undo()
{
    if (isAttached()) _impl->session.undo();
}

void EditorUiLayoutController::redo()
{
    if (isAttached()) _impl->session.redo();
}

bool EditorUiLayoutController::onPointerDown(float x, float y, int button)
{
    return isAttached() && _impl->session.onPointerDown({x, y}, button);
}

bool EditorUiLayoutController::onPointerMove(float x, float y)
{
    return isAttached() && _impl->session.onPointerMove({x, y});
}

bool EditorUiLayoutController::onPointerUp(float x, float y, int button)
{
    return isAttached() && _impl->session.onPointerUp({x, y}, button);
}

bool EditorUiLayoutController::onWheel(float x, float y, float deltaY)
{
    return isAttached() && _impl->session.onWheel({x, y}, deltaY);
}

bool EditorUiLayoutController::onKeyDown(int keyCode)
{
    return isAttached() && _impl->session.onKeyDown(keyCode);
}

void EditorUiLayoutController::onKeyUp(int keyCode)
{
    if (isAttached()) _impl->session.onKeyUp(keyCode);
}

bool EditorUiLayoutController::hasPointerCapture() const noexcept
{
    return isAttached() && _impl->session.isDraggingDocument();
}

ayt::ui::UiCursorHint EditorUiLayoutController::cursorHint(
    float x, float y) const
{
    return isAttached()
        ? _impl->session.canvasCursorHint({x, y})
        : ayt::ui::UiCursorHint::Default;
}

void EditorUiLayoutController::setStateChanged(StateChanged changed)
{
    if (_impl != nullptr) _impl->stateChanged = std::move(changed);
}

const std::shared_ptr<EditorUiLayoutDocument>&
EditorUiLayoutController::document() const noexcept
{
    static const std::shared_ptr<EditorUiLayoutDocument> empty;
    return _impl != nullptr ? _impl->document : empty;
}

namespace {

class EditorUiLayoutWorkspaceView final
    : public IEditorView,
      public IEditorCommandTarget,
      public IEditorViewInputTarget {
public:
    EditorUiLayoutWorkspaceView(
        std::shared_ptr<EditorUiLayoutDocument> document,
        IEditorHostServices& host,
        EditorUiLayoutExtensionConfig config)
        : _document(std::move(document)), _host(host),
          _config(std::move(config))
    {
        _controller = std::make_unique<EditorUiLayoutController>(
            _document, _config);
        _controller->setStateChanged([this]() {
            _host.requestRepaint();
        });
        const std::string chromePath = _config.chromePath != nullptr
            ? _config.chromePath() : std::string{};
        const std::string chromeJson = chromePath.empty()
            ? std::string{} : ayt::io::File::readAllText(chromePath);
        if (chromeJson.empty()) return;

        ayt::ui::UILayoutLoader loader;
        loader.setWidgetFactory(&ayt::ui::WidgetFactory::get());
        _ownedRoot = loader.loadFromString(chromeJson);
        _chromeRoot = _ownedRoot;
        if (_ownedRoot == nullptr) return;

    }

    ~EditorUiLayoutWorkspaceView() override
    {
        prepareForUiShutdown();
        if (_ownedRoot != nullptr) {
            ayt::ui::destroyWidgetTree(_ownedRoot);
        }
    }

    bool valid() const noexcept { return _ownedRoot != nullptr; }

    ayt::ui::Widget* rootWidget() noexcept override { return _ownedRoot; }
    ayt::ui::Widget* releaseRootWidget() noexcept override
    {
        ayt::ui::Widget* root = _ownedRoot;
        _ownedRoot = nullptr;
        return root;
    }
    IEditorCommandTarget* commandTarget() noexcept override { return this; }
    IEditorViewInputTarget* inputTarget() noexcept override { return this; }

    void onActivated() override
    {
        if (_attached || _preparedForUiShutdown || _chromeRoot == nullptr) {
            return;
        }
        ayt::ui::UIManager* ui = _host.uiManager();
        if (ui == nullptr || !_controller->attach(*ui, _chromeRoot)) {
            _host.setStatusText(L"UI Layout Editor failed to attach");
            return;
        }
        _attached = true;
        if (!_document->path().empty()
            && !_controller->openDocument(_document->path())) {
            _host.setStatusText(L"UI layout document failed to open");
        } else {
            _host.setStatusText(L"UI Layout Editor ready");
        }
        _host.requestRepaint();
    }

    void tick(float) override
    {
        if (_attached) _controller->pumpDeferred();
    }

    void prepareForUiShutdown() override
    {
        if (_preparedForUiShutdown) return;
        _preparedForUiShutdown = true;
        _controller->detach();
        _attached = false;
    }

    bool handlesCommand(const std::string& commandId) const override
    {
        return commandId == "file.save"
            || commandId == "edit.undo"
            || commandId == "edit.redo";
    }

    bool canExecuteCommand(const std::string& commandId) const override
    {
        return _attached && handlesCommand(commandId);
    }

    bool executeCommand(const std::string& commandId) override
    {
        if (!canExecuteCommand(commandId)) return false;
        if (commandId == "file.save") {
            std::string error;
            const bool saved = _document->save(&error);
            if (!saved) {
                _host.setStatusText(L"UI layout save failed");
            }
            return saved;
        }
        if (commandId == "edit.undo") _controller->undo();
        if (commandId == "edit.redo") _controller->redo();
        return true;
    }

    bool onPointerDown(float x, float y, int button) override
    {
        return _attached && _controller->onPointerDown(x, y, button);
    }
    bool onPointerMove(float x, float y) override
    {
        return _attached && _controller->onPointerMove(x, y);
    }
    bool onPointerUp(float x, float y, int button) override
    {
        return _attached && _controller->onPointerUp(x, y, button);
    }
    bool onWheel(float x, float y, float deltaY) override
    {
        return _attached && _controller->onWheel(x, y, deltaY);
    }
    bool onKeyDown(int keyCode) override
    {
        return _attached && _controller->onKeyDown(keyCode);
    }
    void onKeyUp(int keyCode) override
    {
        if (_attached) _controller->onKeyUp(keyCode);
    }
    bool hasPointerCapture() const noexcept override
    {
        return _attached && _controller->hasPointerCapture();
    }
    ayt::ui::UiCursorHint cursorHint(float x, float y) const override
    {
        return _attached
            ? _controller->cursorHint(x, y)
            : ayt::ui::UiCursorHint::Default;
    }

private:
    std::shared_ptr<EditorUiLayoutDocument> _document;
    IEditorHostServices& _host;
    EditorUiLayoutExtensionConfig _config;
    std::unique_ptr<EditorUiLayoutController> _controller;
    ayt::ui::Widget* _ownedRoot = nullptr;
    ayt::ui::Widget* _chromeRoot = nullptr;
    bool _attached = false;
    bool _preparedForUiShutdown = false;
};

} // namespace

EditorDescriptor makeEditorUiLayoutDescriptor(
    EditorUiLayoutExtensionConfig config)
{
    EditorDescriptor descriptor;
    descriptor.id = kEditorUiLayoutExtensionId;
    descriptor.displayName = L"UI Layout Editor";
    descriptor.iconPath = "icons/outline/layout-dashboard.svg";
    descriptor.surfaceKind = EditorSurfaceKind::Document;
    descriptor.openPolicy = EditorOpenPolicy::PerResource;
    descriptor.defaultDockSlot = EditorDockSlot::Center;
    descriptor.priority = 120;
    descriptor.extensions = {".ui.json"};
    descriptor.assetTypes = {"ui-layout"};
    descriptor.createDocument =
        [](const EditorOpenRequest& request,
           std::string& error) -> std::shared_ptr<IEditorDocument> {
            auto document = std::make_shared<EditorUiLayoutDocument>();
            if (!document->initialize(
                    request.resourcePath, request.displayPath, &error)) {
                return nullptr;
            }
            return document;
        };
    descriptor.createView =
        [config = std::move(config)](
            const std::shared_ptr<IEditorDocument>& document,
            IEditorHostServices& host) -> std::unique_ptr<IEditorView> {
            auto layout =
                std::dynamic_pointer_cast<EditorUiLayoutDocument>(document);
            if (layout == nullptr) return nullptr;
            auto view = std::make_unique<EditorUiLayoutWorkspaceView>(
                std::move(layout), host, config);
            return view->valid() ? std::move(view) : nullptr;
        };
    return descriptor;
}

bool registerEditorUiLayoutExtension(
    EditorExtensionRegistry& registry,
    EditorUiLayoutExtensionConfig config,
    std::string* error)
{
    return registry.registerEditor(
        makeEditorUiLayoutDescriptor(std::move(config)), error);
}

} // namespace ayt::editor
