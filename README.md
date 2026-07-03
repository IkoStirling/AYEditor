# AYEditor

Minimal editor product layer for AY Engine — toolbar, panels, Edit/Play modes, and viewport composition.

**Status:** E3 — `EditorApp` + AYDevice `WindowManager` host window; child viewport via `createChildWindow`

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

**E3 — `AYEditorShell_Demo`:** `EditorApp` drives `DeviceManager::pollEvents`; `WindowManager` owns host + child viewport HWND; GDI chrome unchanged from E2-interim.

**Next:** E2-composite (AYUI U2 single-window) or E4+ (Inspector / scene I/O).
