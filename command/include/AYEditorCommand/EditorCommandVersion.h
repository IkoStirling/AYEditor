#pragma once

#include <cstdint>

#ifndef AYEDITOR_COMMAND_SOURCE_ABI_VERSION
#define AYEDITOR_COMMAND_SOURCE_ABI_VERSION 1
#endif

static_assert(AYEDITOR_COMMAND_SOURCE_ABI_VERSION == 1,
              "AYEditorCommand headers and target disagree; rebuild consumers.");

#define AYEDITOR_COMMAND_STRINGIZE_IMPL(value) #value
#define AYEDITOR_COMMAND_STRINGIZE(value) AYEDITOR_COMMAND_STRINGIZE_IMPL(value)
#if defined(_MSC_VER)
#pragma detect_mismatch("AYEditorCommand.SourceABI", AYEDITOR_COMMAND_STRINGIZE(AYEDITOR_COMMAND_SOURCE_ABI_VERSION))
#endif

namespace ayt::editor {

inline constexpr std::uint32_t kEditorCommandSourceAbiVersion =
    AYEDITOR_COMMAND_SOURCE_ABI_VERSION;

} // namespace ayt::editor

#undef AYEDITOR_COMMAND_STRINGIZE
#undef AYEDITOR_COMMAND_STRINGIZE_IMPL
