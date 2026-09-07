#pragma once

#include "AYEditor/EditorExtension.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace ayt::editor {

class EditorExtensionRegistry {
public:
    bool registerEditor(EditorDescriptor descriptor,
                        std::string* error = nullptr);
    bool unregisterEditor(const std::string& editorId);

    const EditorDescriptor* find(const std::string& editorId) const noexcept;
    const EditorDescriptor* resolve(
        const EditorOpenRequest& request) const noexcept;
    std::vector<const EditorDescriptor*> descriptors() const;
    size_t size() const noexcept { return _descriptors.size(); }

    static std::string normalizeExtension(std::string extension);
    static std::string normalizeAssetType(std::string assetType);

private:
    std::unordered_map<std::string, EditorDescriptor> _descriptors;
    std::vector<std::string> _registrationOrder;
};

} // namespace ayt::editor
