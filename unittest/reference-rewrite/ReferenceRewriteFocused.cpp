#include "EditorAssetReferenceRewrite.h"

#include <array>
#include <cstdio>
#include <string_view>

using ayt::editor::detail::ReferencePathChange;
using ayt::editor::detail::rewriteAssetReferences;

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

} // namespace

int main() {
    const std::array rename{
        ReferencePathChange{"Assets/a.png", "Assets/aa.png"},
        ReferencePathChange{"a.png", "aa.png"},
    };
    const auto result = rewriteAssetReferences(
        R"({"texture":"Assets/a.png","otherTexture":"a.png","note":"data.png","noteExact":"a.png"})",
        rename);
    expect(static_cast<bool>(result), "valid JSON reference rewrite");
    expect(result.replacements == 2, "only two reference values changed");
    expect(result.text ==
        R"({"texture":"Assets/aa.png","otherTexture":"aa.png","note":"data.png","noteExact":"a.png"})",
        "no cascade or prose corruption");

    const std::array overlap{
        ReferencePathChange{"a.png", "aa.png"},
        ReferencePathChange{"aa.png", "aaa.png"},
    };
    const auto simultaneous = rewriteAssetReferences(
        R"({"textures":["a.png","aa.png"]})", overlap);
    expect(static_cast<bool>(simultaneous), "overlap rewrite accepted");
    expect(simultaneous.text == R"({"textures":["aa.png","aaa.png"]})",
           "multiple moves read only original values");

    const auto unknown = rewriteAssetReferences(
        R"({"unregisteredField":"a.png"})", rename);
    expect(!unknown, "unknown exact path field fails closed");
    const auto unicode = rewriteAssetReferences(
        R"({"texture":"\u0061.png"})", rename);
    expect(!unicode, "unicode-escaped possible reference fails closed");
    expect(ayt::editor::detail::containsPathToken("data.png", "a.png") == false,
           "path token search respects name boundaries");
    expect(ayt::editor::detail::containsPathToken("load(\"a.png\")", "a.png"),
           "script reference is reported for a format adapter");

    return failures == 0 ? 0 : 1;
}
