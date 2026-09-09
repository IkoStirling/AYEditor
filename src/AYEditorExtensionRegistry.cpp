#include "AYEditor/EditorExtensionRegistry.h"

#include <algorithm>
#include <cctype>

namespace ayt::editor {
namespace {

std::string lowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool endsWith(const std::string& value, const std::string& suffix)
{
    return suffix.size() <= value.size()
        && value.compare(value.size() - suffix.size(), suffix.size(), suffix)
            == 0;
}

} // namespace

bool EditorExtensionRegistry::registerEditor(EditorDescriptor descriptor,
                                             std::string* error)
{
    if (descriptor.id.empty()) {
        if (error != nullptr) *error = "Editor id must not be empty.";
        return false;
    }
    if (descriptor.displayName.empty()) {
        if (error != nullptr) *error = "Editor display name must not be empty.";
        return false;
    }
    if (descriptor.createView == nullptr) {
        if (error != nullptr) *error = "Editor view factory is required.";
        return false;
    }
    if (descriptor.surfaceKind == EditorSurfaceKind::Document
        && descriptor.createDocument == nullptr) {
        if (error != nullptr) *error = "Document factory is required.";
        return false;
    }
    if (_descriptors.find(descriptor.id) != _descriptors.end()) {
        if (error != nullptr) *error = "Editor id is already registered.";
        return false;
    }

    for (std::string& extension : descriptor.extensions) {
        extension = normalizeExtension(std::move(extension));
    }
    descriptor.extensions.erase(
        std::remove(descriptor.extensions.begin(), descriptor.extensions.end(),
            std::string{}),
        descriptor.extensions.end());
    std::sort(descriptor.extensions.begin(), descriptor.extensions.end());
    descriptor.extensions.erase(
        std::unique(descriptor.extensions.begin(), descriptor.extensions.end()),
        descriptor.extensions.end());

    for (std::string& assetType : descriptor.assetTypes) {
        assetType = normalizeAssetType(std::move(assetType));
    }
    descriptor.assetTypes.erase(
        std::remove(descriptor.assetTypes.begin(), descriptor.assetTypes.end(),
            std::string{}),
        descriptor.assetTypes.end());
    std::sort(descriptor.assetTypes.begin(), descriptor.assetTypes.end());
    descriptor.assetTypes.erase(
        std::unique(descriptor.assetTypes.begin(), descriptor.assetTypes.end()),
        descriptor.assetTypes.end());

    const std::string id = descriptor.id;
    _descriptors.emplace(id, std::move(descriptor));
    _registrationOrder.push_back(id);
    if (error != nullptr) error->clear();
    return true;
}

bool EditorExtensionRegistry::unregisterEditor(const std::string& editorId)
{
    if (_descriptors.erase(editorId) == 0) return false;
    _registrationOrder.erase(
        std::remove(_registrationOrder.begin(), _registrationOrder.end(), editorId),
        _registrationOrder.end());
    return true;
}

const EditorDescriptor* EditorExtensionRegistry::find(
    const std::string& editorId) const noexcept
{
    const auto it = _descriptors.find(editorId);
    return it == _descriptors.end() ? nullptr : &it->second;
}

const EditorDescriptor* EditorExtensionRegistry::resolve(
    const EditorOpenRequest& request) const noexcept
{
    if (!request.preferredEditorId.empty()) {
        // Explicit tool launchers address ToolPanel descriptors by id. Path
        // based resolution below remains document-only.
        return find(request.preferredEditorId);
    }

    const std::string assetType = normalizeAssetType(request.assetType);
    const std::string path = lowerAscii(request.resourcePath);
    const EditorDescriptor* best = nullptr;

    for (const std::string& id : _registrationOrder) {
        const EditorDescriptor* descriptor = find(id);
        if (descriptor == nullptr
            || descriptor->surfaceKind != EditorSurfaceKind::Document) {
            continue;
        }

        bool matches = false;
        if (!assetType.empty()) {
            matches = std::find(descriptor->assetTypes.begin(),
                                descriptor->assetTypes.end(), assetType)
                != descriptor->assetTypes.end();
        }
        if (!matches && !path.empty()) {
            for (const std::string& extension : descriptor->extensions) {
                if (endsWith(path, extension)) {
                    matches = true;
                    break;
                }
            }
        }
        if (matches && (best == nullptr
                        || descriptor->priority > best->priority)) {
            best = descriptor;
        }
    }
    return best;
}

std::vector<const EditorDescriptor*> EditorExtensionRegistry::descriptors() const
{
    std::vector<const EditorDescriptor*> result;
    result.reserve(_registrationOrder.size());
    for (const std::string& id : _registrationOrder) {
        if (const EditorDescriptor* descriptor = find(id)) {
            result.push_back(descriptor);
        }
    }
    return result;
}

std::string EditorExtensionRegistry::normalizeExtension(std::string extension)
{
    extension = lowerAscii(std::move(extension));
    if (!extension.empty() && extension.front() != '.') {
        extension.insert(extension.begin(), '.');
    }
    return extension;
}

std::string EditorExtensionRegistry::normalizeAssetType(std::string assetType)
{
    return lowerAscii(std::move(assetType));
}

} // namespace ayt::editor
