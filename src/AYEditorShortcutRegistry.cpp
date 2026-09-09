#include "AYEditor/EditorShortcutRegistry.h"

#include <AYUI/UIKeyCode.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>

namespace ayt::editor {
namespace {

bool iequals(const std::wstring& value, const char* ascii)
{
    std::size_t index = 0;
    for (; ascii[index] != '\0'; ++index) {
        if (index >= value.size()) return false;
        const wchar_t a = value[index];
        const wchar_t b = static_cast<unsigned char>(ascii[index]);
        if (std::towlower(a) != std::towlower(b)) return false;
    }
    return index == value.size();
}

std::wstring widenAscii(const std::string& value)
{
    return std::wstring(value.begin(), value.end());
}

int parseKey(const std::wstring& token)
{
    if (token.size() == 1u) {
        const wchar_t c = static_cast<wchar_t>(std::towupper(token.front()));
        if (c >= L'A' && c <= L'Z') return ayt::ui::UIKey_A + (c - L'A');
        if (c >= L'0' && c <= L'9') return ayt::ui::UIKey_Num0 + (c - L'0');
    }
    if (token.size() >= 2u && (token[0] == L'F' || token[0] == L'f')) {
        try {
            const int number = std::stoi(token.substr(1));
            if (number >= 1 && number <= 12) {
                return ayt::ui::UIKey_F1 + number - 1;
            }
        } catch (...) {}
    }
    if (iequals(token, "delete") || iequals(token, "del")) return ayt::ui::UIKey_Delete;
    if (iequals(token, "space")) return ayt::ui::UIKey_Space;
    if (iequals(token, "enter") || iequals(token, "return")) return ayt::ui::UIKey_Enter;
    if (iequals(token, "tab")) return ayt::ui::UIKey_Tab;
    if (iequals(token, "escape") || iequals(token, "esc")) return ayt::ui::UIKey_Escape;
    if (iequals(token, "left")) return ayt::ui::UIKey_Left;
    if (iequals(token, "right")) return ayt::ui::UIKey_Right;
    if (iequals(token, "up")) return ayt::ui::UIKey_Up;
    if (iequals(token, "down")) return ayt::ui::UIKey_Down;
    return 0;
}

} // namespace

EditorShortcutRegistry& EditorShortcutRegistry::instance()
{
    static EditorShortcutRegistry registry;
    return registry;
}

void EditorShortcutRegistry::resetToDefaults()
{
    _bindings = {
        {"file.save", L"Save Scene", L"Ctrl+S", L"Ctrl+S"},
        {"edit.undo", L"Undo", L"Ctrl+Z", L"Ctrl+Z"},
        {"edit.redo", L"Redo", L"Ctrl+Y", L"Ctrl+Y"},
        {"edit.delete", L"Delete Selection", L"Delete", L"Delete"},
        {"dsl.compile", L"Compile Script", L"F7", L"F7"},
        {"play.toggle", L"Play or Resume", L"F5", L"F5"},
        {"play.pause", L"Pause", L"F6", L"F6"},
        {"play.stop", L"Stop", L"Shift+F5", L"Shift+F5"},
    };
    for (auto& binding : _bindings) {
        (void)parse(binding.shortcut, binding.modifiers, binding.keyCode);
    }
}

bool EditorShortcutRegistry::parse(
    const std::wstring& shortcut, std::uint8_t& modifiers, int& keyCode)
{
    modifiers = 0;
    keyCode = 0;
    if (shortcut.empty()) return true;
    std::vector<std::wstring> tokens;
    std::wstring token;
    for (wchar_t c : shortcut) {
        if (c == L'+') {
            if (token.empty()) return false;
            tokens.push_back(token);
            token.clear();
        } else if (c != L' ' && c != L'\t') {
            token.push_back(c);
        }
    }
    if (token.empty()) return false;
    tokens.push_back(token);
    for (std::size_t index = 0; index + 1u < tokens.size(); ++index) {
        if (iequals(tokens[index], "shift")) modifiers |= 0x01u;
        else if (iequals(tokens[index], "ctrl")
                 || iequals(tokens[index], "control")) modifiers |= 0x02u;
        else if (iequals(tokens[index], "alt")
                 || iequals(tokens[index], "option")) modifiers |= 0x04u;
        else return false;
    }
    keyCode = parseKey(tokens.back());
    return keyCode != 0;
}

const EditorShortcutBinding* EditorShortcutRegistry::find(
    std::string_view commandId) const
{
    const auto found = std::find_if(_bindings.begin(), _bindings.end(),
        [commandId](const EditorShortcutBinding& binding) {
            return binding.commandId == commandId;
        });
    return found == _bindings.end() ? nullptr : &*found;
}

std::wstring EditorShortcutRegistry::shortcutFor(
    std::string_view commandId) const
{
    const EditorShortcutBinding* binding = find(commandId);
    return binding != nullptr ? binding->shortcut : std::wstring{};
}

std::string EditorShortcutRegistry::commandFor(
    int keyCode, std::uint8_t modifiers) const
{
    modifiers &= 0x07u;
    for (const auto& binding : _bindings) {
        if (binding.keyCode == keyCode && binding.modifiers == modifiers) {
            return binding.commandId;
        }
    }

    const EditorShortcutBinding* redo = find("edit.redo");
    if (redo != nullptr && redo->shortcut == redo->defaultShortcut
        && keyCode == ayt::ui::UIKey_Z && modifiers == 0x03u) {
        return "edit.redo";
    }
    return {};
}

bool EditorShortcutRegistry::setShortcut(
    std::string_view commandId, const std::wstring& shortcut,
    std::string* error)
{
    auto found = std::find_if(_bindings.begin(), _bindings.end(),
        [commandId](const EditorShortcutBinding& binding) {
            return binding.commandId == commandId;
        });
    if (found == _bindings.end()) {
        if (error) *error = "unknown editor command: " + std::string(commandId);
        return false;
    }
    std::uint8_t modifiers = 0;
    int keyCode = 0;
    if (!parse(shortcut, modifiers, keyCode)) {
        if (error) *error = "invalid shortcut for " + std::string(commandId);
        return false;
    }
    if (keyCode != 0) {
        for (const auto& candidate : _bindings) {
            if (&candidate != &*found && candidate.keyCode == keyCode
                && candidate.modifiers == modifiers) {
                if (error) {
                    *error = "shortcut conflict: " + std::string(commandId)
                        + " and " + candidate.commandId;
                }
                return false;
            }
        }
    }
    found->shortcut = shortcut;
    found->modifiers = modifiers;
    found->keyCode = keyCode;
    return true;
}

bool EditorShortcutRegistry::loadOverrides(
    const std::string& path, std::string* error)
{
    std::ifstream input(path);
    if (!input) return true;
    try {
        nlohmann::json root;
        input >> root;
        const auto& bindings = root.contains("bindings")
            ? root.at("bindings") : root;
        if (!bindings.is_object()) {
            if (error) *error = "shortcut file must contain a bindings object";
            return false;
        }
        for (auto it = bindings.begin(); it != bindings.end(); ++it) {
            if (!it.value().is_string()) continue;
            if (!setShortcut(it.key(), widenAscii(it.value().get<std::string>()),
                             error)) {
                return false;
            }
        }
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }
}

} // namespace ayt::editor
