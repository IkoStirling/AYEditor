#include <AYEditor/EditorMaterialDocument.h>
#include <AYResource/assetsImpl/Material.h>
#include <AYResource/AssetPath.h>
#include <AYAssetFormat/AssetFormat.h>
#include <AYRenderer.h>
#include <AYIO/File.h>
#include <AYIO/PathSafety.h>
#include <AYStorage/Guid.h>
#include "MaterialCopyPublication.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace ayt::editor
{
namespace
{
namespace fs = std::filesystem;
using resource::Material;
using resource::MaterialParamType;
std::string utf8(const fs::path &p)
{
    const auto s = p.u8string();
    return {s.begin(), s.end()};
}
bool fail(std::string *error, const std::string &value)
{
    if (error)
        *error = value;
    return false;
}
bool texturePath(const fs::path &root, const fs::path &candidate, fs::path &resolved, std::string *error)
{
    std::string reason;
    if (!io::path::resolveWithinRoot(root, candidate, resolved, reason))
        return fail(error, reason);
    try
    {
        if (!fs::is_regular_file(resolved))
            return fail(error, "Choose an existing source texture");
        auto ext = resolved.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        if (!asset_format::matchesPath(utf8(resolved), asset_format::Id::Texture) && ext != ".png" &&
            ext != ".jpg" && ext != ".jpeg" && ext != ".bmp" && ext != ".tga")
            return fail(error, "Choose a runtime texture or supported source image");
        return true;
    }
    catch (const std::exception &)
    {
        return fail(error, "Invalid source texture path");
    }
}
bool approvedTexturePath(const std::string& sourceRoot,const std::string& importedRoot,
                         const fs::path& candidate,fs::path& resolved,bool& imported,std::string* error)
{
    std::string reason;
    if(texturePath(fs::u8path(sourceRoot),candidate,resolved,&reason)){imported=false;return true;}
    if(!importedRoot.empty()&&texturePath(fs::u8path(importedRoot),candidate,resolved,&reason)){
        const auto logical=utf8(fs::relative(resolved,fs::u8path(importedRoot)));
        if(logical.rfind("textures/",0)!=0 && logical.rfind("textures\\",0)!=0)
            return fail(error,"Imported texture must use a textures/ virtual path");
        imported=true;return true;
    }
    return fail(error,reason.empty()?"Choose a project source or imported texture":reason);
}
fs::path resolveTextureReference(const std::string& sourceRoot,const std::string& importedRoot,
                                 const std::string& owner,const std::string& reference)
{
    if(reference.rfind("textures/",0)==0||reference.rfind("textures\\",0)==0){
        const auto source=fs::u8path(sourceRoot)/fs::u8path(reference);
        if(fs::is_regular_file(source))return source;
        const auto derived=fs::u8path(importedRoot)/fs::u8path(reference);
        if(!importedRoot.empty()&&fs::is_regular_file(derived))return derived;
    }
    auto path=fs::u8path(resource::resolveAssetPath(owner,reference));
    if(path.is_relative()&&io::path::isPortableRelative(reference)){
        const auto source=fs::u8path(sourceRoot)/fs::u8path(reference);
        if(fs::is_regular_file(source))return source;
        const auto derived=fs::u8path(importedRoot)/fs::u8path(reference);
        if(!importedRoot.empty()&&fs::is_regular_file(derived))return derived;
    }
    return path;
}
std::string textureReference(const std::string& importedRoot,const std::string& owner,
                             const fs::path& resolved,bool imported)
{
    auto result=imported?utf8(fs::relative(resolved,fs::u8path(importedRoot)))
        :"./"+utf8(fs::relative(resolved,fs::u8path(owner).parent_path()));
    std::replace(result.begin(),result.end(),'\\','/');return result;
}

bool read(const std::string &path, std::vector<UInt8> &bytes, std::string *error)
{
    try
    {
        std::ifstream file(fs::u8path(path), std::ios::binary | std::ios::ate);
        if (!file)
            return fail(error, "Cannot read material");
        const auto size = file.tellg();
        if (size < 1 || size > 4 * 1024 * 1024)
            return fail(error, "Material must be between 1 byte and 4 MiB");
        bytes.resize(size_t(size));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char *>(bytes.data()), size))
            return fail(error, "Material read failed");
        return true;
    }
    catch (const std::exception &)
    {
        return fail(error, "Invalid material path");
    }
}
bool valid(const EditorMaterialSettings &s, std::string *error)
{
    const auto range = [](float v, float low, float high) {
        return std::isfinite(v) && v >= low && v <= high;
    };
    for (auto v : s.baseColor)
        if (!range(v, 0, 1))
            return fail(error, "Base color components must be 0..1");
    for (auto v : s.emissive)
        if (!range(v, 0, 64))
            return fail(error, "Emission components must be 0..64");
    for (auto v : {s.metallic, s.roughness, s.ao, s.opacity, s.alphaCutoff})
        if (!range(v, 0, 1))
            return fail(error, "Surface factors must be 0..1");
    if (s.alphaMode > 2 || s.opacitySource > 2)
        return fail(error, "Unsupported alpha mode");
    if (render::sanitizePbrSurface(s.surface) != s.surface)
        return fail(error, "Invalid UV, normal or roughness adjustment");
    if (error)
        error->clear();
    return true;
}
bool extract(const Material &m, EditorMaterialSettings &s, std::string *error)
{
    const auto type = [&](const char *n, MaterialParamType t) {
        return !m.hasParameter(n) || m.getParameterType(n) == t;
    };
    for (const char *n :
         {"metallic", "roughness", "ao", "opacity", "normalStrength", "roughnessBias", "opacitySource"})
        if (!type(n, MaterialParamType::Float))
            return fail(error, std::string("Unexpected parameter type: ") + n);
    if (!type("baseColor", MaterialParamType::Float4) || !type("emissive", MaterialParamType::Float3) ||
        !type("uvTransform", MaterialParamType::Float4))
        return fail(error, "Unexpected material vector parameter type");
    if (m.hasParameter("baseColor"))
    {
        const auto v = m.getVector4("baseColor");
        s.baseColor = {v.x, v.y, v.z, v.w};
    }
    if (m.hasParameter("emissive"))
    {
        const auto v = m.getVector3("emissive");
        s.emissive = {v.x, v.y, v.z};
    }
    const auto scalar = [&](const char *n, float fallback) {
        return m.hasParameter(n) ? m.getFloat(n) : fallback;
    };
    s.metallic = scalar("metallic", s.metallic);
    s.roughness = scalar("roughness", s.roughness);
    s.ao = scalar("ao", s.ao);
    s.opacity = scalar("opacity", s.opacity);
    const float opacitySource = scalar("opacitySource", 0);
    if (!std::isfinite(opacitySource) || opacitySource < 0 || opacitySource > 2 ||
        std::floor(opacitySource) != opacitySource)
        return fail(error, "Unsupported opacity source");
    s.opacitySource = uint8_t(opacitySource);
    if (m.hasParameter("uvTransform"))
    {
        const auto v = m.getVector4("uvTransform");
        s.surface.uvScaleX = v.x;
        s.surface.uvScaleY = v.y;
        s.surface.uvOffsetX = v.z;
        s.surface.uvOffsetY = v.w;
    }
    s.surface.normalStrength = scalar("normalStrength", 1);
    s.surface.roughnessBias = scalar("roughnessBias", 0);
    s.alphaMode = uint8_t(m.getAlphaMode());
    s.alphaCutoff = m.getAlphaCutoff();
    s.doubleSided = m.isDoubleSided();
    for (size_t i = 0; i < kEditorMaterialTextureSlots.size(); ++i)
    {
        const auto *name = kEditorMaterialTextureSlots[i];
        if (m.hasParameter(name) && m.getParameterType(name) != MaterialParamType::Texture2D)
            return fail(error, std::string("Expected a 2D texture: ") + name);
        if (m.hasParameter(name))
            s.textures[i] = m.getTexture(name);
    }
    return valid(s, error);
}
void apply(Material &m, const EditorMaterialSettings &s)
{
    m.setFloat4("baseColor", s.baseColor.data());
    m.setFloat3("emissive", s.emissive.data());
    m.setFloat("metallic", s.metallic);
    m.setFloat("roughness", s.roughness);
    m.setFloat("ao", s.ao);
    m.setFloat("opacity", s.opacity);
    m.setFloat("opacitySource", float(s.opacitySource));
    const float uv[] = {s.surface.uvScaleX, s.surface.uvScaleY, s.surface.uvOffsetX, s.surface.uvOffsetY};
    m.setFloat4("uvTransform", uv);
    m.setFloat("normalStrength", s.surface.normalStrength);
    m.setFloat("roughnessBias", s.surface.roughnessBias);
    m.setSurfaceProperties(static_cast<resource::MaterialAlphaMode>(s.alphaMode), s.alphaCutoff,
                           s.doubleSided);
    for (size_t i = 0; i < kEditorMaterialTextureSlots.size(); ++i)
        if (!s.textures[i].empty() || m.hasParameter(kEditorMaterialTextureSlots[i]))
            m.setTexture(kEditorMaterialTextureSlots[i], s.textures[i].c_str());
}
} // namespace
struct EditorMaterialDocument::Impl
{
    std::string type = "ayeditor.material.document", path, title, root, importedRoot, diagnostic, shader;
    std::vector<UInt8> bytes;
    std::unique_ptr<Material> source;
    EditorMaterialSettings current, saved;
    std::vector<EditorMaterialSettings> history;
    size_t cursor = 0;
    uint64_t revision = 0;
    bool readOnly = true;
    bool snapshot(Material &target) const
    {
        if (!target.loadFromBinary(bytes.data(), bytes.size()))
            return false;
        apply(target, current);
        return true;
    }
};
EditorMaterialDocument::EditorMaterialDocument() : _impl(std::make_unique<Impl>())
{
}
EditorMaterialDocument::~EditorMaterialDocument() = default;
const std::string &EditorMaterialDocument::typeId() const noexcept
{
    return _impl->type;
}
const std::string &EditorMaterialDocument::path() const noexcept
{
    return _impl->path;
}
const std::string &EditorMaterialDocument::title() const noexcept
{
    return _impl->title;
}
const std::string &EditorMaterialDocument::diagnostic() const noexcept
{
    return _impl->diagnostic;
}
const std::string &EditorMaterialDocument::shaderReference() const noexcept
{
    return _impl->shader;
}
const EditorMaterialSettings &EditorMaterialDocument::settings() const noexcept
{
    return _impl->current;
}
bool EditorMaterialDocument::isDirty() const noexcept
{
    return _impl->current != _impl->saved;
}
uint64_t EditorMaterialDocument::revision() const noexcept
{
    return _impl->revision;
}
bool EditorMaterialDocument::readOnly() const noexcept
{
    return _impl->readOnly;
}
bool EditorMaterialDocument::initialize(const EditorOpenRequest &request, std::string &error)
{
    try
    {
        const auto ext = fs::u8path(request.resourcePath).extension().string();
        std::string lowered = ext;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        if (lowered != ".mat" && lowered != ".aymat")
            return fail(&error, "Choose a .mat material");
        const auto previous = _impl->path;
        _impl->path = utf8(fs::absolute(fs::u8path(request.resourcePath)));
        if (!reload(&error))
        {
            _impl->path = previous;
            return false;
        }
        return true;
    }
    catch (const std::exception &)
    {
        return fail(&error, "Invalid material path");
    }
}
void EditorMaterialDocument::configureSourceRoot(std::string root)
{
    auto &p = *_impl;
    p.root = std::move(root);
    p.readOnly = true;
    fs::path resolved;
    std::string reason;
    if (!p.root.empty() &&
        io::path::resolveWithinRoot(fs::u8path(p.root), fs::u8path(p.path), resolved, reason))
        p.readOnly = false;
}
void EditorMaterialDocument::configureImportedRoot(std::string root)
{
    _impl->importedRoot=std::move(root);
}

bool EditorMaterialDocument::replaceSettings(const EditorMaterialSettings &s, std::string *error)
{
    auto &p = *_impl;
    configureSourceRoot(p.root);
    if (p.readOnly)
        return fail(error, "Imported or external material is read-only");
    if (!valid(s, error))
        return false;
    auto next = s;
    try
    {
        for (size_t i = 0; i < next.textures.size(); ++i)
        {
            auto &reference = next.textures[i];
            if (reference == p.current.textures[i] || reference.empty())
                continue;
            if (reference.size() > 4096 || reference.find('\0') != std::string::npos)
                return fail(error, "Invalid texture reference");
            const auto candidate = resolveTextureReference(p.root,p.importedRoot,p.path,reference);
            fs::path resolved;bool imported=false;
            if (!approvedTexturePath(p.root,p.importedRoot,candidate,resolved,imported,error))return false;
            reference=textureReference(p.importedRoot,p.path,resolved,imported);
        }
    }
    catch (const std::exception &)
    {
        return fail(error, "Invalid texture reference");
    }
    if (next == p.current)
        return true;
    p.history.resize(p.cursor + 1);
    p.history.push_back(next);
    ++p.cursor;
    if (p.history.size() > 128)
    {
        p.history.erase(p.history.begin());
        --p.cursor;
    }
    p.current = std::move(next);
    ++p.revision;
    p.diagnostic.clear();
    return true;
}
bool EditorMaterialDocument::setTextureAsset(size_t slot, const std::string &sourcePath, std::string *error)
{
    if (slot >= kEditorMaterialTextureSlots.size())
        return fail(error, "Unknown texture slot");
    auto s = settings();
    if (!textureReferenceForAsset(sourcePath, s.textures[slot], error))
        return false;
    return replaceSettings(s, error);
}
bool EditorMaterialDocument::textureReferenceForAsset(const std::string &sourcePath, std::string &reference,
                                                      std::string *error) const
{
    if (sourcePath.empty())
    {
        reference.clear();
        if (error)
            error->clear();
        return true;
    }
    if (_impl->root.empty())
        return fail(error, "Open a project source asset root first");
    try
    {
        const auto root = fs::u8path(_impl->root);
        auto input=sourcePath;std::replace(input.begin(),input.end(),'\\','/');
        auto request=fs::u8path(input);fs::path candidate;
        const auto prefix=input.size()>=9?input.substr(0,9):std::string{};
        std::string folded=prefix;std::transform(folded.begin(),folded.end(),folded.begin(),
            [](unsigned char value){return char(std::tolower(value));});
        if(folded=="imported/"){
            if(_impl->importedRoot.empty())return fail(error,"No project imported root is configured");
            const auto relative=input.substr(9);
            if(!io::path::isPortableRelative(relative))return fail(error,"Invalid imported texture path");
            candidate=fs::u8path(_impl->importedRoot)/fs::u8path(relative);
        }else{
            candidate=request.is_absolute()?request:root/request;
            if(!request.is_absolute()&&!fs::is_regular_file(candidate)&&!_impl->importedRoot.empty()){
                const auto derived=fs::u8path(_impl->importedRoot)/request;
                if(fs::is_regular_file(derived))candidate=derived;
            }
        }
        fs::path resolved;bool imported=false;
        if(!approvedTexturePath(_impl->root,_impl->importedRoot,candidate,resolved,imported,error))return false;
        reference=textureReference(_impl->importedRoot,path(),resolved,imported);
        if (error)
            error->clear();
        return true;
    }
    catch (const std::exception &)
    {
        return fail(error, "Invalid source texture path");
    }
}
std::string EditorMaterialDocument::textureAssetPath(size_t slot) const
{
    if (slot >= settings().textures.size() || settings().textures[slot].empty())
        return {};
    try
    {
        const auto resolved = resolveTextureReference(_impl->root,_impl->importedRoot,path(),settings().textures[slot]);
        fs::path inside;
        std::string error;
        if (!_impl->root.empty() &&
            io::path::resolveWithinRoot(fs::u8path(_impl->root), resolved, inside, error))
        {
            auto relative = utf8(fs::relative(inside, fs::u8path(_impl->root)));
            std::replace(relative.begin(), relative.end(), '\\', '/');
            return relative;
        }
        if(!_impl->importedRoot.empty()&&io::path::resolveWithinRoot(fs::u8path(_impl->importedRoot),resolved,inside,error)){
            auto relative=utf8(fs::relative(inside,fs::u8path(_impl->importedRoot)));
            std::replace(relative.begin(),relative.end(),'\\','/');
            return "Imported/"+relative;
        }
    }
    catch (const std::exception &)
    {
    }
    return settings().textures[slot];
}
bool EditorMaterialDocument::undo()
{
    auto &p = *_impl;
    if (p.readOnly || !p.cursor)
        return false;
    p.current = p.history[--p.cursor];
    ++p.revision;
    return true;
}
bool EditorMaterialDocument::redo()
{
    auto &p = *_impl;
    if (p.readOnly || p.cursor + 1 >= p.history.size())
        return false;
    p.current = p.history[++p.cursor];
    ++p.revision;
    return true;
}
bool EditorMaterialDocument::reload(std::string *error)
{
    auto &p = *_impl;
    std::vector<UInt8> bytes;
    if (!read(p.path, bytes, error))
        return false;
    auto next = std::make_unique<Material>();
    if (!next->loadFromBinary(bytes.data(), bytes.size()))
        return fail(error, "Invalid single material (material bundles are unsupported)");
    EditorMaterialSettings s;
    if (!extract(*next, s, error))
        return false;
    p.source = std::move(next);
    p.bytes = std::move(bytes);
    p.current = p.saved = s;
    p.history = {s};
    p.cursor = 0;
    ++p.revision;
    p.title = utf8(fs::u8path(p.path).filename());
    p.shader = p.source->getShader();
    p.diagnostic.clear();
    configureSourceRoot(p.root);
    if (error)
        error->clear();
    return true;
}
bool EditorMaterialDocument::save(std::string *error)
{
    auto &p = *_impl;
    configureSourceRoot(p.root);
    if (p.readOnly)
        return fail(error, "Save is limited to the project source asset root");
    std::vector<UInt8> disk;
    if (!read(p.path, disk, error))
        return false;
    if (disk != p.bytes)
        return fail(error, "Material changed on disk; reload it before saving");
    Material next;
    if (!p.snapshot(next))
        return fail(error, "Cannot prepare material snapshot");
    std::vector<UInt8> bytes;
    if (!next.saveToBinary(bytes) || bytes.size() > 4 * 1024 * 1024
        || !io::File::atomicWriteUtf8(p.path, bytes.data(), bytes.size()))
        return fail(error, "Material save failed");
    p.bytes = std::move(bytes);
    p.saved = p.current;
    p.source = std::make_unique<Material>();
    p.source->loadFromBinary(p.bytes.data(), p.bytes.size());
    ++p.revision;
    p.diagnostic.clear();
    if (error)
        error->clear();
    return true;
}
bool EditorMaterialDocument::saveEditableCopy(const std::string& destination,std::string* error) const
{
    const auto& p=*_impl;
    if(!p.source||p.root.empty())return fail(error,"Open a project material before creating a copy");
    try{
        if(destination.empty()||destination.find('\0')!=std::string::npos||destination.size()>4096)
            return fail(error,"Choose a project .mat destination");
        auto request=fs::u8path(destination);
        auto extension=request.extension().string();
        std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return char(std::tolower(c));});
        if(extension!=".mat")return fail(error,"New material copies use the .mat suffix");
        if(!request.is_absolute()&&!io::path::isPortableRelative(destination))
            return fail(error,"Choose a portable path beneath project source assets");
        const auto root=fs::u8path(p.root);
        fs::path source,target;std::string reason;
        const bool sourceOwned=io::path::resolveWithinRoot(root,fs::u8path(p.path),source,reason)
            || (!p.importedRoot.empty()&&io::path::resolveWithinRoot(fs::u8path(p.importedRoot),fs::u8path(p.path),source,reason));
        if(!sourceOwned)return fail(error,"Copy requires a project source or imported project material");
        if(!io::path::resolveWithinRoot(root,request.is_absolute()?request:root/request,target,reason))return fail(error,reason);
        if(fs::exists(target)||fs::is_symlink(target))return fail(error,"The material destination already exists");
        Material next;if(!p.snapshot(next))return fail(error,"Cannot prepare material copy snapshot");
        const auto targetText=utf8(target);
        const auto rebase=[&](const std::string& reference,bool shader){
            if(reference.empty())return reference;
            const auto oldPath=fs::u8path(resource::resolveAssetPath(p.path,reference));
            const auto newPath=fs::u8path(resource::resolveAssetPath(targetText,reference));
            if(oldPath.lexically_normal()==newPath.lexically_normal()||fs::u8path(reference).is_absolute())return reference;
            // A bare shader may be resolved by the renderer's mounted-root
            // fallback. Rebase only an actual sibling, or an explicit relative path.
            if(shader&&reference.front()!='.'&&!fs::is_regular_file(oldPath))return reference;
            auto relative="./"+utf8(fs::relative(oldPath,target.parent_path()));
            std::replace(relative.begin(),relative.end(),'\\','/');return relative;
        };
        std::vector<std::pair<std::string,std::string>> references;
        next.forEachParameter([&](const char* name,MaterialParamType type){
            if(type>=MaterialParamType::Texture2D&&type<=MaterialParamType::TextureCube)
                references.emplace_back(name,next.getTexture(name));
        });
        for(const auto& [name,value]:references)
            if(!next.setTextureReference(name,rebase(value,false)))return fail(error,"Cannot preserve typed texture reference in material copy");
        next.setShader(rebase(next.getShader(),true));
        std::vector<UInt8> bytes;
        if(!next.saveToBinary(bytes))return fail(error,"Cannot encode material copy");
        auto identity=utf8(target.lexically_relative(fs::weakly_canonical(root)));
        identity.append(reinterpret_cast<const char*>(bytes.data()),bytes.size());
        next.setGuid(storage::Guid::computeFromData(identity.data(),identity.size()));
        if(!next.saveToBinary(bytes))return fail(error,"Cannot encode material copy identity");
        if(bytes.size()>4*1024*1024)return fail(error,"Material copy exceeds the 4 MiB document limit");
        detail::publishNewMaterialCopy(root,target,bytes.data(),bytes.size());
        if(error)error->clear();return true;
    }catch(const std::exception& exception){return fail(error,exception.what());}
}

bool EditorMaterialDocument::writeRecoveryCopy(const std::string &path, std::string *error) const
{
    Material next;
    if (!_impl->source || !_impl->snapshot(next))
        return fail(error, "Cannot prepare recovery snapshot");
    std::vector<UInt8> bytes;
    if (!next.saveToBinary(bytes) || !io::File::atomicWriteUtf8(path, bytes.data(), bytes.size()))
        return fail(error, "Material recovery write failed");
    if (error)
        error->clear();
    return true;
}
std::vector<std::pair<std::string, std::string>> EditorMaterialDocument::textureReferences() const
{
    std::vector<std::pair<std::string, std::string>> result;
    if (!_impl->source)
        return result;
    _impl->source->forEachParameter([&](const char *name, MaterialParamType type) {
        if (type >= MaterialParamType::Texture2D && type <= MaterialParamType::TextureCube &&
            std::find(kEditorMaterialTextureSlots.begin(), kEditorMaterialTextureSlots.end(),
                      std::string(name)) == kEditorMaterialTextureSlots.end())
            result.emplace_back(name, _impl->source->getTexture(name));
    });
    for (size_t i = 0; i < kEditorMaterialTextureSlots.size(); ++i)
        if (!settings().textures[i].empty())
            result.emplace_back(kEditorMaterialTextureSlots[i], settings().textures[i]);
    return result;
}
render::MaterialHandle EditorMaterialDocument::createPreviewMaterial(render::Renderer &renderer,
                                                                     std::string *error) const
{
    Material next;
    if (!_impl->source || !_impl->snapshot(next))
    {
        fail(error, "Cannot prepare preview snapshot");
        return {};
    }
    auto handle = renderer.createMaterialFromResource(next, path());
    if (!handle.isValid())
        fail(error, "Material shader or preview resources are unavailable");
    else if (error)
        error->clear();
    return handle;
}
bool EditorMaterialDocument::handlesCommand(const std::string &id) const
{
    return id == "file.save" || id == "file.reload" || id == "edit.undo" || id == "edit.redo";
}
bool EditorMaterialDocument::canExecuteCommand(const std::string &id) const
{
    const auto &p = *_impl;
    if (id == "file.save")
        return !p.readOnly && isDirty();
    if (id == "file.reload")
        return p.source && !isDirty();
    if (id == "edit.undo")
        return !p.readOnly && p.cursor > 0;
    if (id == "edit.redo")
        return !p.readOnly && p.cursor + 1 < p.history.size();
    return false;
}
bool EditorMaterialDocument::executeCommand(const std::string &id)
{
    if (!canExecuteCommand(id))
        return false;
    if (id == "file.save")
        return save(&_impl->diagnostic);
    if (id == "file.reload")
        return reload(&_impl->diagnostic);
    if (id == "edit.undo")
        return undo();
    return redo();
}
} // namespace ayt::editor
