#include <AYEditor/EditorSequenceDocument.h>
#include <AYUI/Authoring/AuthoringPrimitives.h>
#include <cmath>
#include <unordered_map>

namespace ayt::editor {
using namespace ayt::sequence;
using namespace ayt::ui::authoring;
namespace {
class SequenceSource final : public ICurveEditorSource {
  public:
    explicit SequenceSource(std::shared_ptr<EditorSequenceDocument> document)
        : _document(std::move(document)) {}
    std::uint64_t revision() const noexcept override { return _document->revision(); }
    double durationSeconds() const noexcept override {
        return _document->model().snapshot()->data().duration;
    }
    double positionSeconds() const noexcept override { return _document->position(); }
    bool seek(double seconds) override { return _document->seek(seconds); }
    double snapTime(const std::string &, double seconds) const override {
        return snapTimeToInterval(seconds, 1 / _document->model().snapshot()->data().displayFps);
    }
    bool beginEdit(const std::string &label) override { return _document->beginEdit(label); }
    bool endEdit(bool cancel) override { return _document->endEdit(cancel); }
    bool updateKey(std::string &id, double time, const std::vector<float> &values) override {
        return _document->edit("Edit sequence key", [&](Sequence &s) {
            for (const auto &t : s.transforms)
                for (const auto &k : t.keys)
                    if (k.id == id) {
                        const auto width = t.channel == Channel::Rotation ? 4u : 3u;
                        if (values.size() != width)
                            return false;
                        Value v{};
                        for (std::size_t i = 0; i < width; ++i)
                            v[i] = values[i];
                        if (width == 4) {
                            double norm = 0;
                            for (auto a : v)
                                norm += a * a;
                            if (!std::isfinite(norm) || norm < 1e-12)
                                return false;
                            for (auto &a : v)
                                a /= std::sqrt(norm);
                        }
                        return updateSequenceKey(s, id, time, v);
                    }
            return false;
        });
    }
    bool transformKeys(std::vector<std::string> &ids, double delta, std::size_t,
                       float value) override {
        if (value != 0)
            return false; // S4 time-only group move, not per-component curves.
        return _document->edit("Move sequence keys",
                               [&](Sequence &s) { return moveSequenceItems(s, ids, delta); });
    }
    bool removeKeys(const std::vector<std::string> &ids) override {
        return _document->edit("Delete sequence items",
                               [&](Sequence &s) { return removeSequenceItems(s, ids); });
    }
    bool moveTimelineKey(std::string &, std::string &id, double seconds) override {
        for (const auto &t : _document->model().snapshot()->data().transforms)
            for (const auto &k : t.keys)
                if (k.id == id) {
                    const double delta = seconds - k.time;
                    return _document->edit("Move sequence key", [&](Sequence &s) {
                        return moveSequenceItems(s, {id}, delta);
                    });
                }
        for (const auto &t : _document->model().snapshot()->data().eventTracks)
            for (const auto &k : t.events)
                if (k.id == id) {
                    const double delta = seconds - k.time;
                    return _document->edit("Move sequence event", [&](Sequence &s) {
                        return moveSequenceItems(s, {id}, delta);
                    });
                }
        return false;
    }
    std::shared_ptr<const TimelineSnapshot> timelineSnapshot() const override {
        refresh();
        if (_timeline)
            return _timeline;
        auto view = std::make_shared<TimelineSnapshot>();
        for (const auto &t : _document->model().snapshot()->data().transforms) {
            const char *channel = t.channel == Channel::Position ? "position"
                                  : t.channel == Channel::Scale  ? "scale"
                                                                 : "rotation";
            view->tracks.push_back({t.id, t.binding + "/" + channel, TimelineTrackKind::Value});
            for (const auto &k : t.keys)
                view->keys.push_back({k.id, t.id, k.time});
        }
        for (const auto &t : _document->model().snapshot()->data().eventTracks) {
            view->tracks.push_back({t.id, t.id, TimelineTrackKind::Event});
            for (const auto &k : t.events)
                view->keys.push_back({k.id, t.id, k.time});
        }
        _timeline = view;
        return view;
    }
    std::shared_ptr<const CurveTrack> curveTrack(const std::string &id) const override {
        refresh();
        if (auto it = _curves.find(id); it != _curves.end())
            return it->second;
        const auto data = _document->model().snapshot();
        for (const auto &t : data->data().transforms)
            if (t.id == id) {
                auto view = std::make_shared<CurveTrack>();
                view->id = id;
                view->snapIntervalSeconds = 1 / data->data().displayFps;
                const auto width = t.channel == Channel::Rotation ? 4u : 3u;
                for (const auto &k : t.keys) {
                    std::vector<float> values;
                    for (std::size_t i = 0; i < width; ++i)
                        values.push_back(static_cast<float>(k.value[i]));
                    view->keys.push_back({k.id, k.time, std::move(values)});
                }
                const auto binding = t.binding;
                const auto channel = t.channel;
                view->sample = [data, binding, channel, width](std::size_t component, double time) {
                    Evaluation output;
                    if (component >= width || !data->evaluate(time, output))
                        return 0.f;
                    for (const auto &sample : output.transforms)
                        if (sample.binding == binding && sample.channel == channel)
                            return static_cast<float>(sample.value[component]);
                    return 0.f;
                };
                _curves[id] = view;
                return view;
            }
        return {};
    }

  private:
    void refresh() const {
        if (_revision == revision())
            return;
        _revision = revision();
        _timeline.reset();
        _curves.clear();
    }
    std::shared_ptr<EditorSequenceDocument> _document;
    mutable std::uint64_t _revision = ~std::uint64_t{};
    mutable std::shared_ptr<const TimelineSnapshot> _timeline;
    mutable std::unordered_map<std::string, std::shared_ptr<const CurveTrack>> _curves;
};
} // namespace
std::shared_ptr<ICurveEditorSource>
makeEditorSequenceSource(std::shared_ptr<EditorSequenceDocument> document) {
    return std::make_shared<SequenceSource>(std::move(document));
}
} // namespace ayt::editor
