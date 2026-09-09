# AYEditor Project Workflow

Status: **implemented baseline** (2026-09-09).

This document records the project-facing behavior shared by `AYEditor` and
`AYEditorShell_Demo`. Both applications host the same `EditorSession`; the
features below are not demo-specific copies.

## Content Browser operations

- Rename, Move, and Copy operate inside the open project's `Assets` and
  `.ayeditor_cache/assets` trees.
- Rename and Move update portable references in authored text assets before
  completing the transaction. If an update fails, moved files and already
  edited references are rolled back.
- Delete first analyzes references and asks for confirmation. Files are moved
  into `.ayeditor/trash/<transaction>/`; `manifest.tsv` makes the latest
  transaction restorable after restarting the editor.
- Raster and supported cooked-resource thumbnails are decoded asynchronously.
  Their disk cache is stored below `.ayeditor/cache/previews`; source size and
  modification time invalidate stale entries.
- `.ayeditor/cache/asset-index.tsv` stores the scanned file metadata and import
  status. Startup restores this index before the first directory walk, then
  recursive `AYIO::FileWatcher` subscriptions patch changed files and import
  dependency state in place. Source models appear as `Needs import`, `Ready`,
  or `Failed` in the asset Inspector. Generated assets are `Ready`.
- Imports run through one sequential background queue. The Content Browser
  remains responsive, reports progress and the converter's failure reason,
  supports forced retry, and exposes **Tools -> Reimport Changed Source
  Assets** for dependency invalidations.
- Move and Copy choose an existing project directory. The Project Trash view
  can restore individual delete transactions or permanently clear selected
  transactions after confirmation. Rename, Move, Copy, Delete, Restore,
  Purge, and Recovery actions are recorded in the resource-operation history.

All serialized references remain slash-normalized project paths. Absolute
paths may be used transiently for loading and previews but are not written to
authored Scene data.

## Audio and Timeline

Tools -> Audio opens an embedded mixer panel with master and bus gain controls.
The same panel can open the full standalone audio editor when deeper tooling is
needed.

Opening an Animation or Audio asset creates a timed document and opens the
shared Timeline panel. Timeline sources expose typed tracks, clips, keyframes,
waveform envelopes, duration, position, and playback state through
`IEditorTimelineSource`. The panel can add, move and remove clips/keyframes,
undo/redo edits, scrub, and save a portable `<asset>.timeline.json` sidecar.
Imported PCM audio registers a real AYAudio clip; its device voice-frame
cursor drives the playhead and supports exact seek when the user scrubs.

## Run current project

The rocket button and Tools -> Run Current Project first honor the local
`<project>/.ayeditor/run.json` override, then read the committed
`<project>/project.ayproject.json`. Paths are relative to the project root
unless absolute paths are explicitly supplied.

```json
{
  "executable": "out/build/windows-client-debug/BSimmer.exe",
  "workingDirectory": ".",
  "arguments": ["--scene", "Assets/worlds/main.ayscene"]
}
```

If both files are absent, the editor searches conventional build output
folders for an executable named after the project directory.

The canonical project descriptor also records `paths.assets`,
`paths.gameAssembly`, `paths.gameCode`, `startupWorld`, and per-World Scene,
UI, and Tilemap references. Validation rejects a malformed descriptor or a
missing referenced content file. The runtime module graph remains in the C++
`GameProject` composition root because callbacks cannot be represented by a
data file.

## Validation profiles

`AYProjectContentValidationCore` is an application-layer tool library kept out
of `AYApplication` itself. `AYProjectContentValidator` loads every `.ayscene`,
`.ui.json`, and `.aytilemap`/`.aytilemap.json` from an actual selected build
profile. Headless validation checks UI structure without constructing widgets;
Full Client validation also builds the tree through `UILayoutLoader`.
Renderer-free `AYEntity2DComponents` metadata lets headless builds deserialize
the same mixed 2D/3D Scene files without linking 2D render systems.

Saving a Tilemap authoring document below project `Assets` also cooks the
matching `Assets/tilemaps/<name>.aytilemap`. Scene components reference the
cooked file; the editor keeps `.aytilemap.json` as the editable source.

## Interface compatibility

AYEditor 0.2.0 publishes source ABI version 4. AYUI 1.1.0 publishes source ABI
version 111. MSVC object files embed link mismatch records and the public
headers statically check the target-provided version, so a public layout or
vtable change requires a full rebuild instead of allowing mixed stale objects.
The editor extension registry remains an in-process compile-time registry; this
versioning does not claim a stable binary plugin ABI.

Repository verification remains split by assembly:

- `windows-headless-debug` builds without Device, Renderer, Audio, or AY2D
  runtime systems, while retaining renderer-free 2D component schemas for
  Scene validation.
- `windows-client-debug` builds the complete runtime, including UI, 2D,
  Renderer, and Audio, without editor tooling.
- `windows-debug` builds the editor and runs the project-content validator,
  Timeline/Audio adapters, resource workflow, recovery, and Gizmo tests.

Every preset inherits `windows-base` and uses the single
`out/build/vcpkg_installed` directory.

## Autosave and crash recovery

The editor writes dirty in-memory Scene, UI Layout, Tilemap, Phoskia, and Logia
documents every 30 seconds. File -> Autosave Now triggers the same operation.
Recovery data lives below `.ayeditor/recovery` and is removed on a clean editor
shutdown.

When an old session lock is found, its transaction is preserved as
`.ayeditor/recovery-crash-<timestamp>`. Startup presents each recoverable
document separately; selected files can be restored while the rest remain for
later. Every replaced file is kept beside it with the `.before-recovery`
suffix.
