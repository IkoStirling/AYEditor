#pragma once

#include "AYEditor/EditorExtension.h"

#include <functional>
#include <string>

namespace ayt::editor {

class EditorUiLayoutDocument final : public IEditorDocument {
public:
    using SaveHandler = std::function<bool(
        const std::string& path, bool saveAs, std::string& error)>;
    using RecoveryHandler = std::function<bool(
        const std::string& path, std::string& error)>;

    bool initialize(const std::string& path,
                    const std::string& displayPath,
                    std::string* error = nullptr);

    const std::string& typeId() const noexcept override { return _typeId; }
    const std::string& path() const noexcept override { return _path; }
    const std::string& title() const noexcept override { return _title; }
    bool isDirty() const noexcept override { return _dirty; }
    uint64_t revision() const noexcept override { return _revision; }

    bool save(std::string* error = nullptr) override;
    bool canSaveAs() const noexcept override { return true; }
    bool saveAs(const std::string& path,
                std::string* error = nullptr) override;
    bool writeRecoveryCopy(const std::string& path,
                           std::string* error = nullptr) const override;

    void bindView(void* owner, SaveHandler handler,
                  RecoveryHandler recoveryHandler = {});
    void unbindView(void* owner) noexcept;
    void updateViewState(const std::string& path, bool dirty);

private:
    void updateTitle(const std::string& displayPath = {});
    bool invokeSave(const std::string& path, bool saveAs,
                    std::string* error);

    std::string _typeId = "ayeditor.ui-layout.document";
    std::string _path;
    std::string _title = "Untitled UI Layout";
    bool _dirty = false;
    uint64_t _revision = 1;
    void* _viewOwner = nullptr;
    SaveHandler _saveHandler;
    RecoveryHandler _recoveryHandler;
};

} // namespace ayt::editor
