#pragma once
// AYEditor/RegisterDefaultEditorModules.h — Editor shell default assembly (Step 1)
//
// See AYApplication/docs/engine-host.md §2.2.

#include <AYModule/ModuleTypes.h>

namespace ayt::app
{
class EngineModuleRuntime;
}

namespace ayt::editor
{

struct EditorModuleOptions {
    /// When false, skip AudioSubSystem (CLI `-no-audio`).
    bool enableAudio = true;
};

/// Compatibility direct-registration path: Entity + Renderer + Physics +
/// Network + Script (+ optional Audio). New EditorApp startup uses the graph
/// configurator below.
void registerDefaultEditorModules();
void registerDefaultEditorModules(const EditorModuleOptions& options);

/// Add the default runtime modules to the Editor startup graph. Device remains
/// Editor-owned because its WindowManager is part of the editor shell.
[[nodiscard]] ayt::module::ModuleResult configureDefaultEditorModules(
    ayt::app::EngineModuleRuntime& runtime,
    const EditorModuleOptions& options);

} // namespace ayt::editor
