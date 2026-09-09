#include "AYEditor/EditorUiLayoutDocument.h"

#include "AYIO/File.h"

#include <filesystem>
#include <utility>

namespace ayt::editor {

bool EditorUiLayoutDocument::initialize(
    const std::string& path, const std::string& displayPath,
    std::string* error)
{
    if (!path.empty()) {
        if (!ayt::io::File::exists(path)) {
            if (error != nullptr) *error = "UI layout file does not exist.";
            return false;
        }
        if (ayt::io::File::readAllText(path).empty()) {
            if (error != nullptr) *error = "UI layout file is empty or unreadable.";
            return false;
        }
    }
    _path = path;
    _dirty = false;
    _revision = 1;
    updateTitle(displayPath);
    if (error != nullptr) error->clear();
    return true;
}

bool EditorUiLayoutDocument::save(std::string* error)
{
    return invokeSave({}, false, error);
}

bool EditorUiLayoutDocument::saveAs(
    const std::string& path, std::string* error)
{
    if (path.empty()) {
        if (error != nullptr) *error = "Save As path is empty.";
        return false;
    }
    return invokeSave(path, true, error);
}

bool EditorUiLayoutDocument::writeRecoveryCopy(
    const std::string& path, std::string* error) const
{
    if (_recoveryHandler == nullptr) {
        if (error != nullptr) *error = "UI Layout view is not available.";
        return false;
    }
    std::string localError;
    const bool saved = _recoveryHandler(path, localError);
    if (!saved && localError.empty()) {
        localError = "UI layout recovery save failed.";
    }
    if (error != nullptr) *error = std::move(localError);
    return saved;
}

void EditorUiLayoutDocument::bindView(
    void* owner, SaveHandler handler, RecoveryHandler recoveryHandler)
{
    _viewOwner = owner;
    _saveHandler = std::move(handler);
    _recoveryHandler = std::move(recoveryHandler);
}

void EditorUiLayoutDocument::unbindView(void* owner) noexcept
{
    if (_viewOwner != owner) return;
    _viewOwner = nullptr;
    _saveHandler = {};
    _recoveryHandler = {};
}

void EditorUiLayoutDocument::updateViewState(
    const std::string& path, bool dirty)
{
    _path = path;
    _dirty = dirty;
    ++_revision;
    updateTitle();
}

void EditorUiLayoutDocument::updateTitle(const std::string& displayPath)
{
    const std::string& source = displayPath.empty() ? _path : displayPath;
    if (source.empty()) {
        _title = "Untitled UI Layout";
        return;
    }
    const std::string fileName = std::filesystem::path(source).filename().string();
    _title = fileName.empty() ? source : fileName;
}

bool EditorUiLayoutDocument::invokeSave(
    const std::string& path, bool saveAs, std::string* error)
{
    if (_saveHandler == nullptr) {
        if (error != nullptr) *error = "UI Layout view is not available.";
        return false;
    }
    std::string localError;
    const bool saved = _saveHandler(path, saveAs, localError);
    if (!saved && localError.empty()) localError = "UI layout save failed.";
    if (error != nullptr) *error = std::move(localError);
    return saved;
}

} // namespace ayt::editor
