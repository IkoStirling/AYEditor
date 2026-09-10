#pragma once

#include <cstdint>

// AYEditor is linked statically today, but several public structs and virtual
// interfaces cross module boundaries. Keep one source-interface number in the
// target and every public consumer so stale object files fail at compile/link
// time instead of surfacing as a vtable or layout crash.
#ifndef AYEDITOR_SOURCE_ABI_VERSION
#define AYEDITOR_SOURCE_ABI_VERSION 9
#endif

static_assert(AYEDITOR_SOURCE_ABI_VERSION == 9,
              "AYEditor headers and target disagree; perform a full rebuild.");

#define AYEDITOR_STRINGIZE_IMPL(value) #value
#define AYEDITOR_STRINGIZE(value) AYEDITOR_STRINGIZE_IMPL(value)
#if defined(_MSC_VER)
#pragma detect_mismatch("AYEditor.SourceABI", AYEDITOR_STRINGIZE(AYEDITOR_SOURCE_ABI_VERSION))
#endif

namespace ayt::editor {

inline constexpr std::uint32_t kEditorSourceAbiVersion =
    AYEDITOR_SOURCE_ABI_VERSION;
inline constexpr const char* kEditorVersion = "0.2.0";

} // namespace ayt::editor

#undef AYEDITOR_STRINGIZE
#undef AYEDITOR_STRINGIZE_IMPL
