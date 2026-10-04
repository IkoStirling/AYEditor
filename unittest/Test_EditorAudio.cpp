#include "../../AYAudio/demo/audio_editor/ChartDocument.h"
#include <thread>
#include <chrono>
#include <AYAudio/AudioRecorder.h>
#include <AYEditor/EditorAudioFramePacer.h>
#include "../../AYAudio/demo/audio_editor/AudioEditorSession.h"
#include <AYAudio.h>
#include <AYEditor/EditorBuiltInExtensions.h>
#include <AYResource/Loader/AudioLoader.h>
#include <AYResource/assetsImpl/Audio.h>
#include <AYTest.h>
#include <AYUI/Button.h>
#include <AYUI/MockRenderer.h>
#include <AYUI/Slider.h>
#include <AYUI/TabControl.h>
#include <AYUI/TextInput.h>
#include <AYUI/TextLabel.h>
#include <AYUI/Theme.h>
#include <AYUI/Widget.h>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
using namespace ayt::audio;
using namespace ayt::editor;
TEST_SUITE(EditorAudioTests)
TEST_CASE(FocusedAudioPacingLeavesInputLiveBetweenUiFrames)
{
    EditorAudioFramePacer pacer;
    CHECK_FALSE(pacer.advance(false, 0.08f).due);
    CHECK(pacer.advance(true, 0.08f).due); // immediate first focused frame
    AudioSubSystem audio;
    CHECK(audio.initialize());
    ayt::ui::MockRenderer backend;
    ayt::ui::UIManager ui;
    ui.initialize(&backend);
    ui.setClientSize(960, 900);
    ayt::ui::ThemeManager::get().ensureDefaultThemes();
    ayt::ui::ThemeManager::get().setActiveTheme("dark");
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    CHECK(ui.loadLayout((root / "EngineAssets/AYAudio/ui/audio_editor.ui.json").string()));
    AudioEditorSession session;
    session.setAudio(&audio);
    session.setDriveAudioUpdate(false);
    session.setAssetRoot((root / "EngineAssets/AYAudio").string());
    CHECK(session.attach(ui));
    int refreshes = 0;
    float refreshedTime = 0;
    for (int i = 0; i < 50; ++i) {
        const auto refresh = pacer.advance(true, 0.004f);
        if (refresh.due) {
            ++refreshes;
            refreshedTime += refresh.deltaSeconds;
            session.tick(refresh.deltaSeconds);
        } else {
            // Production beforeKey calls noteOn directly during pollEvents,
            // even when telemetry/painting is not due on this iteration.
            CHECK(session.handleKey('A', true));
            CHECK(audio.engine()->activeVoiceCount() == 1);
            CHECK(session.handleKey('A', false));
            session.releaseKeys();
        }
    }
    CHECK(refreshes == 10);
    CHECK(std::abs(refreshedTime - 0.2f) < 0.0001f);
    CHECK_FALSE(pacer.advance(false, 0.0f).due);
    CHECK(pacer.advance(true, 0.0f).due); // reopen/focus return never waits
    const auto resumed = pacer.advance(true, 10.0f);
    CHECK(resumed.due);
    CHECK(resumed.deltaSeconds <= 0.1f); // no unbounded catch-up burst
    CHECK_FALSE(pacer.advance(true, 0.0f).due);
    CHECK_FALSE(pacer.advance(true, -1.0f).due);
    CHECK_FALSE(pacer.advance(true, std::nanf("")).due);
    session.detach();
    ui.shutdown();
    audio.shutdown();
}

TEST_CASE(SessionClosePreservesForeignVoicesAndMixerState)
{
    AudioSubSystem audio;
    CHECK(audio.initialize());
    auto *e = audio.engine();
    float pcm[32]{};
    AudioClipDesc desc;
    desc.channels = 1;
    desc.frameCount = 32;
    desc.pcmData = pcm;
    const auto c = e->registerClip(desc);
    const auto foreign = e->play(c);
    e->setMasterGain(0.6f);
    e->setBusGain(AudioBus::Music, 0.4f);
    struct TraceRenderer : ayt::ui::MockRenderer
    {
        std::unordered_map<size_t, int> fonts;
        void drawText(const ayt::math::FRectangle &b, const std::wstring &t, int font,
                      const ayt::math::FVector4 &color) override
        {
            ayt::ui::MockRenderer::drawText(b, t, font, color);
            fonts[getDrawCalls().size() - 1] = font;
        }
        void drawText(const ayt::math::FRectangle &b, const std::wstring &t, int font,
                      const ayt::ui::IRenderBackend::TextStyle &style) override
        {
            ayt::ui::MockRenderer::drawText(b, t, font, style);
            fonts[getDrawCalls().size() - 1] = font;
        }
    } renderer;
    ayt::ui::UIManager ui;
    ui.initialize(&renderer);
    ayt::ui::ThemeManager::get().ensureDefaultThemes();
    ayt::ui::ThemeManager::get().setActiveTheme("dark");
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    CHECK(ui.loadLayout((root / "EngineAssets/AYAudio/ui/audio_editor.ui.json").string()));
    AudioEditorSession session;
    session.setAudio(&audio);
    session.setDriveAudioUpdate(false);
    session.setAssetRoot((root / "EngineAssets/AYAudio").string());
    CHECK(session.attach(ui));
    auto *instrument = dynamic_cast<ayt::ui::TextLabel *>(ui.findById("lbl_instrument"));
    CHECK(instrument != nullptr);
    CHECK(instrument->getText().find(L"fallback") == std::wstring::npos);
    CHECK(e->masterGain() == 0.6f);
    CHECK(e->busGain(AudioBus::Music) == 0.4f);
    CHECK(session.handleKey('A', true));
    const auto count = e->activeVoiceCount();
    CHECK(session.handleKey('A', true));
    CHECK(e->activeVoiceCount() == count);
    ui.setClientSize(960, 900);
    ui.layout();
    auto *key = ui.findById("piano_keyboard");
    CHECK(key != nullptr);
    const auto bounds = session.pianoKeyBounds(61);
    CHECK(bounds.maxY <= 900);
    CHECK(session.handleMouseButton((bounds.minX + bounds.maxX) / 2, (bounds.minY + bounds.maxY) / 2, 0, true));
    CHECK(e->activeVoiceCount() == count + 1);
    CHECK(session.handleMouseButton(-20, -20, 0, false));
    session.releaseKeys();
    CHECK(session.handleKey('D', true));
    CHECK(session.handleKey(32, true));
    CHECK(session.handleKey('D', false));
    CHECK(session.handleKey(32, false));
    session.releaseKeys();
    CHECK(e->activeVoiceCount() == 1);
    session.detach();
    CHECK(e->clipRegistry().size() == 1);
    VoiceRuntime v;
    CHECK(e->voiceState(foreign, v));
    for (int i = 0; i < 5; ++i)
    {
        CHECK(session.attach(ui));
        session.detach();
    }
    CHECK(e->activeVoiceCount() == 1);
    e->stop(foreign);
    e->releaseClip(c);
    CHECK(session.attach(ui));
    CHECK(session.handleKey('A', true));
    audio.shutdown();
    CHECK(audio.initialize());
    e = audio.engine();
    const auto newClip = e->registerClip(desc);
    const auto newForeign = e->play(newClip);
    // Toolbar actions may arrive before the first tick notices engine replacement.
    auto *stop = dynamic_cast<ayt::ui::Button *>(ui.findById("btn_stop_all"));
    CHECK(stop != nullptr);
    const auto stopBounds = stop->getWorldBounds();
    ayt::ui::UIMouseEvent click({(stopBounds.minX + stopBounds.maxX) / 2, (stopBounds.minY + stopBounds.maxY) / 2}, 0);
    stop->onMouseMove(click);
    stop->onMouseButtonDown(click);
    stop->onMouseButtonUp(click);
    VoiceRuntime fresh;
    CHECK(e->voiceState(newForeign, fresh));
    session.detach();
    CHECK(e->clipRegistry().size() == 1);
    CHECK(session.attach(ui));
    CHECK(session.handleKey('A', true));
    session.detach();
    CHECK(e->voiceState(newForeign, fresh));
    CHECK(e->clipRegistry().size() == 1);
    e->stop(newForeign);
    e->releaseClip(newClip);
    audio.shutdown();
}
TEST_CASE(AudioRangeFadesPersistUndoAndRejectInvalidInput)
{
    const auto temp = std::filesystem::temp_directory_path() / "ay-editor-audio-regression.aud";
    struct Cleanup
    {
        std::filesystem::path path;
        ~Cleanup()
        {
            std::error_code ec;
            std::filesystem::remove(path, ec);
            std::filesystem::remove(path.string() + ".timeline.json", ec);
        }
    } cleanup{temp};
    ayt::resource::Audio source;
    source.createSineWave(440, 1);
    std::vector<UInt8> bytes;
    CHECK(source.saveToBinary(bytes));
    std::ofstream out(temp, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    out.close();
    EditorExtensionRegistry registry;
    std::string error;
    CHECK(registerEditorBuiltInExtensions(registry, {}, &error));
    const auto *extension = registry.find(kEditorAudioTimelineExtensionId);
    EditorOpenRequest request;
    request.resourcePath = temp.string();
    auto document = extension->createDocument(request, error);
    CHECK(document != nullptr);
    if (!document)
        return;
    auto *timeline = dynamic_cast<IEditorTimelineSource *>(document.get());
    CHECK(timeline != nullptr);
    if (!timeline)
        return;
    const auto pianoPath = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path() /
                           "EngineAssets/AYAudio/samples/C4.mp3";
    ayt::resource::AudioLoader loader;
    auto piano = std::dynamic_pointer_cast<ayt::resource::Audio>(loader.load(pianoPath.string()));
    CHECK(piano != nullptr);
    EditorOpenRequest pianoRequest;
    pianoRequest.resourcePath = pianoPath.string();
    auto pianoDocument = extension->createDocument(pianoRequest, error);
    CHECK(pianoDocument != nullptr);
    if (piano && pianoDocument)
    {
        auto *t = dynamic_cast<IEditorTimelineSource *>(pianoDocument.get());
        CHECK(t != nullptr);
        const double duration = static_cast<double>(piano->getDataSize()) /
                                (piano->getChannels() * (piano->getBitsPerSample() / 8u) * piano->getSampleRate());
        CHECK(std::abs(t->timelineDurationSeconds() - duration) < 1e-5);
        CHECK(!t->timelineWaveformPeaks("audio").empty());
    }
    CHECK(timeline->setAudioPreviewSettings({0.2, 0.8, true}));
    CHECK(!timeline->setAudioPreviewSettings({0.9, 0.2, true}));
    CHECK(!timeline->setAudioPreviewSettings({0, 2, false}));
    auto clip = timeline->timelineClips().front();
    clip.durationSeconds = 0.5;
    clip.sourceOffsetSeconds = 0.1;
    clip.gain = 0.4f;
    clip.fadeInSeconds = 0.02;
    clip.fadeOutSeconds = 0.1;
    CHECK(timeline->timelineUpdateClip(clip));
    CHECK(timeline->timelineUndo());
    CHECK(timeline->timelineClips().front().gain == 1);
    CHECK(timeline->timelineRedo());
    auto bad = clip;
    bad.sourceOffsetSeconds = 0.8;
    CHECK(!timeline->timelineUpdateClip(bad));
    CHECK(document->save(&error));
    auto reload = extension->createDocument(request, error);
    CHECK(reload != nullptr);
    if (!reload)
        return;
    auto *rt = dynamic_cast<IEditorTimelineSource *>(reload.get());
    CHECK(rt->audioPreviewSettings().loop);
    CHECK(rt->audioPreviewSettings().beginSeconds == 0.2);
    CHECK(rt->timelineClips().front().fadeOutSeconds == 0.1);
    CHECK(rt->timelineClips().front().gain == 0.4f);
}

TEST_CASE(WorkbenchTransposeZoomMixerAndResourceSidecar)
{
    AudioSubSystem audio;
    CHECK(audio.initialize());
    auto *engine = audio.engine();
    struct TraceRenderer : ayt::ui::MockRenderer
    {
        std::unordered_map<size_t, int> fonts;
        void drawText(const ayt::math::FRectangle &b, const std::wstring &t, int font,
                      const ayt::math::FVector4 &color) override
        {
            ayt::ui::MockRenderer::drawText(b, t, font, color);
            fonts[getDrawCalls().size() - 1] = font;
        }
        void drawText(const ayt::math::FRectangle &b, const std::wstring &t, int font,
                      const ayt::ui::IRenderBackend::TextStyle &style) override
        {
            ayt::ui::MockRenderer::drawText(b, t, font, style);
            fonts[getDrawCalls().size() - 1] = font;
        }
    } renderer;
    ayt::ui::UIManager ui;
    ui.initialize(&renderer);
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    CHECK(ui.loadLayout((root / "EngineAssets/AYAudio/ui/audio_editor.ui.json").string()));
    AudioEditorSession session;
    session.setAudio(&audio);
    session.setDriveAudioUpdate(false);
    session.setAssetRoot((root / "EngineAssets/AYAudio").string());
    CHECK(session.attach(ui));
    ui.setClientSize(1100, 820);
    ui.layout();
    auto *tabs = dynamic_cast<ayt::ui::TabControl *>(ui.findById("audio_tabs"));
    CHECK(tabs);
    // Optional headless render trace for inspecting actual widget geometry.
    auto renderTrace = [&](const char *name) {
        renderer.clear();
        ui.render();
        CHECK(!renderer.getDrawCalls().empty());
        if (const auto *directory = std::getenv("AY_AUDIO_RENDER_TRACE"))
        {
            nlohmann::json trace = nlohmann::json::array();
            size_t drawIndex = 0;
            for (const auto &call : renderer.getDrawCalls())
            {
                std::vector<unsigned> text;
                for (auto c : call.text)
                    text.push_back(static_cast<unsigned>(c));
                trace.push_back(
                    {{"type", static_cast<int>(call.type)},
                     {"bounds", {call.bounds.minX, call.bounds.minY, call.bounds.maxX, call.bounds.maxY}},
                     {"color", {call.color.x, call.color.y, call.color.z, call.color.w}},
                     {"text", text},
                     {"fontSize", call.type == ayt::ui::MockRenderer::DrawCall::Text ? renderer.fonts[drawIndex] : 0}});
                ++drawIndex;
            }
            std::ofstream output(std::filesystem::path(directory) / (std::string(name) + ".json"));
            output << trace.dump();
        }
    };
    renderTrace("performance");
    auto *transpose = dynamic_cast<ayt::ui::TextInput *>(ui.findById("transpose_value"));
    CHECK(transpose);
    ui.setFocus(transpose);
    transpose->setText(L"+1");
    transpose->onKeyDown(13);
    ui.setFocus(nullptr);
    CHECK(session.handleKey('A', true));
    VoiceRuntime note;
    bool found = false;
    for (uint32_t i = 0; i < 64; ++i)
        if (engine->voiceStateAt(i, note))
        {
            found = true;
            break;
        }
    CHECK(found);
    CHECK(std::abs(note.pitch - std::exp2(1.f / 12)) < 1e-5f);
    session.onFocusLost();
    CHECK(engine->activeVoiceCount() == 0);
    ui.setFocus(transpose);
    CHECK(!session.handleKey('E', true));
    ui.setFocus(nullptr);
    auto *piano = ui.findById("piano_keyboard");
    CHECK(piano);
    auto before = session.pianoKeyBounds(60);
    const auto pb = piano->getWorldBounds();
    CHECK(piano->onMouseWheel({{pb.minX + 100, pb.minY + 50}, -40}));
    auto after = session.pianoKeyBounds(60);
    CHECK(after.maxX - after.minX > before.maxX - before.minX);
    CHECK(session.handleMouseButton(pb.minX + 100, pb.minY + 10, 0, true));
    CHECK(session.handleMouseMove(pb.minX + 160, pb.minY + 10));
    CHECK(session.handleMouseButton(pb.minX + 160, pb.minY + 10, 0, false));
    CHECK(session.pianoKeyBounds(60).minX > after.minX);
    auto *meter = ui.findById("audio_output_meter");
    CHECK(meter);
    const auto mb = meter->getWorldBounds();
    CHECK(mb.maxY < pb.minY);
    CHECK(mb.maxY - mb.minY <= 80);
    CHECK(mb.maxX - mb.minX <= 150);
    tabs->setSelectedIndex(2);
    ui.layout();
    renderTrace("mixer");
    CHECK(!ui.findById("piano_keyboard"));
    CHECK(!session.handleKey('A', true));
    auto *value = dynamic_cast<ayt::ui::TextInput *>(ui.findById("val_master"));
    auto *slider = dynamic_cast<ayt::ui::Slider *>(ui.findById("sld_master"));
    CHECK(value);
    CHECK(slider);
    ui.setFocus(value);
    value->setText(L"63");
    value->onKeyDown(13);
    CHECK(std::abs(engine->masterGain() - .63f) < 1e-6f);
    value->setText(L"nan");
    value->onKeyDown(13);
    CHECK(value->getText() == L"63");
    const auto sb = slider->getWorldBounds(), vb = value->getWorldBounds();
    CHECK(vb.minX > sb.maxX);
    CHECK(std::abs(vb.minY - sb.minY) < 1);
    tabs->setSelectedIndex(1);
    ui.layout();
    CHECK(!ui.findById("piano_keyboard"));
    const auto temp = std::filesystem::temp_directory_path() / "ay-workbench-regression.aud";
    struct Cleanup
    {
        std::filesystem::path p;
        ~Cleanup()
        {
            std::error_code ec;
            std::filesystem::remove(p, ec);
            std::filesystem::remove(p.string() + ".timeline.json", ec);
        }
    } cleanup{temp};
    ayt::resource::Audio source;
    source.createSineWave(440, 1);
    std::vector<UInt8> bytes;
    CHECK(source.saveToBinary(bytes));
    std::ofstream output(temp, std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    output.close();
    CHECK(session.openResource(temp.string()));
    renderTrace("resource");
    auto edit = [&](const char *id, const wchar_t *text) {
        auto *input = dynamic_cast<ayt::ui::TextInput *>(ui.findById(id));
        CHECK(input);
        ui.setFocus(input);
        input->setText(text);
        input->onKeyDown(13);
        ui.setFocus(nullptr);
    };
    edit("range_begin", L"0.2");
    edit("range_end", L"0.8");
    edit("clip_gain", L"45");
    edit("fade_in", L"0.05");
    auto click = [&](const char *id) {
        auto *button = dynamic_cast<ayt::ui::Button *>(ui.findById(id));
        CHECK(button);
        const auto b = button->getWorldBounds();
        ayt::ui::UIMouseEvent event({(b.minX + b.maxX) / 2, (b.minY + b.maxY) / 2}, 0);
        button->onMouseMove(event);
        button->onMouseButtonDown(event);
        button->onMouseButtonUp(event);
    };
    click("resource_save");
    std::ifstream input(temp.string() + ".timeline.json");
    nlohmann::json json;
    input >> json;
    CHECK(json["clips"][0]["sourceOffset"] == 0.2);
    CHECK(std::abs(json["clips"][0]["gain"].get<float>() - .45f) < 1e-6f);
    CHECK(json["preview"]["begin"] == 0);
    CHECK(std::abs(json["preview"]["end"].get<double>() - .6) < 1e-6);
    EditorExtensionRegistry registry;
    std::string error;
    CHECK(registerEditorBuiltInExtensions(registry, {}, &error));
    EditorOpenRequest request;
    request.resourcePath = temp.string();
    auto document = registry.find(kEditorAudioTimelineExtensionId)->createDocument(request, error);
    CHECK(document);
    auto *timeline = dynamic_cast<IEditorTimelineSource *>(document.get());
    CHECK(timeline);
    CHECK(timeline->timelineClips().front().sourceOffsetSeconds == .2);
    CHECK(timeline->audioPreviewSettings().beginSeconds == 0);
    click("resource_play");
    CHECK(engine->activeVoiceCount() == 1);
    click("resource_stop");
    CHECK(engine->activeVoiceCount() == 0);
    input.close();
    json["schemaVersion"] = 99;
    std::ofstream unsupported(temp.string() + ".timeline.json");
    unsupported << json;
    unsupported.close();
    CHECK(session.openResource(temp.string()));
    auto *range = dynamic_cast<ayt::ui::TextInput *>(ui.findById("range_begin"));
    CHECK(range->isReadOnly());
    click("resource_save");
    std::ifstream unchanged(temp.string() + ".timeline.json");
    nlohmann::json preserved;
    unchanged >> preserved;
    unchanged.close();
    CHECK(preserved["schemaVersion"] == 99);
    std::ofstream malformed(temp.string() + ".timeline.json");
    malformed << "[]";
    malformed.close();
    CHECK(session.openResource(temp.string()));
    CHECK(range->isReadOnly());
    session.detach();
    audio.shutdown();
}

TEST_CASE(WorkbenchAttachesAfterNativeHostLayoutWithoutResize) {
    ayt::ui::MockRenderer renderer;ayt::ui::UIManager primary;primary.initialize(&renderer);
    ayt::ui::UIManager child;child.initialize(&renderer);
    const auto root=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    // Native child: viewport first, layout loader computes and caches geometry,
    // then the session mounts its programmatic subtree. There is no resize.
    child.setClientSize(960,900);
    CHECK(child.loadLayout((root/"EngineAssets/AYAudio/ui/audio_editor.ui.json").string()));
    child.layout();
    ayt::ui::UIManager::makeActive(&primary);
    AudioEditorSession session;
    for(int attach=0;attach<2;++attach){
        CHECK(session.attach(child));
        auto* piano=child.findById("piano_keyboard");auto* meter=child.findById("audio_output_meter");
        CHECK(piano!=nullptr);CHECK(meter!=nullptr);
        if(piano&&meter){
            const auto p=piano->getWorldBounds(),m=meter->getWorldBounds();
            CHECK(p.maxX-p.minX>600);CHECK(p.maxY-p.minY>=150);
            CHECK(m.maxY<p.minY);CHECK(m.maxY-m.minY<=80);
            CHECK(m.maxX<=960);CHECK(m.maxY<=900);
        }
        auto* tabs=dynamic_cast<ayt::ui::TabControl*>(child.findById("audio_tabs"));
        CHECK(tabs!=nullptr);
        tabs->setSelectedIndex(2);
        auto* slider=child.findById("sld_master");const auto mixer=slider->getWorldBounds();
        CHECK(mixer.maxX-mixer.minX>400);CHECK(mixer.minY>150);
        tabs->setSelectedIndex(1);
        auto* wave=child.findById("resource_waveform");const auto resource=wave->getWorldBounds();
        CHECK(resource.maxX-resource.minX>600);CHECK(resource.minY>150);
        tabs->setSelectedIndex(0);
        CHECK(piano->getWorldBounds().maxX-piano->getWorldBounds().minX>600);
        // GDI child painting has no layout() call in its per-frame loop.
        child.render();CHECK(!renderer.getDrawCalls().empty());session.detach();
    }
    // Standalone: loader then viewport size, still before session attachment.
    CHECK(child.loadLayout((root/"EngineAssets/AYAudio/ui/audio_editor.ui.json").string()));
    child.setClientSize(1100,820);child.layout();CHECK(session.attach(child));
    auto* meter=child.findById("audio_output_meter");const auto initial=meter->getWorldBounds();
    CHECK(initial.minX>900);CHECK(initial.maxY-initial.minY<=80);
    child.setClientSize(900,700);child.layout();const auto resized=meter->getWorldBounds();
    CHECK(resized.minX<initial.minX);CHECK(resized.maxX<=900);CHECK(resized.maxY<=700);
    session.detach();
}

TEST_CASE(TemporaryTransposeLowerOctaveAndLatchedKeyRelease) {
    AudioSubSystem audio;CHECK(audio.initialize());auto* engine=audio.engine();
    ayt::ui::MockRenderer renderer;ayt::ui::UIManager ui;ui.initialize(&renderer);ui.setClientSize(960,900);
    const auto root=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    CHECK(ui.loadLayout((root/"EngineAssets/AYAudio/ui/audio_editor.ui.json").string()));
    AudioEditorSession session;session.setAudio(&audio);session.setDriveAudioUpdate(false);CHECK(session.attach(ui));
    auto* status=dynamic_cast<ayt::ui::TextLabel*>(ui.findById("note_status"));CHECK(status);
    const auto low=session.pianoKeyBounds(48),strip=ui.findById("piano_keyboard")->getWorldBounds();
    CHECK(low.minX>=strip.minX);CHECK(low.maxX<=strip.maxX);
    auto* source=dynamic_cast<ayt::ui::Button*>(ui.findById("btn_instrument"));CHECK(source);const auto sourceBounds=source->getWorldBounds();ayt::ui::UIMouseEvent click({(sourceBounds.minX+sourceBounds.maxX)/2,(sourceBounds.minY+sourceBounds.maxY)/2},0);source->onMouseMove(click);source->onMouseButtonDown(click);source->onMouseButtonUp(click);
    auto firstVoice=[&] { VoiceRuntime voice{};bool found=false;for(uint32_t i=0;i<64;++i) if(engine->voiceStateAt(i,voice)){found=true;break;}CHECK(found);return voice; };
    auto drain=[&] {std::vector<float> pcm(engine->outputSampleRate()/2);engine->render(pcm.data(),engine->outputSampleRate()/4);};
    CHECK(session.handleKey('A',true));const auto upper=firstVoice();session.releaseKeys();
    CHECK(session.handleKey('K',true));const auto octaveUp=firstVoice();session.releaseKeys();
    CHECK(session.handleKey('Z',true));const auto octaveDown=firstVoice();session.releaseKeys();
    constexpr int lowerKeys[]={'Z','X','C','V','B','N','M',188};
    constexpr int pitches[]={48,50,52,53,55,57,59,60};
    constexpr const wchar_t* names[]={L"C3",L"D3",L"E3",L"F3",L"G3",L"A3",L"B3",L"C4"};
    for(int i=0;i<8;++i){
        CHECK(session.handleKey(lowerKeys[i],true));auto v=firstVoice();
        const int regionRoot=((pitches[i]+5)/12)*12;
        CHECK(std::abs(v.pitch-std::exp2((pitches[i]-regionRoot)/12.f))<1e-5f);
        CHECK(status->getText()==std::wstring(L"实际音高：")+names[i]);
        if(i==0)CHECK(v.clip!=upper.clip);if(i==7)CHECK(v.clip==upper.clip);
        CHECK(session.handleKey(lowerKeys[i],false));drain();CHECK(engine->activeVoiceCount()==0);
    }
    CHECK(session.handleKey(0xA0,true));CHECK(session.handleKey('A',true));
    const auto raised=firstVoice();CHECK(raised.clip==octaveUp.clip);CHECK(raised.pitch==octaveUp.pitch);
    CHECK(status->getText()==L"实际音高：C5");
    const auto count=engine->activeVoiceCount();CHECK(session.handleKey('A',true));CHECK(engine->activeVoiceCount()==count);
    CHECK(session.handleKey(0xA0,false));CHECK(session.handleKey('A',false));
    CHECK(status->getText()==L"实际音高：C5");drain();CHECK(engine->activeVoiceCount()==0);
    CHECK(session.handleKey(0xA4,true));ui.onKeyDown(18);CHECK(session.handleKey('A',true));
    const auto lowered=firstVoice();CHECK(lowered.clip==octaveDown.clip);CHECK(lowered.pitch==octaveDown.pitch);
    CHECK(status->getText()==L"实际音高：C3");
    CHECK(session.handleKey(0xA4,false));ui.onKeyUp(18);CHECK(session.handleKey('A',false));drain();CHECK(engine->activeVoiceCount()==0);
    CHECK(session.handleKey(0xA0,true));CHECK(session.handleKey(0xA4,true));CHECK(session.handleKey('A',true));const auto cancelled=firstVoice();CHECK(cancelled.pitch==upper.pitch);CHECK(cancelled.clip==upper.clip);
    session.onFocusLost();CHECK(engine->activeVoiceCount()==0);
    CHECK(!session.handleKey(0xA1,true));CHECK(session.handleKey('A',true));CHECK(firstVoice().pitch==1);session.releaseKeys();
    // Upper A and lower comma share C4: releasing either physical key must preserve the other.
    CHECK(session.handleKey('A',true));CHECK(session.handleKey(188,true));CHECK(engine->activeVoiceCount()==1);
    CHECK(session.handleKey('A',false));drain();CHECK(engine->activeVoiceCount()==1);
    CHECK(session.handleKey(188,false));drain();CHECK(engine->activeVoiceCount()==0);
    auto* input=dynamic_cast<ayt::ui::TextInput*>(ui.findById("transpose_value"));CHECK(input);ui.setFocus(input);
    CHECK(!session.handleKey(0xA0,true));CHECK(!session.handleKey('Z',true));ui.setFocus(nullptr);
    // Releases still work after focus enters a numeric field.
    CHECK(session.handleKey('X',true));ui.setFocus(input);CHECK(session.handleKey('X',false));drain();CHECK(engine->activeVoiceCount()==0);ui.setFocus(nullptr);
    ui.onKeyDown(17);CHECK(!session.handleKey('Z',true));ui.onKeyUp(17);
    CHECK(session.handleKey(0xA4,true));CHECK(session.handleKey('Z',true));session.onFocusLost();
    CHECK(session.handleKey('Z',true));CHECK(status->getText()==L"实际音高：C3");session.releaseKeys();
    renderer.clear();session.tick(1);ui.render();bool silenceReadout=false;
    for(const auto& call:renderer.getDrawCalls())if(call.text==L"-inf dBFS")silenceReadout=true;
    CHECK(silenceReadout);session.detach();audio.shutdown();
}
TEST_CASE(RecordingTabSavesVirtualWavAndWaveSelectionStaysBelowPeaks) {
    namespace fs=std::filesystem;
    const auto assets=ayt::test::testTmpDir()/"recording_ui_assets";
    struct Cleanup {fs::path p;~Cleanup(){std::error_code e;fs::remove_all(p,e);}} cleanup{assets};
    ayt::ui::MockRenderer renderer;ayt::ui::UIManager ui;ui.initialize(&renderer);ui.setClientSize(1100,900);
    ayt::ui::ThemeManager::get().ensureDefaultThemes();ayt::ui::ThemeManager::get().setActiveTheme("dark");
    const auto source=fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    CHECK(ui.loadLayout((source/"EngineAssets/AYAudio/ui/audio_editor.ui.json").string()));
    AudioSubSystem audio;CHECK(audio.initialize());AudioEditorSession session;session.setAudio(&audio);
    session.setDriveAudioUpdate(false);session.setAssetRoot((source/"EngineAssets/AYAudio").string());
    std::string saved;session.setRecordingRootProvider([&]{return assets.string();});
    session.setRecordingSavedCallback([&](const std::string& value){saved=value;});CHECK(session.attach(ui));
    auto* tabs=dynamic_cast<ayt::ui::TabControl*>(ui.findById("audio_tabs"));CHECK(tabs);
    CHECK(tabs->getTabCount()==5);CHECK(tabs->getTabLabel(3)==L"录音台");CHECK(tabs->getTabStrip()->isFlatHeader());
    tabs->setSelectedIndex(3);CHECK_FALSE(session.handleKey('A',true));
    auto* recorder=session.recorder();CHECK(recorder);
    RecordingConfig config;config.sampleRate=8000;config.channels=1;config.maxSeconds=2;
    CHECK(AudioRecorderTestAccess::beginBuffered(*recorder,config));
    std::vector<float> tone(8000);for(size_t i=0;i<tone.size();++i) tone[i]=std::sin(i*.07f)*.4f;
    AudioRecorderTestAccess::receive(*recorder,tone.data(),8000);session.tick(.02f);
    CHECK(recorder->status().frames==8000);
    auto* save=dynamic_cast<ayt::ui::Button*>(ui.findById("recording_save"));CHECK(save);CHECK_FALSE(save->isEnabled());
    auto click=[&](const char* id){auto* button=dynamic_cast<ayt::ui::Button*>(ui.findById(id));CHECK(button);
        const auto b=button->getWorldBounds();ayt::ui::UIMouseEvent event({(b.minX+b.maxX)/2,(b.minY+b.maxY)/2},0);
        button->onMouseMove(event);button->onMouseButtonDown(event);button->onMouseButtonUp(event);};
    click("recording_stop");session.tick(.02f);CHECK(save->isEnabled());
    auto* path=dynamic_cast<ayt::ui::TextInput*>(ui.findById("recording_path"));CHECK(path);path->setText(L"audio/recordings/录音测试.wav");
    click("recording_save");session.tick(.02f);CHECK(saved=="audio/recordings/录音测试.wav");
    CHECK(fs::exists(assets/fs::u8path(saved)));click("recording_edit");CHECK(tabs->getSelectedIndex()==1);
    auto edit=[&](const char* id,const wchar_t* value){auto* field=dynamic_cast<ayt::ui::TextInput*>(ui.findById(id));CHECK(field);
        ui.setFocus(field);field->setText(value);field->onKeyDown(13);ui.setFocus(nullptr);};
    edit("range_begin",L"0.2");edit("range_end",L"0.8");
    auto* wave=ui.findById("resource_waveform");CHECK(wave);renderer.clear();wave->render(renderer);
    const auto bounds=wave->getWorldBounds();bool rangeFound=false,peaksAfter=false;
    for(const auto& call:renderer.getDrawCalls()) if(call.type==ayt::ui::MockRenderer::DrawCall::Rect) {
        const float width=call.bounds.maxX-call.bounds.minX;
        if(width>(bounds.maxX-bounds.minX)*.5f && width<(bounds.maxX-bounds.minX)*.7f && call.bounds.minY==bounds.minY+24) {
            rangeFound=true;CHECK(call.color.w==1);CHECK(call.color.z<.5f);
        }
        if(rangeFound && width<5 && call.bounds.minY>bounds.minY+24 && call.bounds.maxY<bounds.maxY) peaksAfter=true;
    }
    CHECK(rangeFound);CHECK(peaksAfter);
    tabs->setSelectedIndex(3);click("recording_discard");session.tick(.02f);CHECK(recorder->samples().empty());CHECK(fs::exists(assets/fs::u8path(saved)));
    session.detach();ui.shutdown();audio.shutdown();
}
TEST_SUITE_END


TEST_SUITE(EditorAudioChartTests)
namespace {
std::filesystem::path chartTone(const std::filesystem::path& assets) {
    AudioRecorder r;RecordingConfig config;config.sampleRate=8000;config.channels=1;config.maxSeconds=2;
    if(!AudioRecorderTestAccess::beginBuffered(r,config))throw std::runtime_error("fixture capture failed");
    std::vector<float> samples(8000);for(size_t i=0;i<samples.size();++i)samples[i]=float(std::sin(i*.1)*.2);
    AudioRecorderTestAccess::receive(r,samples.data(),8000);r.update();r.stop();std::string error;
    if(!r.saveWav(ayt::audio::chart::utf8(assets),"audio/music.wav",&error))throw std::runtime_error(error);
    return assets/"audio/music.wav";
}
void chartClick(ayt::ui::UIManager& ui,const char* id) {
    auto* button=dynamic_cast<ayt::ui::Button*>(ui.findById(id));CHECK(button);if(!button)return;
    const auto b=button->getWorldBounds();ayt::ui::UIMouseEvent e({(b.minX+b.maxX)/2,(b.minY+b.maxY)/2},0);
    button->onMouseMove(e);button->onMouseButtonDown(e);button->onMouseButtonUp(e);
}
void chartSet(ayt::ui::UIManager& ui,const char* id,const wchar_t* value) {auto* input=dynamic_cast<ayt::ui::TextInput*>(ui.findById(id));CHECK(input);if(input)input->setText(value);}
}
TEST_CASE(ChartEditorSavesRelativeMusicAndRunsUnsavedSnapshot) {
    namespace fs=std::filesystem;const auto assets=ayt::test::testTmpDir()/"chart_editor_assets";fs::create_directories(assets);const auto music=chartTone(assets);
    ayt::ui::MockRenderer renderer;ayt::ui::UIManager ui;ui.initialize(&renderer);ui.setClientSize(1100,900);
    ayt::ui::ThemeManager::get().ensureDefaultThemes();ayt::ui::ThemeManager::get().setActiveTheme("dark");
    const auto repo=fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path();CHECK(ui.loadLayout((repo/"EngineAssets/AYAudio/ui/audio_editor.ui.json").string()));
    AudioSubSystem audio;CHECK(audio.initialize());AudioEditorSession session;session.setAudio(&audio);session.setRecordingRootProvider([&]{return ayt::audio::chart::utf8(assets);});session.setPathPicker([&]{return music.string();});
    std::string saved,snapshot;session.setRecordingSavedCallback([&](const std::string& path){saved=path;});session.setChartLauncher([&](const std::string& file,std::string&){snapshot=file;return true;});
    CHECK(session.attach(ui));auto* tabs=dynamic_cast<ayt::ui::TabControl*>(ui.findById("audio_tabs"));CHECK(tabs);CHECK(tabs->getTabCount()==5);CHECK(tabs->getTabLabel(4)==L"谱面编辑");tabs->setSelectedIndex(4);
    CHECK_FALSE(session.handleKey('D',true));chartClick(ui,"chart_music_open");
    chartSet(ui,"chart_note_start",L"0.5");chartSet(ui,"chart_note_end",L"0.5");chartClick(ui,"chart_note_apply");
    // Canvas drag creates a second-lane hold without altering the audio resource.
    auto* canvas=ui.findById("chart_editor");CHECK(canvas);const auto b=canvas->getWorldBounds();const float row=(b.maxY-b.minY-24)/4;
    canvas->onMouseButtonDown({{b.minX+(b.maxX-b.minX)*.25f,b.minY+24+row*1.5f},0});
    canvas->onMouseMove({{b.minX+(b.maxX-b.minX)*.75f,b.minY+24+row*1.5f}});canvas->onMouseButtonUp({{0,0},0});
    chartSet(ui,"chart_path",L"audio/charts/谱面测试.chart.json");chartClick(ui,"chart_save");CHECK(saved=="audio/charts/谱面测试.chart.json");
    const auto savedFile=assets/fs::u8path(saved);CHECK(fs::exists(savedFile));const auto savedBytes=ayt::audio::chart::read(savedFile);
    const auto doc=ayt::audio::chart::Document::fromJson(nlohmann::json::parse(savedBytes));CHECK(doc.notes.size()==2);CHECK(doc.audioRelative);CHECK(doc.audio=="audio/music.wav");CHECK(doc.notes[1].end>doc.notes[1].time);
    chartClick(ui,"chart_undo");chartClick(ui,"chart_run");CHECK_FALSE(snapshot.empty());
    auto unsaved=ayt::audio::chart::Document::fromJson(nlohmann::json::parse(ayt::audio::chart::read(fs::u8path(snapshot))));CHECK(unsaved.notes.size()==1);CHECK_FALSE(unsaved.audioRelative);CHECK(ayt::audio::chart::read(savedFile)==savedBytes);
    chartClick(ui,"chart_redo");chartClick(ui,"chart_load");CHECK(session.openChart(ayt::audio::chart::utf8(savedFile)));
    // Project switching cannot redirect a take/document into a new project's assets.
    const auto other=assets/"other";fs::create_directories(other);session.setRecordingRootProvider([&]{return ayt::audio::chart::utf8(other);});chartClick(ui,"chart_save");CHECK_FALSE(fs::exists(other/fs::u8path(saved)));
    auto* status=dynamic_cast<ayt::ui::TextLabel*>(ui.findById("chart_status"));session.tick(.01f);CHECK(status->getText().find(L"保存失败")!=std::wstring::npos);
    session.detach();ui.shutdown();audio.shutdown();std::error_code error;fs::remove(fs::u8path(snapshot),error);
}
TEST_CASE(IndependentChartPreviewJudgesInputAndFocusPausePreservesEngine) {
    namespace fs=std::filesystem;const auto assets=ayt::test::testTmpDir()/"chart_preview_assets";fs::create_directories(assets);const auto music=chartTone(assets);
    ayt::audio::chart::Document doc;doc.audio=ayt::audio::chart::utf8(music);doc.notes={{0,.5,.5},{1,.5,.9}};
    const auto file=assets/"preview.chart.json";std::ofstream(file)<<doc.json();
    ayt::ui::MockRenderer renderer;ayt::ui::UIManager ui;ui.initialize(&renderer);ui.setClientSize(960,900);ayt::ui::ThemeManager::get().ensureDefaultThemes();ayt::ui::ThemeManager::get().setActiveTheme("dark");
    const auto repo=fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path();CHECK(ui.loadLayout((repo/"EngineAssets/AYAudio/ui/audio_editor.ui.json").string()));
    AudioSubSystem audio;CHECK(audio.initialize());AudioEditorSession session;session.setAudio(&audio);session.setChartPreviewFile(ayt::audio::chart::utf8(file));CHECK(session.attach(ui));CHECK(ui.findById("chart_player"));CHECK_FALSE(ui.findById("piano_keyboard"));CHECK_FALSE(ui.findById("audio_tabs"));CHECK_FALSE(session.handleKey('A',true));
    std::this_thread::sleep_for(std::chrono::milliseconds(2050));session.tick(.01f);const auto frames=audio.engine()->outputSampleRate()/2;std::vector<float> pcm(frames*audio.engine()->outputChannels());audio.engine()->render(pcm.data(),frames);
    CHECK(session.handleKey('D',true));CHECK(session.handleKey('D',true));CHECK(session.handleKey('D',false));CHECK(session.handleKey('F',true));session.tick(.01f);
    auto* status=dynamic_cast<ayt::ui::TextLabel*>(ui.findById("chart_status"));CHECK(status);CHECK(status->getText().find(L"Perfect 1")!=std::wstring::npos);
    audio.engine()->render(pcm.data(),audio.engine()->outputSampleRate()*4/10);CHECK(session.handleKey('F',false));session.tick(.01f);CHECK(status->getText().find(L"Perfect 2")!=std::wstring::npos);
    session.onFocusLost();session.tick(.01f);CHECK_FALSE(audio.engine()->isPaused());CHECK(status->getText().find(L"已暂停")!=std::wstring::npos);CHECK(session.handleKey(27,true));
    session.detach();CHECK(audio.engine()->activeVoiceCount()==0);ui.shutdown();audio.shutdown();
}
TEST_SUITE_END
