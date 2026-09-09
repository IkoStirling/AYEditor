#include "AYEditor/EditorDockViewHost.h"

#include "AYUI/DockArea.h"
#include "AYUI/DockCard.h"
#include "AYUI/UIManager.h"
#include "AYUI/UnicodeText.h"
#include "AYUI/Widget.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <utility>

namespace ayt::editor {
namespace {

ayt::ui::DockArea::Slot toUiSlot(EditorDockSlot slot)
{
    switch (slot) {
    case EditorDockSlot::Left: return ayt::ui::DockArea::Slot::Left;
    case EditorDockSlot::Right: return ayt::ui::DockArea::Slot::Right;
    case EditorDockSlot::Bottom: return ayt::ui::DockArea::Slot::Bottom;
    case EditorDockSlot::Center: break;
    }
    return ayt::ui::DockArea::Slot::Center;
}

std::wstring cardTitle(const IEditorDocument& document)
{
    std::wstring title = ayt::ui::decodeUtf8Text(document.title());
    if (document.isDirty()) title += L" *";
    return title;
}

} // namespace

EditorDockViewHost::EditorDockViewHost(
    EditorWorkspace& workspace,
    ayt::ui::DockArea& dockArea,
    IEditorHostServices& outerHost,
    ayt::ui::UIManager* uiManager)
    : _workspace(workspace), _dockArea(dockArea),
      _outerHost(outerHost), _uiManager(uiManager)
{
    _documentListener = _workspace.documents().addListener(
        [this](const EditorDocumentEvent& event) {
            onDocumentEvent(event);
        });
}

EditorDockViewHost::~EditorDockViewHost()
{
    if (!_releasedAfterUiShutdown) shutdown();
    _workspace.documents().removeListener(_documentListener);
}

EditorDockOpenResult EditorDockViewHost::open(
    const EditorOpenRequest& request, EditorDockViewOptions options)
{
    EditorDockOpenResult result;
    if (_preparedForUiShutdown) {
        result.error = "Editor view host is shutting down.";
        return result;
    }

    result.document = _workspace.documents().open(request);
    if (!result.document) {
        result.error = result.document.error;
        return result;
    }

    if (EditorHostedView* existing = find(result.document.documentId)) {
        activateHosted(*existing, true);
        result.card = existing->card;
        return result;
    }

    const EditorDocumentRecord* record = _workspace.documents().find(
        result.document.documentId);
    const EditorDescriptor* descriptor = record != nullptr
        ? _workspace.registry().find(record->editorId) : nullptr;
    if (record == nullptr || descriptor == nullptr) {
        result.error = "Opened document has no registered editor descriptor.";
    } else if (descriptor->createView == nullptr) {
        result.error = "Editor descriptor has no view factory.";
    }
    options.cardId = options.cardId.empty()
        ? makeCardId(result.document.documentId) : options.cardId;
    if (result.error.empty()
        && _dockArea.findCard(options.cardId) != nullptr) {
        result.error = "Editor view card id is already in use.";
    }
    if (!result.error.empty()) {
        if (result.document.status == EditorOpenStatus::Opened) {
            (void)_workspace.documents().close(
                result.document.documentId,
                EditorDocumentCloseAction::Discard);
        }
        result.card = nullptr;
        return result;
    }

    std::unique_ptr<IEditorView> view;
    try {
        view = descriptor->createView(result.document.document, *this);
    } catch (const std::exception& exception) {
        result.error = exception.what();
    } catch (...) {
        result.error = "Editor view factory raised an unknown exception.";
    }
    if (view == nullptr && result.error.empty()) {
        result.error = "Editor view factory returned null.";
    }
    if (!result.error.empty()) {
        if (result.document.status == EditorOpenStatus::Opened) {
            (void)_workspace.documents().close(
                result.document.documentId,
                EditorDocumentCloseAction::Discard);
        }
        return result;
    }

    ayt::ui::Widget* root = view->releaseRootWidget();
    if (root == nullptr) {
        result.error = "Editor view returned no root widget.";
        if (result.document.status == EditorOpenStatus::Opened) {
            (void)_workspace.documents().close(
                result.document.documentId,
                EditorDocumentCloseAction::Discard);
        }
        return result;
    }

    auto card = std::make_unique<ayt::ui::DockCard>();
    card->setId(options.cardId);
    card->setTitle(cardTitle(*result.document.document));
    card->setIcon(descriptor->iconPath);
    card->setHeaderHeight(options.headerHeight);
    card->setClosable(options.closable);
    card->setContent(root);
    ayt::ui::DockCard* cardPointer = card.get();

    EditorHostedView hosted;
    hosted.documentId = result.document.documentId;
    hosted.editorId = descriptor->id;
    hosted.cardId = options.cardId;
    hosted.dockSlot = descriptor->defaultDockSlot;
    hosted.document = result.document.document;
    hosted.view = std::move(view);
    hosted.card = cardPointer;
    hosted.presentedRevision = hosted.document->revision();
    hosted.presentedDirty = hosted.document->isDirty();
    hosted.presentedTitle = hosted.document->title();
    _hosted.push_back(std::move(hosted));

    _dockArea.addCard(toUiSlot(descriptor->defaultDockSlot), std::move(card));
    activateHosted(_hosted.back(), true);
    result.card = cardPointer;
    requestRepaint();
    return result;
}

bool EditorDockViewHost::activate(const std::string& documentId)
{
    EditorHostedView* hosted = find(documentId);
    if (hosted == nullptr) return false;
    if (!_workspace.documents().activate(documentId)) return false;
    activateHosted(*hosted, true);
    return true;
}

EditorCloseResult EditorDockViewHost::close(
    const std::string& documentId, EditorDocumentCloseAction action)
{
    EditorHostedView* hosted = find(documentId);
    if (hosted == nullptr) {
        return {EditorCloseStatus::NotFound, "Editor view is not hosted."};
    }

    _closingDocumentId = documentId;
    EditorCloseResult result = _workspace.documents().close(documentId, action);
    _closingDocumentId.clear();
    if (!result) {
        if (result.status == EditorCloseStatus::SaveFailed) {
            _outerHost.setStatusText(
                L"Save failed: " + ayt::ui::decodeUtf8Text(result.error));
            requestRepaint();
        }
        return result;
    }

    removeHostedView(documentId, true);
    requestRepaint();
    return result;
}

bool EditorDockViewHost::requestClose(ayt::ui::DockCard* card)
{
    EditorHostedView* hosted = findByCard(card);
    if (hosted == nullptr) return false;
    EditorDocumentCloseAction action = EditorDocumentCloseAction::Discard;
    if (hosted->document != nullptr && hosted->document->isDirty()) {
        action = _closeActionProvider != nullptr
            ? _closeActionProvider(*hosted)
            : EditorDocumentCloseAction::Cancel;
    }
    (void)close(hosted->documentId, action);
    return true;
}

EditorHostedView* EditorDockViewHost::find(
    const std::string& documentId) noexcept
{
    const auto it = std::find_if(
        _hosted.begin(), _hosted.end(),
        [&](const EditorHostedView& hosted) {
            return hosted.documentId == documentId;
        });
    return it == _hosted.end() ? nullptr : &*it;
}

const EditorHostedView* EditorDockViewHost::find(
    const std::string& documentId) const noexcept
{
    return const_cast<EditorDockViewHost*>(this)->find(documentId);
}

EditorHostedView* EditorDockViewHost::findByCard(
    ayt::ui::DockCard* card) noexcept
{
    const auto it = std::find_if(
        _hosted.begin(), _hosted.end(),
        [card](const EditorHostedView& hosted) {
            return hosted.card == card;
        });
    return it == _hosted.end() ? nullptr : &*it;
}

const EditorHostedView* EditorDockViewHost::active() const noexcept
{
    return find(_activeDocumentId);
}

size_t EditorDockViewHost::count(const std::string& editorId) const noexcept
{
    if (editorId.empty()) return _hosted.size();
    return static_cast<size_t>(std::count_if(
        _hosted.begin(), _hosted.end(),
        [&](const EditorHostedView& hosted) {
            return hosted.editorId == editorId;
        }));
}

void EditorDockViewHost::syncCommandTargetFromFocus()
{
    if (_uiManager == nullptr) return;
    ayt::ui::Widget* focused = _uiManager->getFocusedWidget();
    for (EditorHostedView& hosted : _hosted) {
        if (focused != nullptr && hosted.card != nullptr
            && (focused == hosted.card
                || ayt::ui::UIManager::isDescendantOf(
                    focused, hosted.card))) {
            if (_workspace.documents().activeDocumentId()
                != hosted.documentId) {
                (void)_workspace.documents().activate(hosted.documentId);
            }
            _workspace.commands().setActiveTarget(
                hosted.view != nullptr
                    ? hosted.view->commandTarget() : nullptr);
            return;
        }
    }

    // Canvas-style hosted views keep their own input lease without assigning
    // AYUI focus to a child widget. Preserve that document's command target
    // only when no regular widget has focus; an actual focused widget outside
    // the hosted card must continue to own keyboard command routing.
    if (focused == nullptr) {
        EditorHostedView* hosted = inputFocusedHosted();
        if (hosted != nullptr) {
            if (_workspace.documents().activeDocumentId()
                != hosted->documentId) {
                (void)_workspace.documents().activate(hosted->documentId);
            }
            _workspace.commands().setActiveTarget(
                hosted->view != nullptr
                    ? hosted->view->commandTarget() : nullptr);
            return;
        }
    }
    _workspace.commands().setActiveTarget(nullptr);
}

void EditorDockViewHost::refreshPresentations()
{
    for (EditorHostedView& hosted : _hosted) {
        if (hosted.document == nullptr || hosted.card == nullptr) continue;
        const uint64_t revision = hosted.document->revision();
        const bool dirty = hosted.document->isDirty();
        const std::string& title = hosted.document->title();
        if (revision == hosted.presentedRevision
            && dirty == hosted.presentedDirty
            && title == hosted.presentedTitle) {
            continue;
        }
        hosted.card->setTitle(cardTitle(*hosted.document));
        hosted.presentedRevision = revision;
        hosted.presentedDirty = dirty;
        hosted.presentedTitle = title;
    }
}

void EditorDockViewHost::tick(float dt)
{
    for (EditorHostedView& hosted : _hosted) {
        if (hosted.view == nullptr) continue;
        if (hosted.documentId == _activeDocumentId
            || hosted.view->wantsBackgroundTick()) {
            hosted.view->tick(dt);
        }
    }
    syncCommandTargetFromFocus();
    refreshPresentations();
}

bool EditorDockViewHost::routePointerDown(
    float physicalX, float physicalY, int button)
{
    const ayt::math::FVector2 point = _uiManager != nullptr
        ? _uiManager->physicalToLogical({physicalX, physicalY})
        : ayt::math::FVector2(physicalX, physicalY);
    EditorHostedView* hosted = inputHostedAt(point.x, point.y);
    if (hosted == nullptr) {
        _inputDocumentId.clear();
        return false;
    }
    (void)_workspace.documents().activate(hosted->documentId);
    activateHosted(*hosted, false);
    _inputDocumentId = hosted->documentId;
    // A pointer press anywhere in the hosted input surface transfers command
    // ownership away from a stale text/list focus. If a focusable child is the
    // real target, normal AYUI dispatch below will immediately focus it again.
    if (_uiManager != nullptr
        && _uiManager->getFocusedWidget() != nullptr) {
        _uiManager->setFocus(nullptr);
    }
    const bool handled = hosted->view->inputTarget()->onPointerDown(
        point.x, point.y, button);
    return handled;
}

bool EditorDockViewHost::routePointerMove(float physicalX, float physicalY)
{
    const ayt::math::FVector2 point = _uiManager != nullptr
        ? _uiManager->physicalToLogical({physicalX, physicalY})
        : ayt::math::FVector2(physicalX, physicalY);
    EditorHostedView* hosted = inputFocusedHosted();
    IEditorViewInputTarget* target = hosted != nullptr
        ? hosted->view->inputTarget() : nullptr;
    if (target == nullptr || !target->hasPointerCapture()) {
        hosted = inputHostedAt(point.x, point.y);
        target = hosted != nullptr ? hosted->view->inputTarget() : nullptr;
    }
    return target != nullptr && target->onPointerMove(point.x, point.y);
}

bool EditorDockViewHost::routePointerUp(
    float physicalX, float physicalY, int button)
{
    const ayt::math::FVector2 point = _uiManager != nullptr
        ? _uiManager->physicalToLogical({physicalX, physicalY})
        : ayt::math::FVector2(physicalX, physicalY);
    EditorHostedView* hosted = inputFocusedHosted();
    IEditorViewInputTarget* target = hosted != nullptr
        ? hosted->view->inputTarget() : nullptr;
    if (target == nullptr || !target->hasPointerCapture()) {
        hosted = inputHostedAt(point.x, point.y);
        target = hosted != nullptr ? hosted->view->inputTarget() : nullptr;
    }
    return target != nullptr
        && target->onPointerUp(point.x, point.y, button);
}

bool EditorDockViewHost::routeWheel(
    float physicalX, float physicalY, float deltaY)
{
    const ayt::math::FVector2 point = _uiManager != nullptr
        ? _uiManager->physicalToLogical({physicalX, physicalY})
        : ayt::math::FVector2(physicalX, physicalY);
    EditorHostedView* hosted = inputHostedAt(point.x, point.y);
    if (hosted == nullptr) return false;
    (void)_workspace.documents().activate(hosted->documentId);
    activateHosted(*hosted, false);
    _inputDocumentId = hosted->documentId;
    return hosted->view->inputTarget()->onWheel(
        point.x, point.y, deltaY);
}

bool EditorDockViewHost::routeKeyDown(int keyCode)
{
    EditorHostedView* hosted = inputFocusedHosted();
    IEditorViewInputTarget* target = hosted != nullptr
        ? hosted->view->inputTarget() : nullptr;
    return target != nullptr && target->onKeyDown(keyCode);
}

bool EditorDockViewHost::routeKeyUp(int keyCode)
{
    EditorHostedView* hosted = inputFocusedHosted();
    IEditorViewInputTarget* target = hosted != nullptr
        ? hosted->view->inputTarget() : nullptr;
    if (target == nullptr) return false;
    target->onKeyUp(keyCode);
    return true;
}

bool EditorDockViewHost::resolveCursorHint(
    float physicalX, float physicalY, ayt::ui::UiCursorHint& hint) const
{
    const ayt::math::FVector2 point = _uiManager != nullptr
        ? _uiManager->physicalToLogical({physicalX, physicalY})
        : ayt::math::FVector2(physicalX, physicalY);
    for (auto it = _hosted.rbegin(); it != _hosted.rend(); ++it) {
        if (it->card == nullptr || !it->card->isVisible()
            || it->view == nullptr || it->view->inputTarget() == nullptr
            || !it->card->getWorldBounds().contains(point)) {
            continue;
        }
        hint = it->view->inputTarget()->cursorHint(point.x, point.y);
        return true;
    }
    return false;
}

void EditorDockViewHost::releaseInputFocus()
{
    _inputDocumentId.clear();
}

void EditorDockViewHost::shutdown()
{
    if (_releasedAfterUiShutdown) return;
    _workspace.commands().setActiveTarget(nullptr);
    closeDocumentsForShutdown();
    while (!_hosted.empty()) {
        removeHostedView(_hosted.back().documentId, true);
    }
    _activeDocumentId.clear();
    _releasedAfterUiShutdown = true;
}

void EditorDockViewHost::prepareForUiShutdown()
{
    if (_preparedForUiShutdown || _releasedAfterUiShutdown) return;
    _preparedForUiShutdown = true;
    _workspace.commands().setActiveTarget(nullptr);
    if (EditorHostedView* hosted = find(_activeDocumentId)) {
        if (hosted->view != nullptr) hosted->view->onDeactivated();
    }
    _activeDocumentId.clear();
    _inputDocumentId.clear();
    for (EditorHostedView& hosted : _hosted) {
        prepareHostedUiShutdown(hosted);
    }
    closeDocumentsForShutdown();
}

void EditorDockViewHost::releaseAfterUiShutdown()
{
    if (_releasedAfterUiShutdown) return;
    _hosted.clear();
    _activeDocumentId.clear();
    _inputDocumentId.clear();
    _releasedAfterUiShutdown = true;
}

void EditorDockViewHost::requestRepaint()
{
    refreshPresentations();
    _outerHost.requestRepaint();
}

void EditorDockViewHost::onDocumentEvent(const EditorDocumentEvent& event)
{
    if (event.type == EditorDocumentEventType::Activated) {
        if (EditorHostedView* hosted = find(event.documentId)) {
            activateHosted(*hosted, false);
        }
        return;
    }
    if (event.type == EditorDocumentEventType::Closed
        && event.documentId != _closingDocumentId) {
        removeHostedView(event.documentId, true);
    }
}

void EditorDockViewHost::activateHosted(
    EditorHostedView& hosted, bool revealCard)
{
    if (_activeDocumentId != hosted.documentId) {
        if (EditorHostedView* previous = find(_activeDocumentId)) {
            if (previous->view != nullptr) previous->view->onDeactivated();
        }
        _activeDocumentId = hosted.documentId;
        if (hosted.view != nullptr) hosted.view->onActivated();
    }
    _workspace.commands().setActiveTarget(
        hosted.view != nullptr ? hosted.view->commandTarget() : nullptr);
    _inputDocumentId = hosted.view != nullptr
        && hosted.view->inputTarget() != nullptr
        ? hosted.documentId : std::string{};
    if (revealCard && hosted.card != nullptr) {
        (void)_dockArea.setCardVisible(
            hosted.cardId, true, toUiSlot(hosted.dockSlot));
    }
}

void EditorDockViewHost::removeHostedView(
    const std::string& documentId, bool destroyCard)
{
    const auto it = std::find_if(
        _hosted.begin(), _hosted.end(),
        [&](const EditorHostedView& hosted) {
            return hosted.documentId == documentId;
        });
    if (it == _hosted.end()) return;

    if (_activeDocumentId == documentId) {
        if (it->view != nullptr) it->view->onDeactivated();
        _activeDocumentId.clear();
        _workspace.commands().setActiveTarget(nullptr);
    }
    if (_inputDocumentId == documentId) {
        _inputDocumentId.clear();
    }
    if (_uiManager != nullptr && it->card != nullptr) {
        ayt::ui::Widget* focused = _uiManager->getFocusedWidget();
        if (focused != nullptr
            && (focused == it->card
                || ayt::ui::UIManager::isDescendantOf(focused, it->card))) {
            _uiManager->setFocus(nullptr);
        }
    }
    if (destroyCard && it->card != nullptr) {
        prepareHostedUiShutdown(*it);
        (void)_dockArea.closeCard(it->cardId);
    }
    _hosted.erase(it);
}

void EditorDockViewHost::prepareHostedUiShutdown(EditorHostedView& hosted)
{
    if (hosted.uiShutdownPrepared) return;
    hosted.uiShutdownPrepared = true;
    if (hosted.view != nullptr) hosted.view->prepareForUiShutdown();
}

EditorHostedView* EditorDockViewHost::inputHostedAt(
    float logicalX, float logicalY) noexcept
{
    const ayt::math::FVector2 point(logicalX, logicalY);
    for (auto it = _hosted.rbegin(); it != _hosted.rend(); ++it) {
        if (it->card != nullptr && it->card->isVisible()
            && it->view != nullptr && it->view->inputTarget() != nullptr
            && it->card->getWorldBounds().contains(point)) {
            return &*it;
        }
    }
    return nullptr;
}

EditorHostedView* EditorDockViewHost::inputFocusedHosted() noexcept
{
    EditorHostedView* hosted = find(_inputDocumentId);
    return hosted != nullptr && hosted->view != nullptr
        && hosted->view->inputTarget() != nullptr ? hosted : nullptr;
}

void EditorDockViewHost::closeDocumentsForShutdown()
{
    std::vector<std::string> documentIds;
    documentIds.reserve(_hosted.size());
    for (const EditorHostedView& hosted : _hosted) {
        documentIds.push_back(hosted.documentId);
    }
    for (const std::string& documentId : documentIds) {
        _closingDocumentId = documentId;
        (void)_workspace.documents().close(
            documentId, EditorDocumentCloseAction::Discard);
        _closingDocumentId.clear();
    }
}

std::string EditorDockViewHost::makeCardId(const std::string& documentId)
{
    std::string id = "editor_document_";
    id.reserve(id.size() + documentId.size());
    for (unsigned char ch : documentId) {
        id.push_back(std::isalnum(ch) != 0
            ? static_cast<char>(ch) : '_');
    }
    return id;
}

} // namespace ayt::editor
