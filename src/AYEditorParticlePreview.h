#pragma once
#include <AYEditor/EditorParticleDocument.h>
#include <AYRenderer/UIRenderBackend.h>
namespace ayt::editor {
// Internal rendering adapter. The document keeps authored values and transport;
// this adapter owns target/material/compute lifetimes on the render thread.
class EditorParticleRuntimePreview {
public:
    explicit EditorParticleRuntimePreview(std::shared_ptr<EditorParticleDocument> document);
    ~EditorParticleRuntimePreview();
    bool render(render::UIRenderBackend& ui,const math::FRectangle& bounds);
    void shutdown();
    void focus();
    const std::string& status() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}
