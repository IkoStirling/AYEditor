#pragma once
#include <AYEditor/EditorExtensionRegistry.h>
#include <AYParticle/EffectAssetIO.h>
#include <memory>
#include <unordered_set>
namespace ayt::editor {
inline constexpr const char* kEditorParticleExtensionId="ayeditor.particle";
/// Session-only viewport state. Never serialized into an effect or undo history.
struct EditorParticlePreviewSettings {
    particle::Backend backend=particle::Backend::Auto;
    bool scene3D=false,ground=true,testObject=true;
    uint32_t background=0x101722ff;
    float yaw=.5f,pitch=.3f,distance=10,screenHeight=7;
    particle::Vec3 focus{};
    std::unordered_set<std::string> hidden;
    std::string solo;
};
/// Author schema-3 .ayparticle compositions, visual texture/flipbook and color
/// controls, ordered layers, 2D placement handles and linear lifetime tracks
/// with value-history. A completed canvas drag contributes one history entry.
/// Reads schema 1/2 unchanged; preview uses isolated CPU/GPU runtime state. Curve gestures stage
/// a private draft and publish one validated history entry on release.
/// Transport/step never dirty the asset. Successful edits restart paused preview.
class EditorParticleDocument final : public IEditorDocument, public IEditorCommandTarget {
public:
    bool initialize(const EditorOpenRequest& request,std::string& error);
    const std::string& typeId() const noexcept override { return _type; }
    const std::string& path() const noexcept override { return _path; }
    const std::string& title() const noexcept override { return _title; }
    bool isDirty() const noexcept override { return !(_asset==_saved); }
    uint64_t revision() const noexcept override { return _revision; }
    bool save(std::string* error=nullptr) override;
    bool canSaveAs() const noexcept override { return true; }
    bool saveAs(const std::string& path,std::string* error=nullptr) override;
    bool canReload() const noexcept override { return true; }
    bool reload(std::string* error=nullptr) override;
    bool writeRecoveryCopy(const std::string& path,std::string* error=nullptr) const override;
    const particle::EffectAsset& asset() const noexcept { return _asset; }
    /// CPU instance is null while GPU rendering owns simulation; time is previewTime().
    const particle::EffectInstance* preview() const noexcept { return _preview.get(); }
    EditorParticlePreviewSettings& previewSettings() noexcept { return _previewSettings; }
    const EditorParticlePreviewSettings& previewSettings() const noexcept { return _previewSettings; }
    bool previewLayerVisible(size_t index) const;
    bool previewLayerHidden(size_t index) const { return _previewSettings.hidden.contains(previewLayerKey(index)); }
    bool previewLayerSolo(size_t index) const { return _previewSettings.solo==previewLayerKey(index); }
    void togglePreviewLayerHidden(size_t index);
    void togglePreviewLayerSolo(size_t index);
    uint64_t previewEpoch() const noexcept { return _previewEpoch; }
    double previewTime() const noexcept { return _runtimeGpu?_previewTime:(_preview?_preview->time():0); }
    bool runtimeGpuActive() const noexcept { return _runtimeGpu; }
    bool previewStopped() const noexcept { return _previewStopped; }
    /// Internal render-host handoff. GPU ownership clears CPU arrays; CPU fallback
    /// replays at 60 Hz. Large GPU seeks catch up asynchronously without readback.
    void setRuntimeGpuActive(bool active);
    void setPreviewReplayPending(bool pending) noexcept { _replayPending=pending; }
    void finishRuntimePreview() noexcept { _playing=false; _previewStopped=true; }

    bool replaceAsset(const particle::EffectAsset& next,std::string* error=nullptr);
    bool undo();
    bool redo();
    void setSavePathProvider(std::function<std::string(bool)> provider) { _savePath=std::move(provider); }
    void play();
    void pause() noexcept { _playing=false; }
    void restart();
    void stop();
    void step();
    void tick(float dt);
    /// Deterministic preview-only replay at 60 Hz, bounded to 60 seconds.
    bool seek(float seconds);
    void setPreviewSeed(uint32_t seed);
    void useAssetSeed();
    uint32_t previewSeed() const noexcept {return _seedOverride?_previewSeed:_asset.seed;}

    const std::string& diagnostic() const noexcept { return _diagnostic; }
    bool playing() const noexcept { return _playing; }
    double simulationMilliseconds() const noexcept { return _simulationMs; }
    bool handlesCommand(const std::string& id) const override;
    bool canExecuteCommand(const std::string& id) const override;
    bool executeCommand(const std::string& id) override;
private:
    void resetPreview();
    std::string previewLayerKey(size_t index) const;
    std::string _type="ayeditor.particle.document",_path,_title="Particle Effect";
    particle::EffectAsset _asset,_saved;
    std::vector<particle::EffectAsset> _history;
    size_t _cursor=0;
    uint64_t _revision=1;
    std::unique_ptr<particle::EffectInstance> _preview;
    bool _playing=false,_seedOverride=false;
    bool _runtimeGpu=false,_previewStopped=false,_replayPending=false;
    double _previewTime=0;
    uint64_t _previewEpoch=0;
    EditorParticlePreviewSettings _previewSettings;
    uint32_t _previewSeed=1;
    double _simulationMs=0;
    std::function<std::string(bool)> _savePath;
    std::string _diagnostic;
};
/// Register .ayparticle authoring with isolated playback, visual material
/// selection and multi-layer composition editing.
bool registerEditorParticleExtension(EditorExtensionRegistry& registry,std::string* error=nullptr);
}
