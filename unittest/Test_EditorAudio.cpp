#include <AYTest.h>
#include <AYAudio.h>
#include <AYEditor/EditorBuiltInExtensions.h>
#include <AYResource/assetsImpl/Audio.h>
#include <AYResource/Loader/AudioLoader.h>
#include <cmath>
#include <AYUI/Widget.h>
#include <AYUI/MockRenderer.h>
#include <AYUI/Theme.h>
#include <AYUI/TextInput.h>
#include <AYUI/TextLabel.h>
#include <AYUI/Button.h>
#include "../../AYAudio/demo/audio_editor/AudioEditorSession.h"
#include <filesystem>
#include <fstream>
using namespace ayt::audio;
using namespace ayt::editor;
TEST_SUITE(EditorAudioTests)
TEST_CASE(SessionClosePreservesForeignVoicesAndMixerState) {
    AudioSubSystem audio;CHECK(audio.initialize());auto* e=audio.engine();
    float pcm[32]{};AudioClipDesc desc;desc.channels=1;desc.frameCount=32;desc.pcmData=pcm;const auto c=e->registerClip(desc);const auto foreign=e->play(c);
    e->setMasterGain(0.6f);e->setBusGain(AudioBus::Music,0.4f);
    ayt::ui::MockRenderer renderer;ayt::ui::UIManager ui;ui.initialize(&renderer);ayt::ui::ThemeManager::get().ensureDefaultThemes();ayt::ui::ThemeManager::get().setActiveTheme("dark");const auto root=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();CHECK(ui.loadLayout((root/"EngineAssets/AYAudio/ui/audio_editor.ui.json").string()));
    AudioEditorSession session;session.setAudio(&audio);session.setDriveAudioUpdate(false);session.setAssetRoot((root/"EngineAssets/AYAudio").string());CHECK(session.attach(ui));
    auto* instrument=dynamic_cast<ayt::ui::TextLabel*>(ui.findById("lbl_instrument"));CHECK(instrument!=nullptr);CHECK(instrument->getText().find(L"fallback")==std::wstring::npos);CHECK(e->masterGain()==0.6f);CHECK(e->busGain(AudioBus::Music)==0.4f);
    CHECK(session.handleKey('A',true));const auto count=e->activeVoiceCount();CHECK(session.handleKey('A',true));CHECK(e->activeVoiceCount()==count);
    ui.setClientSize(960,900);ui.layout();
    auto* key=ui.findById("piano_key_1");CHECK(key!=nullptr);const auto bounds=key->getWorldBounds();CHECK(bounds.maxY<=900);
    CHECK(session.handleMouseButton((bounds.minX+bounds.maxX)/2,(bounds.minY+bounds.maxY)/2,0,true));CHECK(e->activeVoiceCount()==count+1);
    CHECK(session.handleMouseButton(-20,-20,0,false));session.releaseKeys();
    CHECK(session.handleKey('D',true));CHECK(session.handleKey(32,true));CHECK(session.handleKey('D',false));CHECK(session.handleKey(32,false));session.releaseKeys();CHECK(e->activeVoiceCount()==1);
    session.detach();CHECK(e->clipRegistry().size()==1);VoiceRuntime v;CHECK(e->voiceState(foreign,v));
    for(int i=0;i<5;++i){CHECK(session.attach(ui));session.detach();}
    CHECK(e->activeVoiceCount()==1);e->stop(foreign);e->releaseClip(c);
    CHECK(session.attach(ui));CHECK(session.handleKey('A',true));audio.shutdown();CHECK(audio.initialize());e=audio.engine();
    const auto newClip=e->registerClip(desc);const auto newForeign=e->play(newClip);
    // Toolbar actions may arrive before the first tick notices engine replacement.
    auto* stop=dynamic_cast<ayt::ui::Button*>(ui.findById("btn_stop_all"));CHECK(stop!=nullptr);
    const auto stopBounds=stop->getWorldBounds();ayt::ui::UIMouseEvent click({(stopBounds.minX+stopBounds.maxX)/2,(stopBounds.minY+stopBounds.maxY)/2},0);
    stop->onMouseMove(click);stop->onMouseButtonDown(click);stop->onMouseButtonUp(click);
    VoiceRuntime fresh;CHECK(e->voiceState(newForeign,fresh));session.detach();CHECK(e->clipRegistry().size()==1);
    CHECK(session.attach(ui));CHECK(session.handleKey('A',true));session.detach();CHECK(e->voiceState(newForeign,fresh));CHECK(e->clipRegistry().size()==1);
    e->stop(newForeign);e->releaseClip(newClip);audio.shutdown();
}
TEST_CASE(AudioRangeFadesPersistUndoAndRejectInvalidInput) {
    const auto temp=std::filesystem::temp_directory_path()/"ay-editor-audio-regression.aud";
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code ec;std::filesystem::remove(path,ec);std::filesystem::remove(path.string()+".timeline.json",ec);}}cleanup{temp};
    ayt::resource::Audio source;source.createSineWave(440,1);std::vector<UInt8> bytes;CHECK(source.saveToBinary(bytes));std::ofstream out(temp,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());out.close();
    EditorExtensionRegistry registry;std::string error;CHECK(registerEditorBuiltInExtensions(registry,{},&error));const auto* extension=registry.find(kEditorAudioTimelineExtensionId);EditorOpenRequest request;request.resourcePath=temp.string();auto document=extension->createDocument(request,error);CHECK(document!=nullptr);if(!document)return;
    auto* timeline=dynamic_cast<IEditorTimelineSource*>(document.get());CHECK(timeline!=nullptr);if(!timeline)return;
    const auto pianoPath=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path()/"EngineAssets/AYAudio/samples/C4.mp3";
    ayt::resource::AudioLoader loader;auto piano=std::dynamic_pointer_cast<ayt::resource::Audio>(loader.load(pianoPath.string()));CHECK(piano!=nullptr);
    EditorOpenRequest pianoRequest;pianoRequest.resourcePath=pianoPath.string();auto pianoDocument=extension->createDocument(pianoRequest,error);CHECK(pianoDocument!=nullptr);
    if(piano && pianoDocument) {auto* t=dynamic_cast<IEditorTimelineSource*>(pianoDocument.get());CHECK(t!=nullptr);const double duration=static_cast<double>(piano->getDataSize())/(piano->getChannels()*(piano->getBitsPerSample()/8u)*piano->getSampleRate());CHECK(std::abs(t->timelineDurationSeconds()-duration)<1e-5);CHECK(!t->timelineWaveformPeaks("audio").empty());}
    CHECK(timeline->setAudioPreviewSettings({0.2,0.8,true}));CHECK(!timeline->setAudioPreviewSettings({0.9,0.2,true}));CHECK(!timeline->setAudioPreviewSettings({0,2,false}));
    auto clip=timeline->timelineClips().front();clip.durationSeconds=0.5;clip.sourceOffsetSeconds=0.1;clip.gain=0.4f;clip.fadeInSeconds=0.02;clip.fadeOutSeconds=0.1;CHECK(timeline->timelineUpdateClip(clip));CHECK(timeline->timelineUndo());CHECK(timeline->timelineClips().front().gain==1);CHECK(timeline->timelineRedo());
    auto bad=clip;bad.sourceOffsetSeconds=0.8;CHECK(!timeline->timelineUpdateClip(bad));CHECK(document->save(&error));auto reload=extension->createDocument(request,error);CHECK(reload!=nullptr);if(!reload)return;auto* rt=dynamic_cast<IEditorTimelineSource*>(reload.get());CHECK(rt->audioPreviewSettings().loop);CHECK(rt->audioPreviewSettings().beginSeconds==0.2);CHECK(rt->timelineClips().front().fadeOutSeconds==0.1);CHECK(rt->timelineClips().front().gain==0.4f);
}
TEST_SUITE_END
