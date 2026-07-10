# AYEditor Cache Path Convention

Status: **frozen** — ED-01 acceptance (2026-07-09).
Owner: Editor team.
Supersedes: ad-hoc hard-coded paths previously scattered in
`AYEditorPlayRuntime::resolvePersistentCacheRoot` and `AYImporter` callers.

## Scope

This convention pins down the on-disk layout that the editor writes under
when it imports assets, bakes previews, or caches compiled shaders.
It does **not** cover the engine-side `ayt::resource::setAssetRoot` global
(see `AYResource/docs/runtime-conventions.md` §3) — that one is for
runtime asset resolution, not editor cache layout. The editor never
calls `setAssetRoot`.

## Root

The cache root is **`<exeDir>/ayeditor_cache/`** — i.e. the directory that
contains the running `AYEditorShell_Demo.exe` (or, in future, the unified
`AYEditorShell.exe`). On Windows, this is whatever `GetModuleFileNameA(nullptr)`
returns with the filename stripped. On failure to resolve, the root falls
back to the literal relative path `ayeditor_cache\` (working-directory-relative).

> The cache lives next to the executable because the editor binary is what
> ships to designers; designers do not have an engine-source checkout.
> Migrating to a project-relative path requires the `AYProject` abstraction
> (Foundation Plan §S-02) — once that lands, this convention is the
> fallback for a project with no explicit cache root.

## Layout

```
<exeDir>/
└── ayeditor_cache/
    ├── assets/              ← Importer writes here; engine reads here
    │   ├── meshes/             (created on demand by FBXConverter)
    │   ├── materials/          (created on demand by FBXConverter)
    │   ├── skeletons/          (created on demand by FBXConverter)
    │   ├── animations/         (created on demand by FBXConverter)
    │   ├── textures/           (created on demand by FBXConverter)
    │   └── <basename>.aydep.json   ← sidecar dependency graph per import
    │
    ├── shaders/             ← compiled .phosc shader cache (AYShader owns)
    │   └── *.phosc
    │
    └── shader_dump/         ← AYShader debug: human-readable disasm
        └── *.txt
```

### assets/ subdirectories

`meshes/`, `materials/`, `skeletons/`, `animations/`, `textures/` are
**created on demand by FBXConverter / GLTFConverter / TextureConverter**
when they emit their first file of that type. The editor does not
pre-create them — if an import only produces a mesh, no `materials/`
directory will appear.

### `<basename>.aydep.json`

For every imported FBX/GLTF source, FBXConverter writes a sidecar file
next to the imported assets (same stem as the source) describing the
mesh → material → texture and mesh → skeleton → animation dependency
edges. `ResourceManager` consumes this sidecar via `AYLooseDependency`
to load loose-file dependencies in the right order. Sidecar lives in
`assets/` at the top level, **not** inside the per-type subdirs.

### shaders/ and shader_dump/

Owned by AYShader, not the editor. The editor only wires the paths
during `EditorPlayRuntime::syncRendererBootstrap()`. Anything written
into these subdirs is an internal AYShader concern and is opaque to
the editor.

## Callers

| Caller | Path passed | Reference |
|---|---|---|
| `EditorPlayRuntime::ensureAssets` (bootstrap cube/material/texture) | `<cacheRoot>/assets/cube.aymesh` etc. | `AYEditorPlayRuntime.cpp:188-198` |
| `EditorPlayRuntime::syncRendererBootstrap` | `<cacheRoot>/shaders`, `<cacheRoot>/shader_dump` | `AYEditorPlayRuntime.cpp:243-248` |
| `Importer::importFile(srcPath, destDir)` | caller passes `<cacheRoot>/assets/` | `AYImporter.cpp:64-103` |

## Rules

1. **Never put user-source files in the cache.** The cache is
   derived/regenerable. Originals (`.fbx`, `.png`, `.json`) belong in
   the project source tree (when that exists) or wherever the user
   keeps them.

2. **Never use absolute paths in serialized assets.** `.aymesh` /
   `.ayskel` / `.ayanm` reference dependencies via relative paths
   (`materials/hero.aymat`, `textures/albedo.aytex`) per
   `AYResource/docs/runtime-conventions.md` §3. The cache root is
   the implicit prefix the loader assumes via the engine asset root.

3. **Idempotent bootstrap.** `EditorPlayRuntime::ensureAssets` must
   skip files that already exist (`if (!fileExists(...))`) so a
   second launch does not re-bake the cube material. New importers
   in this cache should follow the same pattern.

4. **No per-asset hash subdirs.** The R3 / Foundation plan discusses
   content-addressable subdirs for shipping builds (`.pak` path).
   In the editor cache we use **flat names with GUIDs** baked into
   the file header — duplicate-imports dedupe by GUID rewrite, not
   by directory structure.

5. **Cross-platform: use `ayt::io::path::join` and `normalize`.**
   Do not hard-code `\\` in new code paths. The existing
   `resolvePersistentCacheRoot` does this because it must work
   before `AYIO` has migrated its `path` namespace — leave as-is
   for now; new code paths use `ayt::io::path::*`.

## Migration path

When `AYProject` ships (Foundation Plan §S-02), `resolvePersistentCacheRoot`
gains a sibling `resolveProjectCacheRoot(projectPath)` that returns
`<projectPath>/.ayeditor_cache/` instead of `<exeDir>/ayeditor_cache/`.
Until then, the above convention is canonical.

## Versioning

| Version | Date | Change |
|---|---|---|
| 1.0 | 2026-07-09 | ED-01 acceptance; freeze initial layout |