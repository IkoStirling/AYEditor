#include "AYEditor/EditorAnimationExtension.h"

#include "AYEditor/EditorAnimationDocument.h"
#include "AYEditor/EditorBuiltInExtensions.h"
#include "AYEditor/ImportDialog.h"
#include "AYEditorAnimationCanvas.h"

#include <AYResource/assetsDefs/IAnimation.h>
#include <AYResource/assetsImpl/Mesh.h>
#include <AYUI/Box.h>
#include <AYUI/Button.h>
#include <AYUI/ComboBox.h>
#include <AYUI/Slider.h>
#include <AYUI/TextArea.h>
#include <AYUI/TextInput.h>
#include <AYUI/TextLabel.h>
#include <AYUI/UnicodeText.h>
#include <AYUI/Widget.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <memory>
#include <sstream>

namespace ayt::editor {
namespace {

using ayt::anim::editor::AnimationPreviewMode;

std::string encodeUtf8(const std::wstring& value)
{
    if (value.empty()) return {};
    std::string result;
    for (wchar_t raw : value) {
        const std::uint32_t code = static_cast<std::uint32_t>(raw);
        if (code <= 0x7fu) result.push_back(static_cast<char>(code));
        else if (code <= 0x7ffu) {
            result.push_back(static_cast<char>(0xc0u | (code >> 6u)));
            result.push_back(static_cast<char>(0x80u | (code & 0x3fu)));
        } else {
            result.push_back(static_cast<char>(0xe0u | (code >> 12u)));
            result.push_back(static_cast<char>(0x80u | ((code >> 6u) & 0x3fu)));
            result.push_back(static_cast<char>(0x80u | (code & 0x3fu)));
        }
    }
    return result;
}

class EditorAnimationWorkspaceView final : public IEditorView {
public:
    EditorAnimationWorkspaceView(std::shared_ptr<EditorAnimationDocument> document,
                                 IEditorHostServices& host)
        : _document(std::move(document)), _host(host)
    {
        _document->configureProjectRoot(_host.projectRoot());
        build();
        refreshAll();
    }

    ~EditorAnimationWorkspaceView() override
    {
        if (_root != nullptr) ayt::ui::destroyWidgetTree(_root);
    }

    ayt::ui::Widget* rootWidget() noexcept override { return _root; }
    ayt::ui::Widget* releaseRootWidget() noexcept override {
        auto* result = _root;
        _root = nullptr;
        return result;
    }
    void tick(float dt) override
    {
        if (_document == nullptr) return;
        if (_document->timelinePlaying()) _document->timelineTick(dt);
        const auto& preview = _document->preview();
        if (_lastPoseRevision != preview.poseRevision()) {
            _lastPoseRevision = preview.poseRevision();
            refreshTransport();
            if (_canvas != nullptr) _canvas->markDirty();
            _host.requestRepaint();
        }
        if (_lastRevision != preview.revision()) {
            _lastRevision = preview.revision();
            refreshBindings();
            refreshInspector();
            refreshDiagnostics();
            refreshTransport();
            if (_canvas != nullptr) _canvas->markDirty();
            _host.requestRepaint();
        }
    }
    bool wantsBackgroundTick() const noexcept override {
        return _document != nullptr && _document->timelinePlaying();
    }

private:
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

    ayt::ui::HBox* makeBindingRow(const wchar_t* name,
                                  ayt::ui::TextInput*& input,
                                  std::function<void()> load)
    {
        auto* row = new ayt::ui::HBox();
        row->setSpacing(4.0f);
        auto* label = new ayt::ui::TextLabel();
        label->setText(name);
        label->setFontSize(11);
        label->setVerticalAlignment(ayt::ui::TextLabel::VAlignment::Center);
        row->addWidget(label, 64.0f);
        input = new ayt::ui::TextInput();
        row->addWidget(input, 0.0f);
        row->addWidget(makeButton(L"Load", std::move(load)), 48.0f);
        return row;
    }

    void build()
    {
        auto* root = new ayt::ui::VBox();
        _root = root;
        root->setSpacing(5.0f);
        root->setPadding(5.0f, 5.0f, 5.0f, 5.0f);

        auto* toolbar = new ayt::ui::HBox();
        toolbar->setSpacing(5.0f);
        toolbar->addWidget(makeButton(L"Play", [this]() {
            _document->timelinePlay(); refreshTransport();
        }), 50.0f);
        toolbar->addWidget(makeButton(L"Pause", [this]() {
            _document->timelinePause(); refreshTransport();
        }), 54.0f);
        toolbar->addWidget(makeButton(L"Stop", [this]() {
            _document->timelineStop(); refreshTransport();
        }), 50.0f);
        toolbar->addWidget(makeButton(L"Frame", [this]() {
            if (_canvas != nullptr) _canvas->framePreview();
        }), 54.0f);
        _loop = makeButton(L"Loop: On", [this]() {
            _document->setLooping(!_document->preview().looping());
            refreshTransport();
        });
        toolbar->addWidget(_loop, 76.0f);
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
        toolbar->addWidget(makeHeader(L"SPEED"), 46.0f);
        _speed = new ayt::ui::ComboBox();
        _speed->setItems({L"0.25x", L"0.5x", L"1.0x", L"1.5x", L"2.0x"});
        _speed->setOnSelectionChanged([this](int index) {
            if (_syncing || index < 0) return;
            static constexpr float values[] = {0.25f, 0.5f, 1.0f, 1.5f, 2.0f};
            _document->setPlayRate(values[std::min(index, 4)]);
            refreshTransport();
        });
        toolbar->addWidget(_speed, 78.0f);
        _transportStatus = new ayt::ui::TextLabel();
        _transportStatus->setFontSize(11);
        _transportStatus->setVerticalAlignment(
            ayt::ui::TextLabel::VAlignment::Center);
        toolbar->addWidget(_transportStatus, 0.0f);
        root->addWidget(toolbar, 30.0f);

        auto* body = new ayt::ui::HBox();
        body->setSpacing(5.0f);
        _canvas = new EditorAnimationCanvas(_document);
        _canvas->setOnBoneSelected([this](int) { refreshInspector(); });
        body->addWidget(_canvas, 0.0f);

        auto* inspector = new ayt::ui::VBox();
        inspector->setSpacing(4.0f);
        inspector->addWidget(makeHeader(L"ANIMATION PREVIEW"), 20.0f);
        _info = new ayt::ui::TextArea();
        _info->setReadOnly(true);
        _info->setWordWrap(false);
        inspector->addWidget(_info, 164.0f);

        inspector->addWidget(makeHeader(L"PREVIEW BINDINGS"), 20.0f);
        inspector->addWidget(makeBindingRow(L"Skeleton", _skeletonPath,
            [this]() { loadSkeleton(); }), 28.0f);
        inspector->addWidget(makeBindingRow(L"Mesh", _meshPath,
            [this]() { loadMesh(); }), 28.0f);
        inspector->addWidget(makeBindingRow(L"Material", _materialPath,
            [this]() { loadMaterial(); }), 28.0f);
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
        bindingActions->addWidget(makeButton(L"Pick Skeleton", [this]() {
            pickAndBind(true);
        }), 96.0f);
        bindingActions->addWidget(makeButton(L"Pick Mesh", [this]() {
            pickAndBind(false);
        }), 82.0f);
        inspector->addWidget(bindingActions, 28.0f);

        inspector->addWidget(makeHeader(L"TRACKS / EVENTS"), 20.0f);
        _tracks = new ayt::ui::TextArea();
        _tracks->setReadOnly(true);
        _tracks->setWordWrap(false);
        inspector->addWidget(_tracks, 0.0f);
        body->addWidget(inspector, 380.0f);
        root->addWidget(body, 0.0f);

        _diagnostics = new ayt::ui::TextArea();
        _diagnostics->setReadOnly(true);
        _diagnostics->setWordWrap(false);
        root->addWidget(_diagnostics, 72.0f);

        auto* timeline = new ayt::ui::HBox();
        timeline->setSpacing(5.0f);
        timeline->addWidget(makeHeader(L"TIMELINE"), 66.0f);
        _playhead = new ayt::ui::Slider();
        _playhead->setValueRange(0.0f, 1.0f);
        _playhead->setOnValueChanged([this](float value) {
            if (_syncing) return;
            (void)_document->setTimelinePositionSeconds(value);
            refreshTransport();
            if (_canvas != nullptr) _canvas->markDirty();
        });
        timeline->addWidget(_playhead, 0.0f);
        _time = new ayt::ui::TextLabel();
        _time->setFontSize(11);
        _time->setVerticalAlignment(ayt::ui::TextLabel::VAlignment::Center);
        timeline->addWidget(_time, 120.0f);
        auto* readOnly = new ayt::ui::TextLabel();
        readOnly->setText(L"Cooked clip - read-only");
        readOnly->setFontSize(11);
        readOnly->setTextColor({0.82f, 0.62f, 0.30f, 1.0f});
        timeline->addWidget(readOnly, 150.0f);
        root->addWidget(timeline, 30.0f);
    }

    void loadSkeleton()
    {
        std::string error;
        if (!_document->bindSkeleton(encodeUtf8(_skeletonPath->getText()), &error)) {
            _host.setStatusText(L"Skeleton binding failed: "
                + ayt::ui::decodeUtf8Text(error));
            return;
        }
        _host.setStatusText(L"Skeleton bound to animation preview");
        refreshAll();
    }

    void loadMesh()
    {
        std::string error;
        if (!_document->bindMesh(encodeUtf8(_meshPath->getText()), &error)) {
            _host.setStatusText(L"Mesh binding failed: "
                + ayt::ui::decodeUtf8Text(error));
            return;
        }
        _host.setStatusText(L"Mesh bound to animation preview");
        refreshAll();
    }

    void loadMaterial()
    {
        _document->setMaterialPath(encodeUtf8(_materialPath->getText()));
        _host.setStatusText(L"Preview material binding saved");
        refreshAll();
    }

    void pickAndBind(bool skeleton)
    {
        const std::string path = ImportDialog::showOpenAssetFileDialog(nullptr);
        if (path.empty()) return;
        if (skeleton) {
            _skeletonPath->setText(ayt::ui::decodeUtf8Text(path));
            loadSkeleton();
        } else {
            _meshPath->setText(ayt::ui::decodeUtf8Text(path));
            loadMesh();
        }
    }

    void refreshAll()
    {
        refreshBindings();
        refreshInspector();
        refreshDiagnostics();
        refreshTransport();
        _lastRevision = _document->preview().revision();
        _lastPoseRevision = _document->preview().poseRevision();
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
        const float speed = preview.playRate();
        const float values[] = {0.25f, 0.5f, 1.0f, 1.5f, 2.0f};
        int nearest = 0;
        for (int index = 1; index < 5; ++index) {
            if (std::abs(values[index] - speed) < std::abs(values[nearest] - speed)) {
                nearest = index;
            }
        }
        _speed->setSelectedIndex(nearest);
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

    void refreshDiagnostics()
    {
        std::wostringstream text;
        for (const auto& diagnostic : _document->preview().diagnostics()) {
            text << (diagnostic.severity
                        == ayt::anim::editor::AnimationPreviewDiagnosticSeverity::Error
                    ? L"ERROR "
                    : diagnostic.severity
                        == ayt::anim::editor::AnimationPreviewDiagnosticSeverity::Warning
                    ? L"WARN  " : L"INFO  ")
                 << L"[" << ayt::ui::decodeUtf8Text(
                        ayt::anim::editor::AnimationPreviewSession::diagnosticCodeName(
                            diagnostic.code)) << L"] "
                 << ayt::ui::decodeUtf8Text(diagnostic.message) << L"\n";
        }
        const std::wstring value = text.str();
        if (_diagnostics->getText() != value) _diagnostics->setText(value);
    }

    void refreshTransport()
    {
        const auto& preview = _document->preview();
        _syncing = true;
        _playhead->setValueRange(0.0f, std::max(0.001f, preview.duration()));
        _playhead->setValue(preview.time());
        _syncing = false;
        std::wostringstream time;
        time << std::fixed << std::setprecision(3)
             << preview.time() << L" / " << preview.duration() << L" s";
        const std::wstring timeText = time.str();
        if (_time->getText() != timeText) _time->setText(timeText);
        _loop->setText(preview.looping() ? L"Loop: On" : L"Loop: Off");
        const std::wstring status = preview.isPlaying() ? L"Playing"
            : preview.time() > 0.0f ? L"Paused" : L"Stopped";
        if (_transportStatus->getText() != status) {
            _transportStatus->setText(status);
        }
    }

    std::shared_ptr<EditorAnimationDocument> _document;
    IEditorHostServices& _host;
    ayt::ui::Widget* _root = nullptr;
    EditorAnimationCanvas* _canvas = nullptr;
    ayt::ui::Button* _loop = nullptr;
    ayt::ui::ComboBox* _mode = nullptr;
    ayt::ui::ComboBox* _speed = nullptr;
    ayt::ui::TextInput* _skeletonPath = nullptr;
    ayt::ui::TextInput* _meshPath = nullptr;
    ayt::ui::TextInput* _materialPath = nullptr;
    ayt::ui::TextArea* _info = nullptr;
    ayt::ui::TextArea* _tracks = nullptr;
    ayt::ui::TextArea* _diagnostics = nullptr;
    ayt::ui::Slider* _playhead = nullptr;
    ayt::ui::TextLabel* _time = nullptr;
    ayt::ui::TextLabel* _transportStatus = nullptr;
    std::uint64_t _lastRevision = 0u;
    std::uint64_t _lastPoseRevision = 0u;
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
