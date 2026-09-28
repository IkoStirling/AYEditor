#include "AYEditorAnimationCurveSource.h"
#include "AYEditor/EditorAnimationDocument.h"
#include <AYAnimation/KeySampler.h>
#include <AYUI/Authoring/AuthoringPrimitives.h>

#include <cmath>
#include <limits>
#include <unordered_map>

namespace ayt::editor {
namespace {
using namespace ayt::ui::authoring;

struct SampleData {
    ayt::resource::AnimTrackType type;
    ayt::resource::AnimInterpolation interpolation;
    std::vector<float> times;
    std::vector<ayt::math::FQuaternion> rotations, inRotations, outRotations;
};
struct ScalarChannel {
    std::vector<float> values, incoming, outgoing;
};

class AnimationCurveSource final : public ICurveEditorSource {
public:
    explicit AnimationCurveSource(std::shared_ptr<EditorAnimationDocument> document)
        : _document(std::move(document)) { _document->bindAuthoringSelection(selectionState()); }
    std::uint64_t revision() const noexcept override { return _document->revision(); }
    double durationSeconds() const noexcept override { return _document->timelineDurationSeconds(); }
    double positionSeconds() const noexcept override { return _document->timelinePositionSeconds(); }
    bool seek(double seconds) override { return _document->setTimelinePositionSeconds(seconds); }
    double snapTime(const std::string&, double seconds) const override {
        const double rate = _document->animationClipProperties().ticksPerSecond;
        return snapTimeToInterval(seconds, rate > 0.0 ? 1.0 / rate : 0.0);
    }
    bool beginEdit(const std::string& label) override {
        return _document->beginAnimationEditGesture(label);
    }
    bool endEdit(bool cancel) override {
        return cancel ? _document->cancelAnimationEditGesture()
                      : _document->commitAnimationEditGesture();
    }
    bool updateKey(std::string& id, double time, const std::vector<float>& values) override {
        return _document->updateAnimationKeyframe(id, time, values);
    }
    bool transformKeys(std::vector<std::string>& ids, double time,
                       std::size_t component, float value) override {
        return _document->transformAnimationKeyframes(ids, time, component, value);
    }
    bool removeKeys(const std::vector<std::string>& ids) override {
        return _document->removeAnimationKeyframes(ids);
    }
    bool setTangents(const std::string& id, const std::vector<float>& in,
                     const std::vector<float>& out) override {
        return _document->setAnimationKeyframeTangents(id, in, out);
    }
    bool moveTimelineKey(std::string& trackId, std::string& keyId, double time) override {
        if (keyId.rfind("notify.", 0u) == 0u) {
            for (const auto& notify : _document->animationNotifies()) {
                if (notify.id != keyId) continue;
                if (!_document->updateAnimationNotify(keyId, notify.name, time, notify.payload))
                    return false;
                trackId = "event." + keyId.substr(7u);
                return true;
            }
            return false;
        }
        std::vector<float> values;
        return _document->animationKeyframeValues(keyId, values)
            && _document->updateAnimationKeyframe(keyId, time, values);
    }
    std::shared_ptr<const TimelineSnapshot> timelineSnapshot() const override {
        refreshCache();
        if (_timeline) return _timeline;
        auto snapshot = std::make_shared<TimelineSnapshot>();
        for (const auto& track : _document->timelineTracks()) {
            snapshot->tracks.push_back({track.id, track.name,
                track.kind == EditorTimelineTrackKind::Event ? TimelineTrackKind::Event
                    : track.kind == EditorTimelineTrackKind::Audio ? TimelineTrackKind::Audio
                                                                  : TimelineTrackKind::Value});
        }
        for (const auto& key : _document->timelineKeyframes())
            snapshot->keys.push_back({key.id, key.trackId, key.timeSeconds});
        _timeline = snapshot;
        return snapshot;
    }
    std::shared_ptr<const CurveTrack> curveTrack(const std::string& id) const override {
        refreshCache();
        if (const auto found = _curves.find(id); found != _curves.end()) return found->second;
        EditorAnimationCurveTrack authored;
        if (!_document->animationCurveTrack(id, authored)) return nullptr;
        auto snapshot = std::make_shared<CurveTrack>();
        snapshot->id = id;
        snapshot->editableTangents = authored.interpolation
            == ayt::resource::AnimInterpolation::CubicHermite;
        snapshot->snapIntervalSeconds = 1.0 / authored.ticksPerSecond;
        auto data = std::make_shared<SampleData>();
        data->type = authored.valueType;
        data->interpolation = authored.interpolation;
        for (auto& key : authored.keys) {
            data->times.push_back(static_cast<float>(key.timeSeconds));
            // Sampling is in seconds, matching the value/second tangent contract.
            if (data->type == ayt::resource::AnimTrackType::Quaternion) {
                data->rotations.emplace_back(key.values[0], key.values[1], key.values[2], key.values[3]);
                data->inRotations.emplace_back(key.inTangents[0], key.inTangents[1], key.inTangents[2], key.inTangents[3]);
                data->outRotations.emplace_back(key.outTangents[0], key.outTangents[1], key.outTangents[2], key.outTangents[3]);
            }
            snapshot->keys.push_back({key.id, key.timeSeconds, std::move(key.values),
                                       std::move(key.inTangents), std::move(key.outTangents)});
        }
        // Retain immutable typed data in the sampler, never a borrowed resource
        // pointer. A cached snapshot remains safe through undo/reload/new revisions.
        const std::size_t width = snapshot->keys.empty() ? 0u : snapshot->keys.front().values.size();
        auto components = std::make_shared<std::vector<ScalarChannel>>(width);
        if (data->type != ayt::resource::AnimTrackType::Quaternion) {
            for (std::size_t c = 0; c < width; ++c) {
                auto& component = (*components)[c];
                for (const auto& key : snapshot->keys) {
                    component.values.push_back(key.values[c]);
                    component.incoming.push_back(key.inTangents[c]);
                    component.outgoing.push_back(key.outTangents[c]);
                }
            }
        }
        snapshot->sample = [data, components, width](std::size_t component, double seconds) {
            if (component >= width) return 0.0f;
            if (data->type == ayt::resource::AnimTrackType::Quaternion) {
                ayt::math::FQuaternion result;
                ayt::anim::sampleTrackQuaternion(data->rotations.data(), data->times.size(),
                    data->times, static_cast<float>(seconds), result, data->interpolation,
                    data->inRotations.data(), data->outRotations.data());
                const float values[] = {result.x, result.y, result.z, result.w};
                return values[component];
            }
            const auto& channel = (*components)[component];
            float result = 0.0f;
            ayt::anim::sampleTrackFloat(channel.values.data(), data->times.size(), data->times,
                static_cast<float>(seconds), result, data->interpolation,
                channel.incoming.data(), channel.outgoing.data());
            return result;
        };
        _curves.emplace(id, snapshot);
        return snapshot;
    }
private:
    void refreshCache() const {
        const auto current = revision();
        if (_cacheRevision == current) return;
        _cacheRevision = current;
        _curves.clear();
        _timeline.reset();
    }
    std::shared_ptr<EditorAnimationDocument> _document;
    mutable std::uint64_t _cacheRevision = std::numeric_limits<std::uint64_t>::max();
    mutable std::unordered_map<std::string, std::shared_ptr<const CurveTrack>> _curves;
    mutable std::shared_ptr<const TimelineSnapshot> _timeline;
};
} // namespace

std::shared_ptr<ayt::ui::authoring::ICurveEditorSource>
makeAnimationCurveSource(std::shared_ptr<EditorAnimationDocument> document)
{
    return document ? std::make_shared<AnimationCurveSource>(std::move(document)) : nullptr;
}
} // namespace ayt::editor
