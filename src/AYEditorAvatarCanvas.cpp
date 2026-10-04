#include "AYEditorAvatarCanvas.h"
#include <AYMath/MathUtils.h>
#include <AYUI/UIKeyCode.h>
#include <AYUI/UIManager.h>
#include <AYUI/UnicodeText.h>
#include <algorithm>
#include <cmath>
namespace ayt::editor {
namespace {
const char *shader = R"(
material EditorAvatarPreview {
 property baseColor = vec4(0.8, 0.5, 0.3, 1.0)
 vertex {
  in pos : position
  in nrm : normal
  in uv : texcoord
  out worldNormal : normal = (modelMatrix * vec4(nrm, 0.0)).xyz
  return modelViewProjection * vec4(pos, 1.0)
 }
 fragment {
  in worldNormal : normal
  let light = 0.3 + 0.7 * max(dot(normalize(worldNormal), normalize(vec3(-0.4, 0.6, 0.8))), 0.0)
  return vec4(baseColor.rgb * light, baseColor.a)
 }
}
)";
device::KeyCode navigationKey(int key) {
  switch (key) {
  case ui::UIKey_W:
    return device::KeyCode::W;
  case ui::UIKey_S:
    return device::KeyCode::S;
  case ui::UIKey_A:
    return device::KeyCode::A;
  case ui::UIKey_D:
    return device::KeyCode::D;
  case ui::UIKey_Q:
    return device::KeyCode::Q;
  case ui::UIKey_E:
    return device::KeyCode::E;
  default:
    return device::KeyCode::Unknown;
  }
}
} // namespace
EditorAvatarCanvas::EditorAvatarCanvas(
    std::shared_ptr<EditorAvatarDocument> document, ui::UIManager *manager)
    : _document(std::move(document)), _ui(manager) {
  setId("avatar_viewport");
  setDisplayListPolicy(ui::DisplayListPolicy::Immediate);
  focusAvatar();
}
EditorAvatarCanvas::~EditorAvatarCanvas() { shutdown(); }
void EditorAvatarCanvas::shutdown() {
  cancelNavigation();
  _presenter.reset();
  if (_backend && _backend->isInitialized() && _target.isValid())
    _backend->releaseRenderTarget(_target);
  if (_renderer && _renderer->isInitialized())
    for (auto &[name, handle] : _materials)
      _renderer->destroyMaterial(handle);
  _materials.clear();
  _target = {};
  _backend = nullptr;
  _renderer = nullptr;
  _uploaded = UINT64_MAX;
  _uploadedLod = UINT32_MAX;
  _width = _height = 0;
}
void EditorAvatarCanvas::cancelNavigation() {
  _camera.threeD().endLook();
  _keys.releaseAll();
  markDirty();
}
void EditorAvatarCanvas::updateViewport() {
  const auto bounds = getWorldBounds();
  _camera.setViewport(uint32_t(std::max(1.f, bounds.maxX - bounds.minX)),
                      uint32_t(std::max(1.f, bounds.maxY - bounds.minY)));
}
bool EditorAvatarCanvas::assetBounds(math::FVector3 &center,
                                     math::FVector3 &extent) const {
  if (!_document->creator())
    return false;
  auto canonical =
      _document->catalog()->canonical(_document->creator()->preset().canonical);
  if (!canonical || canonical->lods.empty() ||
      canonical->lods[0].vertices.empty())
    return false;
  auto low = canonical->lods[0].vertices.front().position, high = low;
  for (const auto &v : canonical->lods[0].vertices) {
    low = {std::min(low.x, v.position.x), std::min(low.y, v.position.y),
           std::min(low.z, v.position.z)};
    high = {std::max(high.x, v.position.x), std::max(high.y, v.position.y),
            std::max(high.z, v.position.z)};
  }
  center = {(low.x + high.x) * .5f, (low.y + high.y) * .5f,
            (low.z + high.z) * .5f};
  extent = {high.x - low.x, high.y - low.y, high.z - low.z};
  return true;
}
void EditorAvatarCanvas::focusAvatar() {
  math::FVector3 center, extent;
  if (!assetBounds(center, extent))
    return;
  const float radius = std::max({extent.x, extent.y, extent.z, .01f}) * .5f;
  _standardView = AvatarStandardView::Free;
  _camera.setThreeDProjection(ProjectionMode::Perspective);
  _camera.threeD().setPose(center + math::FVector3{0, 0, radius * 3.2f},
                           3.14159265f, 0);
  _camera.threeD().setMoveSpeed(std::max(radius * 3.f, .05f));
  cancelNavigation();
  _focusedAsset = true;
  markDirty();
}
void EditorAvatarCanvas::resetStandardView(AvatarStandardView view) {
  focusAvatar();
  if (view == AvatarStandardView::Free)
    return;
  math::FVector3 center, extent;
  if (!assetBounds(center, extent))
    return;
  updateViewport();
  const float distance = std::max({extent.x, extent.y, extent.z, .01f}) * 2;
  auto state = _camera.state();
  state.threeDProjection = ProjectionMode::Orthographic;
  float width = extent.x, height = extent.y;
  if (view == AvatarStandardView::Front) {
    state.threeDEye = center + math::FVector3{0, 0, distance};
    state.threeDYawRadians = 3.14159265f;
  } else if (view == AvatarStandardView::Right) {
    state.threeDEye = center + math::FVector3{distance, 0, 0};
    state.threeDYawRadians = -1.57079633f;
    width = extent.z;
  } else {
    state.threeDEye = center + math::FVector3{0, distance, 0};
    state.threeDPitchRadians = -1.57079633f;
    height = extent.z;
  }
  state.threeDOrthoHeight =
      std::max({height, width / _camera.viewportAspect(), .01f}) * 1.2f;
  _camera.restoreState(state);
  _standardView = view;
  _standardCenter = center;
  cancelNavigation();
  markDirty();
}
EditorSceneCameraFrame EditorAvatarCanvas::cameraFrame() const noexcept {
  auto result = _camera.frame();
  // Freecam clamps pitch to avoid its singularity. The locked top view has an
  // exact -Y axis and a fixed -Z screen up, independent of that flycam limit.
  if (_standardView == AvatarStandardView::Top)
    result.view =
        math::lh::lookAt(result.position, _standardCenter, {0, 0, -1});
  return result;
}
void EditorAvatarCanvas::setLod(uint32_t value) {
  _lod = value;
  markDirty();
}
void EditorAvatarCanvas::tickNavigation(float dt) {
  if (!_camera.threeD().isLooking() || !hasFocus())
    return;
  if (_ui && (_ui->getModifiers() & 1))
    _keys.onKeyDown(device::KeyCode::LeftShift);
  else
    _keys.onKeyUp(device::KeyCode::LeftShift);
  if (std::isfinite(dt) && dt > 0)
    _camera.threeD().updateMovement(std::min(dt, .1f), _keys);
  markDirty();
}
bool EditorAvatarCanvas::onMouseButtonDown(const ui::UIMouseEvent &e) {
  if (!_document->creator() || !getWorldBounds().contains(e.mousePos) ||
      (e.mouseButton != 0 && e.mouseButton != 1))
    return false;
  if (_ui)
    _ui->setFocus(this);
  if (e.mouseButton == 1) {
    _standardView = AvatarStandardView::Free;
    _camera.setThreeDProjection(ProjectionMode::Perspective);
    _camera.threeD().beginLook(e.mousePos.x, e.mousePos.y);
  }
  return true;
}
bool EditorAvatarCanvas::onMouseMove(const ui::UIMouseEvent &e) {
  if (!_camera.threeD().isLooking())
    return false;
  _camera.threeD().updateLook(e.mousePos.x, e.mousePos.y);
  markDirty();
  return true;
}
bool EditorAvatarCanvas::onMouseButtonUp(const ui::UIMouseEvent &) {
  const bool looking = _camera.threeD().isLooking();
  _camera.threeD().endLook();
  return looking || hasFocus();
}
bool EditorAvatarCanvas::onMouseWheel(const ui::UIMouseWheelEvent &e) {
  if (!_document->creator() || !getWorldBounds().contains(e.mousePos) ||
      !std::isfinite(e.deltaY) || !e.deltaY)
    return false;
  updateViewport();
  const auto b = getWorldBounds();
  if (_standardView != AvatarStandardView::Free) {
    _camera.zoomAt(std::pow(1.15f, std::clamp(-e.deltaY / 40.f, -8.f, 8.f)),
                   {e.mousePos.x - b.minX, e.mousePos.y - b.minY});
    markDirty();
    return true;
  }
  math::FVector3 origin, direction;
  if (!_camera.ray({e.mousePos.x - b.minX, e.mousePos.y - b.minY}, origin,
                   direction))
    return false;
  // The same 40 logical pixels/notch and pointer-ray dolly as EditorSession.
  _camera.threeD().zoomToward(-e.deltaY / 40.f, direction);
  markDirty();
  return true;
}
bool EditorAvatarCanvas::onKeyDown(int key) {
  if (key == ui::UIKey_Escape && _camera.threeD().isLooking()) {
    if (_ui)
      _ui->cancelCapture();
    cancelNavigation();
    return true;
  }
  if (key == ui::UIKey_F && hasFocus()) {
    focusAvatar();
    return true;
  }
  const auto code = navigationKey(key);
  if (code == device::KeyCode::Unknown || !hasFocus())
    return false;
  _keys.onKeyDown(code);
  return _camera.threeD().isLooking();
}
bool EditorAvatarCanvas::onKeyUp(int key) {
  const auto code = navigationKey(key);
  if (code == device::KeyCode::Unknown)
    return false;
  _keys.onKeyUp(code);
  return true;
}
ui::UiCursorHint EditorAvatarCanvas::getCursorHint() const {
  return _camera.threeD().isLooking() ? ui::UiCursorHint::Move
                                      : ui::UiCursorHint::Default;
}
void EditorAvatarCanvas::onRender(ui::IRenderBackend &output) {
  auto bounds = getWorldBounds();
  auto fail = [&](const std::string &message) {
    _diagnostic = message;
    output.drawText(bounds, ui::decodeUtf8Text(message), 14,
                    math::FVector4{1, .6f, .5f, 1});
  };
  output.drawRect(bounds, {.08f, .09f, .11f, 1});
  auto *backend = dynamic_cast<render::UIRenderBackend *>(&output);
  if (!_document->creator()) {
    output.drawText(bounds, L"Open an Avatar bundle (.avb) or preset (.avp).",
                    14, math::FVector4{.8f, .8f, .8f, 1});
    return;
  }
  if (!backend || !backend->previewRenderer()) {
    output.drawText(bounds, L"Avatar preview requires the editor renderer.", 14,
                    math::FVector4{.8f, .8f, .8f, 1});
    return;
  }
  if (_backend != backend) {
    shutdown();
    _backend = backend;
    _renderer = backend->previewRenderer();
    _presenter = std::make_unique<avatar::Presenter>(*_renderer);
  }
  if (!_focusedAsset)
    focusAvatar();
  updateViewport();
  const int width = std::clamp(
      int(std::ceil((bounds.maxX - bounds.minX) * backend->getUiScale())), 1,
      4096);
  const int height = std::clamp(
      int(std::ceil((bounds.maxY - bounds.minY) * backend->getUiScale())), 1,
      4096);
  ui::IRenderBackend::RenderTargetDesc description;
  description.width = width;
  description.height = height;
  description.hasAlpha = false;
  if (!_target.isValid())
    _target = backend->createRenderTarget(description);
  else if (width != _width || height != _height)
    if (!backend->resizeRenderTarget(_target, description)) {
      fail("Preview resize failed");
      return;
    }
  if (!_target.isValid()) {
    fail("Preview target unavailable");
    return;
  }
  _width = width;
  _height = height;
  if (_uploaded != _document->appearanceRevision() || _uploadedLod != _lod) {
    if (_cacheCatalog != _document->catalog()) {
      _cache.clear();
      _cacheCatalog = _document->catalog();
    }
    std::shared_ptr<const avatar::Shape> shape;
    avatar::Frame frame;
    auto status = _document->creator()->preview(_lod, _cache, shape);
    if (status)
      status = avatar::evaluate(*shape, {}, frame);
    if (status)
      status = _presenter->upload(frame, [&](const std::string &name) {
        auto it = _materials.find(name);
        if (it != _materials.end())
          return it->second;
        auto handle = _renderer->createMaterialFromPhoskia(
            shader, "editor-avatar:" +
                        std::to_string(reinterpret_cast<uintptr_t>(this)) +
                        ":" + name);
        if (handle.isValid()) {
          _renderer->setMaterialColor(handle, "baseColor", .8f, .5f, .3f, 1);
          _materials.emplace(name, handle);
        }
        return handle;
      });
    if (!status) {
      fail(status.message);
      return;
    }
    _uploaded = _document->appearanceRevision();
    _uploadedLod = _lod;
    _diagnostic.clear();
  }
  const auto state = cameraFrame();
  render::PreviewSceneCamera camera;
  camera.view = state.view;
  camera.projection = state.projection;
  camera.position = state.position;
  render::RenderScene scene;
  _presenter->append(scene, math::Float4x4::identity());
  if (backend->renderScenePreview(_target, scene, camera, 0x181b22ff))
    backend->blitRenderTarget(_target, bounds);
}
} // namespace ayt::editor
