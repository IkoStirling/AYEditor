# AYEditor Workspace Framework

This document defines the AYEditor-side foundation for future editor surfaces.
The DSL and UI Layout editors are migrated production surfaces; Scene, Audio,
and Tilemap remain on their existing integration paths.

## Editor surface kinds

- `Document`: edits a saveable resource. Most documents live in the center
  dock; spatial authoring tools may use an application-owned modeless window
  when sharing the Scene workspace would harm either task.
- `ToolPanel`: persistent or contextual workspace tool such as Inspector,
  Hierarchy, Timeline, Console, or Palette.
- `ModalWorkflow`: short-lived import, settings, and picker workflows.
- Standalone executables are development hosts, not another surface kind.
- Detachment and dedicated windows are host presentation options. Editor
  extensions never create a native window or choose a rendering backend
  themselves; `EditorSession`/window services own those decisions.

## Ownership and dependency direction

```text
module editor core -> no AYEditor/AYUI/window dependency
module AYEditor adapter -> IEditorDocument + IEditorView
AYEditor workspace -> registry/document/commands/selection + Dock host
```

`IEditorView` owns its content widget tree. The workspace only hosts its
non-owning root for the view lifetime. `IEditorDocument` owns persistent model
state and must not contain Widget, HWND, renderer, or input objects.

## State boundaries

- Resource content belongs to the document and its resource serializer.
- Zoom, pan, hierarchy expansion, active tool, and panel dimensions belong to
  future workspace/view-state persistence.
- Hover, capture, drag gestures, playback sampling, and preview overrides are
  transient and must never mark a document dirty.

## Implemented foundation

- `EditorExtensionRegistry`: descriptor registration and resource routing.
- `EditorDocumentManager`: per-resource/singleton/multiple open policy,
  activation, dirty close decisions, and lifecycle events.
- `EditorCommandHistory`: generic undo/redo, save cursor, merge, and transaction.
- `EditorCommandRouter`: active editor command precedence plus global fallback.
- `EditorSelectionService`: typed, stable, per-document selection contexts.
- `EditorWorkspace`: service root synchronizing document and selection lifetime.
- `EditorDockViewHost`: generic `EditorDescriptor::createView` host for DockCard
  ownership, activation, focus command routing, dirty presentation, close
  policy, ticking, document-local pointer/key routing, cursor hints, and
  two-phase UI-safe shutdown.

## First production integration: DSL

The Phoskia/Logia editor is the first real consumer of the framework:

- `EditorDslDocument` implements `IEditorDocument`, including stable type,
  path/title, content revision, dirty state, save, and reload.
- `EditorDslExtension` registers `.phoskia` and `.logia` routing plus a complete
  document/view factory and the `dsl.compile` command target.
- `EditorSession::openDslAsset` delegates document and view creation to
  `EditorDockViewHost`, so the descriptor factory is the Shell's owning path.
  Per-resource deduplication, DockCard activation, command target selection,
  and shutdown cleanup now use the common workspace.
- Focused DSL `Ctrl+S` and `F7` dispatch through `EditorCommandRouter`.
- Dirty close decisions are applied by `EditorDocumentManager`; the existing
  Win32 prompt only gathers the user's Save/Discard/Cancel choice.

`EditorSession` no longer constructs a second DSL widget tree or stores raw DSL
widget aliases. The extension-owned view provides syntax highlighting,
diagnostics, save, compile, undo, and redo behavior. The generic host owns the
view-to-card association and keeps the view alive until both primary and
promoted AYUI trees have been destroyed.

The registry is compile-time/in-process infrastructure. It is not a binary
plugin ABI and does not promise hot-loaded DLL compatibility.

## Second production integration: UI Layout

`EditorUiLayoutExtension` registers `*.ui.json` as a workspace document, but UI
authoring is intentionally not presented as a Scene-center document. Tools ->
UI Layout Editor opens or focuses one modeless **AYUI Designer** top-level tool
window owned by the primary editor HWND. The Scene center remains available for
scene work, and closing the Designer never redocks it as a floating card.

`EditorUiLayoutDocument` still participates in the common title, dirty, save,
close, revision, and per-resource deduplication lifecycle. A shared
`EditorUiLayoutController` binds that document to the existing
`LayoutEditorSession`. The descriptor's generic View remains a compatibility
and test route; the normal Shell route does not use `EditorDockViewHost`.

`EditorChildWindowManager` supplies the owned native window, one UIManager and
backend per window, AYDevice input callbacks, physical-to-logical conversion,
dynamic dirty titles, close veto, and deferred teardown. Canvas/palette gestures
are offered to the Controller before normal UI dispatch. Buttons, inputs,
combos, scrolling, focus, and popups continue through the child UIManager.

The standalone `AYUI_LayoutEditor` remains a module-level regression host. Both
hosts link the editor-only `AYUILayoutEditorCore`; AYEditor no longer compiles a
copy of a demo source. `LayoutDocumentModel`, `LayoutSelectionModel`,
`LayoutCommandStack`, and `LayoutCanvasViewport` own the authoring state while
`LayoutEditorSession` coordinates chrome and gestures. There is no second save,
undo, serialization, or input state machine. `EditorUiLayoutDocument::save`
delegates to its bound Controller and the workspace document contains no Widget,
native-window, or renderer state.

`WidgetAuthoringRegistry` is the common source for palette metadata, SVG icons,
default creation parameters, initialization, and editable property/event schema.
`PropertySchema` generates Inspector row/section visibility. The command stack
records typed edit intents while retaining full-JSON snapshots as the migration
fallback for reliable undo/redo of composite widgets.

The Designer chrome is a modern six-region layout: title/file actions, command
bar, Widget Library plus Document Outline, Canvas, scrolling Inspector, and a
status bar. Fill regions use the loader's single-axis `h=0` contract so the
outline, canvas, and inspector cannot silently collapse to zero size.

The library exposes the common runtime authoring set, including Image,
collection/tree controls, TabStrip/TabControl, Grid/Scroll containers, Window,
Modal, and ModalDialog. Each row uses the shared AYUI SVG icon path and the
same click/drag creation command. Structured content roots such as a tab page,
scroll content, and modal body are fixed outline slots: widgets may be dropped
into them, but generic hierarchy reorder never detaches the slot itself.

Texture browsing is host-injected. The child host decodes PNG/JPEG/BMP/TGA
previews into a backend-local premultiplied GDI bitmap, while the layout stores
only the texture name. Controller and event-handler names are also authored in
the Inspector and round-trip through AYUI serialization; executable callbacks
remain host-registered C++ behavior and are never read as script from JSON.
The production layout loader reconstructs Scroll, Tab, Modal and Dialog payloads
as authored structures and registers IDs inside inactive tab pages as well as
visible content. Detached composite defaults allocate IDs against both the live
document and their own not-yet-mounted subtree.

## Migration order

1. Move Scene selection and transform commands onto workspace services.
2. Resolve duplicate Tilemap authoring models and host the selected module core.
3. Classify the current Audio mixer as a ToolPanel; a future waveform/resource
   editor is a separate Document editor.
4. Add Timeline as a contextual ToolPanel consuming an active document's future
   timeline-source capability.

Until those migrations occur, the framework must remain additive and existing
`EditorSession` behavior remains the fallback baseline.
