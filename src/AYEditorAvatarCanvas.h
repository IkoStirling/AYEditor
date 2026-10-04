#pragma once
#include <AYAvatar/EngineBridge.h>
#include <AYDevice/KeyboardDevice.h>
#include <AYEditor/EditorAvatarDocument.h>
#include <AYEditor/EditorSceneCamera.h>
#include <AYRenderer/UIRenderBackend.h>
#include <AYUI/FocusableWidget.h>
#include <unordered_map>
namespace ayt::editor {
class EditorAvatarCanvas final : public ui::FocusableWidget {
public:
  explicit EditorAvatarCanvas(std::shared_ptr<EditorAvatarDocument>,
                              ui::UIManager *);
  ~EditorAvatarCanvas() override;
  void shutdown();
  void cancelNavigation();
  void tickNavigation(float dt);
  void focusAvatar();
  void setLod(uint32_t lod);
  uint32_t lod() const noexcept { return _lod; }
  const EditorSceneCamera &camera() const noexcept { return _camera; }
  const std::string &diagnostic() const noexcept { return _diagnostic; }
  bool onMouseButtonDown(const ui::UIMouseEvent &) override;
  bool onMouseMove(const ui::UIMouseEvent &) override;
  bool onMouseButtonUp(const ui::UIMouseEvent &) override;
  bool onMouseWheel(const ui::UIMouseWheelEvent &) override;
  void onCaptureCancelled() override { cancelNavigation(); }
  bool onKeyDown(int) override;
  bool onKeyUp(int) override;
  ui::UiCursorHint getCursorHint() const override;

protected:
  void onFocusLost() override { cancelNavigation(); }
  void onRender(ui::IRenderBackend &) override;

private:
  void updateViewport();
  std::shared_ptr<EditorAvatarDocument> _document;
  ui::UIManager *_ui = nullptr;
  EditorSceneCamera _camera;
  device::KeyboardDevice _keys;
  avatar::ShapeCache _cache{32 * 1024 * 1024};
  std::unique_ptr<avatar::Presenter> _presenter;
  render::Renderer *_renderer = nullptr;
  render::UIRenderBackend *_backend = nullptr;
  ui::IRenderBackend::RenderTargetHandle _target;
  std::unordered_map<std::string, render::MaterialHandle> _materials;
  uint64_t _uploaded = UINT64_MAX;
  const avatar::Catalog *_cacheCatalog = nullptr;
  uint32_t _lod = 0, _uploadedLod = UINT32_MAX;
  int _width = 0, _height = 0;
  bool _focusedAsset = false;
  std::string _diagnostic;
};
} // namespace ayt::editor
