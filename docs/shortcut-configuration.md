# Editor shortcut configuration

AYEditor loads the engine defaults from
`EngineAssets/AYEditor/config/editor_shortcuts.json`. A project can override
individual bindings in `.ayeditor/editor_shortcuts.json` using the same
`bindings` object. Project files only need to list changed commands.

```json
{
  "version": 1,
  "bindings": {
    "file.save": "Ctrl+S",
    "play.toggle": "F5"
  }
}
```

Supported chords use any combination of `Ctrl`, `Shift`, and `Alt`, followed
by a letter, digit, `F1` through `F12`, `Delete`, `Space`, `Enter`, `Tab`,
`Escape`, or an arrow key. AYEditor rejects a project override when it would
give two commands the same chord; the remaining bindings stay usable.
