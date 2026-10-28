#include "AYEditor/EditorDocumentManager.h"

#include <cstdio>

using namespace ayt::editor;

namespace {
class Document final : public IEditorDocument {
public:
    const std::string& typeId() const noexcept override { return value; }
    const std::string& path() const noexcept override { return value; }
    const std::string& title() const noexcept override { return value; }
    bool isDirty() const noexcept override { return false; }
    uint64_t revision() const noexcept override { return 1; }
    bool save(std::string*) override { return true; }
private:
    std::string value = "test";
};
}

int main() {
    EditorExtensionRegistry registry;
    EditorDescriptor descriptor;
    descriptor.id = "test";
    descriptor.displayName = L"Test";
    descriptor.extensions = {".json"};
    descriptor.createDocument = [](const EditorOpenRequest&, std::string&) {
        return std::make_shared<Document>();
    };
    descriptor.createView = [](const std::shared_ptr<IEditorDocument>&,
                               IEditorHostServices&) -> std::unique_ptr<IEditorView> {
        return {};
    };
    if (!registry.registerEditor(std::move(descriptor))) return 1;
    EditorDocumentManager manager(registry);
    const auto first = manager.open(EditorOpenRequest{"first.json"});
    const auto middle = manager.open(EditorOpenRequest{"middle.json"});
    const auto last = manager.open(EditorOpenRequest{"last.json"});
    if (!first || !middle || !last) return 2;
    const auto* retained = manager.find(last.documentId);
    if (!retained) return 3;
    if (!manager.close(middle.documentId, EditorDocumentCloseAction::Discard))
        return 4;
    if (manager.find(last.documentId) != retained ||
        retained->documentId != last.documentId) {
        std::fputs("Borrowed record moved after unrelated close.\n", stderr);
        return 5;
    }
    return 0;
}
