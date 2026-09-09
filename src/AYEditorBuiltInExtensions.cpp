#include "AYEditor/EditorBuiltInExtensions.h"

#include "AYEditor/EditorCommandSystem.h"
#include "AYEditor/EditorWorkspace.h"

#include <AY2DEditor/TilemapEditorModel.h>
#include <AYAudio/AudioSubSystem.h>
#include <AYResource/assetsImpl/Animation.h>
#include <AYResource/assetsImpl/Audio.h>
#include <AYResource/Converter/TilemapConverter.h>
#include <AYUI/Box.h>
#include <AYUI/Button.h>
#include <AYUI/Slider.h>
#include <AYUI/TextLabel.h>
#include <AYUI/UnicodeText.h>
#include <AYUI/Widget.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <utility>

namespace ayt::editor {
namespace {

bool saveAndCookTilemap(ayt::ay2d::editor::TilemapEditorModel& model,
                        const std::string& path, std::string* error)
{
    if (!model.save(path, error)) return false;
    const std::filesystem::path source =
        std::filesystem::absolute(path).lexically_normal();
    std::filesystem::path assetRoot;
    for (std::filesystem::path cursor = source.parent_path();
         !cursor.empty(); cursor = cursor.parent_path()) {
        if (cursor.filename() == "Assets") {
            assetRoot = cursor;
            break;
        }
        const std::filesystem::path parent = cursor.parent_path();
        if (parent == cursor) break;
    }
    if (assetRoot.empty()) {
        if (error != nullptr) {
            *error = "Tilemap source was saved, but cooking requires it below "
                     "the project Assets folder.";
        }
        return false;
    }
    ayt::resource::TilemapConverter converter(source.string());
    converter.setOutputDir(assetRoot.string());
    const ayt::resource::ConversionResult cooked = converter.convert();
    if (cooked.resources.size() != 1u) {
        if (error != nullptr) {
            *error = "Tilemap source was saved, but runtime cooking failed.";
        }
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

class ToolDocument final : public IEditorDocument {
public:
    ToolDocument(std::string type, std::string title)
        : _type(std::move(type)), _title(std::move(title)) {}
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
    std::string _type;
    std::string _title;
    std::string _path;
};

class TilemapWorkspaceDocument final : public IEditorDocument {
public:
    bool initialize(const EditorOpenRequest& request, std::string& error)
    {
        _path = request.resourcePath;
        if (_path.empty()) {
            if (!_model.newDocument(32u, 18u, 32u, 32u, 0u)) {
                error = "Could not initialize the tilemap model.";
                return false;
            }
            _title = request.displayPath.empty()
                ? "Untitled Tilemap" : request.displayPath;
            return true;
        }
        if (!_model.open(_path, &error)) return false;
        _title = std::filesystem::path(_path).filename().string();
        return true;
    }
    const std::string& typeId() const noexcept override { return _type; }
    const std::string& path() const noexcept override { return _path; }
    const std::string& title() const noexcept override { return _title; }
    bool isDirty() const noexcept override { return _model.dirty(); }
    uint64_t revision() const noexcept override { return _revision; }
    bool save(std::string* error) override {
        if (_path.empty()) {
            if (error != nullptr) *error = "Tilemap has no file path.";
            return false;
        }
        const bool saved = saveAndCookTilemap(_model, _path, error);
        if (saved) ++_revision;
        return saved;
    }
    bool canSaveAs() const noexcept override { return true; }
    bool saveAs(const std::string& path, std::string* error) override {
        const bool saved = !path.empty()
            && saveAndCookTilemap(_model, path, error);
        if (saved) {
            _path = path;
            _title = std::filesystem::path(path).filename().string();
            ++_revision;
        }
        return saved;
    }
    bool writeRecoveryCopy(const std::string& path,
                           std::string* error) const override {
        auto snapshot = _model;
        if (snapshot.save(path, error)) return true;
        if (error != nullptr && error->empty()) {
            *error = "Could not write tilemap recovery copy.";
        }
        return false;
    }
    ayt::ay2d::editor::TilemapEditorModel& model() noexcept { return _model; }
    void changed() noexcept { ++_revision; }
private:
    std::string _type = "ayeditor.tilemap.document";
    std::string _path;
    std::string _title = "Untitled Tilemap";
    uint64_t _revision = 1u;
    ayt::ay2d::editor::TilemapEditorModel _model;
};

class TilemapWorkspaceView final
    : public IEditorView, public IEditorCommandTarget {
public:
    TilemapWorkspaceView(std::shared_ptr<TilemapWorkspaceDocument> document,
                         IEditorHostServices& host)
        : _document(std::move(document)), _host(host)
    {
        auto* root = new ayt::ui::VBox();
        _root = root;
        root->setSpacing(8.0f);
        root->setPadding(12.0f, 10.0f, 12.0f, 10.0f);
        auto* heading = new ayt::ui::TextLabel();
        heading->setText(L"Tilemap");
        heading->setFontSize(15);
        root->addWidget(heading, 26.0f);
        _summary = new ayt::ui::TextLabel();
        _summary->setFontSize(12);
        root->addWidget(_summary, 48.0f);
        auto* note = new ayt::ui::TextLabel();
        note->setText(L"Backed by the shared AY2DEditorCore model. Canvas tools can be added here without another document format.");
        note->setFontSize(12);
        root->addWidget(note, 42.0f);
        refresh();
    }
    ~TilemapWorkspaceView() override {
        if (_root != nullptr) ayt::ui::destroyWidgetTree(_root);
    }
    ayt::ui::Widget* rootWidget() noexcept override { return _root; }
    ayt::ui::Widget* releaseRootWidget() noexcept override {
        auto* result = _root; _root = nullptr; return result;
    }
    IEditorCommandTarget* commandTarget() noexcept override { return this; }
    bool handlesCommand(const std::string& id) const override {
        return id == "file.save" || id == "edit.undo" || id == "edit.redo";
    }
    bool canExecuteCommand(const std::string& id) const override {
        if (id == "edit.undo") return _document->model().canUndo();
        if (id == "edit.redo") return _document->model().canRedo();
        return id == "file.save" && !_document->path().empty();
    }
    bool executeCommand(const std::string& id) override {
        if (!canExecuteCommand(id)) return false;
        bool changed = false;
        if (id == "edit.undo") changed = _document->model().undo();
        else if (id == "edit.redo") changed = _document->model().redo();
        else {
            std::string error;
            changed = _document->save(&error);
            if (!changed) _host.setStatusText(
                L"Tilemap save failed: " + ayt::ui::decodeUtf8Text(error));
        }
        if (changed) {
            _document->changed();
            refresh();
            _host.requestRepaint();
        }
        return changed;
    }
private:
    void refresh() {
        const auto& model = _document->model().document();
        _summary->setText(std::to_wstring(model.cols()) + L" × "
            + std::to_wstring(model.rows()) + L" cells   "
            + std::to_wstring(model.layerCount()) + L" layer(s)   "
            + std::to_wstring(model.tileWidth()) + L" × "
            + std::to_wstring(model.tileHeight()) + L" px tiles");
    }
    std::shared_ptr<TilemapWorkspaceDocument> _document;
    IEditorHostServices& _host;
    ayt::ui::Widget* _root = nullptr;
    ayt::ui::TextLabel* _summary = nullptr;
};

class TimedAssetDocument final
    : public IEditorDocument, public IEditorTimelineSource {
public:
    bool initialize(const EditorOpenRequest& request, bool audio,
                    std::string& error)
    {
        _path = request.resourcePath;
        _title = std::filesystem::path(_path).filename().string();
        _type = audio ? "ayeditor.timeline.audio.document"
                      : "ayeditor.timeline.animation.document";
        if (_path.empty() || !std::filesystem::is_regular_file(_path)) {
            error = "Timeline asset does not exist: " + _path;
            return false;
        }
        if (audio) {
            ayt::resource::Audio resource;
            if (resource.load(_path)) _duration = resource.getDuration();
            if (_duration <= 0.0) _duration = wavDuration(_path);
            _tracks.push_back({"audio", _title,
                EditorTimelineTrackKind::Audio, 0.0, _duration, true});
        } else {
            ayt::resource::Animation resource;
            if (!resource.load(_path)) {
                error = "Could not load animation timeline: " + _path;
                return false;
            }
            _duration = resource.getDuration();
            for (std::uint32_t index = 0; index < resource.getTrackCount(); ++index) {
                std::string name = resource.getTrackNodeName(index);
                const char* property = resource.getTrackProperty(index);
                if (property != nullptr && *property != '\0') {
                    name += " / ";
                    name += property;
                }
                _tracks.push_back({"animation." + std::to_string(index),
                    std::move(name), EditorTimelineTrackKind::Animation,
                    0.0, _duration, true});
            }
            for (std::uint32_t index = 0; index < resource.getNotifyCount(); ++index) {
                _tracks.push_back({"event." + std::to_string(index),
                    resource.getNotifyName(index), EditorTimelineTrackKind::Event,
                    resource.getNotifyTime(index),
                    resource.getNotifyTime(index), true});
            }
        }
        if (_tracks.empty()) {
            _tracks.push_back({"timeline", _title,
                audio ? EditorTimelineTrackKind::Audio
                      : EditorTimelineTrackKind::Animation,
                0.0, _duration, true});
        }
        return true;
    }
    const std::string& typeId() const noexcept override { return _type; }
    const std::string& path() const noexcept override { return _path; }
    const std::string& title() const noexcept override { return _title; }
    bool isDirty() const noexcept override { return false; }
    uint64_t revision() const noexcept override { return 1u; }
    bool save(std::string* error) override {
        if (error != nullptr) *error = "Timeline assets are read-only here.";
        return false;
    }
    double timelineDurationSeconds() const noexcept override {
        return _duration;
    }
    double timelinePositionSeconds() const noexcept override {
        return _position;
    }
    bool setTimelinePositionSeconds(double seconds) override {
        const double clamped = std::clamp(seconds, 0.0, _duration);
        if (std::abs(clamped - _position) < 1.0e-9) return false;
        _position = clamped;
        return true;
    }
    std::vector<EditorTimelineTrack> timelineTracks() const override {
        return _tracks;
    }
    bool timelinePlaying() const noexcept override { return _playing; }
    void timelinePlay() override {
        if (_duration > 0.0) {
            if (_position >= _duration) _position = 0.0;
            _playing = true;
        }
    }
    void timelinePause() override { _playing = false; }
    void timelineStop() override { _playing = false; _position = 0.0; }
    void timelineTick(double seconds) override {
        if (!_playing || seconds <= 0.0) return;
        _position += seconds;
        if (_position >= _duration) {
            _position = _duration;
            _playing = false;
        }
    }
private:
    static double wavDuration(const std::string& path)
    {
        std::ifstream input(path, std::ios::binary);
        char riff[4]{};
        std::uint32_t ignored = 0;
        char wave[4]{};
        input.read(riff, 4);
        input.read(reinterpret_cast<char*>(&ignored), 4);
        input.read(wave, 4);
        if (!input || std::string(riff, 4) != "RIFF"
            || std::string(wave, 4) != "WAVE") return 0.0;
        std::uint32_t byteRate = 0;
        std::uint32_t dataBytes = 0;
        while (input) {
            char id[4]{};
            std::uint32_t size = 0;
            input.read(id, 4);
            input.read(reinterpret_cast<char*>(&size), 4);
            if (!input) break;
            if (std::string(id, 4) == "fmt " && size >= 12u) {
                input.seekg(4, std::ios::cur);
                input.seekg(4, std::ios::cur);
                input.read(reinterpret_cast<char*>(&byteRate), 4);
                input.seekg(static_cast<std::streamoff>(size - 12u), std::ios::cur);
            } else if (std::string(id, 4) == "data") {
                dataBytes = size;
                input.seekg(size, std::ios::cur);
            } else {
                input.seekg(size, std::ios::cur);
            }
            if ((size & 1u) != 0u) input.seekg(1, std::ios::cur);
            if (byteRate != 0u && dataBytes != 0u) break;
        }
        return byteRate == 0u ? 0.0
            : static_cast<double>(dataBytes) / byteRate;
    }
    std::string _type;
    std::string _path;
    std::string _title;
    double _duration = 0.0;
    double _position = 0.0;
    bool _playing = false;
    std::vector<EditorTimelineTrack> _tracks;
};

class TimedAssetView final : public IEditorView {
public:
    explicit TimedAssetView(std::shared_ptr<TimedAssetDocument> document)
        : _document(std::move(document))
    {
        auto* root = new ayt::ui::VBox();
        _root = root;
        root->setSpacing(8.0f);
        root->setPadding(12.0f, 10.0f, 12.0f, 10.0f);
        auto* heading = new ayt::ui::TextLabel();
        heading->setText(ayt::ui::decodeUtf8Text(_document->title()));
        heading->setFontSize(15);
        root->addWidget(heading, 28.0f);
        auto* summary = new ayt::ui::TextLabel();
        summary->setText(L"Duration: "
            + std::to_wstring(_document->timelineDurationSeconds())
            + L" s   Tracks: "
            + std::to_wstring(_document->timelineTracks().size()));
        root->addWidget(summary, 26.0f);
        auto* note = new ayt::ui::TextLabel();
        note->setText(L"Use the Timeline panel to inspect tracks and scrub playback.");
        root->addWidget(note, 28.0f);
    }
    ~TimedAssetView() override {
        if (_root != nullptr) ayt::ui::destroyWidgetTree(_root);
    }
    ayt::ui::Widget* rootWidget() noexcept override { return _root; }
    ayt::ui::Widget* releaseRootWidget() noexcept override {
        auto* result = _root; _root = nullptr; return result;
    }
private:
    std::shared_ptr<TimedAssetDocument> _document;
    ayt::ui::Widget* _root = nullptr;
};

class AudioToolView final : public IEditorView {
public:
    explicit AudioToolView(std::function<void()> openAudio)
        : _openAudio(std::move(openAudio))
    {
        auto* root = new ayt::ui::VBox();
        _root = root;
        root->setSpacing(6.0f);
        root->setPadding(12.0f, 10.0f, 12.0f, 10.0f);
        auto* heading = new ayt::ui::TextLabel();
        heading->setText(L"Audio Mixer");
        heading->setFontSize(15);
        root->addWidget(heading, 26.0f);
        _status = new ayt::ui::TextLabel();
        _status->setFontSize(12);
        root->addWidget(_status, 22.0f);
        auto* transport = new ayt::ui::HBox();
        transport->setSpacing(6.0f);
        addButton(transport, L"Pause", [this]() {
            if (auto* value = engine()) value->pause();
        });
        addButton(transport, L"Resume", [this]() {
            if (auto* value = engine()) value->resume();
        });
        addButton(transport, L"Stop All", [this]() {
            if (auto* value = engine()) value->stopAll();
        });
        addButton(transport, L"Full Editor", [this]() {
            if (_openAudio) _openAudio();
        });
        root->addWidget(transport, 28.0f);
        addGain(root, L"Master", ayt::audio::AudioBus::Master, true);
        addGain(root, L"Music", ayt::audio::AudioBus::Music, false);
        addGain(root, L"SFX", ayt::audio::AudioBus::Sfx, false);
        addGain(root, L"UI", ayt::audio::AudioBus::Ui, false);
        addGain(root, L"Voice", ayt::audio::AudioBus::Voice, false);
        tick(0.0f);
    }
    ~AudioToolView() override {
        if (_root != nullptr) ayt::ui::destroyWidgetTree(_root);
    }
    ayt::ui::Widget* rootWidget() noexcept override { return _root; }
    ayt::ui::Widget* releaseRootWidget() noexcept override {
        auto* result = _root; _root = nullptr; return result;
    }
    void tick(float) override {
        auto* value = engine();
        const std::wstring status = value == nullptr
            ? L"Audio subsystem is unavailable."
            : std::wstring(value->isPaused() ? L"Paused" : L"Running")
                + L"   Active voices: "
                + std::to_wstring(value->activeVoiceCount());
        if (_status->getText() != status) _status->setText(status);
        if (value == nullptr) return;
        _updating = true;
        for (GainControl& control : _controls) {
            const float gain = control.master
                ? value->mixer().getMasterGain()
                : value->mixer().getBusGain(control.bus);
            if (std::abs(control.slider->getValue() - gain) > 0.001f) {
                control.slider->setValue(gain);
            }
            control.value->setText(std::to_wstring(
                static_cast<int>(std::round(gain * 100.0f))) + L"%");
        }
        _updating = false;
    }
    bool wantsBackgroundTick() const noexcept override { return true; }
private:
    struct GainControl {
        ayt::audio::AudioBus bus;
        bool master;
        ayt::ui::Slider* slider;
        ayt::ui::TextLabel* value;
    };
    static ayt::audio::AudioEngine* engine() {
        ayt::audio::AudioSubSystem* subsystem =
            ayt::audio::AudioSubSystem::findRegistered();
        return subsystem != nullptr ? subsystem->engine() : nullptr;
    }
    static void addButton(ayt::ui::HBox* row, const std::wstring& text,
                          std::function<void()> clicked) {
        auto* button = new ayt::ui::Button();
        button->setText(text);
        button->setOnClicked(std::move(clicked));
        row->addWidget(button, 82.0f);
    }
    void addGain(ayt::ui::VBox* root, const std::wstring& name,
                 ayt::audio::AudioBus bus, bool master) {
        auto* row = new ayt::ui::HBox();
        row->setSpacing(6.0f);
        auto* label = new ayt::ui::TextLabel();
        label->setText(name);
        row->addWidget(label, 64.0f);
        auto* slider = new ayt::ui::Slider();
        slider->setValueRange(0.0f, 1.5f);
        slider->setValue(1.0f);
        slider->setOnValueChanged([this, bus, master](float gain) {
            if (_updating) return;
            if (auto* value = engine()) {
                if (master) value->setMasterGain(gain);
                else value->setBusGain(bus, gain);
            }
        });
        row->addWidget(slider, 0.0f);
        auto* amount = new ayt::ui::TextLabel();
        amount->setText(L"100%");
        row->addWidget(amount, 48.0f);
        root->addWidget(row, 26.0f);
        _controls.push_back({bus, master, slider, amount});
    }
    std::function<void()> _openAudio;
    ayt::ui::Widget* _root = nullptr;
    ayt::ui::TextLabel* _status = nullptr;
    std::vector<GainControl> _controls;
    bool _updating = false;
};

class TimelineToolView final : public IEditorView {
public:
    explicit TimelineToolView(IEditorHostServices& host) : _host(host)
    {
        auto* root = new ayt::ui::VBox();
        _root = root;
        root->setSpacing(5.0f);
        root->setPadding(12.0f, 10.0f, 12.0f, 10.0f);
        auto* transport = new ayt::ui::HBox();
        transport->setSpacing(6.0f);
        addButton(transport, L"Play", [this]() {
            if (auto* value = source()) value->timelinePlay();
        });
        addButton(transport, L"Pause", [this]() {
            if (auto* value = source()) value->timelinePause();
        });
        addButton(transport, L"Stop", [this]() {
            if (auto* value = source()) value->timelineStop();
        });
        root->addWidget(transport, 28.0f);
        _label = new ayt::ui::TextLabel();
        _label->setFontSize(12);
        root->addWidget(_label, 22.0f);
        _ruler = new ayt::ui::TextLabel();
        _ruler->setFontSize(11);
        root->addWidget(_ruler, 20.0f);
        _playhead = new ayt::ui::Slider();
        _playhead->setValueRange(0.0f, 1.0f);
        _playhead->setOnValueChanged([this](float seconds) {
            if (_updating) return;
            if (auto* value = source()) {
                (void)value->setTimelinePositionSeconds(seconds);
            }
        });
        root->addWidget(_playhead, 24.0f);
        for (std::size_t index = 0; index < 8u; ++index) {
            auto* track = new ayt::ui::TextLabel();
            track->setFontSize(11);
            root->addWidget(track, 20.0f);
            _trackLabels.push_back(track);
        }
        tick(0.0f);
    }
    ~TimelineToolView() override {
        if (_root != nullptr) ayt::ui::destroyWidgetTree(_root);
    }
    ayt::ui::Widget* rootWidget() noexcept override { return _root; }
    ayt::ui::Widget* releaseRootWidget() noexcept override {
        auto* result = _root; _root = nullptr; return result;
    }
    void tick(float dt) override {
        IEditorTimelineSource* timeline = source();
        std::string sourceTitle;
        for (const EditorDocumentRecord& record :
             _host.workspace().documents().records()) {
            if (record.editorId == kEditorTimelineToolExtensionId) continue;
            if (dynamic_cast<IEditorTimelineSource*>(record.document.get())
                == timeline) {
                sourceTitle = record.document->title();
                break;
            }
        }
        if (timeline != nullptr) timeline->timelineTick(dt);
        const std::wstring text = timeline == nullptr
            ? L"No open document exposes timeline data."
            : ayt::ui::decodeUtf8Text(sourceTitle) + L"   "
                + std::to_wstring(timeline->timelinePositionSeconds()) + L" / "
                + std::to_wstring(timeline->timelineDurationSeconds()) + L" s"
                + (timeline->timelinePlaying() ? L"   Playing" : L"");
        if (_label->getText() != text) {
            _label->setText(text);
            _host.requestRepaint();
        }
        const double duration = timeline != nullptr
            ? timeline->timelineDurationSeconds() : 0.0;
        _ruler->setText(duration > 0.0
            ? L"0 s                 " + std::to_wstring(duration * 0.25)
                + L"                 " + std::to_wstring(duration * 0.5)
                + L"                 " + std::to_wstring(duration * 0.75)
                + L"                 " + std::to_wstring(duration) + L" s"
            : L"0 s");
        _updating = true;
        _playhead->setValueRange(0.0f,
            static_cast<float>(std::max(0.001, duration)));
        _playhead->setValue(static_cast<float>(timeline != nullptr
            ? timeline->timelinePositionSeconds() : 0.0));
        _updating = false;
        const std::vector<EditorTimelineTrack> tracks = timeline != nullptr
            ? timeline->timelineTracks() : std::vector<EditorTimelineTrack>{};
        for (std::size_t index = 0; index < _trackLabels.size(); ++index) {
            if (index >= tracks.size()) {
                _trackLabels[index]->setText(L"");
                _trackLabels[index]->setVisible(false);
                continue;
            }
            const EditorTimelineTrack& track = tracks[index];
            const wchar_t* marker = track.kind == EditorTimelineTrackKind::Audio
                ? L"♪" : (track.kind == EditorTimelineTrackKind::Event
                    ? L"◆" : L"●");
            _trackLabels[index]->setText(std::wstring(marker) + L"  "
                + ayt::ui::decodeUtf8Text(track.name) + L"   ["
                + std::to_wstring(track.startSeconds) + L" – "
                + std::to_wstring(track.endSeconds) + L"]");
            _trackLabels[index]->setVisible(true);
        }
    }
    bool wantsBackgroundTick() const noexcept override { return true; }
private:
    IEditorTimelineSource* source() const {
        for (const EditorDocumentRecord& record :
             _host.workspace().documents().records()) {
            if (record.editorId == kEditorTimelineToolExtensionId) continue;
            if (auto* value = dynamic_cast<IEditorTimelineSource*>(
                    record.document.get())) return value;
        }
        return nullptr;
    }
    static void addButton(ayt::ui::HBox* row, const std::wstring& text,
                          std::function<void()> clicked) {
        auto* button = new ayt::ui::Button();
        button->setText(text);
        button->setOnClicked(std::move(clicked));
        row->addWidget(button, 72.0f);
    }
    IEditorHostServices& _host;
    ayt::ui::Widget* _root = nullptr;
    ayt::ui::TextLabel* _label = nullptr;
    ayt::ui::TextLabel* _ruler = nullptr;
    ayt::ui::Slider* _playhead = nullptr;
    std::vector<ayt::ui::TextLabel*> _trackLabels;
    bool _updating = false;
};

bool add(EditorExtensionRegistry& registry, EditorDescriptor descriptor,
         std::string* error)
{
    std::string localError;
    if (registry.registerEditor(std::move(descriptor), &localError)) return true;
    if (error != nullptr) *error = std::move(localError);
    return false;
}

} // namespace

bool registerEditorBuiltInExtensions(
    EditorExtensionRegistry& registry,
    EditorBuiltInExtensionConfig config,
    std::string* error)
{
    EditorDescriptor tilemap;
    tilemap.id = kEditorTilemapExtensionId;
    tilemap.displayName = L"Tilemap";
    tilemap.surfaceKind = EditorSurfaceKind::Document;
    tilemap.openPolicy = EditorOpenPolicy::PerResource;
    tilemap.defaultDockSlot = EditorDockSlot::Center;
    tilemap.extensions = {".aytilemap", ".aytilemap.json"};
    tilemap.assetTypes = {"Tilemap"};
    tilemap.createDocument = [](const EditorOpenRequest& request,
                                std::string& localError) {
        auto document = std::make_shared<TilemapWorkspaceDocument>();
        return document->initialize(request, localError)
            ? std::static_pointer_cast<IEditorDocument>(document) : nullptr;
    };
    tilemap.createView = [](const std::shared_ptr<IEditorDocument>& document,
                            IEditorHostServices& host) {
        auto tilemapDocument = std::dynamic_pointer_cast<
            TilemapWorkspaceDocument>(document);
        return tilemapDocument != nullptr
            ? std::unique_ptr<IEditorView>(
                std::make_unique<TilemapWorkspaceView>(tilemapDocument, host))
            : nullptr;
    };
    if (!add(registry, std::move(tilemap), error)) return false;

    auto addTimedAsset = [&registry, error](
        const char* id, const wchar_t* displayName, bool isAudio,
        std::vector<std::string> extensions,
        std::vector<std::string> assetTypes) {
        EditorDescriptor descriptor;
        descriptor.id = id;
        descriptor.displayName = displayName;
        descriptor.surfaceKind = EditorSurfaceKind::Document;
        descriptor.openPolicy = EditorOpenPolicy::PerResource;
        descriptor.defaultDockSlot = EditorDockSlot::Center;
        descriptor.extensions = std::move(extensions);
        descriptor.assetTypes = std::move(assetTypes);
        descriptor.createDocument = [isAudio](const EditorOpenRequest& request,
                                              std::string& localError) {
            auto document = std::make_shared<TimedAssetDocument>();
            return document->initialize(request, isAudio, localError)
                ? std::static_pointer_cast<IEditorDocument>(document) : nullptr;
        };
        descriptor.createView = [](
            const std::shared_ptr<IEditorDocument>& document,
            IEditorHostServices&) {
            auto timed = std::dynamic_pointer_cast<TimedAssetDocument>(document);
            return timed != nullptr
                ? std::unique_ptr<IEditorView>(
                    std::make_unique<TimedAssetView>(std::move(timed)))
                : nullptr;
        };
        return add(registry, std::move(descriptor), error);
    };
    if (!addTimedAsset(kEditorAnimationTimelineExtensionId, L"Animation",
            false, {".ayanm", ".ayanim"}, {"Animation"})) return false;
    if (!addTimedAsset(kEditorAudioTimelineExtensionId, L"Audio",
            true, {".ayaudio", ".wav", ".ogg", ".mp3", ".flac"},
            {"Audio"})) return false;

    EditorDescriptor audio;
    audio.id = kEditorAudioToolExtensionId;
    audio.displayName = L"Audio Mixer";
    audio.surfaceKind = EditorSurfaceKind::ToolPanel;
    audio.openPolicy = EditorOpenPolicy::Singleton;
    audio.defaultDockSlot = EditorDockSlot::Bottom;
    audio.createDocument = [](const EditorOpenRequest&, std::string&) {
        return std::make_shared<ToolDocument>(
            "ayeditor.tool.audio.document", "Audio Mixer");
    };
    audio.createView = [openAudio = std::move(config.openAudioMixer)](
        const std::shared_ptr<IEditorDocument>&, IEditorHostServices&) {
        return std::unique_ptr<IEditorView>(
            std::make_unique<AudioToolView>(openAudio));
    };
    if (!add(registry, std::move(audio), error)) return false;

    EditorDescriptor timeline;
    timeline.id = kEditorTimelineToolExtensionId;
    timeline.displayName = L"Timeline";
    timeline.surfaceKind = EditorSurfaceKind::ToolPanel;
    timeline.openPolicy = EditorOpenPolicy::Singleton;
    timeline.defaultDockSlot = EditorDockSlot::Bottom;
    timeline.createDocument = [](const EditorOpenRequest&, std::string&) {
        return std::make_shared<ToolDocument>(
            "ayeditor.tool.timeline.document", "Timeline");
    };
    timeline.createView = [](const std::shared_ptr<IEditorDocument>&,
                             IEditorHostServices& host) {
        return std::unique_ptr<IEditorView>(
            std::make_unique<TimelineToolView>(host));
    };
    return add(registry, std::move(timeline), error);
}

} // namespace ayt::editor
