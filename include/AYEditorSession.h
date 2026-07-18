#pragma once

#include "AYEditorGameView.h"
#include "AYEditorPlayRuntime.h"
#include "AYImportedCharacterMapper.h"
#include "AYImportDialog.h"
#include "AYImporter.h"
#include "AYInspectorOverrides.h"
#include "AYUIManager.h"

#include "aymath/MathTypes.h"

#include <functional>
#include <string>

struct HWND__;
using HWND = HWND__*;

namespace ayt::editor {

// `ImportedCharacter` is defined in `AYEditorPlayRuntime.h` (included
// above). The editor session forwards it straight through to the
// Play runtime; no need to redeclare.

struct EditorSessionDesc {
    ayt::ui::IRenderBackend* uiBackend = nullptr;
    std::string layoutPath;
    HWND hostWindow = nullptr;
    ImportedCharacter importedCharacter;  // empty = fall back to cube
};

class EditorSession {
public:
    EditorSession();
    ~EditorSession();

    EditorSession(const EditorSession&) = delete;
    EditorSession& operator=(const EditorSession&) = delete;

    bool initialize(const EditorSessionDesc& desc);
    bool initialize(ayt::ui::IRenderBackend* backend, const std::string& layoutPath);
    void shutdown();

    void setClientSize(float width, float height);
    void update(float dt);
    void syncViewportIfChanged();
    void render();
    void render(bool skipViewportPanel);

    bool shouldCompositeViewport() const;
    bool ensurePresentationReady();
    bool getViewportBounds(ayt::math::FRectangle& outBounds) const;

    bool onMouseMove(float x, float y);
    bool onMouseButtonDown(float x, float y, int button);
    bool onMouseButtonUp(float x, float y, int button);
    void onMouseLeave();

    bool isUiHoverInteractive() const;
    ayt::ui::UiCursorHint getUiCursorHint() const;

    using RepaintCallback = std::function<void()>;
    void setRepaintCallback(RepaintCallback callback);

    EditorGameView& gameView() { return _gameView; }
    const EditorGameView& gameView() const { return _gameView; }
    ayt::ui::UIManager& ui() { return _ui; }
    const ayt::ui::UIManager& ui() const { return _ui; }

private:
    void bindToolbar();
    // Phase 2a: toolbar Import button handler. Opens the Win32
    // file picker, runs Importer::importFile + the G1 mapper,
    // and pushes the result into EditorPlayRuntime via
    // replaceImportedCharacter (which clears any existing entity
    // and respects the startPlay cube-fallback policy). Empty
    // path from the dialog = user cancelled = no-op.
    void importCharacterFromDialog();

    // ED-03: snapshot the current character entity's path
    // strings into the inspector labels, for use after a
    // hot-swap or pick-and-apply. No-op when no character is
    // currently spawned (writes "No selection" to the title
    // label).
    void refreshInspectorLabels();

    // ED-03: select the live character entity for inspection.
    // Bound to the toolbar [Select] button. If no character is
    // currently spawned, logs and continues (the inspector
    // stays in "No selection" state). Triggers a label refresh
    // so the inspector mirrors the just-selected entity.
    void selectCharacter();

    // ED-03: commit the picked paths to the live character
    // (and to the runtime's pending-overrides buffer so a
    // future spawn re-applies them). Bound to [Apply].
    void applyInspectorOverrides();

    // ED-03: clear pending overrides (= reset path picks back
    // to the ImportedCharacter-derived defaults). Bound to
    // [Reset]. Refreshes inspector labels after.
    void resetInspectorOverrides();

    // ED-03: pending paths picked via Pick Skel / Pick Anim
    // buttons. Both empty = nothing to Apply. Apply wraps
    // these into an EntityInspectorOverrides and forwards
    // through selectCharacter / _playRuntime.
    void pickInspectorSkeleton();
    void pickInspectorAnimation();

    // ED-03: thin setter for the inspector's staged override
    // fields, called from pickInspector{Skel,Anim} after the
    // Win32 dialog returns a path.
    void setInspectorSkeletonPath(const std::string& path);
    void setInspectorAnimationPath(const std::string& path);

    // ED-03: shared work for [Apply] - build the override
    // struct from staged fields and forward.
    void commitInspectorOverrides(const EntityInspectorOverrides& ov);

    void setModeLabel(const std::wstring& text);
    void onModeChanged(EditorMode mode);
    void syncViewport();
    bool isChromePoint(float x, float y) const;
    bool isSplitHandlePoint(float x, float y) const;
    void clearSplitterHovers();
    void syncSplitterRevealToMouse();

    EditorPlayRuntime _playRuntime;
    EditorGameView _gameView;
    ayt::ui::UIManager _ui;
    HWND _hostWindow = nullptr;
    std::string _layoutPath;
    RepaintCallback _repaintCallback;
    ayt::math::FRectangle _cachedViewportBounds{};
    bool _viewportBoundsCached = false;
    bool _shutdown = false;

    // Last client-space mouse position observed by onMouseMove /
    // onMouseLeave. Used by syncSplitterRevealToMouse() so a missed
    // leave still un-reveals splitters on the next update tick.
    float _lastMouseX = 0.0f;
    float _lastMouseY = 0.0f;
    bool _hasLastMouse = false;

    // ED-03: staged Inspector pick state. Populated by
    // pickInspector{Skel,Anim} via Win32 dialogs; consumed by
    // applyInspectorOverrides to build the EntityInspector
    // Overrides struct. Both empty = nothing staged = [Apply]
    // is a no-op. Reset clears them.
    std::string _inspectorSkelPick;
    std::string _inspectorAnimPick;
};

} // namespace ayt::editor
