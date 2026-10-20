#include "EditorImportRecipeStore.h"
#include <AYIO/File.h>
#include <AYIO/PathSafety.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <random>
#include <sstream>
#include <stdexcept>

namespace ayt::editor::detail {
namespace {
namespace fs = std::filesystem;
std::string utf8(const fs::path& path)
{
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
std::string portable(const fs::path& project, const std::string& reference)
{
    if (reference.empty()) return {};
    const auto path = fs::weakly_canonical(fs::absolute(fs::u8path(reference)));
    return ayt::io::path::isLexicallyWithin(project, path)
        ? utf8(path.lexically_relative(project)) : utf8(path);
}
}

bool sameImportSource(const std::string& first, const std::string& second)
{
    try {
        std::error_code error;
        if (fs::equivalent(fs::u8path(first), fs::u8path(second), error) && !error) return true;
        auto normalizedFirst = utf8(fs::absolute(fs::u8path(first)).lexically_normal());
        auto normalizedSecond = utf8(fs::absolute(fs::u8path(second)).lexically_normal());
#if defined(_WIN32)
        const auto fold = [](std::string& text) {
            for (auto& byte : text) if (byte >= 'A' && byte <= 'Z') byte += 'a' - 'A';
        };
        fold(normalizedFirst); fold(normalizedSecond);
#endif
        return normalizedFirst == normalizedSecond;
    } catch (...) { return false; }
}

std::vector<StoredImportRecipe> loadImportRecipes(const std::string& projectRoot, std::string* error)
{
    return ayt::resource::readProjectImportRecipes(projectRoot, error);
}

bool saveImportRecipe(const std::string& projectRoot, ayt::resource::ImportOptions& options,
                      std::string* error)
{
    try {
        const auto project = fs::weakly_canonical(fs::absolute(fs::u8path(projectRoot)));
        const auto recipes = loadImportRecipes(projectRoot, error);
        if (error && !error->empty()) return false;
        auto found = std::find_if(recipes.begin(), recipes.end(), [&](const auto& recipe) {
            return (options.sourceId.empty()
                ? sameImportSource(recipe.options.sourcePath, options.sourcePath)
                : recipe.options.sourceId == options.sourceId)
                && sameImportSource(recipe.options.outputDir, options.outputDir);
        });
        fs::path target;
        bool newlyAssigned = found == recipes.end();
        if (found != recipes.end()) {
            target = fs::u8path(found->path);
            if (options.sourceId.empty()) options.sourceId = found->options.sourceId;
            if (options.outputNamespace.empty()) options.outputNamespace = found->options.outputNamespace;
            if (options.outputBaseName.empty()) options.outputBaseName = found->options.outputBaseName;
        }
        else {
            std::random_device random;
            for (int attempt = 0; attempt < 8; ++attempt) {
                std::ostringstream id;
                id << "source-" << std::hex << std::setfill('0');
                for (int word = 0; word < 4; ++word) id << std::setw(8) << std::uint32_t(random());
                target = project / ".ayeditor/imports" / (id.str() + ".json");
                if (!fs::exists(target)) break;
                target.clear();
            }
            if (target.empty()) throw std::runtime_error("could not allocate a unique import recipe identity");
        }
        if (options.sourceId.empty()) options.sourceId = utf8(target.stem());
        if (newlyAssigned && options.outputNamespace.empty()) {
            const auto ext = ayt::resource::importExtensionOf(options.sourcePath);
            const bool supported = ext == "fbx" || ext == "gltf" || ext == "glb"
                || ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp" || ext == "tga" || ext == "dds";
            // Adopt already referenced legacy outputs in place. New sources
            // get isolated stable keys, independent of their filename.
            const auto legacy = fs::u8path(ayt::resource::importDependencySidecarPath(options));
            ayt::resource::ImportRecord existing;
            const bool knownSameSource = ayt::resource::readImportRecord(utf8(legacy), existing)
                && sameImportSource(existing.options.sourcePath, options.sourcePath);
            if (supported && !knownSameSource) options.outputNamespace = utf8(target.stem());
        }
        for (const auto& recipe : recipes) {
            if (recipe.options.sourceId != options.sourceId
                && sameImportSource(recipe.options.sourcePath, options.sourcePath)
                && sameImportSource(recipe.options.outputDir, options.outputDir))
                throw std::runtime_error("source already belongs to another import recipe in this output root");
        }
        const auto extension = ayt::resource::importExtensionOf(options.sourcePath);
        const bool supportsStableStem = extension == "fbx" || extension == "gltf" || extension == "glb"
            || extension == "png" || extension == "jpg" || extension == "jpeg"
            || extension == "bmp" || extension == "tga" || extension == "dds";
        if (supportsStableStem && options.outputBaseName.empty()) {
            const auto& original = found == recipes.end() ? options.sourcePath : found->options.sourcePath;
            options.outputBaseName = utf8(fs::u8path(original).stem());
        }
        auto stored = options;
        stored.sourcePath = portable(project, options.sourcePath);
        stored.outputDir = portable(project, options.outputDir);
        stored.sourceAssetRoot = portable(project, options.sourceAssetRoot);
        const auto text = ayt::resource::serializeImportRecipe(stored);
        ayt::resource::ImportOptions verified;
        std::string recipeError;
        if (!ayt::resource::parseImportRecipe(text, verified, &recipeError))
            throw std::runtime_error("invalid project import settings: " + recipeError);
        fs::path resolved;
        std::string pathError;
        if (!ayt::io::path::resolveWithinRoot(project, target, resolved, pathError))
            throw std::runtime_error("import recipe target escapes the project: " + pathError);
        if (!ayt::io::File::atomicWriteUtf8(utf8(resolved), text.data(), text.size()))
            throw std::runtime_error("could not save project import settings");
        if (error) error->clear();
        return true;
    } catch (const std::exception& failure) {
        if (error) *error = failure.what();
        return false;
    }
}

} // namespace ayt::editor::detail
