#include "EditorAssetReferenceRewrite.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

namespace ayt::editor::detail {
namespace {

enum class ValueRole { Unknown, Reference, Prose };

struct Frame {
    bool array = false;
    ValueRole arrayRole = ValueRole::Unknown;
    std::string pendingKey;
};

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

bool endsWith(std::string_view value, std::string_view suffix) noexcept {
    return value.size() >= suffix.size() &&
        value.substr(value.size() - suffix.size()) == suffix;
}

ValueRole fieldRole(std::string_view field) {
    const auto key = lower(field);
    if (key.starts_with("note") || key.starts_with("comment") ||
        key.starts_with("description") || key.starts_with("tooltip") ||
        key == "text" || key == "title" || key == "label" ||
        key == "name" || key == "help") return ValueRole::Prose;
    if (key == "mesh" || key == "meshes" || key == "texture" ||
        key == "textures" || key == "material" || key == "materials" ||
        key == "atlas" || key == "atlases" || key == "sprite" ||
        key == "sprites" || key == "image" || key == "images" ||
        key == "shader" || key == "shaders" || key == "font" ||
        key == "fonts" || key == "audio" || key == "sound" ||
        key == "scene" || key == "model" || key == "resource" ||
        key == "resources" || key == "references" || key == "file" ||
        endsWith(key, "path") || endsWith(key, "paths") ||
        endsWith(key, "asset") || endsWith(key, "assets") ||
        endsWith(key, "texture") || endsWith(key, "mesh") ||
        endsWith(key, "material") || endsWith(key, "reference"))
        return ValueRole::Reference;
    return ValueRole::Unknown;
}

ValueRole valueRole(const std::vector<Frame>& frames) {
    if (frames.empty()) return ValueRole::Unknown;
    const auto& parent = frames.back();
    return parent.array ? parent.arrayRole : fieldRole(parent.pendingKey);
}

std::string decodeSimpleJsonString(std::string_view raw) {
    std::string decoded;
    decoded.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size() &&
            (raw[i + 1] == '/' || raw[i + 1] == '\\' ||
             raw[i + 1] == '"')) {
            decoded.push_back(raw[++i]);
        } else {
            decoded.push_back(raw[i]);
        }
    }
    return decoded;
}

bool pathBoundary(char c) noexcept {
    const auto value = static_cast<unsigned char>(c);
    return !std::isalnum(value) && c != '_' && c != '-' && c != '.' &&
        c != '/' && c != '\\';
}

} // namespace

bool containsPathToken(std::string_view text, std::string_view path) noexcept {
    if (path.empty()) return false;
    for (std::size_t pos = 0;
         (pos = text.find(path, pos)) != std::string_view::npos; ++pos) {
        const bool before = pos == 0 || pathBoundary(text[pos - 1]);
        const auto end = pos + path.size();
        const bool after = end == text.size() || pathBoundary(text[end]);
        if (before && after) return true;
    }
    return false;
}

ReferenceRewriteResult rewriteAssetReferences(
    std::string_view text, std::span<const ReferencePathChange> changes) {
    ReferenceRewriteResult result;
    result.text.reserve(text.size());
    std::unordered_map<std::string, std::string> replacements;
    for (const auto& change : changes) {
        if (change.from.empty() || change.from == change.to) continue;
        const auto [it, inserted] = replacements.emplace(change.from, change.to);
        if (!inserted && it->second != change.to) {
            result.error = "Conflicting asset reference replacements.";
            return result;
        }
    }

    std::vector<Frame> frames;
    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (c == '"') {
            const auto begin = i++;
            bool escaped = false;
            while (i < text.size()) {
                if (!escaped && text[i] == '"') break;
                if (!escaped && text[i] == '\\') escaped = true;
                else escaped = false;
                ++i;
            }
            if (i == text.size()) {
                result.error = "Unterminated asset reference string.";
                return result;
            }
            const auto raw = text.substr(begin + 1, i - begin - 1);
            const auto decoded = decodeSimpleJsonString(raw);
            ++i;
            std::size_t next = i;
            while (next < text.size() && std::isspace(
                    static_cast<unsigned char>(text[next]))) ++next;
            if (next < text.size() && text[next] == ':') {
                if (!frames.empty() && !frames.back().array)
                    frames.back().pendingKey = decoded;
                result.text.append(text.substr(begin, i - begin));
                continue;
            }

            // Unicode escapes may encode any byte of a path. Until this
            // scanner has a full JSON decoder, do not publish a move after
            // seeing one in a possible reference value.
            if (raw.find("\\u") != std::string_view::npos &&
                valueRole(frames) != ValueRole::Prose) {
                result.error = "Unicode-escaped asset value needs a typed rewriter.";
                return result;
            }

            auto normalizedPath = decoded;
            std::replace(normalizedPath.begin(), normalizedPath.end(),
                         '\\', '/');
            const auto found = replacements.find(normalizedPath);
            if (found != replacements.end()) {
                const auto role = valueRole(frames);
                if (role == ValueRole::Unknown) {
                    result.error = "Exact asset path in an unknown JSON field: "
                        + (frames.empty() ? std::string("<root>")
                                          : frames.back().pendingKey);
                    return result;
                }
                if (role == ValueRole::Reference) {
                    result.text.push_back('"');
                    result.text += found->second;
                    result.text.push_back('"');
                    ++result.replacements;
                    if (!frames.empty() && !frames.back().array)
                        frames.back().pendingKey.clear();
                    continue;
                }
            }
            result.text.append(text.substr(begin, i - begin));
            if (!frames.empty() && !frames.back().array)
                frames.back().pendingKey.clear();
            continue;
        }
        if (c == '{' || c == '[') {
            const auto role = valueRole(frames);
            if (!frames.empty() && !frames.back().array)
                frames.back().pendingKey.clear();
            frames.push_back({c == '[', c == '[' ? role : ValueRole::Unknown,
                              {}});
        } else if (c == '}' || c == ']') {
            if (frames.empty() || frames.back().array != (c == ']')) {
                result.error = "Malformed asset reference container.";
                return result;
            }
            frames.pop_back();
        } else if (c == ',' && !frames.empty() && !frames.back().array) {
            frames.back().pendingKey.clear();
        }
        result.text.push_back(c);
        ++i;
    }
    if (!frames.empty()) result.error = "Unclosed asset reference container.";
    return result;
}

} // namespace ayt::editor::detail
