# AYEditor localization

AYEditor uses embedded localization keys with external translation catalogs.
The UI layout always keeps readable English fallback text, so an incomplete or
missing catalog cannot make controls blank.

## Asset contract

- Catalogs live in `EngineAssets/AYEditor/Localization/<locale>.json`.
- Each catalog declares its locale in `_metadata.language`.
- Editor keys use the `ui.editor.*` namespace and are stable identifiers, not
  English source sentences.
- Layout properties use a sibling key: `textKey` + `text`, `titleKey` +
  `title`, `acceptTextKey` + `acceptText`, and the corresponding accessibility
  properties. Item collections use parallel `itemsKey` and `items` arrays.
- Lookup order is selected locale, base locale, `en-US`, then the inline
  fallback value.

Example:

```json
{
  "type": "Button",
  "textKey": "ui.editor.action.add",
  "text": "Add"
}
```

## Runtime behavior

At startup the product host loads every packaged catalog. The persisted
`Editor.Appearance.Language` setting accepts a locale such as `zh-CN`; the
default value `system` chooses the closest packaged locale from the Windows
user locale. The compatibility/headless initializer remains pinned to
`en-US` so existing tests and tools are deterministic.

The migrated resource slice covers the editor shell's fixed controls,
accessibility labels, render options, asset/console/network chrome, and the
fixed UI Flow editor chrome. Existing inline strings remain valid and can be
migrated incrementally without blocking feature work.

`EditorSession::setLanguage()` switches catalogs without rebuilding the UI.
`UILayoutLoader::retranslate()` walks existing widgets and preserves list,
combo-box, and tile-view selection and scroll state. Editor-owned child windows
are refreshed in the same operation.

Runtime-generated labels use numbered placeholders (`{0}`, `{1}`, ...). The
first formatted slice includes document/scene titles, render-setting values,
and network HP. Placeholder names and counts must match across catalogs.

Validate catalog parity, layout references, and English fallback drift with:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/validate_editor_localization.ps1
```

An in-editor language picker remains a separate UI task; hosts can already call
`EditorSession::setLanguage("zh-CN")` and the selected value is persisted.
