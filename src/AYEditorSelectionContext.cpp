#include "AYEditor/EditorSelectionContext.h"

#include <algorithm>

namespace ayt::editor {

const EditorObjectRef* EditorSelectionContext::primary() const noexcept
{
    return _items.empty() || _primaryIndex >= _items.size()
        ? nullptr : &_items[_primaryIndex];
}

bool EditorSelectionContext::setSelection(
    std::vector<EditorObjectRef> items, const EditorObjectRef* primaryItem)
{
    std::vector<EditorObjectRef> normalized;
    normalized.reserve(items.size());
    for (EditorObjectRef& item : items) {
        if (!normalize(item)
            || std::find(normalized.begin(), normalized.end(), item)
                != normalized.end()) {
            continue;
        }
        normalized.push_back(std::move(item));
    }

    size_t primaryIndex = 0;
    if (primaryItem != nullptr && !normalized.empty()) {
        EditorObjectRef candidate = *primaryItem;
        if (normalize(candidate)) {
            const auto it = std::find(normalized.begin(), normalized.end(), candidate);
            if (it != normalized.end()) {
                primaryIndex = static_cast<size_t>(it - normalized.begin());
            }
        }
    }
    if (_items == normalized && _primaryIndex == primaryIndex) return false;
    _items = std::move(normalized);
    _primaryIndex = _items.empty() ? 0 : primaryIndex;
    changed();
    return true;
}

bool EditorSelectionContext::select(EditorObjectRef item, bool additive)
{
    if (!normalize(item)) return false;
    if (!additive) {
        std::vector<EditorObjectRef> selection{item};
        return setSelection(std::move(selection), &item);
    }

    std::vector<EditorObjectRef> selection = _items;
    if (std::find(selection.begin(), selection.end(), item) == selection.end()) {
        selection.push_back(item);
    }
    return setSelection(std::move(selection), &item);
}

bool EditorSelectionContext::remove(const EditorObjectRef& item)
{
    EditorObjectRef normalized = item;
    if (!normalize(normalized)) return false;
    const auto it = std::find(_items.begin(), _items.end(), normalized);
    if (it == _items.end()) return false;

    const size_t removedIndex = static_cast<size_t>(it - _items.begin());
    _items.erase(it);
    if (_items.empty()) {
        _primaryIndex = 0;
    } else if (removedIndex < _primaryIndex) {
        --_primaryIndex;
    } else if (removedIndex == _primaryIndex) {
        _primaryIndex = std::min(removedIndex, _items.size() - 1);
    }
    changed();
    return true;
}

bool EditorSelectionContext::clear()
{
    if (_items.empty()) return false;
    _items.clear();
    _primaryIndex = 0;
    changed();
    return true;
}

EditorSelectionContext::ListenerId EditorSelectionContext::addListener(
    Listener listener)
{
    if (listener == nullptr) return 0;
    const ListenerId id = _nextListenerId++;
    _listeners.emplace_back(id, std::move(listener));
    return id;
}

void EditorSelectionContext::removeListener(ListenerId listenerId)
{
    _listeners.erase(
        std::remove_if(_listeners.begin(), _listeners.end(),
            [listenerId](const auto& entry) {
                return entry.first == listenerId;
            }),
        _listeners.end());
}

bool EditorSelectionContext::normalize(EditorObjectRef& item) const
{
    if (item.documentId.empty()) item.documentId = _documentId;
    return !_documentId.empty() && item.documentId == _documentId
        && !item.domain.empty() && !item.objectId.empty();
}

void EditorSelectionContext::changed()
{
    ++_revision;
    const auto listeners = _listeners;
    for (const auto& entry : listeners) {
        if (entry.second != nullptr) entry.second();
    }
}

EditorSelectionContext& EditorSelectionService::contextFor(
    const std::string& documentId)
{
    auto& context = _contexts[documentId];
    if (context == nullptr) {
        context = std::make_unique<EditorSelectionContext>(documentId);
    }
    return *context;
}

EditorSelectionContext* EditorSelectionService::find(
    const std::string& documentId) noexcept
{
    const auto it = _contexts.find(documentId);
    return it == _contexts.end() ? nullptr : it->second.get();
}

const EditorSelectionContext* EditorSelectionService::find(
    const std::string& documentId) const noexcept
{
    const auto it = _contexts.find(documentId);
    return it == _contexts.end() ? nullptr : it->second.get();
}

bool EditorSelectionService::remove(const std::string& documentId)
{
    const bool wasActive = _activeId == documentId;
    if (_contexts.erase(documentId) == 0) return false;
    if (wasActive) {
        _activeId.clear();
        notifyActiveChanged();
    }
    return true;
}

bool EditorSelectionService::activate(const std::string& documentId)
{
    if (find(documentId) == nullptr) return false;
    if (_activeId == documentId) return true;
    _activeId = documentId;
    notifyActiveChanged();
    return true;
}

void EditorSelectionService::clearActive()
{
    if (_activeId.empty()) return;
    _activeId.clear();
    notifyActiveChanged();
}

EditorSelectionContext* EditorSelectionService::active() noexcept
{
    return find(_activeId);
}

const EditorSelectionContext* EditorSelectionService::active() const noexcept
{
    return find(_activeId);
}

void EditorSelectionService::notifyActiveChanged()
{
    if (_activeChanged != nullptr) _activeChanged(active());
}

} // namespace ayt::editor
