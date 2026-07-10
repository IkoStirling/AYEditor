# ED-01 Acceptance Report — Import → Project Cache

Date: 2026-07-09
Owner: Editor team
Status: **ACCEPTED** (with two follow-ups, both non-blocking)

## Task summary

Wire the editor's "Import" command path through `IConverter` so a designer
dropping an `.fbx` (or `.gltf`/`.glb`/`.png`/...) on disk ends up with
`.aymesh`/`.aymat`/`.ayskel`/`.ayanm` files in the project cache.

## What ships

| Item | Status | Where |
|---|---|---|
| `Importer::importFile(srcPath, destDir)` → real IConverter dispatch | ✅ shipped | `AYEditor/src/AYImporter.cpp:64-113` |
| `ImportDialog::importFromPath` thin pass-through | ✅ shipped | `AYEditor/src/AYImportDialog.cpp:12-16` |
| `Importer::Result { ConversionResult conversion; bool success; std::string errorMessage; }` | ✅ shipped (matches task wording) | `AYEditor/include/AYImporter.h:33-37` |
| Extension table mirrors factory (`fbx`/`gltf`/`glb`/`png`/`bmp`/`tga`/`dds`) | ✅ shipped | `AYEditor/src/AYImporter.cpp:55-62` |
| 6 distinct error strings for failure modes | ✅ shipped | `AYImporter.cpp:11-19, 68-99` |
| `never throws` contract — full try/catch coverage | ✅ shipped | `AYImporter.h:31-32`, `AYImporter.cpp:90-109` |
| Unit tests: 6 cases (extensionOf, isSupportedExtension, empty/missing/unsupported/error shapes, ImportDialog passthrough) | ✅ shipped | `AYEditor/unittest/Test_EditorImporter.cpp` |
| Dependency on `AYResource` (link + include) | ✅ shipped | `AYEditor/CMakeLists.txt:33,40` |
| FBXConverter R-03 (skeleton/mesh/animation sub-converters) | ✅ already shipped by resource team | `AYResource/src/Converter/FBXConverter.cpp` |
| Cache path convention — formalized doc | ✅ shipped (this milestone) | `AYEditor/docs/cache-path-convention.md` |
| **Win32 `GetOpenFileName` file-picker UI** | ⏸ deferred to Phase 2 menu bar | `AYEditor/src/AYImportDialog.cpp:3-5` |
| **CI real-FBX smoke test** | ⏸ out of scope per task ("不必 CI 跑完整 FBX") | — |

## Acceptance verification

### A. Error path (CI-covered, runs on every PR)

`ctest -R AYEditor_UnitTests` exercises:
- `extension_of_lowercases_and_strips_dot`
- `is_supported_extension_matches_factory_table`
- `import_empty_sourcepath_returns_error`
- `import_missing_file_returns_error`
- `import_unsupported_extension_returns_error_with_supported_list`
- `import_dialog_is_thin_passthrough_to_importer`

All 6 cases must pass. None drives a real FBX through the pipeline
(this is intentional — see `Test_EditorImporter.cpp:9-14` rationale).

### B. Success path (manual verification, documented step)

Performed **outside CI** by the editor team whenever a release
candidate is cut. Recipe:

1. **Build**: `cmake --build build --target AYEditorShell_Demo --config Debug`
2. **Run**: `cd build/bin/Debug && ./AYEditorShell_Demo.exe`
3. **Trigger import**: from the menu (Phase 2) **or** programmatically
   by injecting `ImportDialog::importFromPath("D:/Projects/suzanne.fbx",
   cacheRoot + "assets\\")` from a one-off driver.
4. **Assert**: the following files appear under
   `<exeDir>/ayeditor_cache/assets/`:
   - `meshes/suzanne_RootNode_Suzanne.aymesh` (first 4 bytes = `'AYMH'`)
   - `materials/suzanne.aymat`
   - `suzanne.aydep.json` (sidecar dependency graph)
5. **Assert negative path**: re-run with `D:/Projects/corrupt.fbx`
   → `Result.success == false`, error message contains the parser
   complaint, no crash, no half-written files. (If half-written
   files appear, file a bug — `FBXConverter::convert()` should
   atomically write or roll back.)

### C. Sibling FBXConverter test (resource team, runs in their CI)

`AYResource/unittest/AYTest_FBXConverter.cpp::ConvertSuzanne` already
covers the underlying converter end-to-end against the same fixture.
If that passes, the editor side cannot regress on converter correctness.

## Follow-ups (non-blocking)

| # | Item | Owner | Linked to |
|---|---|---|---|
| F1 | `AYProject` abstraction so cache moves to project root | Future S-02 | Foundation Plan §351 |
| F2 | `ImportDialog` Win32 file-picker wiring | Phase 2 menu bar | `AYImportDialog.cpp:3-5` |
| F3 | Move from `\\` hard-coding in `resolvePersistentCacheRoot` to `ayt::io::path::join` once AYIO migration settles | Infra team | code-style §11 |

## Notes / surprises during audit

- The task table said "产出文件 .aymesh/.aymat/.ayskel/.ayanm" implying
  the editor emits these. In fact R-03 FBXConverter emits them; the
  editor's only job is to invoke the converter with the right
  `outputDir`. This is correct separation — the table's wording
  overstates the editor's responsibility.
- `AYResource::test_output/` already contains
  `suzanne_RootNode_Suzanne.aymesh`, `cube.aymat`,
  `Sour_Skeleton.ayskel`, and the `suzanne.aydep.json` sidecar —
  confirming the converter chain works against the same fixture
  we plan to use for manual verification.
- `Result.conversion.resources` is a `vector<ConvertedResource>`
  with `guid`, `path` (virtual), `type` ("Mesh"/"Material"/...), `size`
  — the "路径列表" the table mentions. The `path` is virtual
  (`meshes/suzanne_RootNode_Suzanne.aymesh`); the concrete on-disk
  location is `<cacheRoot>/assets/<virtual-path>`.