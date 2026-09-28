#include "AYEditor/EditorAnimationExtension.h"

#include "AYEditor/EditorAnimationDocument.h"
#include "AYEditor/EditorBuiltInExtensions.h"
#include "AYEditor/ImportDialog.h"
#include "AYEditorAnimationCanvas.h"
#include <AYEditor/EditorCommandButtons.h>
#include <AYEditor/EditorAuthoringSelectionBridge.h>
#include <AYUI/Authoring/DiagnosticsPanel.h>
#include "AYEditorAnimationCurveCanvas.h"
#include "AYEditorAnimationCurveSource.h"
#include "AYEditorAnimationDopeSheet.h"
#include "AYEditorTimelinePlaybackSource.h"

#include <AYResource/assetsDefs/IAnimation.h>
#include <AYResource/assetsImpl/Mesh.h>
#include <AYUI/Box.h>
#include <AYUI/Authoring/AuthoringPrimitives.h>
#include <AYUI/Authoring/TimelineSelectionOps.h>
#include <AYUI/Authoring/NumericFields.h>
#include <AYUI/Authoring/ResourceReferenceField.h>
#include <AYUI/Button.h>
#include <AYUI/ComboBox.h>
#include <AYUI/Slider.h>
#include <AYUI/TextArea.h>
#include <AYUI/TextInput.h>
#include <AYUI/TextLabel.h>
#include <AYUI/UnicodeText.h>
#include <AYUI/Widget.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>

namespace ayt::editor {
namespace {

using ayt::anim::editor::AnimationPreviewMode;

std::string encodeUtf8(const std::wstring& text)
{
    return ayt::ui::encodeUtf8Text(text);
}

using ayt::ui::authoring::parseFiniteFloat;

class EditorAnimationWorkspaceView final
    : public IEditorView, public IEditorCommandTarget {
public:
    EditorAnimationWorkspaceView(std::shared_ptr<EditorAnimationDocument> document,
                                 IEditorHostServices& host)
        : _document(std::move(document)), _host(host), _commands([this] { return commandTarget(); }),
          _selectionBridge(host.workspace(), _document)
    {
        _document->configureProjectRoot(_host.projectRoot());
        build();
        refreshAll();
    }

    ~EditorAnimationWorkspaceView() override
    {
        _commands.detach();
        if (_root != nullptr) ayt::ui::destroyWidgetTree(_root);
    }

    ayt::ui::Widget* rootWidget() noexcept override { return _root; }
    ayt::ui::Widget* releaseRootWidget() noexcept override {
        auto* result = _root;
        _root = nullptr;
        return result;
    }
    void prepareForUiShutdown() override { _commands.detach(); }
    EditorSelectionContext* selectionContext() noexcept override { return _selectionBridge.context(); }
    IEditorCommandTarget* commandTarget() noexcept override { return this; }
    bool handlesCommand(const std::string& commandId) const override {
        return commandId == "edit.delete" || (_document && _document->handlesCommand(commandId));
    }
    bool canExecuteCommand(const std::string& commandId) const override {
        if (commandId == "edit.delete") return _curveCanvas && _curveCanvas->selectedKeyCount() > 0u;
        return _document && _document->canExecuteCommand(commandId);
    }
    bool executeCommand(const std::string& commandId) override {
        if (!canExecuteCommand(commandId)) return false;
        std::string error;
        const bool success = commandId == "file.save" ? _document->save(&error)
            : commandId == "edit.delete" ? _curveCanvas->deleteSelectedKeys() : _document->executeCommand(commandId);
        if (!success) { _host.setStatusText(L"Animation command failed: " + ayt::ui::decodeUtf8Text(error)); return false; }
        _host.setStatusText(L"Animation command completed");
        refreshAll(); return true;
    }
    void tick(float dt) override
    {
        if (_document == nullptr) return;
        _commands.refresh();
        if (_document->timelinePlaying()) _document->timelineTick(dt);
        const auto changes = _refreshGate.consume(stateStamp());
        if (!changes.content && (changes.pose || changes.transport)) {
            refreshTransport();
            if (_canvas != nullptr) _canvas->markDirty();
            if (_curveCanvas != nullptr) _curveCanvas->markDirty();
            if (_dopeSheet != nullptr) _dopeSheet->markDirty();
            _host.requestRepaint();
        }
        if (changes.content) {
            refreshBindings();
            refreshInspector();
            refreshDiagnostics();
            refreshTransport();
            refreshAuthoring();
            if (_canvas != nullptr) _canvas->markDirty();
            if (_curveCanvas != nullptr) _curveCanvas->markDirty();
            if (_dopeSheet != nullptr) _dopeSheet->markDirty();
            _host.requestRepaint();
        }
    }
    bool wantsBackgroundTick() const noexcept override {
        return _document != nullptr && _document->timelinePlaying();
    }

private:
    ayt::ui::authoring::AuthoringStateStamp stateStamp() const {
        const auto& preview = _document->preview();
        return {preview.revision(), 0u, preview.poseRevision(),
                _document->timelinePositionSeconds(), _document->timelinePlaying()};
    }
    ayt::ui::Button* makeButton(const wchar_t* text,
                                std::function<void()> callback,
                                float horizontalPadding = 7.0f)
    {
        auto* button = new ayt::ui::Button();
        button->setText(text);
        button->setPadding(horizontalPadding, 3.0f,
                           horizontalPadding, 3.0f);
        button->setOnClicked(std::move(callback));
        return button;
    }

    ayt::ui::TextLabel* makeHeader(const wchar_t* text)
    {
        auto* label = new ayt::ui::TextLabel();
        label->setText(text);
        label->setFontSize(11);
        label->setTextColor({0.58f, 0.63f, 0.72f, 1.0f});
        label->setVerticalAlignment(ayt::ui::TextLabel::VAlignment::Center);
        return label;
    }

    ayt::ui::authoring::ResourceReferenceField* makeBindingRow(const wchar_t* name,
        ayt::ui::TextInput*& input, std::function<ayt::ui::authoring::ResourceReferenceResult()> load,
        bool picker = false)
    {
        auto* field = new ayt::ui::authoring::ResourceReferenceField({name});
        input = field->input();
        field->setOnLoad([load = std::move(load)](const auto&) { return load(); });
        if (picker) field->setPicker([] {
            return ayt::ui::decodeUtf8Text(ImportDialog::showOpenAssetFileDialog(nullptr));
        });
        return field;
    }

    void build()
    {
        auto* root = new ayt::ui::VBox();
        _root = root;
        root->setSpacing(5.0f);
        root->setPadding(5.0f, 5.0f, 5.0f, 5.0f);

        auto* toolbar = new ayt::ui::HBox();
        toolbar->setSpacing(5.0f);
        const auto owner = _document;
        auto playback = std::make_shared<TimelinePlaybackSource>([owner] { return owner; },
            TimelinePlaybackExtras{[owner] { return owner->preview().looping(); },
                [owner](bool value) { owner->setLooping(value); },
                [owner] { return owner->preview().playRate(); },
                [owner](float value) { owner->setPlayRate(value); }});
        _transport = new ayt::ui::authoring::PlaybackControls(playback,
            {true, false, false, true, true, true});
        _transport->setOnChanged([this] { refreshTransport(); });
        toolbar->addWidget(_transport, 501.0f);
        toolbar->addWidget(makeButton(L"Frame", [this]() {
            if (_canvas != nullptr) _canvas->framePreview();
        }), 54.0f);
        _commands.add(*toolbar, L"Undo", "edit.undo", 52);
        _commands.add(*toolbar, L"Redo", "edit.redo", 52);
        _commands.add(*toolbar, L"Save", "file.save", 52);
        toolbar->addWidget(makeHeader(L"VIEW"), 38.0f);
        _mode = new ayt::ui::ComboBox();
        _mode->setItems({L"Model + Skeleton", L"Model", L"Skeleton"});
        _mode->setOnSelectionChanged([this](int index) {
            if (_syncing || index < 0) return;
            _document->setPreviewMode(index == 1
                ? AnimationPreviewMode::ModelOnly
                : index == 2 ? AnimationPreviewMode::SkeletonOnly
                             : AnimationPreviewMode::ModelAndSkeleton);
            refreshAll();
        });
        toolbar->addWidget(_mode, 150.0f);
        root->addWidget(toolbar, 30.0f);

        auto* body = new ayt::ui::HBox();
        body->setSpacing(5.0f);
        auto* previewColumn = new ayt::ui::VBox();
        previewColumn->setSpacing(4.0f);
        _canvas = new EditorAnimationCanvas(_document);
        _canvas->setOnBoneSelected([this](int) { refreshInspector(); });
        previewColumn->addWidget(_canvas, 0.0f);
        auto* curveToolbar = new ayt::ui::HBox();
        curveToolbar->setSpacing(4.0f);
        curveToolbar->addWidget(makeHeader(L"CURVE EDITOR"), 92.0f);
        static constexpr const wchar_t* curveComponents[] = {
            L"X", L"Y", L"Z", L"W"
        };
        for (std::size_t component = 0u; component < 4u; ++component) {
            _curveComponents[component] = makeButton(
                curveComponents[component], [this, component]() {
                    if (_curveCanvas == nullptr) return;
                    _curveCanvas->setComponentVisible(component,
                        !_curveCanvas->componentVisible(component));
                    refreshCurveControls();
                }, 5.0f);
            curveToolbar->addWidget(_curveComponents[component], 30.0f);
        }
        curveToolbar->addWidget(makeButton(L"Frame All", [this]() {
            if (_curveCanvas != nullptr) _curveCanvas->frameAll();
        }), 68.0f);
        curveToolbar->addWidget(makeButton(L"Select All", [this]() {
            if (_curveCanvas != nullptr) _curveCanvas->selectAllKeys();
        }), 72.0f);
        _commands.add(*curveToolbar, L"Delete", "edit.delete", 56);
        auto* curveHelp = new ayt::ui::TextLabel();
        curveHelp->setText(L"Drag key/value or tangent · Wheel zoom · Middle/right pan");
        curveHelp->setFontSize(10);
        curveHelp->setTextColor({0.46f, 0.52f, 0.62f, 1.0f});
        curveHelp->setVerticalAlignment(ayt::ui::TextLabel::VAlignment::Center);
        curveToolbar->addWidget(curveHelp, 0.0f);
        previewColumn->addWidget(curveToolbar, 26.0f);
        const auto curveSource = _curveSource = makeAnimationCurveSource(_document);
        _curveCanvas = new EditorAnimationCurveCanvas(curveSource);
        _curveCanvas->setOnSelectionChanged(
            [this](const std::string& keyId, std::size_t) {
                _selectedKeyId = keyId;
                _selectionCleared = keyId.empty();
                refreshAuthoring();
            });
        _curveCanvas->setOnEdited([this]() {
            refreshInspector();
            refreshAuthoring();
            refreshTransport();
            _host.requestRepaint();
        });
        previewColumn->addWidget(_curveCanvas, 230.0f);
        _dopeSheet = new EditorAnimationDopeSheet(curveSource);
        _dopeSheet->setOnSelectionChanged(
            [this](const std::string& trackId, const std::string& keyId) {
                if (keyId.rfind("notify.", 0u) == 0u) {
                    _selectedNotifyId = keyId;
                    refreshAuthoring();
                    return;
                }
                _selectedTrackId = trackId;
                _selectedKeyId = keyId;
                _selectedNotifyId.clear();
                if (_curveCanvas != nullptr) {
                    _curveCanvas->setTrackId(trackId);
                }
                refreshAuthoring();
            });
        _dopeSheet->setOnEdited([this]() {
            refreshInspector();
            refreshAuthoring();
            refreshTransport();
            _host.requestRepaint();
        });
        previewColumn->addWidget(_dopeSheet, 170.0f);
        body->addWidget(previewColumn, 0.0f);

        auto* inspector = new ayt::ui::VBox();
        inspector->setSpacing(4.0f);
        inspector->addWidget(makeHeader(L"ANIMATION PREVIEW"), 20.0f);
        _info = new ayt::ui::TextArea();
        _info->setReadOnly(true);
        _info->setWordWrap(false);
        inspector->addWidget(_info, 164.0f);

        inspector->addWidget(makeHeader(L"CLIP PROPERTIES"), 20.0f);
        auto* clipProperties = new ayt::ui::HBox();
        clipProperties->setSpacing(4.0f);
        _clipName = new ayt::ui::TextInput();
        _clipName->setPlaceholder(L"Clip name");
        clipProperties->addWidget(_clipName, 0.0f);
        _clipDuration = new ayt::ui::TextInput();
        _clipDuration->setPlaceholder(L"Seconds");
        _clipDuration->setNumericScrubEnabled(true);
        clipProperties->addWidget(_clipDuration, 76.0f);
        _clipTicksPerSecond = new ayt::ui::TextInput();
        _clipTicksPerSecond->setPlaceholder(L"Ticks/s");
        _clipTicksPerSecond->setNumericScrubEnabled(true);
        clipProperties->addWidget(_clipTicksPerSecond, 72.0f);
        clipProperties->addWidget(makeButton(L"Apply", [this]() {
            applyClipProperties();
        }), 56.0f);
        inspector->addWidget(clipProperties, 28.0f);

        inspector->addWidget(makeHeader(L"PREVIEW BINDINGS"), 20.0f);
        inspector->addWidget(makeBindingRow(L"Skeleton", _skeletonPath,
            [this]() { return loadSkeleton(); }, true), 28.0f);
        inspector->addWidget(makeBindingRow(L"Mesh", _meshPath,
            [this]() { return loadMesh(); }, true), 28.0f);
        inspector->addWidget(makeBindingRow(L"Material", _materialPath,
            [this]() { return loadMaterial(); }), 28.0f);
        auto* bindingActions = new ayt::ui::HBox();
        bindingActions->setSpacing(4.0f);
        bindingActions->addWidget(makeButton(L"Auto Resolve", [this]() {
            const auto inferred = _document->preview().inferCompanionAssets();
            std::string error;
            if (!_document->preview().applyBindings(inferred, &error)) {
                _host.setStatusText(L"Preview binding failed: "
                    + ayt::ui::decodeUtf8Text(error));
            } else {
                (void)_document->persistPreviewMetadata(nullptr);
                _host.setStatusText(L"Animation preview bindings resolved");
                refreshAll();
            }
        }), 96.0f);
        inspector->addWidget(bindingActions, 28.0f);

        inspector->addWidget(makeHeader(L"TRACK AUTHORING"), 20.0f);
        _trackPicker = new ayt::ui::ComboBox();
        _trackPicker->setOnSelectionChanged([this](int index) {
            if (_syncing || index < 0
                || index >= static_cast<int>(_trackIds.size())) return;
            _selectedTrackId = _trackIds[static_cast<std::size_t>(index)];
            _selectionCleared = false;
            _selectedKeyId.clear();
            _selectedNotifyId.clear();
            refreshAuthoring();
        });
        inspector->addWidget(_trackPicker, 28.0f);

        auto* trackDefinition = new ayt::ui::HBox();
        trackDefinition->setSpacing(4.0f);
        _trackNode = new ayt::ui::TextInput();
        trackDefinition->addWidget(_trackNode, 0.0f);
        _trackProperty = new ayt::ui::ComboBox();
        _trackProperty->setItems({L"Position", L"Rotation", L"Scale", L"Float"});
        _trackProperty->setSelectedIndex(0);
        trackDefinition->addWidget(_trackProperty, 92.0f);
        inspector->addWidget(trackDefinition, 28.0f);

        auto* trackActions = new ayt::ui::HBox();
        trackActions->setSpacing(4.0f);
        trackActions->addWidget(makeButton(L"Add Track", [this]() {
            addTrack();
        }), 76.0f);
        trackActions->addWidget(makeButton(L"Remove Track", [this]() {
            removeTrack();
        }), 92.0f);
        _editState = new ayt::ui::TextLabel();
        _editState->setFontSize(11);
        _editState->setVerticalAlignment(
            ayt::ui::TextLabel::VAlignment::Center);
        trackActions->addWidget(_editState, 0.0f);
        inspector->addWidget(trackActions, 28.0f);

        _keyPicker = new ayt::ui::ComboBox();
        _keyPicker->setOnSelectionChanged([this](int index) {
            if (_syncing || index < 0
                || index >= static_cast<int>(_keyIds.size())) return;
            _selectedKeyId = _keyIds[static_cast<std::size_t>(index)];
            _selectionCleared = false;
            _selectedNotifyId.clear();
            refreshAuthoring();
        });
        inspector->addWidget(_keyPicker, 28.0f);
        _keyFields = new ayt::ui::authoring::NumericFields(
            {"animation_key_value_0", "animation_key_value_1", "animation_key_value_2", "animation_key_value_3"},
            {L"X", L"Y", L"Z", L"W"});
        _keyFields->setOnSubmitted([this] { applyKeyValues(); });
        inspector->addWidget(_keyFields, 28.0f);
        auto* keyActions = new ayt::ui::HBox();
        keyActions->setSpacing(4.0f);
        keyActions->addWidget(makeButton(L"Add Key", [this]() {
            addKey();
        }), 66.0f);
        keyActions->addWidget(makeButton(L"Move Here", [this]() {
            moveKey();
        }), 78.0f);
        keyActions->addWidget(makeButton(L"Delete Key", [this]() {
            deleteKey();
        }), 78.0f);
        keyActions->addWidget(makeButton(L"Apply Value", [this]() {
            applyKeyValues();
        }), 82.0f);
        inspector->addWidget(keyActions, 28.0f);

        inspector->addWidget(makeHeader(L"CURVE / TANGENTS"), 20.0f);
        _curveMode = new ayt::ui::ComboBox();
        _curveMode->setItems({L"Linear", L"Step", L"Cubic Hermite"});
        _curveMode->setOnSelectionChanged([this](int index) {
            if (_syncing || _selectedTrackId.empty() || index < 0) return;
            const auto mode = index == 1
                ? ayt::resource::AnimInterpolation::Step
                : index == 2
                    ? ayt::resource::AnimInterpolation::CubicHermite
                    : ayt::resource::AnimInterpolation::Linear;
            if (_document->setAnimationTrackInterpolation(
                    _selectedTrackId, mode)) {
                _host.setStatusText(L"Animation curve interpolation updated");
                refreshAll();
            }
        });
        inspector->addWidget(_curveMode, 28.0f);
        for (std::size_t direction = 0u; direction < 2u; ++direction) {
            auto* row = new ayt::ui::HBox();
            row->setSpacing(4.0f);
            _tangentRows[direction] = row;
            auto* label = new ayt::ui::TextLabel();
            label->setText(direction == 0u ? L"In" : L"Out");
            label->setFontSize(11);
            label->setVerticalAlignment(ayt::ui::TextLabel::VAlignment::Center);
            row->addWidget(label, 28.0f);
            const std::string prefix = direction == 0u ? "animation_in_tangent_" : "animation_out_tangent_";
            auto* fields = new ayt::ui::authoring::NumericFields(
                {prefix + "0", prefix + "1", prefix + "2", prefix + "3"},
                {L"X", L"Y", L"Z", L"W"});
            fields->setUnit(L"value/s");
            fields->setOnSubmitted([this] { applyTangents(); });
            (direction == 0u ? _inFields : _outFields) = fields;
            row->addWidget(fields, 0.0f);
            inspector->addWidget(row, 28.0f);
        }
        auto* tangentActions = new ayt::ui::HBox();
        tangentActions->setSpacing(4.0f);
        tangentActions->addWidget(makeButton(L"Apply Tangents", [this]() {
            applyTangents();
        }), 106.0f);
        tangentActions->addWidget(makeButton(L"Auto Tangents", [this]() {
            autoTangents();
        }), 98.0f);
        inspector->addWidget(tangentActions, 28.0f);

        inspector->addWidget(makeHeader(L"NOTIFY / EVENT"), 20.0f);
        _notifyPicker = new ayt::ui::ComboBox();
        _notifyPicker->setOnSelectionChanged([this](int index) {
            if (_syncing || index < 0
                || index >= static_cast<int>(_notifyIds.size())) return;
            _selectedNotifyId = _notifyIds[static_cast<std::size_t>(index)];
            refreshAuthoring();
        });
        inspector->addWidget(_notifyPicker, 28.0f);
        auto* notifyDefinition = new ayt::ui::HBox();
        notifyDefinition->setSpacing(4.0f);
        _notifyName = new ayt::ui::TextInput();
        _notifyName->setPlaceholder(L"Event name");
        notifyDefinition->addWidget(_notifyName, 0.0f);
        _notifyPayload = new ayt::ui::TextInput();
        _notifyPayload->setPlaceholder(L"Payload");
        _notifyPayload->setNumericScrubEnabled(true);
        notifyDefinition->addWidget(_notifyPayload, 92.0f);
        inspector->addWidget(notifyDefinition, 28.0f);
        auto* notifyActions = new ayt::ui::HBox();
        notifyActions->setSpacing(4.0f);
        notifyActions->addWidget(makeButton(L"Add Here", [this]() {
            addNotify();
        }), 72.0f);
        notifyActions->addWidget(makeButton(L"Apply Here", [this]() {
            updateNotify();
        }), 82.0f);
        notifyActions->addWidget(makeButton(L"Delete", [this]() {
            deleteNotify();
        }), 58.0f);
        inspector->addWidget(notifyActions, 28.0f);

        inspector->addWidget(makeHeader(L"TRACKS / EVENTS"), 20.0f);
        _tracks = new ayt::ui::TextArea();
        _tracks->setReadOnly(true);
        _tracks->setWordWrap(false);
        inspector->addWidget(_tracks, 0.0f);
        body->addWidget(inspector, 380.0f);
        root->addWidget(body, 0.0f);

        _diagnostics = new ayt::ui::authoring::DiagnosticsPanel();
        _diagnostics->setId("animation_diagnostics");
        _diagnostics->setOnLocate([this](const std::string& target) { locateDiagnostic(target); });
        root->addWidget(_diagnostics, 96.0f);

        auto* timeline = new ayt::ui::HBox();
        timeline->setSpacing(5.0f);
        timeline->addWidget(makeHeader(L"TIMELINE"), 66.0f);
        _scrubBar = new ayt::ui::authoring::PlaybackControls(playback,
            {false, true, true, false, false, false});
        _scrubBar->setOnChanged([this] {
            refreshTransport();
            if (_canvas != nullptr) _canvas->markDirty();
            if (_curveCanvas != nullptr) _curveCanvas->markDirty();
            if (_dopeSheet != nullptr) _dopeSheet->markDirty();
        });
        timeline->addWidget(_scrubBar, 0.0f);
        auto* editable = new ayt::ui::TextLabel();
        editable->setText(L"Editable tracks + keys");
        editable->setFontSize(11);
        editable->setTextColor({0.42f, 0.78f, 0.50f, 1.0f});
        timeline->addWidget(editable, 150.0f);
        root->addWidget(timeline, 30.0f);
    }

    ayt::ui::authoring::ResourceReferenceResult loadSkeleton()
    {
        std::string error;
        if (!_document->bindSkeleton(encodeUtf8(_skeletonPath->getText()), &error)) {
            const auto message = L"Skeleton binding failed: " + ayt::ui::decodeUtf8Text(error);
            _host.setStatusText(message); return {false, message};
        }
        _host.setStatusText(L"Skeleton bound to animation preview");
        refreshAll(); return {true, L"Skeleton bound"};
    }

    ayt::ui::authoring::ResourceReferenceResult loadMesh()
    {
        std::string error;
        if (!_document->bindMesh(encodeUtf8(_meshPath->getText()), &error)) {
            const auto message = L"Mesh binding failed: " + ayt::ui::decodeUtf8Text(error);
            _host.setStatusText(message); return {false, message};
        }
        _host.setStatusText(L"Mesh bound to animation preview");
        refreshAll(); return {true, L"Mesh bound"};
    }

    ayt::ui::authoring::ResourceReferenceResult loadMaterial()
    {
        _document->setMaterialPath(encodeUtf8(_materialPath->getText()));
        _host.setStatusText(L"Preview material binding saved");
        refreshAll(); return {true, L"Material reference saved"};
    }

    void refreshAll()
    {
        _commands.refresh();
        refreshBindings();
        refreshInspector();
        refreshDiagnostics();
        refreshTransport();
        refreshAuthoring();
        refreshCurveControls();
        _commands.refresh();
        _refreshGate.acknowledge(stateStamp());
        if (_canvas != nullptr) _canvas->markDirty();
        _host.requestRepaint();
    }

    void refreshBindings()
    {
        const auto& preview = _document->preview();
        _syncing = true;
        _skeletonPath->setText(ayt::ui::decodeUtf8Text(preview.skeletonPath()));
        _meshPath->setText(ayt::ui::decodeUtf8Text(preview.meshPath()));
        _materialPath->setText(ayt::ui::decodeUtf8Text(preview.materialPath()));
        _mode->setSelectedIndex(preview.requestedPreviewMode()
                == AnimationPreviewMode::ModelOnly ? 1
            : preview.requestedPreviewMode() == AnimationPreviewMode::SkeletonOnly
                ? 2 : 0);
        _syncing = false;
    }

    void refreshInspector()
    {
        const auto& preview = _document->preview();
        const auto* animation = preview.animation();
        std::wostringstream info;
        info << L"Clip: " << ayt::ui::decodeUtf8Text(_document->title()) << L"\n"
             << std::fixed << std::setprecision(3)
             << L"Duration: " << preview.duration() << L" s\n"
             << L"Tracks: " << (animation != nullptr ? animation->getTrackCount() : 0u)
             << L"   Events: " << (animation != nullptr ? animation->getNotifyCount() : 0u)
             << L"\nBones: " << preview.bones().size();
        if (const auto mesh = preview.mesh()) {
            info << L"   Vertices: " << mesh->getVertexCount()
                 << L"   Triangles: " << mesh->getIndexCount() / 3u;
        }
        info << L"\nRequested: " << ayt::ui::decodeUtf8Text(
                    ayt::anim::editor::AnimationPreviewSession::previewModeName(
                        preview.requestedPreviewMode()))
             << L"\nEffective: " << ayt::ui::decodeUtf8Text(
                    ayt::anim::editor::AnimationPreviewSession::previewModeName(
                        preview.effectivePreviewMode()))
             << L"\nMissing track bones: " << preview.missingTrackCount();
        const int selected = _document->selectedBone();
        if (selected >= 0 && selected < static_cast<int>(preview.bones().size())) {
            info << L"\nSelected bone: "
                 << ayt::ui::decodeUtf8Text(preview.bones()[selected].name)
                 << L" [" << selected << L"]";
        }
        const std::wstring text = info.str();
        if (_info->getText() != text) _info->setText(text);

        std::wostringstream tracks;
        const auto timelineTracks = _document->timelineTracks();
        const auto keys = _document->timelineKeyframes();
        for (std::size_t index = 0; index < timelineTracks.size(); ++index) {
            const auto& track = timelineTracks[index];
            tracks << (track.kind == EditorTimelineTrackKind::Event ? L"◆ " : L"● ")
                   << ayt::ui::decodeUtf8Text(track.name) << L"  ("
                   << std::count_if(keys.begin(), keys.end(), [&track](const auto& key) {
                        return key.trackId == track.id;
                   }) << L")\n";
            if (index >= 127u) { tracks << L"...\n"; break; }
        }
        const std::wstring trackText = tracks.str();
        if (_tracks->getText() != trackText) _tracks->setText(trackText);
    }

    void refreshAuthoring()
    {
        const auto selectionBefore = _curveSource ? *_curveSource->selectionState()
            : ayt::ui::authoring::TimelineSelection{};
        const auto tracks = _document->timelineTracks();
        _trackIds.clear();
        std::vector<std::wstring> trackItems;
        for (const auto& track : tracks) {
            if (track.kind != EditorTimelineTrackKind::Animation) continue;
            _trackIds.push_back(track.id);
            trackItems.push_back(ayt::ui::decodeUtf8Text(track.name));
        }
        if (!_selectedTrackId.empty()
            && std::find(_trackIds.begin(), _trackIds.end(), _selectedTrackId)
                == _trackIds.end()) {
            _selectedTrackId.clear();
        }
        if (_selectedTrackId.empty() && !_trackIds.empty()) {
            _selectedTrackId = _trackIds.front();
        }
        if (_curveCanvas != nullptr) {
            _curveCanvas->setTrackId(_selectedTrackId);
        }
        const auto selectedTrack = std::find(
            _trackIds.begin(), _trackIds.end(), _selectedTrackId);
        const int trackIndex = selectedTrack == _trackIds.end() ? -1
            : static_cast<int>(selectedTrack - _trackIds.begin());

        _keyIds.clear();
        std::vector<std::wstring> keyItems;
        for (const auto& key : _document->timelineKeyframes()) {
            if (key.trackId != _selectedTrackId) continue;
            _keyIds.push_back(key.id);
            std::wostringstream label;
            label << L"Key " << _keyIds.size() << L"   @ "
                  << std::fixed << std::setprecision(3)
                  << key.timeSeconds << L" s";
            keyItems.push_back(label.str());
        }
        if (!_selectedKeyId.empty()
            && std::find(_keyIds.begin(), _keyIds.end(), _selectedKeyId)
                == _keyIds.end()) {
            _selectedKeyId.clear();
        }
        if (_selectedKeyId.empty() && !_keyIds.empty() && !_selectionCleared) {
            _selectedKeyId = _keyIds.front();
        }
        const auto selectedKey = std::find(
            _keyIds.begin(), _keyIds.end(), _selectedKeyId);
        const int keyIndex = selectedKey == _keyIds.end() ? -1
            : static_cast<int>(selectedKey - _keyIds.begin());

        _syncing = true;
        const auto clip = _document->animationClipProperties();
        _clipName->setText(ayt::ui::decodeUtf8Text(clip.name));
        {
            std::wostringstream value;
            value << std::setprecision(7) << clip.durationSeconds;
            _clipDuration->setText(value.str());
        }
        {
            std::wostringstream value;
            value << std::setprecision(7) << clip.ticksPerSecond;
            _clipTicksPerSecond->setText(value.str());
        }
        _trackPicker->setItems(trackItems);
        _trackPicker->setSelectedIndex(trackIndex);
        _keyPicker->setItems(keyItems);
        _keyPicker->setSelectedIndex(keyIndex);
        ayt::resource::AnimInterpolation interpolation =
            ayt::resource::AnimInterpolation::Linear;
        const bool hasInterpolation = !_selectedTrackId.empty()
            && _document->animationTrackInterpolation(
                _selectedTrackId, interpolation);
        _curveMode->setSelectedIndex(!hasInterpolation ? -1
            : interpolation == ayt::resource::AnimInterpolation::Step ? 1
            : interpolation == ayt::resource::AnimInterpolation::CubicHermite
                ? 2 : 0);
        std::vector<float> values;
        ayt::resource::AnimTrackType valueType{};
        const bool hasValues = !_selectedKeyId.empty()
            && _document->animationKeyframeValues(
                _selectedKeyId, values, &valueType);
        (void)_keyFields->setValues(hasValues ? values : std::vector<float>{}, !hasValues);
        std::vector<float> incoming;
        std::vector<float> outgoing;
        const bool hasTangents = hasValues
            && _document->animationKeyframeTangents(
                _selectedKeyId, incoming, outgoing);
        const bool showTangents = hasTangents
            && interpolation == ayt::resource::AnimInterpolation::CubicHermite;
        for (auto* row : _tangentRows) row->setVisible(showTangents);
        (void)_inFields->setValues(showTangents ? incoming : std::vector<float>{}, !showTangents);
        (void)_outFields->setValues(showTangents ? outgoing : std::vector<float>{}, !showTangents);

        const auto notifies = _document->animationNotifies();
        _notifyIds.clear();
        std::vector<std::wstring> notifyItems;
        for (const auto& notify : notifies) {
            _notifyIds.push_back(notify.id);
            std::wostringstream label;
            label << ayt::ui::decodeUtf8Text(notify.name) << L"   @ "
                  << std::fixed << std::setprecision(3)
                  << notify.timeSeconds << L" s";
            notifyItems.push_back(label.str());
        }
        if (!_selectedNotifyId.empty()
            && std::find(_notifyIds.begin(), _notifyIds.end(),
                         _selectedNotifyId) == _notifyIds.end()) {
            _selectedNotifyId.clear();
        }
        const auto selectedNotify = std::find(
            _notifyIds.begin(), _notifyIds.end(), _selectedNotifyId);
        const int notifyIndex = selectedNotify == _notifyIds.end() ? -1
            : static_cast<int>(selectedNotify - _notifyIds.begin());
        _notifyPicker->setItems(notifyItems);
        _notifyPicker->setSelectedIndex(notifyIndex);
        if (notifyIndex >= 0) {
            const auto& notify = notifies[static_cast<std::size_t>(notifyIndex)];
            _notifyName->setText(ayt::ui::decodeUtf8Text(notify.name));
            std::wostringstream payload;
            payload << std::setprecision(7) << notify.payload;
            _notifyPayload->setText(payload.str());
        } else {
            _notifyName->setText(L"");
            _notifyPayload->setText(L"0");
        }
        _syncing = false;
        if (_dopeSheet != nullptr) {
            _dopeSheet->setSelection(
                notifyIndex >= 0
                    ? "event." + std::to_string(notifyIndex)
                    : _selectedTrackId,
                notifyIndex >= 0 ? _selectedNotifyId : _selectedKeyId);
        }
        if (_curveSource && selectionBefore.keyIds.size() > 1)
            ayt::ui::authoring::TimelineSelectionOps::replace(
                *_curveSource->selectionState(), selectionBefore);
        _editState->setText(_document->isDirty() ? L"Modified" : L"Saved");
        if (_curveSource) _selectionBridge.publish(*_curveSource->selectionState());
        _commands.refresh();
    }

    void refreshCurveControls()
    {
        if (_curveCanvas == nullptr) return;
        for (std::size_t component = 0u; component < 4u; ++component) {
            _curveComponents[component]->setText(
                _curveCanvas->componentVisible(component)
                    ? (component == 0u ? L"X ●" : component == 1u ? L"Y ●"
                        : component == 2u ? L"Z ●" : L"W ●")
                    : (component == 0u ? L"X ○" : component == 1u ? L"Y ○"
                        : component == 2u ? L"Z ○" : L"W ○"));
        }
    }

    void addTrack()
    {
        std::string node = encodeUtf8(_trackNode->getText());
        if (node.empty()) {
            const int selected = _document->selectedBone();
            if (selected >= 0
                && selected < static_cast<int>(_document->preview().bones().size())) {
                node = _document->preview().bones()[selected].name;
                _trackNode->setText(ayt::ui::decodeUtf8Text(node));
            }
        }
        const int propertyIndex = std::max(0, _trackProperty->getSelectedIndex());
        const char* property = propertyIndex == 1 ? "rotation"
            : propertyIndex == 2 ? "scale"
            : propertyIndex == 3 ? "value" : "position";
        const auto type = propertyIndex == 1
            ? ayt::resource::AnimTrackType::Quaternion
            : propertyIndex == 3 ? ayt::resource::AnimTrackType::Float
                                 : ayt::resource::AnimTrackType::Vector3;
        if (!_document->addAnimationTrack(node, property, type)) {
            _host.setStatusText(L"Track was not added; choose a bone/name and a unique property");
            return;
        }
        const auto tracks = _document->timelineTracks();
        const auto last = std::find_if(tracks.rbegin(), tracks.rend(),
            [](const auto& track) {
                return track.kind == EditorTimelineTrackKind::Animation;
            });
        if (last != tracks.rend()) _selectedTrackId = last->id;
        _selectedKeyId.clear();
        _host.setStatusText(L"Animation track added");
        refreshAll();
    }

    void removeTrack()
    {
        if (_selectedTrackId.empty()
            || !_document->removeAnimationTrack(_selectedTrackId)) return;
        _selectedTrackId.clear();
        _selectedKeyId.clear();
        _host.setStatusText(L"Animation track removed");
        refreshAll();
    }

    void addKey()
    {
        if (_selectedTrackId.empty()
            || !_document->timelineAddKeyframe(_selectedTrackId,
                _document->timelinePositionSeconds(), 0.0)) {
            _host.setStatusText(L"Key was not added; the track may already have a key here");
            return;
        }
        _selectedKeyId.clear();
        _host.setStatusText(L"Animation key added at playhead");
        refreshAll();
    }

    void moveKey()
    {
        if (_selectedKeyId.empty()
            || !_document->timelineMoveKeyframe(_selectedKeyId,
                _document->timelinePositionSeconds())) {
            _host.setStatusText(L"Key was not moved; another key may occupy this time");
            return;
        }
        _selectedKeyId.clear();
        _host.setStatusText(L"Animation key moved to playhead");
        refreshAll();
    }

    void deleteKey()
    {
        if (_selectedKeyId.empty()
            || !_document->timelineRemoveKeyframe(_selectedKeyId)) return;
        _selectedKeyId.clear();
        _host.setStatusText(L"Animation key deleted");
        refreshAll();
    }

    void applyKeyValues()
    {
        if (_selectedKeyId.empty()) return;
        std::vector<float> current;
        if (!_document->animationKeyframeValues(_selectedKeyId, current)) return;
        std::vector<float> values;
        if (!_keyFields->readValues(current.size(), values)) {
            _host.setStatusText(L"Key value must contain finite numbers");
            return;
        }
        if (!_document->setAnimationKeyframeValues(_selectedKeyId, values)) {
            _host.setStatusText(L"Key value was unchanged or invalid");
            refreshAuthoring();
            return;
        }
        _host.setStatusText(L"Animation key value updated");
        refreshAll();
    }

    void applyTangents()
    {
        if (_selectedKeyId.empty()) return;
        std::vector<float> incoming;
        std::vector<float> outgoing;
        if (!_document->animationKeyframeTangents(
                _selectedKeyId, incoming, outgoing)) return;
        if (!_inFields->readValues(incoming.size(), incoming)
            || !_outFields->readValues(outgoing.size(), outgoing)) {
            _host.setStatusText(L"Tangents must contain finite numbers");
            return;
        }
        if (!_document->setAnimationKeyframeTangents(
                _selectedKeyId, incoming, outgoing)) {
            _host.setStatusText(L"Tangents were unchanged or invalid");
            refreshAuthoring();
            return;
        }
        _host.setStatusText(L"Animation key tangents updated");
        refreshAll();
    }

    void autoTangents()
    {
        if (_selectedTrackId.empty()
            || !_document->autoAnimationTrackTangents(_selectedTrackId)) return;
        _host.setStatusText(L"Animation track tangents generated");
        refreshAll();
    }

    void applyClipProperties()
    {
        const auto duration = parseFiniteFloat(_clipDuration->getText());
        const auto ticks = parseFiniteFloat(_clipTicksPerSecond->getText());
        const std::string name = encodeUtf8(_clipName->getText());
        if (name.empty() || !duration || *duration <= 0.0f
            || !ticks || *ticks <= 0.0f) {
            _host.setStatusText(
                L"Clip name, duration and ticks/s must be valid positive values");
            return;
        }
        std::string error;
        if (!_document->setAnimationClipProperties(
                {name, *duration, *ticks}, &error)) {
            _host.setStatusText(error.empty()
                ? L"Clip properties were unchanged"
                : L"Clip properties rejected: "
                    + ayt::ui::decodeUtf8Text(error));
            refreshAuthoring();
            return;
        }
        if (_curveCanvas != nullptr) _curveCanvas->frameAll();
        if (_dopeSheet != nullptr) _dopeSheet->frameAll();
        _host.setStatusText(L"Animation clip properties updated");
        refreshAll();
    }

    void addNotify()
    {
        const std::string name = encodeUtf8(_notifyName->getText());
        const auto payload = parseFiniteFloat(_notifyPayload->getText());
        if (name.empty() || !payload) {
            _host.setStatusText(L"Notify requires a name and finite payload");
            return;
        }
        if (!_document->addAnimationNotify(name,
                _document->timelinePositionSeconds(), *payload)) return;
        const auto notifies = _document->animationNotifies();
        const auto selected = std::find_if(notifies.begin(), notifies.end(),
            [&](const auto& notify) {
                return notify.name == name
                    && std::fabs(notify.timeSeconds
                        - _document->timelinePositionSeconds()) < 1.0e-5
                    && notify.payload == *payload;
            });
        _selectedNotifyId = selected != notifies.end()
            ? selected->id : std::string{};
        _host.setStatusText(L"Animation notify added at playhead");
        refreshAll();
    }

    void updateNotify()
    {
        if (_selectedNotifyId.empty()) return;
        const std::string name = encodeUtf8(_notifyName->getText());
        const auto payload = parseFiniteFloat(_notifyPayload->getText());
        if (name.empty() || !payload) {
            _host.setStatusText(L"Notify requires a name and finite payload");
            return;
        }
        std::string updated = _selectedNotifyId;
        if (!_document->updateAnimationNotify(updated, name,
                _document->timelinePositionSeconds(), *payload)) return;
        _selectedNotifyId = std::move(updated);
        _host.setStatusText(L"Animation notify updated at playhead");
        refreshAll();
    }

    void deleteNotify()
    {
        if (_selectedNotifyId.empty()
            || !_document->removeAnimationNotify(_selectedNotifyId)) return;
        _selectedNotifyId.clear();
        _host.setStatusText(L"Animation notify deleted");
        refreshAll();
    }

    void locateDiagnostic(const std::string& target)
    {
        if (std::find(_trackIds.begin(), _trackIds.end(), target) != _trackIds.end()) {
            _selectedTrackId = target; _selectedKeyId.clear(); _selectionCleared = false;
            refreshAuthoring(); return;
        }
        for (const auto& bone : _document->preview().bones()) {
            if (target != "bone." + std::to_string(bone.index)) continue;
            if (_document->selectBone(bone.index)) {
                _selectionBridge.publish("skeleton.bone", {std::to_string(bone.index)}, std::to_string(bone.index));
                refreshInspector(); if (_canvas) _canvas->markDirty(); _host.requestRepaint();
            }
            return;
        }
    }

    void refreshDiagnostics()
    {
        using namespace ayt::ui::authoring;
        std::vector<DiagnosticEntry> entries;
        const auto tracks = _document->timelineTracks();
        for (const auto& diagnostic : _document->preview().diagnostics()) {
            const auto severity = diagnostic.severity == ayt::anim::editor::AnimationPreviewDiagnosticSeverity::Error
                ? DiagnosticSeverity::Error : diagnostic.severity == ayt::anim::editor::AnimationPreviewDiagnosticSeverity::Warning
                ? DiagnosticSeverity::Warning : DiagnosticSeverity::Info;
            std::string target;
            const auto track = "animation." + std::to_string(diagnostic.trackIndex);
            if (std::any_of(tracks.begin(), tracks.end(), [&](const auto& item) {
                return item.id == track && item.kind == EditorTimelineTrackKind::Animation;
            })) target = track;
            else if (diagnostic.boneIndex >= 0 && static_cast<std::size_t>(diagnostic.boneIndex) < _document->preview().bones().size())
                target = "bone." + std::to_string(diagnostic.boneIndex);
            entries.push_back({severity, ayt::ui::decodeUtf8Text(
                ayt::anim::editor::AnimationPreviewSession::diagnosticCodeName(diagnostic.code)),
                ayt::ui::decodeUtf8Text(diagnostic.message), target});
        }
        _diagnostics->setEntries(std::move(entries));
    }

    void refreshTransport()
    {
        _transport->refresh();
        _scrubBar->refresh();
    }

    std::shared_ptr<EditorAnimationDocument> _document;
    IEditorHostServices& _host;
    EditorCommandButtons _commands;
    EditorAuthoringSelectionBridge _selectionBridge;
    std::shared_ptr<ayt::ui::authoring::ICurveEditorSource> _curveSource;
    bool _selectionCleared = false;
    ayt::ui::Widget* _root = nullptr;
    EditorAnimationCanvas* _canvas = nullptr;
    EditorAnimationCurveCanvas* _curveCanvas = nullptr;
    EditorAnimationDopeSheet* _dopeSheet = nullptr;
    std::array<ayt::ui::Button*, 4> _curveComponents{};
    ayt::ui::ComboBox* _mode = nullptr;
    ayt::ui::TextInput* _skeletonPath = nullptr;
    ayt::ui::TextInput* _meshPath = nullptr;
    ayt::ui::TextInput* _materialPath = nullptr;
    ayt::ui::TextArea* _info = nullptr;
    ayt::ui::TextInput* _clipName = nullptr;
    ayt::ui::TextInput* _clipDuration = nullptr;
    ayt::ui::TextInput* _clipTicksPerSecond = nullptr;
    ayt::ui::TextArea* _tracks = nullptr;
    ayt::ui::ComboBox* _trackPicker = nullptr;
    ayt::ui::TextInput* _trackNode = nullptr;
    ayt::ui::ComboBox* _trackProperty = nullptr;
    ayt::ui::ComboBox* _keyPicker = nullptr;
    ayt::ui::authoring::NumericFields* _keyFields = nullptr;
    ayt::ui::TextLabel* _editState = nullptr;
    ayt::ui::ComboBox* _curveMode = nullptr;
    std::array<ayt::ui::HBox*, 2> _tangentRows{};
    ayt::ui::authoring::NumericFields* _inFields = nullptr;
    ayt::ui::authoring::NumericFields* _outFields = nullptr;
    ayt::ui::ComboBox* _notifyPicker = nullptr;
    ayt::ui::TextInput* _notifyName = nullptr;
    ayt::ui::TextInput* _notifyPayload = nullptr;
    ayt::ui::authoring::DiagnosticsPanel* _diagnostics = nullptr;
    ayt::ui::authoring::PlaybackControls* _transport = nullptr;
    ayt::ui::authoring::PlaybackControls* _scrubBar = nullptr;
    std::vector<std::string> _trackIds;
    std::vector<std::string> _keyIds;
    std::vector<std::string> _notifyIds;
    std::string _selectedTrackId;
    std::string _selectedKeyId;
    std::string _selectedNotifyId;
    ayt::ui::authoring::AuthoringRefreshGate _refreshGate;
    bool _syncing = false;
};

} // namespace

EditorDescriptor makeEditorAnimationDescriptor()
{
    EditorDescriptor descriptor;
    descriptor.id = kEditorAnimationTimelineExtensionId;
    descriptor.displayName = L"Animation";
    descriptor.surfaceKind = EditorSurfaceKind::Document;
    descriptor.openPolicy = EditorOpenPolicy::PerResource;
    descriptor.defaultDockSlot = EditorDockSlot::Center;
    descriptor.priority = 100;
    descriptor.extensions = {".ayanm", ".ayanim"};
    descriptor.assetTypes = {"Animation"};
    descriptor.createDocument = [](const EditorOpenRequest& request,
                                   std::string& error) {
        auto document = std::make_shared<EditorAnimationDocument>();
        return document->initialize(request, error)
            ? std::static_pointer_cast<IEditorDocument>(document) : nullptr;
    };
    descriptor.createView = [](const std::shared_ptr<IEditorDocument>& document,
                               IEditorHostServices& host) {
        auto animation = std::dynamic_pointer_cast<EditorAnimationDocument>(document);
        return animation != nullptr
            ? std::unique_ptr<IEditorView>(
                std::make_unique<EditorAnimationWorkspaceView>(animation, host))
            : nullptr;
    };
    return descriptor;
}

bool registerEditorAnimationExtension(EditorExtensionRegistry& registry,
                                      std::string* error)
{
    return registry.registerEditor(makeEditorAnimationDescriptor(), error);
}

} // namespace ayt::editor
