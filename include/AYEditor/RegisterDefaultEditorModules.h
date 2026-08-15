#pragma once
// AYEditor/RegisterDefaultEditorModules.h — Editor shell default assembly (Step 1)
//
// See AYApplication/docs/engine-host.md §2.2.

namespace ayt::editor
{

struct EditorModuleOptions {
    /// When false, skip AudioSubSystem (CLI `-no-audio`).
    bool enableAudio = true;
};

/// Entity full bootstrap + Network + Script (+ optional Audio) + Physics.
/// Does not create DeviceManager or wire Script input (EditorApp owns that).
void registerDefaultEditorModules();
void registerDefaultEditorModules(const EditorModuleOptions& options);

} // namespace ayt::editor
