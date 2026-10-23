#include <AYEditor/EditorCommandSystem.h>
#include <AYEditor/EditorDslDocument.h>
#include <AYEditor/EditorDslExtension.h>
#include <AYEditor/EditorExtension.h>
#include <AYRenderer.h>
#include <AYRenderer/PbrShaderSources.h>
#include <AYRenderer/RendererSubSystem.h>
#include <AYSequence/LightShowAssetIO.h>
#include <AYSequence/LightShowAuthoring.h>
#include <AYSequence/LightShowPresenter.h>
#include <AYUI/Box.h>
#include <AYUI/Button.h>
#include <AYUI/ScrollView.h>
#include <AYUI/TextArea.h>
#include <AYUI/TextInput.h>
#include <AYUI/TextLabel.h>
#include <AYUI/UIManager.h>
#include <AYUI/UnicodeText.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
namespace ayt::editor {
namespace {
using namespace ayt::ui;
using namespace ayt::sequence;
namespace mr = ayt::render;
constexpr double keyTolerance = 1e-6;
std::wstring wide(const std::string &s) { return decodeUtf8Text(s); }
std::string narrow(const std::wstring &s) { return encodeUtf8Text(s); }
std::wstring number(double x) {
  std::wostringstream s;
  s.precision(7);
  s << x;
  return s.str();
}
bool numeric(TextInput *field, double &value) {
  const auto s = narrow(field->getText());
  const auto [end, error] =
      std::from_chars(s.data(), s.data() + s.size(), value);
  return error == std::errc{} && end == s.data() + s.size() &&
         std::isfinite(value);
}
template <class K> void putKey(std::vector<K> &keys, K key) {
  const auto it = std::lower_bound(
      keys.begin(), keys.end(), key.time,
      [](const auto &a, double t) { return a.time < t - keyTolerance; });
  if (it != keys.end() && std::abs(it->time - key.time) <= keyTolerance)
    *it = std::move(key);
  else
    keys.insert(it, std::move(key));
}
struct Lane {
  std::string target, label;
  std::vector<double> times;
};
// Visible rows only; repeated keys in one pixel bin produce one marker.
class ShowTimeline final : public Widget {
public:
  std::vector<Lane> lanes;
  double duration = 10, position = 0;
  std::string target = "all";
  std::function<void(double)> seek;
  std::function<void(const std::string &, double)> select;
  std::function<void(const std::string &, double, double)> move;
  bool onMouseButtonDown(const UIMouseEvent &e) override {
    if (e.mouseButton != 0 || !getWorldBounds().contains(e.mousePos))
      return false;
    const auto b = getWorldBounds();
    const int row = int((e.mousePos.y - b.minY - 22) / 24) + firstRow;
    if (e.mousePos.y >= b.minY + 22 && row >= 0 && row < int(lanes.size())) {
      const auto &lane = lanes[row];
      double t = time(e.mousePos.x), nearest = t;
      bool hit = false;
      if (e.mousePos.x >= b.minX + 160) {
        const auto it =
            std::lower_bound(lane.times.begin(), lane.times.end(), t);
        double distance = 7;
        for (auto candidate :
             {it, it == lane.times.begin() ? lane.times.end() : std::prev(it)})
          if (candidate != lane.times.end() &&
              std::abs(x(*candidate) - e.mousePos.x) < distance) {
            distance = std::abs(x(*candidate) - e.mousePos.x);
            nearest = *candidate;
            hit = true;
          }
      }
      target = lane.target;
      if (select)
        select(target, hit ? nearest : t);
      if (seek)
        seek(hit ? nearest : t);
      if (hit) {
        dragging = true;
        original = nearest;
        ghost = nearest;
        dragTarget = target;
      }
    } else if (seek)
      seek(time(e.mousePos.x));
    markDirty();
    return true;
  }
  bool onMouseMove(const UIMouseEvent &e) override {
    if (!dragging)
      return false;
    ghost =
        std::clamp(std::round(time(e.mousePos.x) * 60.) / 60., 0., duration);
    markDirty();
    return true;
  }
  bool onMouseButtonUp(const UIMouseEvent &e) override {
    if (!dragging || e.mouseButton != 0)
      return false;
    dragging = false;
    if (std::abs(ghost - original) > keyTolerance && move)
      move(dragTarget, original, ghost);
    markDirty();
    return true;
  }
  void onCaptureCancelled() override {
    dragging = false;
    markDirty();
  }
  bool onMouseWheel(const UIMouseWheelEvent &e) override {
    firstRow = std::clamp(firstRow + (e.deltaY > 0 ? -3 : 3), 0,
                          std::max(0, int(lanes.size()) - 1));
    markDirty();
    return true;
  }

protected:
  void onRender(IRenderBackend &r) override {
    const auto b = getWorldBounds();
    r.pushClip(b);
    r.drawRect(b, {.035f, .045f, .065f, 1});
    r.drawText({b.minX + 4, b.minY, b.minX + 158, b.minY + 22},
               L"GROUP / FIXTURE / FOG", 10,
               ayt::math::FVector4{.7f, .8f, .9f, 1});
    for (int i = 0; i <= 10; ++i) {
      float px = x(duration * i / 10.);
      r.drawRect({px, b.minY, px + 1, b.maxY}, {.13f, .15f, .19f, 1});
      r.drawText({px + 2, b.minY, px + 55, b.minY + 22},
                 number(duration * i / 10.), 9,
                 ayt::math::FVector4{.6f, .7f, .8f, 1});
    }
    firstRow = std::min(firstRow, std::max(0, int(lanes.size()) - 1));
    for (int row = firstRow; row < int(lanes.size()); ++row) {
      float y = b.minY + 22 + (row - firstRow) * 24;
      if (y >= b.maxY)
        break;
      const auto &lane = lanes[row];
      if (lane.target == target)
        r.drawRect({b.minX, y, b.maxX, y + 24}, {.1f, .2f, .28f, 1});
      r.drawText({b.minX + 4, y, b.minX + 158, y + 23}, wide(lane.label), 10,
                 ayt::math::FVector4{.78f, .84f, .9f, 1});
      for (auto it = lane.times.begin(); it != lane.times.end();) {
        int px = int(x(*it));
        r.drawRect({float(px) - 2, y + 9, float(px) + 3, y + 15},
                   {.35f, .8f, 1, 1});
        const double limit = double(px + 1 - b.minX - 160) /
                             std::max(1.f, b.maxX - b.minX - 164) * duration;
        it = std::upper_bound(std::next(it), lane.times.end(), limit);
      }
    }
    float px = x(position);
    r.drawRect({px, b.minY, px + 2, b.maxY}, {1, .55f, .18f, 1});
    if (dragging) {
      px = x(ghost);
      r.drawRect({px, b.minY + 22, px + 2, b.maxY}, {1, 1, .5f, 1});
    }
    r.popClip();
  }

private:
  float x(double t) const {
    const auto b = getWorldBounds();
    return b.minX + 160 +
           float(t / std::max(.001, duration)) *
               std::max(1.f, b.maxX - b.minX - 164);
  }
  double time(float px) const {
    const auto b = getWorldBounds();
    return std::clamp(double(px - b.minX - 160) /
                          std::max(1.f, b.maxX - b.minX - 164),
                      0., 1.) *
           duration;
  }
  int firstRow = 0;
  bool dragging = false;
  double original = 0, ghost = 0;
  std::string dragTarget;
};
class ShowViewport final : public Widget {
public:
  std::function<void(float, float)> orbit;
  std::function<void(float)> zoom;
  bool onMouseButtonDown(const UIMouseEvent &e) override {
    if (e.mouseButton != 1)
      return false;
    dragging = true;
    previous = e.mousePos;
    return true;
  }
  bool onMouseMove(const UIMouseEvent &e) override {
    if (!dragging)
      return false;
    if (orbit)
      orbit(e.mousePos.x - previous.x, e.mousePos.y - previous.y);
    previous = e.mousePos;
    return true;
  }
  bool onMouseButtonUp(const UIMouseEvent &e) override {
    if (e.mouseButton != 1 || !dragging)
      return false;
    dragging = false;
    return true;
  }
  void onCaptureCancelled() override { dragging = false; }
  bool onMouseWheel(const UIMouseWheelEvent &e) override {
    if (zoom)
      zoom(e.deltaY);
    return true;
  }

private:
  bool dragging = false;
  ayt::math::FVector2 previous;
};
class LightShowView final : public IEditorView, public IEditorCommandTarget {
public:
  LightShowView(std::shared_ptr<EditorDslDocument> doc,
                IEditorHostServices &host)
      : document(std::move(doc)), host(host) {
    build();
    refresh();
  }
  ~LightShowView() override {
    detach();
    releaseAssets();
    if (root)
      destroyWidgetTree(root);
  }
  Widget *rootWidget() noexcept override { return root; }
  Widget *releaseRootWidget() noexcept override {
    auto *result = root;
    root = nullptr;
    return result;
  }
  IEditorCommandTarget *commandTarget() noexcept override { return this; }
  bool handlesCommand(const std::string &id) const override {
    return id == "file.save" || id == kEditorDslCompileCommand ||
           id == "edit.undo" || id == "edit.redo" ||
           id.starts_with("lightshow.key.");
  }
  bool canExecuteCommand(const std::string &id) const override {
    if (id == "edit.undo")
      return source && source->canUndo();
    if (id == "edit.redo")
      return source && source->canRedo();
    return handlesCommand(id);
  }
  bool executeCommand(const std::string &id) override {
    if (!canExecuteCommand(id))
      return false;
    if (id == "file.save") {
      std::string error;
      bool ok = document->save(&error);
      status(ok ? "Saved" : error);
      return ok;
    }
    if (id == "edit.undo" || id == "edit.redo") {
      if (id == "edit.undo")
        source->undo();
      else
        source->redo();
      syncSource();
      return true;
    }
    if (id == kEditorDslCompileCommand) {
      refresh(true);
      return bool(compiled);
    }
    if (id.starts_with("lightshow.key."))
      return editKey(id.substr(14));
    return false;
  }
  void onActivated() override { active = true; }
  void onDeactivated() override {
    active = false;
    playing = false;
    timeline->onCaptureCancelled();
    detach();
  }
  void prepareForUiShutdown() override {
    onDeactivated();
    releaseAssets();
  }
  bool visible() const {
    for (auto *w = static_cast<Widget *>(viewport); w; w = w->getParent())
      if (!w->isVisible())
        return false;
    return viewport != nullptr;
  }
  bool mainPreviewBounds(ayt::math::FRectangle &b) const override {
    if (!active || sourceMode || !visible())
      return false;
    b = viewport->getWorldBounds();
    return b.maxX - b.minX >= 32 && b.maxY - b.minY >= 32;
  }
  void tick(float dt) override {
    if (document->revision() != revision) {
      if (source->getText() != wide(document->sourceUtf8()))
        source->setText(wide(document->sourceUtf8()));
      refresh();
    }
    if (playing && compiled) {
      double next = position + std::clamp(double(dt), 0., .25);
      position = compiled->data().loop
                     ? std::fmod(next, compiled->data().duration)
                     : std::min(next, compiled->data().duration);
      if (!compiled->data().loop && position == compiled->data().duration)
        playing = false;
    }
    timeline->position = position;
    timeline->markDirty();
    if (active && !sourceMode && visible()) {
      if (auto *sub = mr::RendererSubSystem::findRegistered();
          sub && sub->isReady())
        sub->setMainPresentationForOwner(
            this,
            [this](mr::Renderer &r, mr::RenderScene &s) { present(r, s); });
    } else
      detach();
  }

private:
  void status(const std::string &value) {
    if (label)
      label->setText(wide(value));
    host.setStatusText(wide(value));
    host.requestRepaint();
  }
  Button *button(HBox *bar, const char *id, const wchar_t *text,
                 std::function<void()> cb, float width = 72) {
    auto *b = new Button();
    b->setId(id);
    b->setText(text);
    b->setAccessibilityLabel(text);
    b->setOnClicked(std::move(cb));
    bar->addWidget(b, width);
    return b;
  }
  TextInput *input(VBox *box, const char *id, const wchar_t *title,
                   const wchar_t *value) {
    auto *row = new HBox();
    auto *caption = new TextLabel();
    caption->setText(title);
    row->addWidget(caption, 100);
    auto *f = new TextInput();
    f->setId(id);
    f->setText(value);
    f->setAccessibilityLabel(title);
    row->addWidget(f);
    box->addWidget(row, 25);
    return f;
  }
  void build();
  void syncSource() {
    document->setSourceUtf8(narrow(source->getText()));
    refresh();
  }
  void refresh(bool force = false);
  bool install(const LightShow &next) {
    std::string bytes, error;
    if (!encodeLightShow(next, bytes, &error)) {
      status(error);
      return false;
    }
    if (bytes.size() > 8u * 1024u * 1024u) {
      status("Show exceeds the 8 MiB Editor document limit");
      return false;
    }
    auto text = wide(bytes);
    if (source->getText() != text) {
      source->selectAll();
      source->insertText(text);
      syncSource();
    }
    return true;
  }
  void seek(double t) {
    if (!compiled || !std::isfinite(t))
      return;
    position = std::clamp(t, 0., compiled->data().duration);
    timeField->setText(number(position));
    presenter.resetPointLightingHistory();
    if (auto *sub = mr::RendererSubSystem::findRegistered();
        sub && sub->isReady())
      sub->renderer().resetVolumetricHistory();
    refreshInspector();
    host.requestRepaint();
  }
  std::vector<uint32_t> selected() const {
    std::vector<uint32_t> ids;
    if (!compiled)
      return ids;
    auto value = narrow(targetField->getText());
    for (const auto &track : compiled->data().fixtures)
      if (value == "all" || value == std::to_string(track.id) ||
          (value.starts_with("group:") && track.group == value.substr(6)))
        ids.push_back(track.id);
    return ids;
  }
  bool editKey(const std::string &op) {
    if (!compiled)
      return false;
    double from = 0, to = 0;
    if (!numeric(timeField, from) || !numeric(destination, to)) {
      status("Invalid key time");
      return false;
    }
    auto value = narrow(targetField->getText());
    LightShow next;
    std::string error;
    auto operation = LightShowKeyEdit::Interpolation;
    auto mode = ShowInterpolation::Linear;
    if (op == "move")
      operation = LightShowKeyEdit::Move;
    else if (op == "copy")
      operation = LightShowKeyEdit::Copy;
    else if (op == "delete")
      operation = LightShowKeyEdit::Remove;
    else if (op == "step")
      mode = ShowInterpolation::Step;
    else if (op == "smooth")
      mode = ShowInterpolation::Smooth;
    else if (op != "linear")
      return false;
    if (!editLightShowKeys(compiled->data(), selected(), value == "formation",
                           from, to, operation, mode, next, &error,
                           value == "fog")) {
      status(error);
      return false;
    }
    return install(next);
  }
  void changeGroup(bool rename) {
    if (!compiled)
      return;
    auto ids = selected();
    if (ids.empty()) {
      status("Select fixtures or a group");
      return;
    }
    auto next = compiled->data();
    auto name = narrow(groupField->getText());
    std::string old;
    for (const auto &track : next.fixtures)
      if (track.id == ids.front())
        old = track.group;
    for (auto &track : next.fixtures)
      if (rename ? track.group == old
                 : std::find(ids.begin(), ids.end(), track.id) != ids.end())
        track.group = name;
    if (install(next)) {
      targetField->setText(wide("group:" + name));
      refreshInspector();
    }
  }
  void refreshInspector();
  bool values(const std::map<std::string, TextInput *> &fields,
              std::map<std::string, double> &out) {
    for (const auto &[id, f] : fields) {
      double v;
      if (!numeric(f, v)) {
        status("Invalid " + id);
        return false;
      }
      out[id] = v;
    }
    return true;
  }
  bool keyTime(double &t) {
    if (!compiled || !numeric(timeField, t) || t < 0 ||
        t > compiled->data().duration) {
      status("Key time is outside the show");
      return false;
    }
    return true;
  }
  void fixtureOperation(int operation);
  void applyQuality();
  void applyFixture();
  void applyTrack();
  void applyFog();
  void formation(bool sphere);
  void route();
  void configurePresenter() {
    mr::EmissivePointLightingSettings s;
    s.maxLights = aggregate ? 16 : 0;
    s.smoothCells = true;
    s.contactDistance = 2;
    presenter.setPointLighting(s, .12f);
    presenter.setFixtureEmitters(true);
    presenter.setFixtureHousings(true);
    presenter.setProfilingEnabled(true);
  }
  void detach() {
    if (auto *sub = mr::RendererSubSystem::findRegistered())
      sub->clearMainPresentationForOwner(this);
  }
  void releaseAssets();
  bool prepareAssets(mr::Renderer &renderer);
  void present(mr::Renderer &renderer, mr::RenderScene &scene);
  struct Asset {
    mr::TextureHandle texture;
    std::shared_ptr<const mr::IesProfile> profile;
  };
  std::shared_ptr<EditorDslDocument> document;
  IEditorHostServices &host;
  VBox *root = nullptr;
  VBox *visual = nullptr;
  TextArea *source = nullptr;
  TextLabel *label = nullptr;
  TextLabel *hud = nullptr;
  ShowViewport *viewport = nullptr;
  ShowTimeline *timeline = nullptr;
  TextInput *targetField = nullptr, *timeField = nullptr,
            *destination = nullptr, *groupField = nullptr, *strobe = nullptr,
            *duty = nullptr, *sweepPan = nullptr, *sweepTilt = nullptr,
            *sweepFrequency = nullptr, *phase = nullptr, *goboPath = nullptr,
            *iesPath = nullptr, *pointCount = nullptr,
            *formationRadius = nullptr, *formationHeight = nullptr;
  std::map<std::string, TextInput *> fixtureFields, fogFields;
  std::unordered_map<std::string, Asset> assets;
  std::shared_ptr<const CompiledLightShow> compiled;
  uint64_t revision = std::numeric_limits<uint64_t>::max();
  bool active = false, sourceMode = false, playing = false, lens = true,
       froxel = false, gpuPoints = false, readback = false, aggregate = true,
       assetsDirty = true, assetsValid = false, temporalReset = true;
  double position = 0, shutter = 0;
  float yaw = 2.5f, pitch = .22f, distance = 35;
  std::set<std::string> changedFixtureFields;
  std::set<TextInput *> changedTrackFields;
  bool updatingInspector = false;
  mr::PerformanceLightBudget lightBudget{64, 12, 4};
  mr::RenderPipelineDesc previewPipeline =
      mr::RenderPipelineDesc::makeDeferred();
  uint32_t pointBudget = 65536;
  double exposureEV = 0;
  TextInput *surfaceBudgetField = nullptr, *volumeBudgetField = nullptr,
            *shadowBudgetField = nullptr, *pointBudgetField = nullptr,
            *exposureField = nullptr;
  LightShowFrame renderedFrame;
  LightShowPresenter presenter;
  mr::Renderer *assetRenderer = nullptr;
  mr::MeshHandle cube;
  mr::MaterialHandle material;
};

void LightShowView::build() {
  root = new VBox();
  root->setId("lightshow_editor");
  auto *bar = new HBox();
  bar->setSpacing(3);
  button(
      bar, "lightshow_save", L"Save", [this] { executeCommand("file.save"); },
      54);
  button(
      bar, "lightshow_play", L"Play/Pause", [this] { playing = !playing; }, 82);
  button(
      bar, "lightshow_stop", L"Stop",
      [this] {
        playing = false;
        seek(0);
      },
      50);
  button(
      bar, "lightshow_source_toggle", L"JSON / Visual",
      [this] {
        sourceMode = !sourceMode;
        visual->setVisible(!sourceMode);
        source->setVisible(sourceMode);
        if (sourceMode)
          detach();
        host.requestRepaint();
      },
      100);
  button(bar, "lightshow_compile", L"Compile", [this] { refresh(true); });
  label = new TextLabel();
  bar->addWidget(label);
  root->addWidget(bar, 27);
  visual = new VBox();
  auto *body = new HBox();
  auto *left = new VBox();
  auto *quality = new HBox();
  button(quality, "lightshow_lens", L"Lens", [this] { lens = !lens; });
  button(quality, "lightshow_froxel", L"Froxel", [this] { froxel = !froxel; });
  button(
      quality, "lightshow_gpu", L"GPU points",
      [this] {
        gpuPoints = !gpuPoints;
        if (!gpuPoints)
          readback = false;
      },
      90);
  left->addWidget(quality, 26);
  auto *quality2 = new HBox();
  button(quality2, "lightshow_readback", L"Count", [this] {
    readback = !readback;
    if (readback)
      gpuPoints = true;
  });
  button(quality2, "lightshow_shutter", L"Shutter", [this] {
    shutter = shutter == 0 ? 1. / 120 : shutter < .01 ? 1. / 60 : 0;
  });
  button(
      quality2, "lightshow_aggregate", L"Point light",
      [this] {
        aggregate = !aggregate;
        configurePresenter();
      },
      88);
  left->addWidget(quality2, 26);
  viewport = new ShowViewport();
  viewport->setId("lightshow_viewport");
  viewport->orbit = [this](float x, float y) {
    yaw += x * .008f;
    pitch = std::clamp(pitch + y * .008f, -1.3f, 1.3f);
  };
  viewport->zoom = [this](float delta) {
    distance = std::clamp(distance * std::exp(-delta * .08f), 2.f, 3000.f);
  };
  left->addWidget(viewport);
  hud = new TextLabel();
  hud->setId("lightshow_budget_hud");
  hud->setFontSize(10);
  left->addWidget(hud, 60);
  body->addWidget(left);
  auto *inspector = new VBox();
  auto *scroll = new ScrollView();
  scroll->setContent(inspector);
  body->addWidget(scroll, 270);
  visual->addWidget(body);
  surfaceBudgetField =
      input(inspector, "lightshow_surface_budget", L"Surface cap", L"64");
  volumeBudgetField =
      input(inspector, "lightshow_volume_budget", L"Volume cap", L"12");
  shadowBudgetField =
      input(inspector, "lightshow_shadow_budget", L"Shadow cap", L"4");
  pointBudgetField =
      input(inspector, "lightshow_point_budget", L"Point cap", L"65536");
  exposureField = input(inspector, "lightshow_exposure", L"Exposure EV", L"0");
  auto *budgetBar = new HBox();
  button(
      budgetBar, "lightshow_quality_apply", L"Apply preview",
      [this] { applyQuality(); }, 120);
  inspector->addWidget(budgetBar, 26);
  targetField = input(inspector, "lightshow_target", L"Target", L"all");
  timeField = input(inspector, "lightshow_from", L"Key / seek s", L"0");
  destination = input(inspector, "lightshow_to", L"Destination s", L"1");
  auto *transport = new HBox();
  button(transport, "lightshow_seek", L"Seek", [this] {
    double t;
    if (numeric(timeField, t))
      seek(t);
    else
      status("Invalid seek time");
  });
  button(transport, "lightshow_select", L"Select",
         [this] { refreshInspector(); });
  inspector->addWidget(transport, 26);
  groupField = input(inspector, "lightshow_group", L"Group", L"stage");
  auto *fixtureBar = new HBox();
  button(fixtureBar, "lightshow_fixture_add", L"Add",
         [this] { fixtureOperation(0); });
  button(fixtureBar, "lightshow_fixture_clone", L"Clone",
         [this] { fixtureOperation(1); });
  button(fixtureBar, "lightshow_fixture_remove", L"Remove",
         [this] { fixtureOperation(2); });
  inspector->addWidget(fixtureBar, 26);
  auto *groups = new HBox();
  button(groups, "lightshow_group_assign", L"Assign",
         [this] { changeGroup(false); });
  button(groups, "lightshow_group_rename", L"Rename",
         [this] { changeGroup(true); });
  inspector->addWidget(groups, 26);
  const std::pair<const char *, const wchar_t *> fields[] = {
      {"intensity", L"Intensity"},
      {"pan", L"Pan rad"},
      {"tilt", L"Tilt rad"},
      {"range", L"Range m"},
      {"inner", L"Core rad"},
      {"outer", L"Field rad"},
      {"aperture", L"Aperture m"},
      {"iris", L"Iris"},
      {"focus", L"Focus m"},
      {"prism", L"Prism count"},
      {"prismAngle", L"Prism rad"},
      {"gobo", L"Gobo 0..5"},
      {"red", L"Red"},
      {"green", L"Green"},
      {"blue", L"Blue"},
      {"x", L"Position X"},
      {"y", L"Position Y"},
      {"z", L"Position Z"},
      {"shadow", L"Shadow 0/1"},
      {"areaShadow", L"PCSS 0/1"},
      {"unit", L"Unit 0/1/2"},
      {"volume", L"Volume gain"},
      {"goboFocus", L"Gobo blur"},
      {"prismRotation", L"Prism rot"},
      {"shadowSoftness", L"PCF texels"},
      {"shadowBias", L"PCSS bias m"}};
  for (const auto &[id, title] : fields)
    fixtureFields[id] =
        input(inspector, (std::string("lightshow_") + id).c_str(), title, L"0");
  auto *apply = new HBox();
  button(
      apply, "lightshow_fixture_apply", L"Key fixture",
      [this] { applyFixture(); }, 100);
  inspector->addWidget(apply, 26);
  strobe = input(inspector, "lightshow_strobe", L"Strobe Hz", L"0");
  duty = input(inspector, "lightshow_duty", L"Duty", L"0.5");
  sweepPan = input(inspector, "lightshow_sweep_pan", L"Sweep pan", L"0");
  sweepTilt = input(inspector, "lightshow_sweep_tilt", L"Sweep tilt", L"0");
  sweepFrequency = input(inspector, "lightshow_sweep_hz", L"Sweep Hz", L"0");
  phase = input(inspector, "lightshow_phase", L"Phase", L"0");
  goboPath = input(inspector, "lightshow_gobo_path", L"Gobo path", L"");
  iesPath = input(inspector, "lightshow_ies_path", L"IES path", L"");
  auto *trackbar = new HBox();
  button(
      trackbar, "lightshow_track_apply", L"Apply track",
      [this] { applyTrack(); }, 110);
  inspector->addWidget(trackbar, 26);
  const std::pair<const char *, const wchar_t *> fogs[] = {
      {"enabled", L"Fog 0/1"},       {"local", L"Local 0/1"},
      {"density", L"Density /m"},    {"scattering", L"Albedo"},
      {"anisotropy", L"Anisotropy"}, {"maxDistance", L"Fog far m"},
      {"centerX", L"Fog center X"},  {"centerY", L"Fog center Y"},
      {"centerZ", L"Fog center Z"},  {"extentX", L"Fog extent X"},
      {"extentY", L"Fog extent Y"},  {"extentZ", L"Fog extent Z"},
      {"tintR", L"Fog red"},         {"tintG", L"Fog green"},
      {"tintB", L"Fog blue"},        {"noise", L"Noise strength"},
      {"noiseScale", L"Noise scale"}};
  for (const auto &[id, title] : fogs)
    fogFields[id] = input(
        inspector, (std::string("lightshow_fog_") + id).c_str(), title, L"0");
  auto *fogbar = new HBox();
  button(fogbar, "lightshow_fog_apply", L"Key fog", [this] { applyFog(); });
  inspector->addWidget(fogbar, 26);
  pointCount =
      input(inspector, "lightshow_formation_count", L"Point count", L"1000");
  formationRadius =
      input(inspector, "lightshow_formation_radius", L"Radius m", L"8");
  formationHeight =
      input(inspector, "lightshow_formation_height", L"Height m", L"5");
  auto *formations = new HBox();
  button(formations, "lightshow_circle", L"Circle",
         [this] { formation(false); });
  button(formations, "lightshow_sphere", L"Sphere",
         [this] { formation(true); });
  button(formations, "lightshow_route", L"Route", [this] { route(); });
  inspector->addWidget(formations, 26);
  auto *keys = new HBox();
  keys->setSpacing(2);
  for (const auto &[id, title] :
       std::initializer_list<std::pair<const char *, const wchar_t *>>{
           {"move", L"Move"},
           {"copy", L"Copy"},
           {"delete", L"Delete"},
           {"step", L"Step"},
           {"linear", L"Linear"},
           {"smooth", L"Smooth"}})
    button(
        keys, (std::string("lightshow_key_") + id).c_str(), title,
        [this, id] { editKey(id); }, 65);
  visual->addWidget(keys, 26);
  timeline = new ShowTimeline();
  timeline->setId("lightshow_timeline");
  timeline->seek = [this](double t) { seek(t); };
  timeline->select = [this](const auto &id, double t) {
    targetField->setText(wide(id));
    timeField->setText(number(t));
    refreshInspector();
  };
  timeline->move = [this](const auto &id, double from, double to) {
    targetField->setText(wide(id));
    timeField->setText(number(from));
    destination->setText(number(to));
    if (editKey("move"))
      seek(to);
  };
  visual->addWidget(timeline, 190);
  root->addWidget(visual);
  source = new TextArea();
  source->setId("lightshow_source");
  source->setMaxLength(8u * 1024u * 1024u);
  source->setWordWrap(false);
  source->setLineNumbersVisible(true);
  source->setText(wide(document->sourceUtf8()));
  source->setOnTextChanged([this](const auto &) { syncSource(); });
  source->setVisible(false);
  root->addWidget(source);
  for (const auto &[id, field] : fixtureFields)
    field->setOnTextChanged([this, id](const auto &) {
      if (!updatingInspector)
        changedFixtureFields.insert(id);
    });
  for (auto *field : {strobe, duty, sweepPan, sweepTilt, sweepFrequency, phase,
                      goboPath, iesPath})
    field->setOnTextChanged([this, field](const auto &) {
      if (!updatingInspector)
        changedTrackFields.insert(field);
    });
  configurePresenter();
}
void LightShowView::refresh(bool force) {
  if (!force && document->revision() == revision)
    return;
  revision = document->revision();
  timeline->onCaptureCancelled();
  LightShow decoded;
  std::string error;
  if (!decodeLightShow(document->sourceUtf8(), decoded, &error) ||
      (compiled = CompiledLightShow::compile(std::move(decoded), &error)) ==
          nullptr) {
    compiled.reset();
    playing = false;
    timeline->lanes.clear();
    status(error);
    return;
  }
  position = std::clamp(position, 0., compiled->data().duration);
  assetsDirty = true;
  temporalReset = true;
  presenter.resetPointLightingHistory();
  const auto &show = compiled->data();
  timeline->duration = show.duration;
  timeline->lanes.clear();
  std::map<std::string, std::vector<double>> groups;
  for (const auto &track : show.fixtures) {
    auto &times = groups[track.group];
    for (const auto &key : track.keys)
      times.push_back(key.time);
  }
  for (auto &[group, times] : groups) {
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    timeline->lanes.push_back(
        {"group:" + group, "Group: " + group, std::move(times)});
  }
  for (const auto &track : show.fixtures) {
    Lane lane{std::to_string(track.id),
              "  " + std::to_string(track.id) + " / " + track.group,
              {}};
    for (const auto &key : track.keys)
      lane.times.push_back(key.time);
    timeline->lanes.push_back(std::move(lane));
  }
  Lane a{"formation", "Formation", {}}, b{"fog", "Fog", {}};
  for (const auto &key : show.formations)
    a.times.push_back(key.time);
  for (const auto &key : show.fog.keys)
    b.times.push_back(key.time);
  timeline->lanes.push_back(std::move(a));
  timeline->lanes.push_back(std::move(b));
  refreshInspector();
  status("Ready: " + std::to_string(show.fixtures.size()) +
         " fixtures; right-drag orbit / wheel zoom");
}
void LightShowView::refreshInspector() {
  if (!compiled)
    return;
  updatingInspector = true;
  LightShowFrame frame;
  compiled->evaluate(position, frame, false);
  auto ids = selected();
  auto set = [&](const char *id, double v) {
    fixtureFields.at(id)->setText(number(v));
  };
  for (const auto &fixture : frame.fixtures)
    if (!ids.empty() && fixture.id == ids.front()) {
      const auto &v = fixture.value;
      set("intensity", v.intensity);
      set("pan", v.pan);
      set("tilt", v.tilt);
      set("range", v.range);
      set("inner", v.innerAngle);
      set("outer", v.outerAngle);
      set("aperture", v.apertureRadius);
      set("iris", v.iris);
      set("focus", v.focusDistance);
      set("prism", v.prismCount);
      set("prismAngle", v.prismAngle);
      set("gobo", int(v.gobo));
      set("red", v.color[0]);
      set("green", v.color[1]);
      set("blue", v.color[2]);
      set("x", v.position[0]);
      set("y", v.position[1]);
      set("z", v.position[2]);
      set("shadow", v.shadow);
      set("areaShadow", v.areaShadow);
      set("unit", int(v.intensityUnit));
      set("volume", v.volumetric);
      set("goboFocus", v.goboFocus);
      set("prismRotation", v.prismRotation);
      set("shadowSoftness", v.shadowSoftness);
      set("shadowBias", v.areaShadowBias);
      for (const auto &track : compiled->data().fixtures)
        if (track.id == fixture.id) {
          groupField->setText(wide(track.group));
          strobe->setText(number(track.strobeFrequency));
          duty->setText(number(track.strobeDuty));
          sweepPan->setText(number(track.sweepPan));
          sweepTilt->setText(number(track.sweepTilt));
          sweepFrequency->setText(number(track.sweepFrequency));
          phase->setText(number(track.phase));
          goboPath->setText(wide(track.goboTexturePath));
          iesPath->setText(wide(track.iesProfilePath));
        }
      break;
    }
  const auto &f = frame.fog.value;
  auto fog = [&](const char *id, double x) {
    fogFields.at(id)->setText(number(x));
  };
  fog("enabled", f.enabled);
  fog("local", f.local);
  fog("density", f.density);
  fog("scattering", f.scattering);
  fog("anisotropy", f.anisotropy);
  fog("maxDistance", f.maxDistance);
  fog("noise", f.noiseStrength);
  fog("noiseScale", f.noiseScale);
  for (int i = 0; i < 3; ++i) {
    fog(std::array{"centerX", "centerY", "centerZ"}[i], f.center[i]);
    fog(std::array{"extentX", "extentY", "extentZ"}[i], f.halfExtent[i]);
    fog(std::array{"tintR", "tintG", "tintB"}[i], f.tint[i]);
  }
  updatingInspector = false;
  changedFixtureFields.clear();
  changedTrackFields.clear();
  timeline->target = narrow(targetField->getText());
  timeline->markDirty();
}

void LightShowView::fixtureOperation(int operation) {
  if (!compiled)
    return;
  auto next = compiled->data();
  auto ids = selected();
  if (operation == 2) {
    if (ids.empty()) {
      status("Select fixtures to remove");
      return;
    }
    std::erase_if(next.fixtures, [&](const auto &track) {
      return std::find(ids.begin(), ids.end(), track.id) != ids.end();
    });
    install(next);
    return;
  }
  if (next.fixtures.size() >= 256) {
    status("Fixture cap is 256");
    return;
  }
  FixtureTrack track;
  if (operation == 1) {
    if (ids.empty()) {
      status("Select a fixture to clone");
      return;
    }
    track = *std::find_if(next.fixtures.begin(), next.fixtures.end(),
                          [&](const auto &t) { return t.id == ids.front(); });
  } else {
    track.group = narrow(groupField->getText());
    FixtureState value;
    value.position = {0, 8, 0};
    track.keys.push_back({position, value});
  }
  uint32_t id = 1;
  while (std::any_of(next.fixtures.begin(), next.fixtures.end(),
                     [&](const auto &t) { return t.id == id; }))
    ++id;
  track.id = id;
  next.fixtures.push_back(std::move(track));
  if (install(next)) {
    targetField->setText(number(id));
    refreshInspector();
  }
}
void LightShowView::applyQuality() {
  double surface, volume, shadow, points, ev;
  if (!numeric(surfaceBudgetField, surface) ||
      !numeric(volumeBudgetField, volume) ||
      !numeric(shadowBudgetField, shadow) ||
      !numeric(pointBudgetField, points) || !numeric(exposureField, ev) ||
      surface < 0 || surface > 256 || volume < 0 || volume > 64 || shadow < 0 ||
      shadow > 8 || points < 0 || points > 65536 || ev < -16 || ev > 16 ||
      surface != std::floor(surface) || volume != std::floor(volume) ||
      shadow != std::floor(shadow) || points != std::floor(points)) {
    status("Invalid preview budget or exposure");
    return;
  }
  lightBudget.surfaceLights = uint16_t(surface);
  lightBudget.volumeLights = uint8_t(volume);
  lightBudget.shadowLights = uint8_t(shadow);
  pointBudget = uint32_t(points);
  exposureEV = ev;
  temporalReset = true;
  presenter.resetPointLightingHistory();
  status("Preview quality applied (session only)");
}
void LightShowView::applyFixture() {
  double t;
  std::map<std::string, double> v;
  if (!keyTime(t) || !values(fixtureFields, v))
    return;
  auto ids = selected();
  if (ids.empty()) {
    status("Select fixture(s)");
    return;
  }
  for (auto id : {"prism", "gobo", "shadow", "areaShadow", "unit"})
    if (v[id] != std::floor(v[id])) {
      status("Integer value required: " + std::string(id));
      return;
    }
  if (v["prism"] < 1 || v["prism"] > 8 || v["gobo"] < 0 || v["gobo"] > 5 ||
      v["unit"] < 0 || v["unit"] > 2 || v["shadow"] < 0 || v["shadow"] > 1 ||
      v["areaShadow"] < 0 || v["areaShadow"] > 1) {
    status("Enum or boolean out of range");
    return;
  }
  LightShowFrame frame;
  compiled->evaluate(t, frame, false);
  auto next = compiled->data();
  for (auto &track : next.fixtures)
    if (std::find(ids.begin(), ids.end(), track.id) != ids.end()) {
      auto value = std::find_if(frame.fixtures.begin(), frame.fixtures.end(),
                                [&](const auto &a) { return a.id == track.id; })
                       ->value;
      if (changedFixtureFields.contains("intensity"))
        value.intensity = float(v["intensity"]);
      if (changedFixtureFields.contains("pan"))
        value.pan = float(v["pan"]);
      if (changedFixtureFields.contains("tilt"))
        value.tilt = float(v["tilt"]);
      if (changedFixtureFields.contains("range"))
        value.range = float(v["range"]);
      if (changedFixtureFields.contains("inner"))
        value.innerAngle = float(v["inner"]);
      if (changedFixtureFields.contains("outer"))
        value.outerAngle = float(v["outer"]);
      if (changedFixtureFields.contains("aperture"))
        value.apertureRadius = float(v["aperture"]);
      if (changedFixtureFields.contains("iris"))
        value.iris = float(v["iris"]);
      if (changedFixtureFields.contains("focus"))
        value.focusDistance = float(v["focus"]);
      if (changedFixtureFields.contains("prism"))
        value.prismCount = uint8_t(v["prism"]);
      if (changedFixtureFields.contains("prismAngle"))
        value.prismAngle = float(v["prismAngle"]);
      if (changedFixtureFields.contains("gobo"))
        value.gobo = ShowGobo(int(v["gobo"]));
      for (int i = 0; i < 3; ++i) {
        auto key = std::array{"red", "green", "blue"}[i];
        if (changedFixtureFields.contains(key))
          value.color[i] = float(v[key]);
      }
      for (int i = 0; i < 3; ++i) {
        auto key = std::array{"x", "y", "z"}[i];
        if (changedFixtureFields.contains(key))
          value.position[i] = float(v[key]);
      }
      if (changedFixtureFields.contains("shadow"))
        value.shadow = v["shadow"] != 0;
      if (changedFixtureFields.contains("areaShadow"))
        value.areaShadow = v["areaShadow"] != 0;
      if (changedFixtureFields.contains("unit"))
        value.intensityUnit = ShowIntensityUnit(int(v["unit"]));
      if (changedFixtureFields.contains("volume"))
        value.volumetric = float(v["volume"]);
      if (changedFixtureFields.contains("goboFocus"))
        value.goboFocus = float(v["goboFocus"]);
      if (changedFixtureFields.contains("prismRotation"))
        value.prismRotation = float(v["prismRotation"]);
      if (changedFixtureFields.contains("shadowSoftness"))
        value.shadowSoftness = float(v["shadowSoftness"]);
      if (changedFixtureFields.contains("shadowBias"))
        value.areaShadowBias = float(v["shadowBias"]);
      auto mode = ShowInterpolation::Linear;
      for (const auto &key : track.keys)
        if (std::abs(key.time - t) <= keyTolerance)
          mode = key.interpolation;
      putKey(track.keys, FixtureKey{t, value, mode});
    }
  install(next);
}
void LightShowView::applyTrack() {
  if (!compiled)
    return;
  double hz, d, sp, st, sf, ph;
  if (!numeric(strobe, hz) || !numeric(duty, d) || !numeric(sweepPan, sp) ||
      !numeric(sweepTilt, st) || !numeric(sweepFrequency, sf) ||
      !numeric(phase, ph)) {
    status("Invalid track value");
    return;
  }
  auto ids = selected();
  if (ids.empty()) {
    status("Select fixture(s)");
    return;
  }
  auto next = compiled->data();
  for (auto &track : next.fixtures)
    if (std::find(ids.begin(), ids.end(), track.id) != ids.end()) {
      if (changedTrackFields.contains(strobe))
        track.strobeFrequency = float(hz);
      if (changedTrackFields.contains(duty))
        track.strobeDuty = float(d);
      if (changedTrackFields.contains(sweepPan))
        track.sweepPan = float(sp);
      if (changedTrackFields.contains(sweepTilt))
        track.sweepTilt = float(st);
      if (changedTrackFields.contains(sweepFrequency))
        track.sweepFrequency = float(sf);
      if (changedTrackFields.contains(phase))
        track.phase = float(ph);
      if (changedTrackFields.contains(goboPath))
        track.goboTexturePath = narrow(goboPath->getText());
      if (changedTrackFields.contains(iesPath))
        track.iesProfilePath = narrow(iesPath->getText());
    }
  install(next);
}
void LightShowView::applyFog() {
  double t;
  std::map<std::string, double> v;
  if (!keyTime(t) || !values(fogFields, v))
    return;
  if ((v["enabled"] != 0 && v["enabled"] != 1) ||
      (v["local"] != 0 && v["local"] != 1)) {
    status("Fog flags must be 0 or 1");
    return;
  }
  auto next = compiled->data();
  ShowFogState f;
  f.enabled = v["enabled"] != 0;
  f.local = v["local"] != 0;
  f.density = float(v["density"]);
  f.scattering = float(v["scattering"]);
  f.anisotropy = float(v["anisotropy"]);
  f.maxDistance = float(v["maxDistance"]);
  f.noiseStrength = float(v["noise"]);
  f.noiseScale = float(v["noiseScale"]);
  for (int i = 0; i < 3; ++i) {
    f.center[i] = float(v[std::array{"centerX", "centerY", "centerZ"}[i]]);
    f.halfExtent[i] = float(v[std::array{"extentX", "extentY", "extentZ"}[i]]);
    f.tint[i] = float(v[std::array{"tintR", "tintG", "tintB"}[i]]);
  }
  auto mode = ShowInterpolation::Linear;
  for (const auto &key : next.fog.keys)
    if (std::abs(key.time - t) <= keyTolerance)
      mode = key.interpolation;
  putKey(next.fog.keys, ShowFogKey{t, f, mode});
  install(next);
}
void LightShowView::formation(bool sphere) {
  double t, n, r, h;
  if (!keyTime(t) || !numeric(pointCount, n) || !numeric(formationRadius, r) ||
      !numeric(formationHeight, h) || n < 1 || n > 65536 ||
      n != std::floor(n) || r <= 0 || r > 10000) {
    status("Invalid formation count/radius/height");
    return;
  }
  auto next = compiled->data();
  LightShowFrame sample;
  compiled->evaluate(t, sample, false);
  if (!sample.points.empty() && sample.points.size() != size_t(n)) {
    status("Existing formations require the same stable point count");
    return;
  }
  FormationKey key;
  key.time = t;
  key.points = std::move(sample.points);
  if (key.points.empty()) {
    key.points.resize(size_t(n));
    for (size_t i = 0; i < key.points.size(); ++i)
      key.points[i].id = uint32_t(i + 1);
  }
  constexpr double pi = 3.14159265358979323846;
  for (size_t i = 0; i < key.points.size(); ++i) {
    double angle = sphere ? i * pi * (3 - std::sqrt(5.)) : i * 2 * pi / n;
    double y = sphere ? 1 - 2 * (i + .5) / n : 0;
    double ring = std::sqrt(std::max(0., 1 - y * y));
    key.points[i].position = {float(r * ring * std::cos(angle)),
                              float(h + r * y),
                              float(r * ring * std::sin(angle))};
  }
  putKey(next.formations, std::move(key));
  install(next);
}
void LightShowView::route() {
  if (!compiled || compiled->data().formations.size() < 2) {
    status("Route requires two formation keys");
    return;
  }
  double t;
  if (!keyTime(t))
    return;
  const auto &keys = compiled->data().formations;
  auto it =
      std::upper_bound(keys.begin(), keys.end(), t,
                       [](double x, const auto &k) { return x < k.time; });
  size_t index = it == keys.begin() ? 0 : size_t(it - keys.begin() - 1);
  index = std::min(index, keys.size() - 2);
  LightShow next;
  std::string error;
  if (!planFormationTransition(compiled->data(), index, {}, next, &error)) {
    status(error);
    return;
  }
  install(next);
}
void LightShowView::releaseAssets() {
  if (!assetRenderer)
    return;
  auto *sub = mr::RendererSubSystem::findRegistered();
  if (!sub || &sub->renderer() != assetRenderer || !sub->isReady()) {
    assetRenderer = nullptr;
    assets.clear();
    return;
  }
  for (auto &[path, asset] : assets)
    assetRenderer->destroyTexture(asset.texture);
  assets.clear();
  presenter.releaseResources(*assetRenderer);
  assetRenderer->destroyMesh(cube);
  assetRenderer->destroyMaterial(material);
  assetRenderer = nullptr;
}
bool LightShowView::prepareAssets(mr::Renderer &renderer) {
  if (!assetsDirty)
    return assetsValid;
  assetsDirty = false;
  assetsValid = false;
  presenter.clearGoboTextures();
  presenter.clearIesProfiles();
  for (auto &[path, asset] : assets)
    renderer.destroyTexture(asset.texture);
  assets.clear();
  auto parent = std::filesystem::u8path(document->absolutePath()).parent_path();
  std::string error;
  for (const auto &track : compiled->data().fixtures) {
    bool gobo =
        std::any_of(track.keys.begin(), track.keys.end(), [](const auto &k) {
          return k.value.gobo == ShowGobo::Texture;
        });
    for (bool ies : {false, true}) {
      const auto &path = ies ? track.iesProfilePath : track.goboTexturePath;
      if (path.empty() || (!ies && !gobo))
        continue;
      auto resolved =
          (parent / std::filesystem::u8path(path)).lexically_normal();
      auto utf = resolved.u8string();
      std::string key(reinterpret_cast<const char *>(utf.data()), utf.size());
      auto it = assets.find(key + (ies ? "#ies" : "#image"));
      if (it == assets.end()) {
        Asset asset;
        if (ies) {
          std::error_code ec;
          auto size = std::filesystem::file_size(resolved, ec);
          if (ec || size > 4 * 1024 * 1024) {
            status("Missing or oversized IES: " + key);
            return false;
          }
          std::ifstream input(resolved, std::ios::binary);
          std::string bytes(size, '\0');
          if (!input.read(bytes.data(), std::streamsize(size))) {
            status("Cannot read IES: " + key);
            return false;
          }
          auto profile = std::make_shared<mr::IesProfile>();
          if (!mr::IesProfile::parse(bytes, *profile, &error)) {
            status(error);
            return false;
          }
          asset.profile = profile;
          asset.texture = mr::createIesTexture(renderer, *profile);
        } else
          asset.texture = renderer.createTextureFromFile(key);
        if (!asset.texture.isValid()) {
          status("Cannot upload asset: " + key);
          return false;
        }
        it = assets.emplace(key + (ies ? "#ies" : "#image"), std::move(asset))
                 .first;
      }
      if (ies)
        presenter.bindIesProfile(track.id, it->second.profile,
                                 it->second.texture);
      else
        presenter.bindGoboTexture(track.id, it->second.texture);
    }
  }
  assetsValid = true;
  return true;
}
void LightShowView::present(mr::Renderer &renderer, mr::RenderScene &scene) {
  if (temporalReset) {
    renderer.resetVolumetricHistory();
    temporalReset = false;
  }
  if (assetRenderer != &renderer) {
    releaseAssets();
    assetRenderer = &renderer;
    assetsDirty = true;
  }
  if (renderer.pipelineDesc().path != previewPipeline.path ||
      renderer.pipelineDesc().passes != previewPipeline.passes)
    renderer.configurePipeline(previewPipeline);
  renderer.setSceneLights(nullptr);
  renderer.setSkySource(nullptr);
  renderer.setSkySourceCube({});
  renderer.setAmbientStrength(.06f);
  renderer.setDepthHazeEnabled(false);
  renderer.setSsaoEnabled(false);
  renderer.setGBufferDebugEnabled(false);
  renderer.setDeferredDecalsEnabled(false);
  renderer.setColorGradingEnabled(false);
  renderer.setSsrSettings({});
  renderer.setScreenSpaceIndirectSettings({});
  if (!renderer.fxaaEnabled())
    renderer.setFxaaEnabled(true);
  renderer.setWireframeEnabled(false);
  renderer.setEditorGrid2DState({});
  renderer.setEditorTransformGizmoState({});
  renderer.setViewportOrientationAxisEnabled(false);
  renderer.setEditorDebugLines({});
  renderer.setEditorSelectionOutline2DState({});
  renderer.setPostProcessBloomStrength(.2f);
  renderer.setPostProcessExposure(float(std::exp2(exposureEV)));
  renderer.setAutoExposureEnabled(false);
  renderer.setPostProcessTonemapMode(mr::Renderer::TonemapMode::ACES);
  mr::LensEffectsSettings effects;
  effects.enabled = lens;
  renderer.setLensEffects(effects);
  renderer.setDebugOverlayEnabled(true);
  renderer.setDebugOverlaySuppressed(true);
  mr::VolumetricSettings fog;
  fog.enabled = true;
  fog.steps = 24;
  fog.downsample = 2;
  fog.froxel = froxel;
  fog.froxelHistory = froxel;
  renderer.setVolumetricSettings(fog);
  renderer.setSimulationTimeSeconds(float(position));
  auto *sub = mr::RendererSubSystem::findRegistered();
  auto width = uint16_t(sub ? sub->viewportWidth() : 640),
       height = uint16_t(sub ? sub->viewportHeight() : 480);
  ayt::math::FVector3 eye{distance * std::cos(pitch) * std::sin(yaw),
                          5 + distance * std::sin(pitch),
                          distance * std::cos(pitch) * std::cos(yaw)};
  auto view = ayt::math::lh::lookAt(eye, {0, 5, 0}, {0, 1, 0});
  auto projection = ayt::math::lh::perspective(
      .872665f, float(width) / std::max(1.f, float(height)), .1f, 5000.f);
  renderer.setMainCamera(view, projection, eye);
  if (!compiled || !prepareAssets(renderer)) {
    renderer.setPerformanceLights({});
    renderer.setVolumetricDensityField({});
    return;
  }
  if (!cube.isValid()) {
    cube = renderer.createTexturedUnitCube();
    material = renderer.createMaterialFromPhoskia(mr::kPbrPhoskiaSource,
                                                  "editor_lightshow_stage");
    renderer.setMaterialColor(material, "baseColor", .3f, .32f, .36f, 1);
    renderer.setMaterialFloat(material, "roughness", .65f);
  }
  scene.add(cube, material,
            ayt::math::translate(0.f, -.2f, 0.f) *
                ayt::math::scale(30.f, .4f, 30.f));
  auto lights = renderer.performanceLightStats();
  auto timings = renderer.getFrameStats();
  compiled->evaluateShutter(position, shutter, renderedFrame);
  mr::EmissivePointSettings points;
  points.maxPoints = pointBudget;
  points.preferGpuCulling = gpuPoints;
  points.gpuDiagnostics = readback;
  std::string error;
  if (!presenter.append(renderer, scene, renderedFrame, view, projection, width,
                        height, lightBudget, points, &error)) {
    scene.clear();
    renderer.setPerformanceLights({});
    status(error);
    return;
  }
  auto counts = presenter.pointStats();
  auto aggregates = presenter.pointLightingStats();
  auto cpu = presenter.frameStats();
  std::ostringstream text;
  text.precision(4);
  text << "t " << position << " / " << compiled->data().duration << " | cap "
       << lightBudget.surfaceLights << " / "
       << unsigned(lightBudget.volumeLights) << " / "
       << unsigned(lightBudget.shadowLights) << " | last draws "
       << lights.surfaceDraws << " / " << lights.volumeDraws << " / "
       << lights.shadowLights << " skipped " << lights.surfaceBudgetSkipped
       << " / " << lights.volumeBudgetSkipped << "\nPoints ";
  if (counts.countsExact)
    text << counts.visible << " visible / " << counts.source;
  else if (counts.gpuSampleValid)
    text << "GPU sample#" << counts.gpuSampleSerial << " "
         << counts.gpuSampleVisible << " / " << counts.gpuSampleSource;
  else
    text << "GPU source " << counts.source << " (visible pending)";
  text << " | lens " << lens << " froxel " << froxel << " GPU " << gpuPoints
       << " | aggregate " << aggregates.selectedLights << " skipped "
       << aggregates.budgetSkipped << " | CPU prepare " << cpu.totalMs
       << " ms GPU compute " << timings.pointGpuTimeMs << " ms | frame CPU "
       << timings.renderCpuTimeMs << " GPU " << timings.gpuFrameTimeMs
       << " ms (delayed) | shutter " << shutter * 1000 << " ms";
  hud->setText(wide(text.str()));
}
} // namespace
std::unique_ptr<IEditorView>
makeEditorLightShowView(std::shared_ptr<EditorDslDocument> document,
                        IEditorHostServices &host) {
  return std::make_unique<LightShowView>(std::move(document), host);
}
} // namespace ayt::editor
