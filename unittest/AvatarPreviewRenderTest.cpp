#include "../src/AYEditorAvatarCanvas.h"
#include "detail/TextureImageLoader.h"
#include <AYEditor/EditorWorkspace.h>
#include <AYRenderer.h>
#include <AYUI/DeviceInputBridge.h>
#include <AYUI/UIManager.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
using namespace ayt;
namespace {
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
struct Host : editor::IEditorHostServices {
  editor::EditorWorkspace value;
  std::string path;
  ui::UIManager *manager = nullptr;
  editor::EditorWorkspace &workspace() noexcept override { return value; }
  const std::string &projectRoot() const noexcept override { return path; }
  ui::UIManager *uiManager() noexcept override { return manager; }
  void requestRepaint() override {}
  void setStatusText(const std::wstring &) override {}
};
struct Fixture {
  HWND window = nullptr;
  render::Renderer renderer;
  render::UIRenderBackend backend;
  ui::UIManager manager;
  Host host;
  std::shared_ptr<editor::EditorAvatarDocument> document =
      std::make_shared<editor::EditorAvatarDocument>();
  std::unique_ptr<editor::IEditorView> view;
  ui::Widget *root = nullptr;
  editor::EditorAvatarCanvas *canvas = nullptr;
  std::filesystem::path output;
  Fixture(const std::string &bundle, std::filesystem::path destination)
      : output(std::move(destination)) {
    std::filesystem::create_directories(output);
    window = CreateWindowExW(0, L"STATIC", L"Avatar editor render regression",
                             WS_OVERLAPPED, 0, 0, 1000, 720, nullptr, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    require(window != nullptr, "hidden window");
    render::InitDesc init;
    init.windowHandle = window;
    init.width = 1000;
    init.height = 720;
    init.vsync = false;
    init.msaa = 0;
    init.backend = render::Backend::Direct3D11;
    require(renderer.initialize(init), "D3D11 initialize");
    require(backend.initialize(renderer), "UI backend initialize");
    backend.setFramebufferSize(1000, 720);
    manager.initialize(&backend);
    manager.setClientSize(1000, 720);
    host.manager = &manager;
    std::string error;
    require(document->initialize({bundle}, error), error.c_str());
    editor::EditorExtensionRegistry registry;
    require(editor::registerEditorAvatarExtension(registry), "register tool");
    view = registry.find(editor::kEditorAvatarExtensionId)
               ->createView(document, host);
    root = view->releaseRootWidget();
    root->setSize({1000, 720});
    manager.root()->addChildExternal(root);
    root->performLayout();
    manager.layout();
    canvas = dynamic_cast<editor::EditorAvatarCanvas *>(
        manager.findById("avatar_viewport"));
    require(canvas != nullptr, "embedded viewport");
  }
  ~Fixture() {
    if (view)
      view->prepareForUiShutdown();
    if (root) {
      root->detachFromParent();
      ui::destroyWidgetTree(root);
    }
    view.reset();
    manager.shutdown();
    backend.shutdown();
    renderer.shutdown();
    if (window)
      DestroyWindow(window);
  }
  void frame(const char *capture = nullptr) {
    manager.update(1.f / 60);
    view->tick(1.f / 60);
    manager.layout();
    renderer.beginCompositeFrame({}, 1000, 720);
    manager.populateFrame();
    manager.flushFrame();
    if (capture)
      require(renderer.captureScreenshot((output / capture).string()),
              "capture");
    renderer.endFrame();
    require(canvas->diagnostic().empty(), canvas->diagnostic().c_str());
  }
  render::detail::DecodedImage capture(const char *name) {
    frame();
    frame(name);
    for (int i = 0; i < 8; ++i)
      frame();
    auto pixels = render::detail::decodeImageFile((output / name).string());
    require(pixels.isValid(), "screenshot pixels");
    return pixels;
  }
};
size_t changed(const render::detail::DecodedImage &a,
               const render::detail::DecodedImage &b,
               const math::FRectangle &viewport) {
  require(a.width == b.width && a.height == b.height, "equal capture sizes");
  size_t count = 0;
  for (int y = int(viewport.minY) + 2; y < int(viewport.maxY) - 2; ++y)
    for (int x = int(viewport.minX) + 2; x < int(viewport.maxX) - 2; ++x) {
      auto p = (size_t(y) * a.width + x) * 4;
      if (std::abs(int(a.rgba8[p]) - int(b.rgba8[p])) > 8 ||
          std::abs(int(a.rgba8[p + 1]) - int(b.rgba8[p + 1])) > 8)
        ++count;
    }
  return count;
}
} // namespace
int main(int argc, char **argv) {
  try {
    require(argc == 3,
            "usage: AYEditor_AvatarPreviewRenderTest HEAD.avb OUTPUT_DIR");
    Fixture f(argv[1], argv[2]);
    auto neutral = f.capture("editor-neutral.tga");
    auto bounds = f.canvas->getWorldBounds();
    size_t clay = 0;
    for (int y = int(bounds.minY); y < int(bounds.maxY); ++y)
      for (int x = int(bounds.minX); x < int(bounds.maxX); ++x) {
        auto p = (size_t(y) * neutral.width + x) * 4;
        if (neutral.rgba8[p] > 65 &&
            neutral.rgba8[p] > neutral.rgba8[p + 1] + 10)
          ++clay;
      }
    require(clay > 5000, "head actually renders inside editor viewport");
    require(f.document->setParameter("headWidth", .6f), "edit real head");
    require(f.document->setParameter("noseProjection", .6f), "edit real nose");
    auto modified = f.capture("editor-modified.tga");
    require(changed(neutral, modified, bounds) > 500,
            "parameter edit changes viewport pixels");
    auto preset = avatar::encodePreset(f.document->creator()->preset());
    auto revision = f.document->revision();
    ui::DeviceInputBridge input(f.manager);
    device::DeviceInputEvent event;
    event.type = device::DeviceInputEventType::MouseButton;
    event.mouseButton = device::MouseButton::Right;
    event.x = (bounds.minX + bounds.maxX) * .5f;
    event.y = (bounds.minY + bounds.maxY) * .5f;
    event.pressed = true;
    input.dispatch(event);
    event.type = device::DeviceInputEventType::MouseMove;
    event.x += 25;
    input.dispatch(event);
    event.type = device::DeviceInputEventType::MouseButton;
    event.pressed = false;
    input.dispatch(event);
    auto side = f.capture("editor-look.tga");
    require(changed(modified, side, bounds) > 1000,
            "right drag changes actual rendered camera");
    require(!f.manager.isCapturing(), "right release ends capture");
    event.type = device::DeviceInputEventType::MouseWheel;
    // One platform notch becomes -40 AYUI pixels through DeviceInputBridge.
    event.deltaY = 1;
    input.dispatch(event);
    auto zoom = f.capture("editor-zoom.tga");
    require(changed(side, zoom, bounds) > 100,
            "wheel changes actual rendered camera");
    require(avatar::encodePreset(f.document->creator()->preset()) == preset &&
                f.document->revision() == revision,
            "navigation preserves parameter document");
    std::cout << "PASS embedded D3D11 head, morph, right-drag, wheel, "
                 "teardown; clay pixels="
              << clay << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
