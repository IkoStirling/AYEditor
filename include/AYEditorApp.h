#pragma once

#include "IAYApplication.h"

#include <memory>

namespace ayt::editor {

class EditorApp : public ayt::app::IApplication {
public:
    explicit EditorApp(const ayt::app::GameDesc& desc);
    EditorApp(const ayt::app::GameDesc& desc, const ayt::app::AppCommandLine& cmdLine);

    static std::unique_ptr<EditorApp> create(const ayt::app::GameDesc& desc);
    static std::unique_ptr<EditorApp> create(const ayt::app::GameDesc& desc,
                                             const ayt::app::AppCommandLine& cmdLine);

    void run() override;

    const ayt::app::GameDesc& getDesc() const override { return _desc; }
    ayt::game::GameLoop& getGameLoop() override;
    const ayt::app::AppCommandLine& getCommandLine() const override { return _cmdLine; }

    const char* getVersion() const override { return "0.3.0"; }
    const char* getEngineVersion() const override { return "1.0.0"; }

    void onInit() override;
    void onPreShutdown() override;
    void onShutdown() override;

private:
    ayt::app::GameDesc       _desc;
    ayt::app::AppCommandLine _cmdLine;
};

} // namespace ayt::editor
