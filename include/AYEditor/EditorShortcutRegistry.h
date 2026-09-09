#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ayt::editor {

struct EditorShortcutBinding {
    std::string commandId;
    std::wstring displayName;
    std::wstring defaultShortcut;
    std::wstring shortcut;
    std::uint8_t modifiers = 0;
    int keyCode = 0;
};

// Process-wide shortcut catalog shared by every AYEditor host. The engine
// config supplies defaults and a project may override individual commands.
class EditorShortcutRegistry {
public:
    static EditorShortcutRegistry& instance();

    void resetToDefaults();
    bool loadOverrides(const std::string& path, std::string* error = nullptr);
    bool setShortcut(std::string_view commandId,
                     const std::wstring& shortcut,
                     std::string* error = nullptr);

    const EditorShortcutBinding* find(std::string_view commandId) const;
    std::wstring shortcutFor(std::string_view commandId) const;
    std::string commandFor(int keyCode, std::uint8_t modifiers) const;
    const std::vector<EditorShortcutBinding>& bindings() const noexcept {
        return _bindings;
    }

    static bool parse(const std::wstring& shortcut,
                      std::uint8_t& modifiers, int& keyCode);

private:
    std::vector<EditorShortcutBinding> _bindings;
};

} // namespace ayt::editor
