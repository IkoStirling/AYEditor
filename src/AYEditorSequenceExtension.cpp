#include <AYEditor/EditorAuthoringSelectionBridge.h>
#include <AYEditor/EditorCommandButtons.h>
#include <AYEditor/EditorSequenceDocument.h>
#include <AYEntity.h>
#include <AYEntity/components/SkeletonComponent.h>
#include <AYScene.h>
#include <AYUI/Authoring/AuthoringPrimitives.h>
#include <AYUI/Authoring/DopeSheet.h>
#include <AYUI/Authoring/NumericFields.h>
#include <AYUI/Authoring/PreviewViewport.h>
#include <AYUI/Authoring/TimelineSelectionOps.h>
#include <AYUI/Box.h>
#include <AYUI/Button.h>
#include <AYUI/ComboBox.h>
#include <AYUI/ScrollView.h>
#include <AYUI/TextInput.h>
#include <AYUI/TextLabel.h>
#include <AYUI/UnicodeText.h>
#include <algorithm>
#include <cmath>

namespace ayt::editor {
namespace {
using namespace ayt::sequence;
using namespace ayt::ui;
using namespace ayt::ui::authoring;

class Transport final : public IPlaybackSource {
  public:
    explicit Transport(std::shared_ptr<EditorSequenceDocument> d) : document(std::move(d)) {}
    ayt::ui::authoring::PlaybackState playbackState() const override {
        return {true, document->playing(), document->position(),
                document->model().snapshot()->data().duration};
    }
    void play() override { document->play(); }
    void pause() override { document->pause(); }
    void stop() override { document->stop(); }
    bool seek(double t) override { return document->seek(t); }
    std::shared_ptr<EditorSequenceDocument> document;
};

// Dedicated section view: blocks are intervals, not fake key pairs. Whole drag
// uses one document history record. Capture cancellation restores its baseline.
class SectionTimeline final : public Widget {
  public:
    explicit SectionTimeline(std::shared_ptr<EditorSequenceDocument> d) : document(std::move(d)) {
        setId("sequence_sections");
    }
    ~SectionTimeline() override {
        changed = {};
        onCaptureCancelled();
    }
    std::function<void(const std::string &, const std::string &)> selected;
    std::function<void()> changed;
    void detach() {
        selected = {};
        changed = {};
        onCaptureCancelled();
    }
    bool onMouseButtonDown(const UIMouseEvent &e) override {
        if (e.mouseButton != 0 || !getWorldBounds().contains(e.mousePos))
            return false;
        const auto snapshot = document->model().snapshot();
        const auto bounds = getWorldBounds();
        const double t = time(e.mousePos.x);
        const int row = static_cast<int>((e.mousePos.y - bounds.minY - 22) / 26) + firstRow;
        if (e.mousePos.y >= bounds.minY + 22 && row >= 0 &&
            row < static_cast<int>(snapshot->data().animations.size())) {
            const auto &track = snapshot->data().animations[row];
            for (const auto &section : track.sections)
                if (t >= section.start && t <= section.end && e.mousePos.x >= bounds.minX + 168) {
                    if (selected)
                        selected(track.id, section.id);
                    if (!document->beginEdit("Move/trim sequence section"))
                        return true;
                    dragging = true;
                    baseline = section;
                    pointer = t;
                    dragDuration = snapshot->data().duration;
                    mode = std::abs(e.mousePos.x - x(section.start)) <= 7 ? 1
                           : std::abs(e.mousePos.x - x(section.end)) <= 7 ? 2
                                                                          : 0;
                    return true;
                }
            if (selected)
                selected(track.id, {});
        }
        document->seek(t);
        if (changed)
            changed();
        return true;
    }
    bool onMouseMove(const UIMouseEvent &e) override {
        if (!dragging)
            return false;
        const double delta = snapTimeToInterval(
            time(e.mousePos.x) - pointer, 1 / document->model().snapshot()->data().displayFps);
        AnimationSection next = baseline;
        if (mode == 0) {
            next.start += delta;
            next.end += delta;
        }
        if (mode == 1) {
            next.start += delta;
            next.sourceOffset += delta * baseline.speed;
        }
        if (mode == 2)
            next.end += delta;
        document->edit("Move/trim sequence section", [&](Sequence &s) {
            for (auto &track : s.animations)
                for (auto &section : track.sections)
                    if (section.id == baseline.id) {
                        section = next;
                        std::stable_sort(
                            track.sections.begin(), track.sections.end(),
                            [](const auto &a, const auto &b) { return a.start < b.start; });
                        return true;
                    }
            return false;
        });
        if (changed)
            changed();
        markDirty();
        return true;
    }
    bool onMouseButtonUp(const UIMouseEvent &e) override {
        if (!dragging || e.mouseButton != 0)
            return false;
        if (document->endEdit(false))
            dragging = false;
        if (changed)
            changed();
        return true;
    }
    void onCaptureCancelled() override {
        if (dragging) {
            document->endEdit(true);
            dragging = false;
            if (changed)
                changed();
        }
    }
    bool onMouseWheel(const UIMouseWheelEvent &e) override {
        if (!getWorldBounds().contains(e.mousePos))
            return false;
        onCaptureCancelled();
        firstRow = std::clamp(
            firstRow + (e.deltaY > 0 ? 1 : -1), 0,
            std::max(0,
                     static_cast<int>(document->model().snapshot()->data().animations.size()) - 1));
        markDirty();
        return true;
    }
    UiCursorHint getCursorHint() const override {
        return dragging ? (mode ? UiCursorHint::SizeWe : UiCursorHint::Move)
                        : UiCursorHint::Default;
    }

  protected:
    void onRender(IRenderBackend &r) override {
        const auto bounds = getWorldBounds();
        r.pushClip(bounds);
        r.drawRect(bounds, {0.04f, 0.05f, 0.07f, 1});
        const auto snapshot = document->model().snapshot();
        r.drawText({bounds.minX + 4, bounds.minY, bounds.minX + 164, bounds.minY + 22},
                   L"ANIMATION SECTIONS", 10, ayt::math::FVector4{0.7f, 0.8f, 0.9f, 1});
        for (double t :
             timelineTicks({0, snapshot->data().duration}, bounds.maxX - bounds.minX - 168)) {
            r.drawRect({x(t), bounds.minY, x(t) + 1, bounds.maxY}, {0.12f, 0.14f, 0.18f, 1});
            r.drawText({x(t) + 3, bounds.minY, x(t) + 63, bounds.minY + 22},
                       formatTime(t, TimeDisplay::Seconds, 2), 9,
                       ayt::math::FVector4{0.5f, 0.6f, 0.7f, 1});
        }
        for (std::size_t i = static_cast<std::size_t>(firstRow);
             i < snapshot->data().animations.size(); ++i) {
            const float y = bounds.minY + 22 + (i - firstRow) * 26;
            if (y >= bounds.maxY)
                break;
            const auto &track = snapshot->data().animations[i];
            r.drawText({bounds.minX + 4, y, bounds.minX + 164, y + 24},
                       decodeUtf8Text(track.binding + "/animation"), 10,
                       ayt::math::FVector4{0.7f, 0.8f, 0.9f, 1});
            for (const auto &section : track.sections) {
                r.drawRect({x(section.start), y + 3, x(section.end), y + 23},
                           {0.15f, 0.37f, 0.53f, 1});
                r.drawText({x(section.start) + 7, y + 3, x(section.end) - 3, y + 23},
                           decodeUtf8Text(section.source), 10,
                           ayt::math::FVector4{0.9f, 0.95f, 1, 1});
            }
        }
        r.drawRect({x(document->position()), bounds.minY, x(document->position()) + 2, bounds.maxY},
                   {1, 0.55f, 0.2f, 1});
        r.popClip();
    }

  private:
    double time(float px) const {
        const auto b = getWorldBounds();
        return std::clamp(double(px - b.minX - 168) / std::max(1.f, b.maxX - b.minX - 168), 0.,
                          1.) *
               (dragging ? dragDuration : document->model().snapshot()->data().duration);
    }
    float x(double t) const {
        const auto b = getWorldBounds();
        return b.minX + 168 +
               float(t / document->model().snapshot()->data().duration) *
                   std::max(1.f, b.maxX - b.minX - 168);
    }
    std::shared_ptr<EditorSequenceDocument> document;
    bool dragging = false;
    int mode = 0, firstRow = 0;
    double pointer = 0, dragDuration = 1;
    AnimationSection baseline;
};

class ScenePreview final : public Widget {
  public:
    explicit ScenePreview(std::shared_ptr<EditorSequenceDocument> d) : document(std::move(d)) {
        setId("sequence_preview");
    }
    bool onMouseButtonDown(const UIMouseEvent &e) override {
        if (e.mouseButton != 1 || !getWorldBounds().contains(e.mousePos))
            return false;
        orbit.begin(e.mousePos);
        return true;
    }
    bool onMouseMove(const UIMouseEvent &e) override {
        if (!orbit.move(e.mousePos))
            return false;
        markDirty();
        return true;
    }
    bool onMouseButtonUp(const UIMouseEvent &) override { return orbit.end(); }
    void onCaptureCancelled() override { orbit.cancel(); }
    bool onMouseWheel(const UIMouseWheelEvent &e) override {
        if (!getWorldBounds().contains(e.mousePos))
            return false;
        orbit.wheel(e.deltaY);
        markDirty();
        return true;
    }

  protected:
    void onRender(IRenderBackend &r) override {
        const auto b = getWorldBounds();
        r.pushClip(b);
        r.drawRect(b, {0.035f, 0.045f, 0.06f, 1});
        const auto *scene = document->previewScene();
        if (scene) {
            std::vector<ayt::math::FVector3> points;
            std::vector<std::pair<std::size_t, std::size_t>> lines;
            PreviewBounds bounds;
            // No scene tick, asset load, document rebuild or script execution in render.
            for (auto *entity : scene->world().getAllEntities()) {
                const auto *t = entity->getComponent<ayt::entity::Transform>();
                if (!t)
                    continue;
                points.push_back(t->position);
                bounds.include(t->position);
                const auto *skeleton = entity->getComponent<ayt::entity::SkeletonComponent>();
                if (!skeleton || !skeleton->loaded)
                    continue;
                const auto first = points.size();
                for (std::uint32_t i = 0; i < skeleton->jointCount; ++i) {
                    const auto &bone = skeleton->skeleton->getBones()[i];
                    if (!std::isfinite(bone.inverseBindMatrix.determinant()) ||
                        std::abs(bone.inverseBindMatrix.determinant()) < 1e-12f)
                        break;
                    const auto matrix =
                        skeleton->skinMatrices[i] * bone.inverseBindMatrix.inverse();
                    ayt::math::FVector3 local{matrix.row[0].w, matrix.row[1].w, matrix.row[2].w};
                    local = {local.x * t->scale.x, local.y * t->scale.y, local.z * t->scale.z};
                    // Quaternion rotation without an alternate coordinate convention.
                    const ayt::math::FVector3 q{t->rotation.x, t->rotation.y, t->rotation.z};
                    const auto cross = [](auto a, auto v) {
                        return ayt::math::FVector3{a.y * v.z - a.z * v.y, a.z * v.x - a.x * v.z,
                                                   a.x * v.y - a.y * v.x};
                    };
                    const auto uv = cross(q, local);
                    local += uv * (2 * t->rotation.w) + cross(q, uv) * 2;
                    points.push_back(local + t->position);
                    bounds.include(points.back());
                    if (bone.parentIndex >= 0)
                        lines.emplace_back(first + i, first + bone.parentIndex);
                }
            }
            if (fittedRevision != document->revision() || fittedScene != scene) {
                fittedBounds = bounds;
                fittedBounds.include({0, 0, 0});
                fittedBounds.include({1, 1, 1});
                for (const auto &track : document->model().snapshot()->data().transforms)
                    if (track.channel == Channel::Position)
                        for (const auto &key : track.keys)
                            fittedBounds.include(
                                {float(key.value[0]), float(key.value[1]), float(key.value[2])});
                fittedRevision = document->revision();
                fittedScene = scene;
            }
            PreviewProjection projection(fittedBounds, {b.minX, b.minY + 24, b.maxX, b.maxY}, orbit,
                                         0.7f);
            const auto path = r.createPath();
            if (path.id >= 0) {
                for (auto [a, c] : lines) {
                    auto p = projection(points[a]), q = projection(points[c]);
                    ayt::math::FVector2 segment[] = {{p.x, p.y}, {q.x, q.y}};
                    r.addPathContour(path, segment, 2, false);
                }
                r.setPathStrokeColor(path, {0.3f, 0.8f, 1, 1});
                r.setPathStrokeWidth(path, 2);
                r.drawPath(path, PathFillMode::Stroke);
                r.releasePath(path);
            }
            for (auto point : points) {
                auto p = projection(point);
                r.drawRect({p.x - 3, p.y - 3, p.x + 3, p.y + 3}, {0.8f, 0.9f, 1, 1});
            }
        }
        r.drawText({b.minX + 6, b.minY + 3, b.maxX - 6, b.minY + 23},
                   scene ? L"ISOLATED SCENE / SKELETON | RMB orbit | Wheel zoom"
                         : L"Set Scene path, entity bindings, then scrub to preview",
                   10, ayt::math::FVector4{0.7f, 0.8f, 0.9f, 1});
        r.popClip();
    }

  private:
    std::shared_ptr<EditorSequenceDocument> document;
    PreviewOrbit orbit;
    PreviewBounds fittedBounds;
    std::uint64_t fittedRevision = ~std::uint64_t{};
    const ayt::scene::Scene *fittedScene = nullptr;
};

class SequenceView final : public IEditorView {
  public:
    SequenceView(std::shared_ptr<EditorSequenceDocument> d, IEditorHostServices &h)
        : document(std::move(d)), host(h), commands([this] { return document.get(); }),
          bridge(h.workspace(), document), source(makeEditorSequenceSource(document)) {
        document->configureProjectRoot(host.projectRoot());
        build();
        refresh();
    }
    ~SequenceView() override {
        prepareForUiShutdown();
        if (root)
            destroyWidgetTree(root);
    }
    Widget *rootWidget() noexcept override { return root; }
    Widget *releaseRootWidget() noexcept override {
        auto *result = root;
        root = nullptr;
        return result;
    }
    IEditorCommandTarget *commandTarget() noexcept override { return document.get(); }
    EditorSelectionContext *selectionContext() noexcept override { return bridge.context(); }
    void onDeactivated() override {
        if (closed)
            return;
        document->pause();
        cancelGestures();
    }
    void prepareForUiShutdown() override {
        if (closed)
            return;
        closed = true;
        commands.detach();
        sections->detach();
        sheet->setOnEdited({});
        sheet->setOnSelectionChanged({});
        sheet->onCaptureCancelled();
        preview->onCaptureCancelled();
        document->closePreview();
    }
    void tick(float dt) override {
        if (closed)
            return;
        document->tick(dt);
        if (stamp != document->revision())
            refresh();
        if (lastPosition != document->position() || lastPlaying != document->playing()) {
            lastPosition = document->position();
            lastPlaying = document->playing();
            transport->refresh();
            preview->markDirty();
            sheet->markDirty();
            sections->markDirty();
        }
        commands.refresh();
        if (lastDiagnostic != document->diagnostic()) {
            lastDiagnostic = document->diagnostic();
            status->setText(decodeUtf8Text(lastDiagnostic));
        }
    }

  private:
    const Sequence &data() const { return document->model().snapshot()->data(); }
    std::string text(TextInput *input) const { return encodeUtf8Text(input->getText()); }
    void cancelGestures() {
        sections->onCaptureCancelled();
        sheet->onCaptureCancelled();
    }
    HBox *row(VBox &parent) {
        auto *r = new HBox();
        r->setSpacing(4);
        parent.addWidget(r, 28);
        return r;
    }
    TextInput *input(HBox &r, const std::string &id, const std::wstring &hint) {
        auto *v = new TextInput();
        v->setId(id);
        v->setPlaceholder(hint);
        r.addWidget(v, 0);
        return v;
    }
    void button(HBox &r, const std::wstring &label, const std::string &id,
                std::function<void()> action) {
        auto *b = new Button();
        b->setText(label);
        b->setId(id);
        b->setOnClicked([this, action = std::move(action)] {
            action();
            refresh();
            host.requestRepaint();
        });
        r.addWidget(b, 100);
    }
    void build() {
        root = new VBox();
        root->setId("sequence_editor");
        auto *toolbar = row(*root);
        commands.add(*toolbar, L"Save", "file.save", 70);
        commands.add(*toolbar, L"Undo", "edit.undo", 70);
        commands.add(*toolbar, L"Redo", "edit.redo", 70);
        transport = new PlaybackControls(std::make_shared<Transport>(document));
        toolbar->addWidget(transport, 0);
        auto *body = new HBox();
        root->addWidget(body, 0);
        auto *left = new VBox();
        body->addWidget(left, 0);
        preview = new ScenePreview(document);
        left->addWidget(preview, 0);
        sections = new SectionTimeline(document);
        left->addWidget(sections, 150);
        sections->selected = [this](const auto &track, const auto &section) {
            source->selectionState()->trackId = track;
            source->selectionState()->keyIds.clear();
            source->selectionState()->primaryKeyId.clear();
            sectionId = section;
            refresh();
        };
        sections->changed = [this] { refresh(); };
        sheet = new DopeSheet(source);
        left->addWidget(sheet, 200);
        sheet->setOnEdited([this] { refresh(); });
        sheet->setOnSelectionChanged([this](const auto &, const auto &) {
            sectionId.clear();
            refresh();
        });
        auto *scroll = new ScrollView();
        body->addWidget(scroll, 430);
        auto *inspector = new VBox();
        inspector->setSpacing(4);
        scroll->setContentOwned(inspector);
        auto *sceneRow = row(*inspector);
        scenePath = input(*sceneRow, "sequence_scene", L"Scene relative to Assets");
        button(*sceneRow, L"Set Scene", "sequence_set_scene", [this] {
            document->edit("Set sequence scene", [&](Sequence &s) {
                s.scenePath = text(scenePath);
                return true;
            });
        });
        properties = new NumericFields({"sequence_duration", "sequence_fps"}, {L"Seconds", L"FPS"});
        inspector->addWidget(properties, 28);
        auto *propertyRow = row(*inspector);
        button(*propertyRow, L"Apply length", "sequence_apply_length", [this] {
            std::vector<float> v;
            if (properties->readValues(2, v))
                document->edit("Sequence properties", [&](Sequence &s) {
                    s.duration = v[0];
                    s.displayFps = v[1];
                    return true;
                });
        });
        auto *bindRow = row(*inspector);
        bindings = new ComboBox();
        bindings->setId("sequence_bindings");
        bindRow->addWidget(bindings, 130);
        entityName = input(*bindRow, "sequence_entity_name", L"Exact unique entity name");
        bindings->setOnSelectionChanged([this](int) { refreshBinding(); });
        auto *bindButtons = row(*inspector);
        button(*bindButtons, L"Add binding", "sequence_add_binding", [this] {
            const auto id = document->model().nextId("binding");
            document->edit("Add binding", [&](Sequence &s) {
                s.bindings.push_back({id, text(entityName)});
                return true;
            });
        });
        button(*bindButtons, L"Set target", "sequence_set_target", [this] {
            auto id = bindingId();
            document->edit("Set binding target", [&](Sequence &s) {
                for (auto &b : s.bindings)
                    if (b.id == id) {
                        b.entityName = text(entityName);
                        return true;
                    }
                return false;
            });
        });
        button(*bindButtons, L"Remove", "sequence_remove_binding", [this] {
            auto id = bindingId();
            document->edit("Remove binding", [&](Sequence &s) {
                return std::erase_if(s.bindings, [&](const auto &b) { return b.id == id; }) != 0;
            });
        });
        auto *trackRow = row(*inspector);
        tracks = new ComboBox();
        tracks->setId("sequence_tracks");
        trackRow->addWidget(tracks, 0);
        tracks->setOnSelectionChanged([this](int i) {
            cancelGestures();
            if (i >= 0 && i < int(trackIds.size()))
                TimelineSelectionOps::single(*source->selectionState(), trackIds[i], {});
            sectionId.clear();
            refreshSelection();
        });
        auto *addRow = row(*inspector);
        button(*addRow, L"Position", "sequence_add_position",
               [this] { addTransform(Channel::Position); });
        button(*addRow, L"Rotation", "sequence_add_rotation",
               [this] { addTransform(Channel::Rotation); });
        button(*addRow, L"Scale", "sequence_add_scale", [this] { addTransform(Channel::Scale); });
        auto *trackButtons = row(*inspector);
        button(*trackButtons, L"Events", "sequence_add_events", [this] {
            auto id = document->model().nextId("events");
            document->edit("Add event track", [&](Sequence &s) {
                s.eventTracks.push_back({id, {}});
                return true;
            });
            TimelineSelectionOps::single(*source->selectionState(), id, {});
        });
        button(*trackButtons, L"Remove track", "sequence_remove_track", [this] {
            const auto id = source->selectionState()->trackId;
            document->edit("Remove track", [&](Sequence &s) {
                return std::erase_if(s.transforms, [&](const auto &t) { return t.id == id; }) +
                           std::erase_if(s.animations, [&](const auto &t) { return t.id == id; }) +
                           std::erase_if(s.eventTracks,
                                         [&](const auto &t) { return t.id == id; }) !=
                       0;
            });
        });
        auto *keyRow = row(*inspector);
        keys = new ComboBox();
        keys->setId("sequence_keys");
        keyRow->addWidget(keys, 0);
        keys->setOnSelectionChanged([this](int i) {
            if (i >= 0 && i < int(keyIds.size())) {
                TimelineSelectionOps::single(*source->selectionState(),
                                             source->selectionState()->trackId, keyIds[i]);
                refreshSelection();
            }
        });
        keyTime = new NumericFields({"sequence_key_time"}, {L"Time"});
        inspector->addWidget(keyTime, 28);
        keyValues = new NumericFields(
            {"sequence_key_x", "sequence_key_y", "sequence_key_z", "sequence_key_w"},
            {L"X", L"Y", L"Z", L"W"});
        inspector->addWidget(keyValues, 28);
        auto *eventRow = row(*inspector);
        eventName = input(*eventRow, "sequence_event_name", L"Event name");
        eventPayload = input(*eventRow, "sequence_event_payload", L"Payload (text)");
        auto *keyButtons = row(*inspector);
        button(*keyButtons, L"Add at head", "sequence_add_key", [this] { addKey(); });
        button(*keyButtons, L"Apply key", "sequence_apply_key", [this] { applyKey(); });
        button(*keyButtons, L"Delete item", "sequence_delete_item", [this] {
            if (!sectionId.empty())
                document->edit("Delete section",
                               [&](Sequence &s) { return removeSequenceItems(s, {sectionId}); });
            else
                source->removeKeys(source->selectionState()->keyIds);
        });
        auto *clipRow = row(*inspector);
        clipPath = input(*clipRow, "sequence_clip", L"Clip relative to Assets (.anm)");
        sectionFields = new NumericFields({"sequence_start", "sequence_end", "sequence_offset",
                                           "sequence_speed", "sequence_source_duration"},
                                          {L"Start", L"End", L"Offset", L"Speed", L"Clip sec"});
        inspector->addWidget(sectionFields, 28);
        sectionFields->setValues({0, 1, 0, 1, 1});
        auto *sectionButtons = row(*inspector);
        button(*sectionButtons, L"Add section", "sequence_add_section", [this] { addSection(); });
        button(*sectionButtons, L"Apply section", "sequence_apply_section",
               [this] { applySection(); });
        auto *previewRow = row(*inspector);
        button(*previewRow, L"Load preview", "sequence_load_preview",
               [this] { document->seek(document->position()); });
        status = new TextLabel();
        status->setId("sequence_diagnostics");
        inspector->addWidget(status, 72);
    }
    std::string bindingId() const {
        int i = bindings->getSelectedIndex();
        return i >= 0 && i < int(bindingIds.size()) ? bindingIds[i] : std::string{};
    }
    void refreshBinding() {
        const auto id = bindingId();
        for (const auto &b : data().bindings)
            if (b.id == id)
                entityName->setText(decodeUtf8Text(b.entityName));
    }
    void addTransform(Channel channel) {
        const auto id = document->model().nextId("transform"),
                   key = document->model().nextId("key"), binding = bindingId();
        Value value{};
        if (channel == Channel::Scale)
            value = {1, 1, 1, 0};
        if (channel == Channel::Rotation)
            value = {0, 0, 0, 1};
        if (document->edit("Add transform track", [&](Sequence &s) {
                s.transforms.push_back(
                    {id, binding, channel, {{key, document->position(), value}}});
                return true;
            }))
            TimelineSelectionOps::single(*source->selectionState(), id, key);
    }
    void addKey() {
        const auto track = source->selectionState()->trackId, id = document->model().nextId("key");
        const double time = document->position();
        if (document->edit("Add sequence key/event", [&](Sequence &s) {
                for (auto &t : s.transforms)
                    if (t.id == track) {
                        Value value = t.keys.back().value;
                        t.keys.push_back({id, time, value});
                        std::stable_sort(
                            t.keys.begin(), t.keys.end(),
                            [](const auto &a, const auto &b) { return a.time < b.time; });
                        return true;
                    }
                for (auto &t : s.eventTracks)
                    if (t.id == track) {
                        t.events.push_back({id, time,
                                            text(eventName).empty() ? "event" : text(eventName),
                                            text(eventPayload)});
                        std::stable_sort(
                            t.events.begin(), t.events.end(),
                            [](const auto &a, const auto &b) { return a.time < b.time; });
                        return true;
                    }
                return false;
            }))
            TimelineSelectionOps::single(*source->selectionState(), track, id);
    }
    void applyKey() {
        auto id = source->selectionState()->primaryKeyId;
        std::vector<float> t, v;
        if (!keyTime->readValues(1, t))
            return;
        if (keyValues->componentCount() && keyValues->readValues(keyValues->componentCount(), v)) {
            source->updateKey(id, t[0], v);
            return;
        }
        document->edit("Edit sequence event", [&](Sequence &s) {
            for (auto &track : s.eventTracks)
                for (auto &e : track.events)
                    if (e.id == id) {
                        e.time = t[0];
                        e.name = text(eventName);
                        e.payload = text(eventPayload);
                        std::stable_sort(
                            track.events.begin(), track.events.end(),
                            [](const auto &a, const auto &b) { return a.time < b.time; });
                        return true;
                    }
            return false;
        });
    }
    AnimationSection sectionDraft(const std::string &id, const std::vector<float> &v) {
        return {id, text(clipPath), v[0], v[1], v[2], v[3], v[4]};
    }
    void addSection() {
        std::vector<float> v;
        if (!sectionFields->readValues(5, v))
            return;
        const auto id = document->model().nextId("section"),
                   trackId = document->model().nextId("animation"), binding = bindingId(),
                   selected = source->selectionState()->trackId;
        std::string actual = trackId;
        if (document->edit("Add animation section", [&](Sequence &s) {
                for (auto &t : s.animations)
                    if (t.id == selected) {
                        actual = t.id;
                        t.sections.push_back(sectionDraft(id, v));
                        std::stable_sort(
                            t.sections.begin(), t.sections.end(),
                            [](const auto &a, const auto &b) { return a.start < b.start; });
                        return true;
                    }
                s.animations.push_back({trackId, binding, {sectionDraft(id, v)}});
                return true;
            })) {
            TimelineSelectionOps::single(*source->selectionState(), actual, {});
            sectionId = id;
        }
    }
    void applySection() {
        std::vector<float> v;
        if (!sectionFields->readValues(5, v))
            return;
        document->edit("Edit animation section", [&](Sequence &s) {
            for (auto &t : s.animations)
                for (auto &k : t.sections)
                    if (k.id == sectionId) {
                        k = sectionDraft(sectionId, v);
                        std::stable_sort(
                            t.sections.begin(), t.sections.end(),
                            [](const auto &a, const auto &b) { return a.start < b.start; });
                        return true;
                    }
            return false;
        });
    }
    void refreshSelection() {
        const auto selection = source->selectionState();
        std::vector<std::wstring> labels;
        keyIds.clear();
        bool found = false;
        for (const auto &t : data().transforms)
            if (t.id == selection->trackId)
                for (const auto &k : t.keys) {
                    labels.push_back(decodeUtf8Text(k.id));
                    keyIds.push_back(k.id);
                    if (k.id == selection->primaryKeyId) {
                        keyTime->setValues({float(k.time)});
                        std::vector<float> v;
                        for (std::size_t i = 0; i < (t.channel == Channel::Rotation ? 4u : 3u); ++i)
                            v.push_back(float(k.value[i]));
                        keyValues->setValues(v);
                        found = true;
                    }
                }
        for (const auto &t : data().eventTracks)
            if (t.id == selection->trackId)
                for (const auto &k : t.events) {
                    labels.push_back(decodeUtf8Text(k.id));
                    keyIds.push_back(k.id);
                    if (k.id == selection->primaryKeyId) {
                        keyTime->setValues({float(k.time)});
                        eventName->setText(decodeUtf8Text(k.name));
                        eventPayload->setText(decodeUtf8Text(k.payload));
                        found = true;
                        keyValues->setValues({});
                    }
                }
        keys->setItems(labels);
        const auto it = std::find(keyIds.begin(), keyIds.end(), selection->primaryKeyId);
        keys->setSelectedIndex(it == keyIds.end() ? -1 : int(it - keyIds.begin()));
        if (!found) {
            keyValues->setValues({});
            keyTime->setValues({float(document->position())});
        }
        for (const auto &t : data().animations)
            if (t.id == selection->trackId) {
                if (sectionId.empty() && !t.sections.empty())
                    sectionId = t.sections.front().id;
                for (const auto &k : t.sections)
                    if (k.id == sectionId) {
                        clipPath->setText(decodeUtf8Text(k.source));
                        sectionFields->setValues({float(k.start), float(k.end),
                                                  float(k.sourceOffset), float(k.speed),
                                                  float(k.sourceDuration)});
                    }
            }
        bridge.publish(*selection);
        sheet->markDirty();
        sections->markDirty();
    }
    void refresh() {
        if (closed)
            return;
        stamp = document->revision();
        scenePath->setText(decodeUtf8Text(data().scenePath));
        properties->setValues({float(data().duration), float(data().displayFps)});
        const auto selectedBinding = bindingId();
        bindingIds.clear();
        std::vector<std::wstring> labels;
        for (const auto &b : data().bindings) {
            bindingIds.push_back(b.id);
            labels.push_back(decodeUtf8Text(b.id));
        }
        bindings->setItems(labels);
        auto bi = std::find(bindingIds.begin(), bindingIds.end(), selectedBinding);
        bindings->setSelectedIndex(bi == bindingIds.end() ? (bindingIds.empty() ? -1 : 0)
                                                          : int(bi - bindingIds.begin()));
        refreshBinding();
        labels.clear();
        trackIds.clear();
        for (const auto &t : data().transforms) {
            trackIds.push_back(t.id);
            labels.push_back(decodeUtf8Text(t.id + " / " + t.binding));
        }
        for (const auto &t : data().animations) {
            trackIds.push_back(t.id);
            labels.push_back(decodeUtf8Text(t.id + " / " + t.binding));
        }
        for (const auto &t : data().eventTracks) {
            trackIds.push_back(t.id);
            labels.push_back(decodeUtf8Text(t.id));
        }
        tracks->setItems(labels);
        auto selected = source->selectionState();
        auto ti = std::find(trackIds.begin(), trackIds.end(), selected->trackId);
        if (ti == trackIds.end()) {
            TimelineSelectionOps::clear(*selected);
            sectionId.clear();
        }
        tracks->setSelectedIndex(ti == trackIds.end() ? -1 : int(ti - trackIds.begin()));
        refreshSelection();
        transport->refresh();
        commands.refresh();
        status->setText(decodeUtf8Text(document->diagnostic()));
        preview->markDirty();
    }
    std::shared_ptr<EditorSequenceDocument> document;
    IEditorHostServices &host;
    EditorCommandButtons commands;
    EditorAuthoringSelectionBridge bridge;
    std::shared_ptr<ICurveEditorSource> source;
    VBox *root = nullptr;
    bool closed = false;
    std::uint64_t stamp = ~std::uint64_t{};
    PlaybackControls *transport = nullptr;
    ScenePreview *preview = nullptr;
    SectionTimeline *sections = nullptr;
    DopeSheet *sheet = nullptr;
    TextInput *scenePath = nullptr, *entityName = nullptr, *eventName = nullptr,
              *eventPayload = nullptr, *clipPath = nullptr;
    NumericFields *properties = nullptr, *keyTime = nullptr, *keyValues = nullptr,
                  *sectionFields = nullptr;
    ComboBox *bindings = nullptr, *tracks = nullptr, *keys = nullptr;
    TextLabel *status = nullptr;
    std::vector<std::string> bindingIds, trackIds, keyIds;
    std::string sectionId, lastDiagnostic;
    double lastPosition = -1;
    bool lastPlaying = false;
};
} // namespace
EditorDescriptor makeEditorSequenceDescriptor() {
    EditorDescriptor d;
    d.id = kEditorSequenceExtensionId;
    d.displayName = L"Scene Sequence";
    d.priority = 100;
    d.extensions = {".seq", ".ayseq"};
    d.assetTypes = {"Scene Sequence", "authoring.scene-sequence"};
    d.createDocument = [](const EditorOpenRequest &request, std::string &error) {
        auto d = std::make_shared<EditorSequenceDocument>();
        return d->initialize(request, error) ? std::static_pointer_cast<IEditorDocument>(d)
                                             : nullptr;
    };
    d.createView = [](const std::shared_ptr<IEditorDocument> &document,
                      IEditorHostServices &host) -> std::unique_ptr<IEditorView> {
        auto d = std::dynamic_pointer_cast<EditorSequenceDocument>(document);
        return d ? std::make_unique<SequenceView>(d, host) : nullptr;
    };
    return d;
}
bool registerEditorSequenceExtension(EditorExtensionRegistry &registry, std::string *error) {
    return registry.registerEditor(makeEditorSequenceDescriptor(), error);
}
} // namespace ayt::editor
