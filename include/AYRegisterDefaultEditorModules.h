#pragma once
// AYRegisterDefaultEditorModules.h — Editor shell default assembly (Step 1)
//
// See AYApplication/docs/engine-host.md §2.2.

namespace ayt::editor
{

/// Entity full bootstrap + Network + Renderer + Script.
/// Does not create DeviceManager or wire Script input (EditorApp owns that).
void registerDefaultEditorModules();

} // namespace ayt::editor
