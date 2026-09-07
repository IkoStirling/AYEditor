#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ayt::editor {

struct EditorObjectRef {
    std::string documentId;
    std::string domain;
    std::string objectId;

    bool operator==(const EditorObjectRef& other) const noexcept {
        return documentId == other.documentId
            && domain == other.domain
            && objectId == other.objectId;
    }
};

class EditorSelectionContext {
public:
    using ListenerId = uint64_t;
    using Listener = std::function<void()>;

    explicit EditorSelectionContext(std::string documentId)
        : _documentId(std::move(documentId)) {}

    const std::string& documentId() const noexcept { return _documentId; }
    const std::vector<EditorObjectRef>& items() const noexcept { return _items; }
    const EditorObjectRef* primary() const noexcept;
    bool empty() const noexcept { return _items.empty(); }
    uint64_t revision() const noexcept { return _revision; }

    bool setSelection(std::vector<EditorObjectRef> items,
                      const EditorObjectRef* primary = nullptr);
    bool select(EditorObjectRef item, bool additive = false);
    bool remove(const EditorObjectRef& item);
    bool clear();

    ListenerId addListener(Listener listener);
    void removeListener(ListenerId listenerId);

private:
    bool normalize(EditorObjectRef& item) const;
    void changed();

    std::string _documentId;
    std::vector<EditorObjectRef> _items;
    size_t _primaryIndex = 0;
    uint64_t _revision = 0;
    ListenerId _nextListenerId = 1;
    std::vector<std::pair<ListenerId, Listener>> _listeners;
};

class EditorSelectionService {
public:
    using ActiveChangedCallback = std::function<void(
        EditorSelectionContext* context)>;

    EditorSelectionContext& contextFor(const std::string& documentId);
    EditorSelectionContext* find(const std::string& documentId) noexcept;
    const EditorSelectionContext* find(
        const std::string& documentId) const noexcept;
    bool remove(const std::string& documentId);
    bool activate(const std::string& documentId);
    void clearActive();

    EditorSelectionContext* active() noexcept;
    const EditorSelectionContext* active() const noexcept;
    const std::string& activeDocumentId() const noexcept { return _activeId; }
    size_t size() const noexcept { return _contexts.size(); }

    void setActiveChangedCallback(ActiveChangedCallback callback) {
        _activeChanged = std::move(callback);
    }

private:
    void notifyActiveChanged();

    std::unordered_map<std::string,
        std::unique_ptr<EditorSelectionContext>> _contexts;
    std::string _activeId;
    ActiveChangedCallback _activeChanged;
};

} // namespace ayt::editor
