#pragma once

#include "AYEditor/EditorExtensionRegistry.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ayt::editor {

enum class EditorOpenStatus : uint8_t {
    Opened,
    FocusedExisting,
    NoEditor,
    Failed,
};

struct EditorOpenResult {
    EditorOpenStatus status = EditorOpenStatus::Failed;
    std::string documentId;
    std::shared_ptr<IEditorDocument> document;
    std::string error;

    explicit operator bool() const noexcept {
        return status == EditorOpenStatus::Opened
            || status == EditorOpenStatus::FocusedExisting;
    }
};

enum class EditorDocumentCloseAction : uint8_t {
    Save,
    Discard,
    Cancel,
};

enum class EditorCloseStatus : uint8_t {
    Closed,
    NotFound,
    Cancelled,
    SaveFailed,
};

struct EditorCloseResult {
    EditorCloseStatus status = EditorCloseStatus::NotFound;
    std::string error;

    explicit operator bool() const noexcept {
        return status == EditorCloseStatus::Closed;
    }
};

struct EditorDocumentRecord {
    std::string documentId;
    std::string editorId;
    std::string resourceKey;
    std::shared_ptr<IEditorDocument> document;
};

enum class EditorDocumentEventType : uint8_t {
    Opened,
    Activated,
    Closed,
};

struct EditorDocumentEvent {
    EditorDocumentEventType type = EditorDocumentEventType::Opened;
    std::string documentId;
    std::string editorId;
};

class EditorDocumentManager {
public:
    using ListenerId = uint64_t;
    using Listener = std::function<void(const EditorDocumentEvent&)>;

    explicit EditorDocumentManager(EditorExtensionRegistry& registry)
        : _registry(registry) {}

    EditorOpenResult open(const EditorOpenRequest& request);
    bool activate(const std::string& documentId);
    EditorCloseResult close(const std::string& documentId,
                            EditorDocumentCloseAction action);

    const EditorDocumentRecord* find(
        const std::string& documentId) const noexcept;
    EditorDocumentRecord* find(const std::string& documentId) noexcept;
    const EditorDocumentRecord* active() const noexcept;
    const std::string& activeDocumentId() const noexcept { return _activeId; }
    const std::vector<EditorDocumentRecord>& records() const noexcept {
        return _records;
    }
    size_t size() const noexcept { return _records.size(); }

    ListenerId addListener(Listener listener);
    void removeListener(ListenerId listenerId);

    static std::string normalizeResourceKey(std::string key);

private:
    std::string deduplicationKey(const EditorDescriptor& descriptor,
                                 const EditorOpenRequest& request) const;
    std::string nextDocumentId(const std::string& editorId);
    void emit(EditorDocumentEventType type,
              const EditorDocumentRecord& record);

    EditorExtensionRegistry& _registry;
    std::vector<EditorDocumentRecord> _records;
    std::string _activeId;
    uint64_t _nextDocumentSerial = 1;
    ListenerId _nextListenerId = 1;
    std::vector<std::pair<ListenerId, Listener>> _listeners;
};

} // namespace ayt::editor
