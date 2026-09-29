#include "AYEditor/EditorSpriteAnimationExtension.h"

#include "AYEditor/EditorProductPaths.h"
#include "AYEditorSpriteAnimationCanvas.h"

#include <AY2DEditor/SpriteAnimationAuthoringModel.h>
#include <AYUI/Box.h>
#include <AYUI/Button.h>
#include <AYUI/CheckBox.h>
#include <AYUI/ComboBox.h>
#include <AYUI/Panel.h>
#include <AYUI/ScrollView.h>
#include <AYUI/Slider.h>
#include <AYUI/SvgIcon.h>
#include <AYUI/TextInput.h>
#include <AYUI/TextLabel.h>
#include <AYUI/Tooltip.h>
#include <AYUI/UIManager.h>
#include <AYUI/UnicodeText.h>
#include <AYUI/Widget.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ayt::editor {
namespace {

using ayt::ay2d::editor::SpriteAnimationAuthoringModel;
using ayt::ay2d::editor::SpriteAnimationDraft;
using ayt::ay2d::editor::SpriteAnimationPlaybackMode;

class SpriteAnimationToolDocument final : public IEditorDocument {
public:
    const std::string& typeId() const noexcept override { return _type; }
    const std::string& path() const noexcept override { return _path; }
    const std::string& title() const noexcept override { return _title; }
    bool isDirty() const noexcept override { return false; }
    uint64_t revision() const noexcept override { return 1u; }
    bool save(std::string* error) override {
        if (error != nullptr) error->clear();
        return true;
    }
private:
    std::string _type = "ayeditor.tool.sprite-animation.document";
    std::string _path;
    std::string _title = "Sprite Animation";
};

bool parseUInt(const std::wstring& text, uint32_t& value)
{
    if (text.empty()) return false;
    wchar_t* end = nullptr;
    const unsigned long long parsed = std::wcstoull(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != L'\0'
        || parsed > (std::numeric_limits<uint32_t>::max)()) {
        return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

ayt::ui::TextLabel* label(const std::wstring& text, int size = 12)
{
    auto* result = new ayt::ui::TextLabel();
    result->setText(text);
    result->setFontSize(size);
    return result;
}

class SpriteAnimationToolView final
    : public IEditorView,
      public IEditorCommandTarget {
public:
    explicit SpriteAnimationToolView(IEditorHostServices& host)
        : _host(host),
          _sceneHost(dynamic_cast<IEditorSpriteAnimationHost*>(&host)),
          _iconRoot(EditorProductPaths::detect().engineAssetsRoot
                    / "Icons/Tabler")
    {
        auto* root = new ayt::ui::VBox();
        _root = root;
        root->setId("sprite_animation_workspace");
        root->setSpacing(6.0f);
        root->setPadding(8.0f, 7.0f, 8.0f, 8.0f);

        auto* heading = new ayt::ui::HBox();
        heading->setSpacing(8.0f);
        _modeStatus = label(L"NO SPRITE SELECTED", 13);
        _modeStatus->setId("sprite_animation_mode_status");
        heading->addWidget(_modeStatus, 0.0f);
        _sourceLabel = label(L"", 11);
        heading->addWidget(_sourceLabel, 260.0f);
        root->addWidget(heading, 26.0f);

        auto* body = new ayt::ui::HBox();
        body->setSpacing(7.0f);
        root->addWidget(body, 0.0f);

        auto* sourceColumn = new ayt::ui::VBox();
        sourceColumn->setSpacing(5.0f);
        sourceColumn->addWidget(label(L"Source Sheet · drag across cells to select frames", 12),
                                23.0f);
        auto* sheetPanel = new ayt::ui::Panel();
        sheetPanel->setId("sprite_animation_sheet_panel");
        sheetPanel->setPadding(0.0f, 0.0f, 0.0f, 0.0f);
        _sheet = new EditorSpriteAnimationCanvas(false);
        _sheet->setModel(&_model);
        _sheet->setOnSelectionChanged([this]() {
            _dirty = true;
            syncSelectionInputs();
            refreshStatus();
            _host.requestRepaint();
        });
        sheetPanel->addChild(_sheet);
        sourceColumn->addWidget(sheetPanel, 0.0f);
        auto* sheetFooter = new ayt::ui::HBox();
        sheetFooter->setSpacing(5.0f);
        addTextButton(sheetFooter, L"Frame Sheet", 92.0f, [this]() {
            _sheet->frameImage();
            _host.requestRepaint();
        });
        auto* hint = label(L"Wheel: zoom   Middle drag: pan   Left drag: select", 10);
        sheetFooter->addWidget(hint, 0.0f);
        sourceColumn->addWidget(sheetFooter, 28.0f);
        ayt::ui::BoxSlotLimits sourceLimits;
        sourceLimits.minWidth = 430.0f;
        body->addWidget(sourceColumn, 0.0f, sourceLimits);

        auto* inspector = new ayt::ui::VBox();
        inspector->setId("sprite_animation_inspector");
        inspector->setSpacing(6.0f);
        inspector->setPadding(6.0f, 4.0f, 6.0f, 5.0f);
        inspector->addWidget(label(L"Animation Preview", 13), 24.0f);
        auto* previewPanel = new ayt::ui::Panel();
        previewPanel->setId("sprite_animation_preview_panel");
        previewPanel->setPadding(0.0f, 0.0f, 0.0f, 0.0f);
        _preview = new EditorSpriteAnimationCanvas(true);
        _preview->setModel(&_model);
        previewPanel->addChild(_preview);
        inspector->addWidget(previewPanel, 220.0f);

        auto* transport = new ayt::ui::HBox();
        transport->setSpacing(4.0f);
        addIconButton(transport, "outline/arrow-up.svg", L"Previous frame",
            [this]() { _model.stepBackward(); updatePlayhead(); });
        addIconButton(transport, "filled/player-play.svg", L"Play preview",
            [this]() { _model.play(); refreshStatus(); });
        addIconButton(transport, "filled/player-pause.svg", L"Pause preview",
            [this]() { _model.pause(); refreshStatus(); });
        addIconButton(transport, "filled/player-stop.svg", L"Stop preview",
            [this]() { _model.stop(); updatePlayhead(); });
        addIconButton(transport, "outline/arrow-down.svg", L"Next frame",
            [this]() { _model.stepForward(); updatePlayhead(); });
        inspector->addWidget(transport, 30.0f);

        _scrub = new ayt::ui::Slider();
        _scrub->setId("sprite_animation_scrub");
        _scrub->setValueRange(0.0f, 1.0f);
        _scrub->setOnValueChanged([this](float value) {
            if (_syncing) return;
            _model.scrubToOffset(static_cast<uint32_t>(std::lround(value)));
            updatePlayhead(false);
        });
        inspector->addWidget(_scrub, 25.0f);
        _playhead = label(L"Frame 1 / 1", 11);
        inspector->addWidget(_playhead, 20.0f);

        inspector->addWidget(label(L"Grid and Range", 13), 23.0f);
        _columns = addField(inspector, L"Columns", "sprite_animation_columns");
        _rows = addField(inspector, L"Rows", "sprite_animation_rows");
        _firstFrame = addField(inspector, L"First frame", "sprite_animation_first");
        _frameCount = addField(inspector, L"Frame count", "sprite_animation_count");
        _duration = addField(inspector, L"Frame time (ms)", "sprite_animation_duration");

        auto* modeRow = new ayt::ui::HBox();
        modeRow->setSpacing(6.0f);
        modeRow->addWidget(label(L"Playback", 11), 98.0f);
        _playbackMode = new ayt::ui::ComboBox();
        _playbackMode->setId("sprite_animation_playback_mode");
        _playbackMode->setItems({L"Loop", L"Once"});
        _playbackMode->setSelectedIndex(0);
        _playbackMode->setOnSelectionChanged([this](int) {
            controlsChanged();
        });
        modeRow->addWidget(_playbackMode, 0.0f);
        inspector->addWidget(modeRow, 29.0f);

        _startsPlaying = new ayt::ui::CheckBox();
        _startsPlaying->setId("sprite_animation_starts_playing");
        _startsPlaying->setText(L"Play when the Scene starts");
        _startsPlaying->setChecked(true);
        _startsPlaying->setOnToggled([this](bool) { controlsChanged(); });
        inspector->addWidget(_startsPlaying, 28.0f);

        auto* actions = new ayt::ui::HBox();
        actions->setSpacing(5.0f);
        _apply = addTextButton(actions, L"Apply to Selected Sprite", 0.0f,
            [this]() { apply(); });
        _apply->setId("sprite_animation_apply");
        _revert = addTextButton(actions, L"Reload", 72.0f,
            [this]() { reloadSelection(true); });
        _revert->setId("sprite_animation_reload");
        inspector->addWidget(actions, 31.0f);
        _message = label(L"Select a Sprite entity to begin.", 11);
        _message->setWordWrap(true);
        inspector->addWidget(_message, 48.0f);

        auto* inspectorScroll = new ayt::ui::ScrollView();
        inspectorScroll->setId("sprite_animation_inspector_scroll");
        inspectorScroll->setVerticalScrollBarVisibility(
            ayt::ui::ScrollView::ScrollBarVisibility::Auto);
        inspectorScroll->setHorizontalScrollBarVisibility(
            ayt::ui::ScrollView::ScrollBarVisibility::Hidden);
        inspectorScroll->setContentOwned(inspector);
        ayt::ui::BoxSlotLimits inspectorLimits;
        inspectorLimits.minWidth = 285.0f;
        inspectorLimits.maxWidth = 430.0f;
        body->addWidget(inspectorScroll, 330.0f, inspectorLimits);

        reloadSelection(true);
    }

    ~SpriteAnimationToolView() override {
        clearCallbacks();
        if (_root != nullptr) ayt::ui::destroyWidgetTree(_root);
    }

    ayt::ui::Widget* rootWidget() noexcept override { return _root; }
    ayt::ui::Widget* releaseRootWidget() noexcept override {
        auto* result = _root;
        _root = nullptr;
        return result;
    }
    IEditorCommandTarget* commandTarget() noexcept override { return this; }
    bool wantsBackgroundTick() const noexcept override { return true; }

    void tick(float dt) override {
        _selectionPoll += std::max(0.0f, dt);
        if (_selectionPoll >= 0.2f) {
            _selectionPoll = 0.0f;
            reloadSelection(false);
        }
        const uint32_t before = _model.currentFrameOffset();
        _model.tick(static_cast<uint64_t>(
            std::max(0.0f, dt) * 1000000.0f));
        if (before != _model.currentFrameOffset()) updatePlayhead();
    }

    void prepareForUiShutdown() override { clearCallbacks(); }

    bool handlesCommand(const std::string& id) const override {
        return id == "edit.undo" || id == "edit.redo";
    }
    bool canExecuteCommand(const std::string& id) const override {
        if (_sceneHost == nullptr) return false;
        if (id == "edit.undo") return _sceneHost->canUndoSpriteAnimationEdit();
        if (id == "edit.redo") return _sceneHost->canRedoSpriteAnimationEdit();
        return false;
    }
    bool executeCommand(const std::string& id) override {
        if (_sceneHost == nullptr) return false;
        const bool changed = id == "edit.undo"
            ? _sceneHost->undoSpriteAnimationEdit()
            : id == "edit.redo" && _sceneHost->redoSpriteAnimationEdit();
        if (changed) reloadSelection(true);
        return changed;
    }

private:
    ayt::ui::Button* addTextButton(
        ayt::ui::BoxBase* parent, const std::wstring& text, float width,
        std::function<void()> callback)
    {
        auto* button = new ayt::ui::Button();
        button->setText(text);
        button->setPadding(7.0f, 3.0f, 7.0f, 3.0f);
        button->setOnClicked(std::move(callback));
        parent->addWidget(button, width);
        _buttons.push_back(button);
        return button;
    }

    ayt::ui::Button* addIconButton(
        ayt::ui::BoxBase* parent, const char* icon,
        const std::wstring& tooltipText, std::function<void()> callback)
    {
        auto* button = new ayt::ui::Button();
        button->setText(L"");
        button->setPadding(6.0f, 3.0f, 6.0f, 3.0f);
        button->setAccessibilityLabel(tooltipText);
        std::string error;
        auto document = ayt::ui::SvgDocument::loadFromFile(
            _iconRoot / icon, &error);
        if (document != nullptr) {
            button->setIconDocument(std::move(document));
            button->setIconSize(16.0f);
        }
        button->setOnClicked(std::move(callback));
        parent->addWidget(button, 32.0f);
        _buttons.push_back(button);
        auto activeScope = ayt::ui::UIManager::pushActive(_host.uiManager());
        if (auto* tooltip = ayt::ui::Tooltip::attachTo(button)) {
            tooltip->setText(tooltipText);
            _tooltips.push_back(tooltip);
        }
        return button;
    }

    ayt::ui::TextInput* addField(
        ayt::ui::VBox* parent, const std::wstring& name, const char* id)
    {
        auto* row = new ayt::ui::HBox();
        row->setSpacing(6.0f);
        row->addWidget(label(name, 11), 98.0f);
        auto* input = new ayt::ui::TextInput();
        input->setId(id);
        input->setMaxLength(10u);
        input->setOnTextChanged([this](const std::wstring&) {
            controlsChanged();
        });
        row->addWidget(input, 0.0f);
        parent->addWidget(row, 28.0f);
        _inputs.push_back(input);
        return input;
    }

    bool readControls()
    {
        uint32_t columns = 0u;
        uint32_t rows = 0u;
        uint32_t first = 0u;
        uint32_t count = 0u;
        uint32_t duration = 0u;
        if (!parseUInt(_columns->getText(), columns) || columns == 0u
            || columns > 4096u
            || !parseUInt(_rows->getText(), rows) || rows == 0u
            || rows > 4096u
            || !parseUInt(_firstFrame->getText(), first)
            || !parseUInt(_frameCount->getText(), count) || count == 0u
            || !parseUInt(_duration->getText(), duration) || duration == 0u
            || duration > 3600000u) {
            setMessage(L"Enter a valid grid, range and frame time.");
            return false;
        }
        const uint64_t cells = static_cast<uint64_t>(columns) * rows;
        if (first >= cells || count > cells - first) {
            setMessage(L"The selected frame range is outside the grid.");
            return false;
        }
        SpriteAnimationDraft draft;
        draft.columns = columns;
        draft.rows = rows;
        draft.firstFrame = first;
        draft.frameCount = count;
        draft.frameDurationMs = duration;
        draft.playbackMode = _playbackMode->getSelectedIndex() == 1
            ? SpriteAnimationPlaybackMode::Once
            : SpriteAnimationPlaybackMode::Loop;
        draft.playing = _startsPlaying->isChecked();
        _model.setDraft(draft);
        updatePlayhead();
        return true;
    }

    void controlsChanged()
    {
        if (_syncing) return;
        if (readControls()) {
            _dirty = true;
            setMessage(L"Preview updated. Apply to write one undoable Scene edit.");
            refreshStatus();
        }
        _host.requestRepaint();
    }

    void syncControls()
    {
        _syncing = true;
        const auto& draft = _model.draft();
        _columns->setText(std::to_wstring(draft.columns));
        _rows->setText(std::to_wstring(draft.rows));
        _firstFrame->setText(std::to_wstring(draft.firstFrame));
        _frameCount->setText(std::to_wstring(draft.frameCount));
        _duration->setText(std::to_wstring(draft.frameDurationMs));
        _playbackMode->setSelectedIndex(
            draft.playbackMode == SpriteAnimationPlaybackMode::Once ? 1 : 0);
        _startsPlaying->setChecked(draft.playing);
        _syncing = false;
        updatePlayhead();
    }

    void syncSelectionInputs()
    {
        _syncing = true;
        _firstFrame->setText(std::to_wstring(_model.draft().firstFrame));
        _frameCount->setText(std::to_wstring(_model.draft().frameCount));
        _syncing = false;
        updatePlayhead();
    }

    EditorSpriteAnimationState editedState() const
    {
        EditorSpriteAnimationState state = _bound;
        const auto& draft = _model.draft();
        state.columns = draft.columns;
        state.rows = draft.rows;
        state.firstFrame = draft.firstFrame;
        state.frameCount = draft.frameCount;
        state.frameDurationMs = draft.frameDurationMs;
        state.playbackMode = draft.playbackMode
            == SpriteAnimationPlaybackMode::Once ? 1u : 0u;
        state.playing = draft.playing;
        state.hasAnimation = true;
        return state;
    }

    void apply()
    {
        if (_bound.entityId == 0u || !readControls()) return;
        const EditorSpriteAnimationState next = editedState();
        std::string error;
        if (_sceneHost == nullptr
            || !_sceneHost->applySelectedSpriteAnimation(next, &error)) {
            if (error.empty()) {
                error = "Sprite animation scene binding is unavailable.";
            }
            setMessage(L"Apply failed: " + ayt::ui::decodeUtf8Text(error));
            return;
        }
        _bound = next;
        _dirty = false;
        setMessage(L"Applied to the Scene. Ctrl+Z / Ctrl+Shift+Z are available.");
        refreshStatus();
        _host.requestRepaint();
    }

    void reloadSelection(bool force)
    {
        EditorSpriteAnimationState state;
        std::string error;
        if (_sceneHost == nullptr
            || !_sceneHost->querySelectedSpriteAnimation(state, &error)) {
            if (error.empty()) {
                error = "Sprite animation scene binding is unavailable.";
            }
            if (!force && _bound.entityId == 0u) return;
            _bound = {};
            _dirty = false;
            _sheet->clearImage();
            _preview->clearImage();
            _modeStatus->setText(L"NO SPRITE SELECTED");
            _sourceLabel->setText(L"");
            setMessage(ayt::ui::decodeUtf8Text(error));
            return;
        }
        const bool selectionChanged = state.entityId != _bound.entityId;
        if (!force && !selectionChanged && _dirty) return;
        if (!force && state == _bound) return;
        _bound = state;
        SpriteAnimationDraft draft;
        draft.columns = state.columns;
        draft.rows = state.rows;
        draft.firstFrame = state.firstFrame;
        draft.frameCount = state.frameCount;
        draft.frameDurationMs = state.frameDurationMs;
        draft.playbackMode = state.playbackMode == 1u
            ? SpriteAnimationPlaybackMode::Once
            : SpriteAnimationPlaybackMode::Loop;
        draft.playing = state.playing;
        _model.setDraft(draft);
        _dirty = false;
        syncControls();

        std::string imageError;
        const EditorAuthoringImage image = _host.loadAuthoringImage(
            state.texturePath, &imageError);
        if (image) {
            _sheet->setImage(image.texture, image.width, image.height);
            _preview->setImage(image.texture, image.width, image.height);
            setMessage(state.hasAnimation
                ? L"Loaded from the selected Sprite Animation."
                : L"No animation component yet. Apply to create it.");
        } else {
            _sheet->clearImage();
            _preview->clearImage();
            setMessage(L"Source image unavailable: "
                + ayt::ui::decodeUtf8Text(imageError));
        }
        _sourceLabel->setText(
            ayt::ui::decodeUtf8Text(state.texturePath));
        refreshStatus();
        _host.requestRepaint();
    }

    void updatePlayhead(bool updateSlider = true)
    {
        const uint32_t count = _model.draft().frameCount;
        if (updateSlider) {
            _syncing = true;
            _scrub->setValueRange(0.0f,
                static_cast<float>(std::max(1u, count) - 1u));
            _scrub->setValue(
                static_cast<float>(_model.currentFrameOffset()));
            _syncing = false;
        }
        _playhead->setText(L"Frame "
            + std::to_wstring(_model.currentFrameOffset() + 1u)
            + L" / " + std::to_wstring(count)
            + L"  ·  Cell " + std::to_wstring(_model.currentCell()));
        _sheet->markDirty();
        _preview->markDirty();
        refreshStatus();
        _host.requestRepaint();
    }

    void refreshStatus()
    {
        if (_bound.entityId == 0u) return;
        const auto& draft = _model.draft();
        std::wstring text = _model.isPlaying() ? L"PLAYING" : L"PAUSED";
        if (_model.isFinished()) text = L"FINISHED";
        text += L"  ·  " + ayt::ui::decodeUtf8Text(_bound.entityName)
            + L"  ·  Cells " + std::to_wstring(draft.firstFrame)
            + L"–" + std::to_wstring(_model.selectedLastFrame());
        if (_dirty) text += L"  ·  NOT APPLIED";
        _modeStatus->setText(text);
    }

    void setMessage(const std::wstring& text)
    {
        if (_message != nullptr) _message->setText(text);
        _host.setStatusText(text);
    }

    void clearCallbacks()
    {
        if (_callbacksCleared) return;
        _callbacksCleared = true;
        if (_sheet != nullptr) _sheet->setOnSelectionChanged({});
        if (_scrub != nullptr) _scrub->setOnValueChanged({});
        if (_playbackMode != nullptr) _playbackMode->setOnSelectionChanged({});
        if (_startsPlaying != nullptr) _startsPlaying->setOnToggled({});
        for (auto* input : _inputs) {
            if (input != nullptr) input->setOnTextChanged({});
        }
        for (auto* button : _buttons) {
            if (button != nullptr) button->setOnClicked({});
        }
        for (auto* tooltip : _tooltips) {
            if (tooltip == nullptr) continue;
            tooltip->detach();
            ayt::ui::destroyWidgetTree(tooltip);
        }
        _tooltips.clear();
    }

    IEditorHostServices& _host;
    IEditorSpriteAnimationHost* _sceneHost = nullptr;
    std::filesystem::path _iconRoot;
    SpriteAnimationAuthoringModel _model;
    EditorSpriteAnimationState _bound;
    ayt::ui::Widget* _root = nullptr;
    EditorSpriteAnimationCanvas* _sheet = nullptr;
    EditorSpriteAnimationCanvas* _preview = nullptr;
    ayt::ui::TextLabel* _modeStatus = nullptr;
    ayt::ui::TextLabel* _sourceLabel = nullptr;
    ayt::ui::TextLabel* _playhead = nullptr;
    ayt::ui::TextLabel* _message = nullptr;
    ayt::ui::TextInput* _columns = nullptr;
    ayt::ui::TextInput* _rows = nullptr;
    ayt::ui::TextInput* _firstFrame = nullptr;
    ayt::ui::TextInput* _frameCount = nullptr;
    ayt::ui::TextInput* _duration = nullptr;
    ayt::ui::ComboBox* _playbackMode = nullptr;
    ayt::ui::CheckBox* _startsPlaying = nullptr;
    ayt::ui::Slider* _scrub = nullptr;
    ayt::ui::Button* _apply = nullptr;
    ayt::ui::Button* _revert = nullptr;
    std::vector<ayt::ui::TextInput*> _inputs;
    std::vector<ayt::ui::Button*> _buttons;
    std::vector<ayt::ui::Tooltip*> _tooltips;
    float _selectionPoll = 0.0f;
    bool _syncing = false;
    bool _dirty = false;
    bool _callbacksCleared = false;
};

} // namespace

bool registerEditorSpriteAnimationExtension(
    EditorExtensionRegistry& registry, std::string* error)
{
    EditorDescriptor descriptor;
    descriptor.id = kEditorSpriteAnimationExtensionId;
    descriptor.displayName = L"Sprite Animation";
    descriptor.surfaceKind = EditorSurfaceKind::ToolPanel;
    descriptor.openPolicy = EditorOpenPolicy::Singleton;
    descriptor.defaultDockSlot = EditorDockSlot::Center;
    descriptor.createDocument = [](const EditorOpenRequest&, std::string&) {
        return std::static_pointer_cast<IEditorDocument>(
            std::make_shared<SpriteAnimationToolDocument>());
    };
    descriptor.createView = [](
        const std::shared_ptr<IEditorDocument>&,
        IEditorHostServices& host) {
        return std::unique_ptr<IEditorView>(
            std::make_unique<SpriteAnimationToolView>(host));
    };
    return registry.registerEditor(std::move(descriptor), error);
}

} // namespace ayt::editor
