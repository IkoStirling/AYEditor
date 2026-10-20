#pragma once
#include <AYEditor/EditorExtensionRegistry.h>
#include <AYRenderer/PbrSurfaceSettings.h>
#include <array>
#include <memory>
#include <utility>

namespace ayt::render
{
class Renderer;
struct MaterialHandle;
} // namespace ayt::render
namespace ayt::editor
{
inline constexpr const char *kEditorMaterialExtensionId = "ayeditor.material";
inline constexpr std::array<const char *, 7> kEditorMaterialTextureSlots = {
    "baseColorTexture", "normalTexture",   "roughnessTexture", "metallicTexture",
    "aoTexture",        "emissiveTexture", "opacityTexture"};
struct EditorMaterialSettings
{
    std::array<float, 4> baseColor{1, 1, 1, 1};
    std::array<float, 3> emissive{0, 0, 0};
    float metallic = 0, roughness = .5f, ao = 1, opacity = 1, alphaCutoff = .5f;
    uint8_t alphaMode = 0;
    /// 0=base-color alpha, 1=opacity texture red, 2=opacity texture alpha.
    uint8_t opacitySource = 0;
    bool doubleSided = false;
    render::PbrSurfaceSettings surface;
    // Empty means the renderer's established fallback. References loaded from
    // a file retain their spelling until that slot is explicitly changed.
    std::array<std::string, 7> textures{};
    bool operator==(const EditorMaterialSettings &) const = default;
};
/// Single runtime .mat/.aymat authoring through the existing AYResource codec.
/// Owns a decoded snapshot, preserving unedited parameters, texture references,
/// name, shader and GUID. Bundle files are rejected. Changes have local undo/redo;
/// preview materials are independent GPU snapshots and never edit the Scene.
class EditorMaterialDocument final : public IEditorDocument, public IEditorCommandTarget
{
  public:
    EditorMaterialDocument();
    ~EditorMaterialDocument() override;
    bool initialize(const EditorOpenRequest &, std::string &error);
    /// Save/edit is enabled only below this project source root (custom roots
    /// are supported). Imported/generated files remain inspectable read-only.
    void configureSourceRoot(std::string root);
    const std::string &typeId() const noexcept override;
    const std::string &path() const noexcept override;
    const std::string &title() const noexcept override;
    bool isDirty() const noexcept override;
    uint64_t revision() const noexcept override;
    bool readOnly() const noexcept;
    const EditorMaterialSettings &settings() const noexcept;
    bool replaceSettings(const EditorMaterialSettings &, std::string *error = nullptr);
    /// Choose a source-root-relative texture asset; absolute paths are accepted
    /// only inside the same root. Saves an explicit owner-relative reference.
    /// Empty clears the slot. Failure retains the document and its history.
    bool setTextureAsset(size_t slot, const std::string &sourcePath, std::string *error = nullptr);
    std::string textureAssetPath(size_t slot) const;
    /// Resolve one texture choice without editing, for atomic multi-field UI commits.
    bool textureReferenceForAsset(const std::string &sourcePath, std::string &reference,
                                  std::string *error = nullptr) const;
    bool undo();
    bool redo();
    bool save(std::string *error = nullptr) override;
    bool canReload() const noexcept override
    {
        return true;
    }
    bool reload(std::string *error = nullptr) override;
    bool writeRecoveryCopy(const std::string &, std::string *error = nullptr) const override;
    bool handlesCommand(const std::string &) const override;
    bool canExecuteCommand(const std::string &) const override;
    bool executeCommand(const std::string &) override;
    const std::string &diagnostic() const noexcept;
    const std::string &shaderReference() const noexcept;
    std::vector<std::pair<std::string, std::string>> textureReferences() const;
    /// Owner-thread independent preview; caller destroys only this material.
    render::MaterialHandle createPreviewMaterial(render::Renderer &, std::string *error = nullptr) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
EditorDescriptor makeEditorMaterialDescriptor();
bool registerEditorMaterialExtension(EditorExtensionRegistry &, std::string *error = nullptr);
} // namespace ayt::editor
