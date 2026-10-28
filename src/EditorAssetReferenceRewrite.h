#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace ayt::editor::detail {

struct ReferencePathChange {
    std::string from;
    std::string to;
};

struct ReferenceRewriteResult {
    std::string text;
    std::size_t replacements = 0;
    std::string error;

    explicit operator bool() const noexcept { return error.empty(); }
};

// Rewrites only whole JSON string values in recognized reference fields.
// Every lookup is against the original input, so overlapping moves cannot
// rewrite an output twice. Unknown exact path values fail closed.
ReferenceRewriteResult rewriteAssetReferences(
    std::string_view text, std::span<const ReferencePathChange> changes);

// Non-JSON authoring needs a format-specific adapter. Detect a possible old
// path before moving the file rather than silently leaving a stale reference.
bool containsPathToken(std::string_view text, std::string_view path) noexcept;

} // namespace ayt::editor::detail
