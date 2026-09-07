#include "AYEditor/EditorDocumentManager.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>

namespace ayt::editor {

EditorOpenResult EditorDocumentManager::open(const EditorOpenRequest& request)
{
    const EditorDescriptor* descriptor = _registry.resolve(request);
    if (descriptor == nullptr) {
        return {EditorOpenStatus::NoEditor, {}, {},
                "No registered editor accepts this resource."};
    }

    const std::string resourceKey = deduplicationKey(*descriptor, request);
    if (!resourceKey.empty()) {
        const auto existing = std::find_if(
            _records.begin(), _records.end(),
            [&](const EditorDocumentRecord& record) {
                return record.editorId == descriptor->id
                    && record.resourceKey == resourceKey;
            });
        if (existing != _records.end()) {
            activate(existing->documentId);
            return {EditorOpenStatus::FocusedExisting, existing->documentId,
                    existing->document, {}};
        }
    }

    std::string error;
    std::shared_ptr<IEditorDocument> document;
    try {
        document = descriptor->createDocument(request, error);
    } catch (const std::exception& exception) {
        error = exception.what();
    } catch (...) {
        error = "Editor document factory raised an unknown exception.";
    }
    if (document == nullptr) {
        if (error.empty()) error = "Editor document factory returned null.";
        return {EditorOpenStatus::Failed, {}, {}, std::move(error)};
    }

    EditorDocumentRecord record;
    record.documentId = nextDocumentId(descriptor->id);
    record.editorId = descriptor->id;
    record.resourceKey = resourceKey;
    record.document = std::move(document);
    _records.push_back(record);
    emit(EditorDocumentEventType::Opened, _records.back());
    activate(record.documentId);
    return {EditorOpenStatus::Opened, record.documentId,
            record.document, {}};
}

bool EditorDocumentManager::activate(const std::string& documentId)
{
    EditorDocumentRecord* record = find(documentId);
    if (record == nullptr) return false;
    if (_activeId == documentId) return true;
    _activeId = documentId;
    emit(EditorDocumentEventType::Activated, *record);
    return true;
}

EditorCloseResult EditorDocumentManager::close(
    const std::string& documentId, EditorDocumentCloseAction action)
{
    const auto it = std::find_if(
        _records.begin(), _records.end(),
        [&](const EditorDocumentRecord& record) {
            return record.documentId == documentId;
        });
    if (it == _records.end()) {
        return {EditorCloseStatus::NotFound, "Document is not open."};
    }

    if (it->document != nullptr && it->document->isDirty()) {
        if (action == EditorDocumentCloseAction::Cancel) {
            return {EditorCloseStatus::Cancelled, {}};
        }
        if (action == EditorDocumentCloseAction::Save) {
            std::string error;
            bool saved = false;
            try {
                saved = it->document->save(&error);
            } catch (const std::exception& exception) {
                error = exception.what();
            } catch (...) {
                error = "Document save raised an unknown exception.";
            }
            if (!saved) {
                if (error.empty()) error = "Document save failed.";
                return {EditorCloseStatus::SaveFailed, std::move(error)};
            }
        }
    }

    const bool wasActive = _activeId == documentId;
    const EditorDocumentRecord closing = *it;
    _records.erase(it);
    if (wasActive) _activeId.clear();
    emit(EditorDocumentEventType::Closed, closing);

    if (wasActive && !_records.empty()) {
        activate(_records.back().documentId);
    }
    return {EditorCloseStatus::Closed, {}};
}

const EditorDocumentRecord* EditorDocumentManager::find(
    const std::string& documentId) const noexcept
{
    const auto it = std::find_if(
        _records.begin(), _records.end(),
        [&](const EditorDocumentRecord& record) {
            return record.documentId == documentId;
        });
    return it == _records.end() ? nullptr : &*it;
}

EditorDocumentRecord* EditorDocumentManager::find(
    const std::string& documentId) noexcept
{
    return const_cast<EditorDocumentRecord*>(
        static_cast<const EditorDocumentManager*>(this)->find(documentId));
}

const EditorDocumentRecord* EditorDocumentManager::active() const noexcept
{
    return find(_activeId);
}

EditorDocumentManager::ListenerId EditorDocumentManager::addListener(
    Listener listener)
{
    if (listener == nullptr) return 0;
    const ListenerId id = _nextListenerId++;
    _listeners.emplace_back(id, std::move(listener));
    return id;
}

void EditorDocumentManager::removeListener(ListenerId listenerId)
{
    _listeners.erase(
        std::remove_if(_listeners.begin(), _listeners.end(),
            [listenerId](const auto& entry) {
                return entry.first == listenerId;
            }),
        _listeners.end());
}

std::string EditorDocumentManager::normalizeResourceKey(std::string key)
{
    std::replace(key.begin(), key.end(), '\\', '/');
    key = std::filesystem::path(key).lexically_normal().generic_string();
    while (key.size() > 1 && key.back() == '/') key.pop_back();
#if defined(_WIN32)
    std::transform(key.begin(), key.end(), key.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
    return key;
}

std::string EditorDocumentManager::deduplicationKey(
    const EditorDescriptor& descriptor,
    const EditorOpenRequest& request) const
{
    if (descriptor.openPolicy == EditorOpenPolicy::Multiple) return {};
    if (descriptor.openPolicy == EditorOpenPolicy::Singleton) {
        return "editor:" + descriptor.id;
    }
    const std::string source = request.resourceKey.empty()
        ? request.resourcePath : request.resourceKey;
    const std::string normalized = normalizeResourceKey(source);
    return normalized.empty() ? std::string{} : "resource:" + normalized;
}

std::string EditorDocumentManager::nextDocumentId(
    const std::string& editorId)
{
    return editorId + "#" + std::to_string(_nextDocumentSerial++);
}

void EditorDocumentManager::emit(EditorDocumentEventType type,
                                 const EditorDocumentRecord& record)
{
    const EditorDocumentEvent event{type, record.documentId, record.editorId};
    const auto listeners = _listeners;
    for (const auto& entry : listeners) {
        if (entry.second != nullptr) entry.second(event);
    }
}

} // namespace ayt::editor
