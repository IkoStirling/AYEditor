#pragma once

#include "IAYApplication.h"

#include <memory>
#include <string>

// Forward declarations only — AYDevice.h / AYScriptRuntimeBridge.h
// would otherwise leak their full include graph onto every
// translation unit that pulls AYEditorApp.h.
namespace ayt::device {
class DeviceManager;
class DeviceInputProvider;
} // namespace ayt::device

namespace ayt::event {
class EventBus;
} // namespace ayt::event

namespace ayt::editor {

class EditorApp : public ayt::app::IApplication {
public:
    explicit EditorApp(const ayt::app::GameDesc& desc);
    EditorApp(const ayt::app::GameDesc& desc, const ayt::app::AppCommandLine& cmdLine);
    ~EditorApp() override;

    static std::unique_ptr<EditorApp> create(const ayt::app::GameDesc& desc);
    static std::unique_ptr<EditorApp> create(const ayt::app::GameDesc& desc,
                                             const ayt::app::AppCommandLine& cmdLine);

    // Used by AYEditorShell_Demo to preload a character FBX when the
    // user did not pass `--import <path>`. Empty = no default (cube).
    void setDefaultImportPath(std::string path) { _defaultImportPath = std::move(path); }

    void registerSubSystems() override;
    void run() override;

    const ayt::app::GameDesc& getDesc() const override { return _desc; }
    ayt::game::GameLoop& getGameLoop() override;
    const ayt::app::AppCommandLine& getCommandLine() const override { return _cmdLine; }
    ayt::event::EventBus& eventBus() override;

    const char* getVersion() const override { return "0.3.0"; }
    const char* getEngineVersion() const override { return "1.0.0"; }

    void onInit() override;
    void onPreShutdown() override;
    void onShutdown() override;

private:
    ayt::app::GameDesc       _desc;
    ayt::app::AppCommandLine _cmdLine;
    std::string              _defaultImportPath;

    // INT-02 (2026-07-15): hoist DeviceManager to a member so its
    // lifetime == EditorApp's lifetime. The Logia InputProvider
    // (DeviceInputProvider) holds a raw pointer to _devices — if
    // _devices were stack-local to run(), the provider would dangle
    // after EditorApp returns. _inputProvider's lifetime matches
    // _devices and is reset before _devices in the dtor so the
    // bridge always sees a safe state during teardown.
    std::unique_ptr<ayt::device::DeviceManager>       _devices;
    std::unique_ptr<ayt::device::DeviceInputProvider> _inputProvider;
};

} // namespace ayt::editor
