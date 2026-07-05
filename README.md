# AYEditor

Minimal editor product layer for AY Engine — toolbar, panels, Edit/Play modes, and viewport composition.

**Status:** E2-composite — single-window bgfx UI + viewport sub-rect via `AYUIRenderBackend`

## Quick links

| Topic | Document |
|-------|----------|
| Phases E0–E4+ | [design.md §3](design.md#3-phase-roadmap) |
| Interim vs target composite | [design.md §2.3](design.md#23-viewport-presentation-interim-vs-target) |
| AYDevice timing | [design.md §2.4](design.md#24-aydevice-when-to-introduce) |
| Edit vs Play | [design.md §2.1](design.md#21-mode-matrix) |
| Editor UI JSON | [design.md §5](design.md#5-editor-chrome-ayui) |
| Inspector / scene save (later) | [design.md §8](design.md#8-deferred-metadata--serialization) |
| AYUI chrome contract | [AYUI/design.md §13](../AYUI/design.md#13-editor-chrome) |

## Current milestone

**E2-composite — `AYEditorShell_Demo`:** `EditorApp` + `DeviceManager`; `UIRenderBackend` draws chrome; 3D in viewport sub-rect on the same swap chain.

**Next:** E3 cleanup (`BuildType::Editor`, unified entry) or E4+ (Inspector / scene I/O).
