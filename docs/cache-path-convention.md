# AYEditor Cache Path Convention

Status: **active** — ED-01 acceptance, project-root amendment (2026-08-31).
Owner: Editor team.
Supersedes: ad-hoc hard-coded paths previously scattered in
`AYEditorPlayRuntime::resolvePersistentCacheRoot` and `AYImporter` callers.

## Scope

This convention pins down the on-disk layout that the editor writes under
when it imports assets, bakes previews, or caches compiled shaders.
It does **not** cover the engine-side `ayt::resource::setAssetRoot` global
(see `AYResource/docs/runtime-conventions.md` §3) — that one is for
runtime asset resolution, not editor cache layout. `EditorPlayRuntime` wires
that runtime root to this cache's `assets/` directory after resolving it.

## Root

The primary cache root is **`<project>/.ayeditor_cache/`** whenever
`AYProject::Project` has an opened root. This keeps imports, preview assets and
shader output with the project and makes `.ayeditor_cache/assets` the Content
Browser's `Imported` root.

For hosts/tests that have not opened a project, the compatibility fallback is
**`<exeDir>/ayeditor_cache/`**. On Windows this is resolved with
`GetModuleFileNameA(nullptr)`; if that also fails, the literal relative path
`ayeditor_cache\` is used.

## Layout

```
<project>/
└── .ayeditor_cache/
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

Editor metadata that is safe to delete but is not an imported runtime asset is
kept in a separate tree:

```
<project>/.ayeditor/
├── cache/
│   ├── asset-index.tsv
│   └── previews/*.aypreview
├── recovery/
└── trash/<transaction>/manifest.tsv
```

`.ayeditor_cache` remains the runtime/import output root. `.ayeditor/cache`
contains only editor indexing and thumbnail acceleration data; recovery and
trash are transaction stores and must not be treated as ordinary cache during
an editor session.

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

5. **Cross-platform: use `std::filesystem` or `ayt::io::path`.**
   New code must not construct project-relative paths by string concatenation.
   A trailing preferred separator is retained only at the legacy string API
   boundary used by existing converter/bootstrap callers.

## Migration path

The AYProject migration is complete: `resolvePersistentCacheRoot()` consults
the process-wide project path authority first and retains the executable-local
root only as a compatibility fallback. A future cache service may split
user-imported products from transient preview/shader products without changing
the Content Browser's `Imported` virtual root.

## Versioning

| Version | Date | Change |
|---|---|---|
| 1.0 | 2026-07-09 | ED-01 acceptance; freeze initial layout |
| 1.1 | 2026-08-31 | Prefer `<project>/.ayeditor_cache`; expose `assets/` as Content Browser `Imported` |
