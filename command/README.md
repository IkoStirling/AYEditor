# AYEditorCommandCore

`AYEditorCommandCore` is the dependency-light undo/redo foundation shared by
Aliyat authoring surfaces. It is a static CMake target inside the AYEditor
source repository, but it does not depend on AYEditor, AYUI, World, windows,
or rendering.

## Ownership boundary

- This target owns `IEditorCommand`, `EditorCommandHistory`, transactions,
  merge rules, save cursors, bounded retention, and failure/lifetime handling.
- AYEditor owns `EditorCommandRouter`, active-document routing, shortcuts,
  menus, and document dirty presentation.
- Scene, GameFlow, UIFlow, UI Layout, and other editors own their domain command
  classes and one history instance per open document.

Include the public API with:

```cpp
#include <AYEditorCommand/EditorCommandHistory.h>
```

Link the consuming authoring target to `AYEditor::CommandCore` (the concrete
target remains `AYEditorCommandCore`).

## Command contract

`execute()` and `undo()` return `false` without changing the model when an
operation cannot be completed. A command that references document-owned data
uses stable handles or weak lifetime state and returns `false` from `isAlive()`
after the owner expires.

`cancelTransaction()` applies undo to the pending transaction. If cancellation
fails, it restores already-undone commands and keeps the transaction active so
the caller can retry or discard the document history. `discardHistory()` only
destroys command objects; it is the reset operation for close, reload, or World
replacement where the old model must not be touched.

The default history capacity is 256 commands. Trimming keeps a save cursor when
the retained baseline still represents that saved state; otherwise the history
remains dirty until the next successful save.
