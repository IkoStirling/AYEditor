#include "AYEditor/EditorBuiltInExtensions.h"

#include "AYEditor/EditorCommandSystem.h"
#include "AYEditor/EditorProductPaths.h"
#include "AYEditor/EditorWorkspace.h"
#include "AYEditorTileAtlasPicker.h"
#include "AYEditorTilemapCanvas.h"

#include <AY2DEditor/TileAtlasImportModel.h>
#include <AY2DEditor/TilemapEditorModel.h>
#include <AYAudio/AudioSubSystem.h>
#include <AYAudio/AudioEngine.h>
#include <AYResource/assetsImpl/Animation.h>
#include <AYResource/assetsImpl/Audio.h>
#include <AYResource/Converter/TilemapConverter.h>
#include <AYUI/Box.h>
#include <AYUI/Button.h>
#include <AYUI/ComboBox.h>
#include <AYUI/ListView.h>
#include <AYUI/Modal.h>
#include <AYUI/Panel.h>
#include <AYUI/ScrollView.h>
#include <AYUI/Slider.h>
#include <AYUI/SplitterHandle.h>
#include <AYUI/SvgIcon.h>
#include <AYUI/TextLabel.h>
#include <AYUI/TextInput.h>
#include <AYUI/ToolBar.h>
#include <AYUI/Tooltip.h>
#include <AYUI/UIKeyCode.h>
#include <AYUI/UIManager.h>
#include <AYUI/UnicodeText.h>
#include <AYUI/Widget.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <Windows.h>
#  include <commdlg.h>
#endif

namespace ayt::editor {
namespace {

struct TilemapSaveResult {
    bool sourceSaved = false;
    bool cooked = false;
    std::string notice;
};

std::wstring rgbaText(uint32_t rgba)
{
    wchar_t buffer[11]{};
    std::swprintf(buffer, 11u, L"0x%08X", rgba);
    return buffer;
}

std::filesystem::path tilemapAssetRoot(
    const std::filesystem::path& source)
{
    for (std::filesystem::path cursor = source.parent_path();
         !cursor.empty(); cursor = cursor.parent_path()) {
        if (cursor.filename() == "Assets") return cursor;
        const std::filesystem::path parent = cursor.parent_path();
        if (parent == cursor) break;
    }
    return {};
}

std::optional<std::filesystem::path> existingAtlasSource(
    const std::string& authoredPath,
    const std::filesystem::path& assetRoot,
    const std::filesystem::path& currentDocument,
    const std::filesystem::path& targetDocument)
{
    const std::filesystem::path authored =
        std::filesystem::u8path(authoredPath);
    std::vector<std::filesystem::path> candidates;
    if (authored.is_absolute()) {
        candidates.push_back(authored);
    } else {
        if (!assetRoot.empty()) candidates.push_back(assetRoot / authored);
        if (!currentDocument.empty()) {
            candidates.push_back(currentDocument.parent_path() / authored);
        }
        candidates.push_back(targetDocument.parent_path() / authored);
    }
    for (const std::filesystem::path& candidate : candidates) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(candidate, error) || error) {
            continue;
        }
        const std::filesystem::path absolute =
            std::filesystem::absolute(candidate, error);
        return (error ? candidate : absolute).lexically_normal();
    }
    return std::nullopt;
}

std::optional<std::string> atlasFileHash(
    const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    uint64_t hash = 14695981039346656037ull;
    std::array<char, 64u * 1024u> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        for (std::streamsize index = 0; index < count; ++index) {
            hash ^= static_cast<unsigned char>(buffer[static_cast<size_t>(index)]);
            hash *= 1099511628211ull;
        }
    }
    if (input.bad()) return std::nullopt;
    std::ostringstream text;
    text << std::hex << std::setfill('0') << std::setw(16) << hash;
    return text.str();
}

std::string portableAtlasStem(std::string value)
{
    for (char& ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (!std::isalnum(byte) && ch != '_' && ch != '-') ch = '_';
    }
    while (!value.empty() && value.back() == '_') value.pop_back();
    return value.empty() ? "tile_atlas" : value;
}

bool pathInside(const std::filesystem::path& child,
                const std::filesystem::path& parent,
                std::filesystem::path& relative)
{
    std::error_code error;
    const std::filesystem::path normalizedChild =
        std::filesystem::weakly_canonical(child, error);
    if (error) return false;
    const std::filesystem::path normalizedParent =
        std::filesystem::weakly_canonical(parent, error);
    if (error) return false;
    relative = normalizedChild.lexically_relative(normalizedParent);
    return !relative.empty() && !relative.is_absolute()
        && relative.begin() != relative.end()
        && *relative.begin() != "..";
}

std::string makeAtlasSourcesPortable(
    ayt::ay2d::editor::TilemapEditorModel& model,
    const std::filesystem::path& targetDocument,
    const std::filesystem::path& assetRoot)
{
    std::map<uint32_t, std::string> replacements;
    std::string notice;
    const std::filesystem::path currentDocument =
        std::filesystem::u8path(model.document().path());
    for (const auto& [atlasId, atlas] : model.document().tileAtlases()) {
        const auto source = existingAtlasSource(
            atlas.sourcePath, assetRoot, currentDocument, targetDocument);
        if (!source) {
            if (!notice.empty()) notice += " ";
            notice += "Atlas '" + atlas.name
                + "' could not be found and was not made portable.";
            continue;
        }
        std::filesystem::path relative;
        if (!pathInside(*source, assetRoot, relative)) {
            const auto hash = atlasFileHash(*source);
            if (!hash) {
                if (!notice.empty()) notice += " ";
                notice += "Atlas '" + atlas.name
                    + "' could not be read and was not made portable.";
                continue;
            }
            std::string extension = source->extension().string();
            std::transform(extension.begin(), extension.end(),
                           extension.begin(), [](unsigned char ch) {
                               return static_cast<char>(std::tolower(ch));
                           });
            const std::filesystem::path destination = assetRoot
                / "Imported" / "Tilemaps"
                / (portableAtlasStem(source->stem().string()) + "_"
                   + *hash + extension);
            std::error_code error;
            std::filesystem::create_directories(
                destination.parent_path(), error);
            if (!error && !std::filesystem::exists(destination, error)) {
                error.clear();
                std::filesystem::copy_file(
                    *source, destination,
                    std::filesystem::copy_options::none, error);
            }
            if (error || !std::filesystem::is_regular_file(destination)) {
                if (!notice.empty()) notice += " ";
                notice += "Atlas '" + atlas.name
                    + "' could not be copied into project Assets.";
                continue;
            }
            relative = destination.lexically_relative(assetRoot);
        }
        const std::string portablePath = relative.generic_string();
        if (portablePath != atlas.sourcePath) {
            replacements.emplace(atlasId, portablePath);
        }
    }
    if (!replacements.empty()
        && !model.relinkTileAtlasSources(replacements)) {
        if (!notice.empty()) notice += " ";
        notice += "Atlas references could not be updated.";
    }
    return notice;
}

TilemapSaveResult saveTilemapSourceAndTryCook(
    ayt::ay2d::editor::TilemapEditorModel& model,
    const std::string& path, std::string* error)
{
    TilemapSaveResult result;
    const std::filesystem::path source =
        std::filesystem::absolute(path).lexically_normal();
    const std::filesystem::path assetRoot = tilemapAssetRoot(source);
    if (!assetRoot.empty()) {
        result.notice = makeAtlasSourcesPortable(model, source, assetRoot);
    }
    if (!model.save(path, error)) return result;
    result.sourceSaved = true;
    if (assetRoot.empty()) {
        result.notice = "Authoring source saved. Runtime cooking was skipped "
            "because the file is outside the project Assets folder.";
        if (error != nullptr) error->clear();
        return result;
    }
    ayt::resource::TilemapConverter converter(source.string());
    converter.setOutputDir(assetRoot.string());
    const ayt::resource::ConversionResult cooked = converter.convert();
    const size_t tilemapCount = static_cast<size_t>(std::count_if(
        cooked.resources.begin(), cooked.resources.end(),
        [](const ayt::resource::ConversionResult::ConvertedResource& resource) {
            return resource.type == "Tilemap";
        }));
    if (tilemapCount != 1u) {
        if (!result.notice.empty()) result.notice += " ";
        result.notice += "Authoring source saved, but runtime cooking failed.";
        if (error != nullptr) error->clear();
        return result;
    }
    result.cooked = true;
    if (error != nullptr) error->clear();
    return result;
}

bool hasTilemapSourceExtension(const std::filesystem::path& path)
{
    std::string value = path.filename().string();
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) {
                       return static_cast<char>(std::tolower(ch));
                   });
    constexpr const char* sourceExtension = ".aytilemap.json";
    constexpr const char* legacyExtension = ".aytilemap";
    return (value.size() >= std::strlen(sourceExtension)
            && value.compare(value.size() - std::strlen(sourceExtension),
                             std::strlen(sourceExtension), sourceExtension)
                == 0)
        || (value.size() >= std::strlen(legacyExtension)
            && value.compare(value.size() - std::strlen(legacyExtension),
                             std::strlen(legacyExtension), legacyExtension)
                == 0);
}

std::string showTilemapSaveDialog(const std::string& projectRoot,
                                  const std::string& currentPath)
{
#if defined(_WIN32)
    std::filesystem::path initialDirectory;
    if (!currentPath.empty()) {
        initialDirectory = std::filesystem::path(currentPath).parent_path();
    } else if (!projectRoot.empty()) {
        initialDirectory = std::filesystem::path(projectRoot)
            / "Assets" / "tilemaps";
        std::error_code error;
        std::filesystem::create_directories(initialDirectory, error);
        if (error) {
            initialDirectory = std::filesystem::path(projectRoot) / "Assets";
            error.clear();
            std::filesystem::create_directories(initialDirectory, error);
            if (error) initialDirectory = std::filesystem::path(projectRoot);
        }
    }

    std::array<char, 4096> selected{};
    const std::string suggested = currentPath.empty()
        ? "NewTilemap.aytilemap.json"
        : std::filesystem::path(currentPath).filename().string();
    std::snprintf(selected.data(), selected.size(), "%s", suggested.c_str());
    const std::string initial = initialDirectory.string();

    OPENFILENAMEA dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = ::GetActiveWindow();
    if (dialog.hwndOwner == nullptr) dialog.hwndOwner = ::GetForegroundWindow();
    dialog.lpstrFile = selected.data();
    dialog.nMaxFile = static_cast<DWORD>(selected.size());
    dialog.lpstrInitialDir = initial.empty() ? nullptr : initial.c_str();
    dialog.lpstrFilter =
        "AY Tilemap Source (*.aytilemap.json)\0*.aytilemap.json\0"
        "Legacy AY Tilemap (*.aytilemap)\0*.aytilemap\0"
        "All files (*.*)\0*.*\0";
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT
        | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    dialog.lpstrDefExt = "aytilemap.json";
    if (!::GetSaveFileNameA(&dialog)) return {};

    std::filesystem::path result(selected.data());
    if (!hasTilemapSourceExtension(result)) result += ".aytilemap.json";
    return result.lexically_normal().string();
#else
    (void)projectRoot;
    (void)currentPath;
    return {};
#endif
}

class ToolDocument final : public IEditorDocument {
public:
    ToolDocument(std::string type, std::string title)
        : _type(std::move(type)), _title(std::move(title)) {}
    const std::string& typeId() const noexcept override { return _type; }
    const std::string& path() const noexcept override { return _path; }
    const std::string& title() const noexcept override { return _title; }
    bool isDirty() const noexcept override { return false; }
    uint64_t revision() const noexcept override { return 1u; }
    bool save(std::string* error) override {
        if (error != nullptr) error->clear();
        return true;
    }
private:
    std::string _type;
    std::string _title;
    std::string _path;
};

class TilemapWorkspaceDocument final : public IEditorDocument {
public:
    using SavePathProvider = std::function<std::string(bool saveAs)>;

    bool initialize(const EditorOpenRequest& request, std::string& error)
    {
        _path = request.resourcePath;
        if (_path.empty()) {
            if (!_model.newDocument(32u, 18u, 32u, 32u, 0u)) {
                error = "Could not initialize the tilemap model.";
                return false;
            }
            _title = request.displayPath.empty()
                ? "Untitled Tilemap" : request.displayPath;
            return true;
        }
        if (!_model.open(_path, &error)) return false;
        _title = std::filesystem::path(_path).filename().string();
        return true;
    }
    const std::string& typeId() const noexcept override { return _type; }
    const std::string& path() const noexcept override { return _path; }
    const std::string& title() const noexcept override { return _title; }
    bool isDirty() const noexcept override { return _model.dirty(); }
    uint64_t revision() const noexcept override { return _revision; }
    bool save(std::string* error) override {
        if (_path.empty()) {
            if (_savePathProvider == nullptr) {
                if (error != nullptr) *error = "Tilemap has no file path.";
                return false;
            }
            const std::string selected = _savePathProvider(false);
            if (selected.empty()) {
                if (error != nullptr) *error = "Tilemap save was canceled.";
                return false;
            }
            return saveAs(selected, error);
        }
        const TilemapSaveResult result = saveTilemapSourceAndTryCook(
            _model, _path, error);
        _lastSaveNotice = result.notice;
        if (result.sourceSaved) ++_revision;
        return result.sourceSaved;
    }
    bool canSaveAs() const noexcept override { return true; }
    bool saveAs(const std::string& path, std::string* error) override {
        if (path.empty()) {
            if (error != nullptr) *error = "Tilemap has no file path.";
            return false;
        }
        const TilemapSaveResult result = saveTilemapSourceAndTryCook(
            _model, path, error);
        _lastSaveNotice = result.notice;
        if (result.sourceSaved) {
            _path = path;
            _title = std::filesystem::path(path).filename().string();
            ++_revision;
        }
        return result.sourceSaved;
    }
    bool writeRecoveryCopy(const std::string& path,
                           std::string* error) const override {
        auto snapshot = _model;
        if (snapshot.save(path, error)) return true;
        if (error != nullptr && error->empty()) {
            *error = "Could not write tilemap recovery copy.";
        }
        return false;
    }
    ayt::ay2d::editor::TilemapEditorModel& model() noexcept { return _model; }
    const std::string& lastSaveNotice() const noexcept {
        return _lastSaveNotice;
    }
    void setSavePathProvider(SavePathProvider provider) {
        _savePathProvider = std::move(provider);
    }
    bool hasSavePathProvider() const noexcept {
        return _savePathProvider != nullptr;
    }
    void changed() noexcept { ++_revision; }
private:
    std::string _type = "ayeditor.tilemap.document";
    std::string _path;
    std::string _title = "Untitled Tilemap";
    std::string _lastSaveNotice;
    SavePathProvider _savePathProvider;
    uint64_t _revision = 1u;
    ayt::ay2d::editor::TilemapEditorModel _model;
};

void appendUtf8CodePoint(std::string& output, uint32_t codePoint)
{
    if (codePoint <= 0x7fu) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ffu) {
        output.push_back(static_cast<char>(0xc0u | (codePoint >> 6u)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    } else if (codePoint <= 0xffffu) {
        output.push_back(static_cast<char>(0xe0u | (codePoint >> 12u)));
        output.push_back(static_cast<char>(
            0x80u | ((codePoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    } else {
        output.push_back(static_cast<char>(0xf0u | (codePoint >> 18u)));
        output.push_back(static_cast<char>(
            0x80u | ((codePoint >> 12u) & 0x3fu)));
        output.push_back(static_cast<char>(
            0x80u | ((codePoint >> 6u) & 0x3fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    }
}

std::string encodeUtf8(const std::wstring& text)
{
    std::string output;
    output.reserve(text.size());
    for (size_t index = 0u; index < text.size(); ++index) {
        uint32_t codePoint = static_cast<uint32_t>(text[index]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (codePoint >= 0xd800u && codePoint <= 0xdbffu
                && index + 1u < text.size()) {
                const uint32_t low = static_cast<uint32_t>(text[index + 1u]);
                if (low >= 0xdc00u && low <= 0xdfffu) {
                    codePoint = 0x10000u
                        + ((codePoint - 0xd800u) << 10u)
                        + (low - 0xdc00u);
                    ++index;
                }
            }
        }
        if (codePoint >= 0xd800u && codePoint <= 0xdfffu) {
            codePoint = 0xfffdu;
        }
        appendUtf8CodePoint(
            output, std::min(codePoint, uint32_t{0x10ffffu}));
    }
    return output;
}

bool parseUint32(const std::wstring& text, uint32_t& value)
{
    if (text.empty()) return false;
    wchar_t* end = nullptr;
    const unsigned long long parsed = std::wcstoull(text.c_str(), &end, 0);
    if (end == text.c_str() || *end != L'\0'
        || parsed > (std::numeric_limits<uint32_t>::max)()) {
        return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

bool parseRgba(const std::wstring& text, uint32_t& value)
{
    if (text.size() == 9u && text.front() == L'#') {
        wchar_t* end = nullptr;
        const unsigned long long parsed = std::wcstoull(
            text.c_str() + 1, &end, 16);
        if (end != text.c_str() + text.size()
            || parsed > (std::numeric_limits<uint32_t>::max)()) {
            return false;
        }
        value = static_cast<uint32_t>(parsed);
        return true;
    }
    return parseUint32(text, value);
}

enum class AtlasImportMode : uint8_t {
    Grid,
    Metadata,
    FreeRegions,
};

struct AtlasMetadataDiscovery {
    std::filesystem::path path;
    ayt::ay2d::editor::TileAtlasMetadataParseResult parsed;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(parsed);
    }
};

std::string lowerPathName(const std::filesystem::path& path)
{
    std::string name = path.filename().string();
    std::transform(name.begin(), name.end(), name.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return name;
}

bool metadataImageMatches(const std::filesystem::path& metadataFile,
                          const std::string& declaredImage,
                          const std::filesystem::path& chosenImage)
{
    try {
        std::filesystem::path declared = std::filesystem::u8path(declaredImage);
        if (declared.is_relative()) declared = metadataFile.parent_path() / declared;
        declared = std::filesystem::absolute(declared).lexically_normal();
        std::filesystem::path chosen = std::filesystem::absolute(
            chosenImage).lexically_normal();
        std::string declaredKey = declared.generic_string();
        std::string chosenKey = chosen.generic_string();
        std::transform(declaredKey.begin(), declaredKey.end(),
            declaredKey.begin(), [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
        std::transform(chosenKey.begin(), chosenKey.end(), chosenKey.begin(),
            [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
        return declaredKey == chosenKey;
    } catch (const std::exception&) {
        return false;
    }
}

AtlasMetadataDiscovery discoverAtlasMetadata(
    const std::filesystem::path& imagePath,
    uint32_t imageWidth, uint32_t imageHeight)
{
    constexpr uintmax_t kMaxMetadataBytes = 8u * 1024u * 1024u;
    constexpr size_t kMaxSiblingCandidates = 64u;
    AtlasMetadataDiscovery firstInvalid;
    std::vector<std::filesystem::path> candidates;
    const std::filesystem::path directory = imagePath.parent_path();
    for (const char* extension : {".json", ".tsj", ".tsx"}) {
        std::filesystem::path exact = directory / imagePath.stem();
        exact += extension;
        candidates.push_back(std::move(exact));
    }
    std::error_code scanError;
    for (std::filesystem::directory_iterator iterator(
             directory, std::filesystem::directory_options::skip_permission_denied,
             scanError), end;
         !scanError && iterator != end
         && candidates.size() < kMaxSiblingCandidates;
         iterator.increment(scanError)) {
        if (!iterator->is_regular_file(scanError)) continue;
        const std::string extension = lowerPathName(
            iterator->path().extension());
        if (extension != ".json" && extension != ".tsj"
            && extension != ".tsx") continue;
        const auto duplicate = std::find_if(
            candidates.begin(), candidates.end(),
            [&iterator](const std::filesystem::path& candidate) {
                return lowerPathName(candidate) == lowerPathName(iterator->path());
            });
        if (duplicate == candidates.end()) candidates.push_back(iterator->path());
    }

    for (const std::filesystem::path& candidate : candidates) {
        std::error_code fileError;
        const uintmax_t size = std::filesystem::file_size(candidate, fileError);
        if (fileError || size == 0u || size > kMaxMetadataBytes) continue;
        std::ifstream file(candidate, std::ios::binary);
        if (!file) continue;
        std::string text(static_cast<size_t>(size), '\0');
        if (!file.read(text.data(), static_cast<std::streamsize>(text.size()))) {
            continue;
        }
        auto parsed = ayt::ay2d::editor::parseTileAtlasMetadata(
            text, candidate.extension().string(), imageWidth, imageHeight);
        if (!parsed.recognized || !metadataImageMatches(
                candidate, parsed.metadata.imagePath, imagePath)) continue;
        AtlasMetadataDiscovery discovery{candidate, std::move(parsed)};
        if (discovery) return discovery;
        if (!firstInvalid.parsed.recognized) {
            firstInvalid = std::move(discovery);
        }
    }
    return firstInvalid;
}

class TilemapWorkspaceView final
    : public IEditorView,
      public IEditorCommandTarget,
      public IEditorViewInputTarget {
public:
    TilemapWorkspaceView(std::shared_ptr<TilemapWorkspaceDocument> document,
                         IEditorHostServices& host)
        : _document(std::move(document)), _host(host),
          _iconRoot(EditorProductPaths::detect().engineAssetsRoot
                    / "Icons/Tabler/outline")
    {
        auto* root = new ayt::ui::VBox();
        _root = root;
        root->setId("tilemap_workspace");
        root->setSpacing(6.0f);
        root->setPadding(8.0f, 6.0f, 8.0f, 8.0f);

        auto* commandRow = new ayt::ui::HBox();
        commandRow->setId("tilemap_workspace_command_row");
        commandRow->setSpacing(6.0f);
        auto* toolbar = new ayt::ui::ToolBar();
        toolbar->setId("tilemap_workspace_toolbar");
        _save = addIconButton(
            toolbar, "tilemap_file_save", "device-floppy.svg", L"Save",
            L"Save Tilemap (Ctrl+S)", [this]() {
                (void)saveDocument(false);
            });
        _saveAs = addIconButton(
            toolbar, "tilemap_file_save_as", "file-export.svg", L"Save As",
            L"Save Tilemap As…", [this]() {
                (void)saveDocument(true);
            });
        _selection = addIconButton(
            toolbar, "tilemap_tool_selection", "select.svg", L"M",
            L"Select and move a map region (M)", [this]() {
                setTool(ayt::ay2d::editor::PaintTool::Selection);
            });
        _pencil = addIconButton(
            toolbar, "tilemap_tool_pencil", "pencil.svg", L"P",
            L"Pencil (P)", [this]() {
                setTool(ayt::ay2d::editor::PaintTool::Pencil);
            });
        _eraser = addIconButton(
            toolbar, "tilemap_tool_eraser", "eraser.svg", L"E",
            L"Eraser (E)", [this]() {
                setTool(ayt::ay2d::editor::PaintTool::Eraser);
            });
        _fill = addIconButton(
            toolbar, "tilemap_tool_fill", "bucket.svg", L"F",
            L"Flood Fill (F)", [this]() {
                setTool(ayt::ay2d::editor::PaintTool::FloodFill);
            });
        _rectangle = addIconButton(
            toolbar, "tilemap_tool_rectangle", "rectangle.svg", L"R",
            L"Rectangle (R)", [this]() {
                setTool(ayt::ay2d::editor::PaintTool::Rectangle);
            });
        _terrain = addIconButton(
            toolbar, "tilemap_tool_terrain", "world.svg", L"T",
            L"Auto terrain brush (T)", [this]() {
                if (_document->model().document().terrainDefinition(
                        _document->model().selectedTerrainId()) == nullptr) {
                    _host.setStatusText(
                        L"Create or select an auto-tile terrain first.");
                    return;
                }
                setTool(ayt::ay2d::editor::PaintTool::Terrain);
            });
        _stamp = addIconButton(
            toolbar, "tilemap_tool_stamp", "rubber-stamp.svg", L"S",
            L"Stamp (S)", [this]() {
                if (_document->model().selectedStampId() == 0u) {
                    _host.setStatusText(
                        L"Choose a saved Stamp or create one from Source Sheet.");
                    return;
                }
                setTool(ayt::ay2d::editor::PaintTool::Stamp);
            });
        _shadow = addIconButton(
            toolbar, "tilemap_tool_shadow", "shadow.svg", L"H",
            L"Full shadow (H)", [this]() {
                setShadowBrush(ayt::ay2d::editor::ShadowMask_All);
            });
        _shadowClear = addIconButton(
            toolbar, "tilemap_tool_shadow_clear", "shadow-off.svg", L"0",
            L"Clear shadow", [this]() {
                setShadowBrush(ayt::ay2d::editor::ShadowMask_None);
            });
        _shadowAdvanced = addIconButton(
            toolbar, "tilemap_tool_shadow_advanced", "layout-grid.svg", L"4",
            L"Advanced shadow quadrants…", [this]() {
                openShadowBrushDialog();
            });
        _grid = addIconButton(
            toolbar, "tilemap_tool_grid", "grid.svg", L"G",
            L"Show or hide grid", [this]() {
                _canvas->setShowGrid(!_canvas->showGrid());
                refresh();
                _host.requestRepaint();
            });
        _collision = addIconButton(
            toolbar, "tilemap_tool_collision", "shield.svg", L"C",
            L"Show or hide collision overlay", [this]() {
                _canvas->setShowCollision(!_canvas->showCollision());
                refresh();
                _host.requestRepaint();
            });
        _shadowVisibility = addIconButton(
            toolbar, "tilemap_tool_shadow_visibility", "eye.svg", L"V",
            L"Show or hide shadow overlay", [this]() {
                _canvas->setShowShadows(!_canvas->showShadows());
                refresh();
                _host.requestRepaint();
            });
        _frame = addIconButton(
            toolbar, "tilemap_tool_frame", "frame.svg", L"Home",
            L"Fit entire map in canvas (Home)", [this]() {
                _canvas->frameDocument();
                _host.requestRepaint();
            });
        _summary = new ayt::ui::TextLabel();
        _summary->setFontSize(12);
        ayt::ui::BoxSlotLimits toolbarLimits;
        toolbarLimits.minWidth = 320.0f;
        commandRow->addWidget(toolbar, 0.0f, toolbarLimits);
        ayt::ui::BoxSlotLimits summaryLimits;
        summaryLimits.minWidth = 150.0f;
        commandRow->addWidget(_summary, 270.0f, summaryLimits);
        root->addWidget(commandRow, 36.0f);

        _documentPath = new ayt::ui::TextLabel();
        _documentPath->setId("tilemap_workspace_document_path");
        _documentPath->setFontSize(11);
        root->addWidget(_documentPath, 20.0f);
        {
            auto activeScope = ayt::ui::UIManager::pushActive(
                _host.uiManager());
            if (auto* tooltip = ayt::ui::Tooltip::attachTo(_documentPath)) {
                _documentPathTooltip = tooltip;
                _tooltips.push_back(tooltip);
            }
        }

        _document->setSavePathProvider([this](bool saveAs) {
            if (auto* provider = dynamic_cast<
                    IEditorDocumentSavePathProvider*>(&_host)) {
                const std::string selected =
                    provider->chooseDocumentSavePath(*_document, saveAs);
                if (!selected.empty()) return selected;
            }
            return showTilemapSaveDialog(
                _host.projectRoot(), _document->path());
        });

        auto* body = new ayt::ui::HBox();
        body->setId("tilemap_workspace_body");
        body->setSpacing(0.0f);
        root->addWidget(body, 0.0f);

        auto* assets = new ayt::ui::VBox();
        assets->setId("tilemap_workspace_assets");
        assets->setSpacing(5.0f);
        assets->setPadding(5.0f, 5.0f, 5.0f, 5.0f);
        assets->addWidget(makeLabel(L"Source Sheet", 14), 24.0f);
        _atlasSelector = new ayt::ui::ComboBox();
        _atlasSelector->setId("tilemap_workspace_atlas_selector");
        _atlasSelector->setItems({L"No tile sheet imported"});
        _atlasSelector->setSelectedIndex(0);
        _atlasSelector->setOnSelectionChanged([this](int index) {
            if (_syncing || index < 0
                || index >= static_cast<int>(_atlasIds.size())) return;
            const uint32_t atlasId = _atlasIds[static_cast<size_t>(index)];
            for (const auto& [tileId, asset] :
                 _document->model().document().tileAssets()) {
                if (asset.atlasId == atlasId) {
                    _shownAtlasId = atlasId;
                    selectTile(tileId);
                    return;
                }
            }
            _shownAtlasId = atlasId;
            syncSourcePicker();
            _host.requestRepaint();
        });
        assets->addWidget(_atlasSelector, 28.0f);
        auto* pickerPanel = new ayt::ui::Panel();
        pickerPanel->setPadding(0.0f, 0.0f, 0.0f, 0.0f);
        _atlasPicker = new EditorTileAtlasPicker();
        _atlasPicker->setId("tilemap_workspace_source_sheet");
        _atlasPicker->setOnTileSelected([this](uint32_t tileId) {
            selectTile(tileId);
        });
        _atlasPicker->setOnRectangleSelected(
            [this](const EditorTileAtlasPicker::SourceRect& rectangle) {
                createStampFromSourceRectangle(rectangle);
            });
        _atlasPicker->setOnStatus([this](const std::wstring& text) {
            _host.setStatusText(text);
        });
        pickerPanel->addChild(_atlasPicker);
        assets->addWidget(pickerPanel, 0.0f);
        auto* sourceButtons = new ayt::ui::HBox();
        sourceButtons->setSpacing(4.0f);
        addButton(sourceButtons, L"Import Sheet…", 142.0f, [this]() {
            openAtlasImport();
        })->setId("tilemap_workspace_import_sheet");
        addButton(sourceButtons, L"Fit Sheet", 80.0f, [this]() {
            if (_atlasPicker != nullptr) _atlasPicker->frameAtlas();
            _host.requestRepaint();
        })->setId("tilemap_workspace_fit_sheet");
        assets->addWidget(sourceButtons, 29.0f);
        auto* stampRow = new ayt::ui::HBox();
        stampRow->setSpacing(4.0f);
        _stampSelector = new ayt::ui::ComboBox();
        _stampSelector->setId("tilemap_workspace_stamp_selector");
        _stampSelector->setItems({L"Single tile"});
        _stampSelector->setSelectedIndex(0);
        _stampSelector->setOnSelectionChanged([this](int index) {
            if (_syncing || index < 0
                || index >= static_cast<int>(_stampIds.size())) return;
            if (index == 0) {
                _document->model().setSelectedTileId(
                    _document->model().selectedTileId());
                _document->model().setTool(
                    ayt::ay2d::editor::PaintTool::Pencil);
                refresh();
                _host.requestRepaint();
                return;
            }
            const uint32_t stampId = _stampIds[static_cast<size_t>(index)];
            if (_document->model().setSelectedStampId(stampId)) {
                _document->model().setTool(
                    ayt::ay2d::editor::PaintTool::Stamp);
                refresh();
                _host.requestRepaint();
            }
        });
        stampRow->addWidget(_stampSelector, 0.0f);
        _stampSelect = new ayt::ui::Button();
        _stampSelect->setId("tilemap_workspace_stamp_select");
        _stampSelect->setText(L"Select");
        _stampSelect->setPadding(7.0f, 2.0f, 7.0f, 2.0f);
        _stampSelect->setOnClicked([this]() { toggleStampSelection(); });
        stampRow->addWidget(_stampSelect, 58.0f);
        _stampDelete = new ayt::ui::Button();
        _stampDelete->setText(L"−");
        _stampDelete->setPadding(6.0f, 2.0f, 6.0f, 2.0f);
        _stampDelete->setOnClicked([this]() { deleteSelectedStamp(); });
        stampRow->addWidget(_stampDelete, 34.0f);
        assets->addWidget(stampRow, 29.0f);
        assets->addWidget(makeLabel(L"Tile Assets", 13), 22.0f);
        _tileList = new ayt::ui::ListView();
        _tileList->setId("tilemap_workspace_tile_list");
        _tileList->setItemHeight(25.0f);
        _tileList->setOnSelectionChanged([this](int index) {
            if (_syncing || index < 0
                || index >= static_cast<int>(_tileIds.size())) return;
            _document->model().setSelectedTileId(
                _tileIds[static_cast<size_t>(index)]);
            _document->model().setTool(
                ayt::ay2d::editor::PaintTool::Pencil);
            refresh();
            _host.requestRepaint();
        });
        assets->addWidget(_tileList, 132.0f);
        _tileId = new ayt::ui::TextInput();
        _tileId->setId("tilemap_workspace_tile_id");
        _tileId->setPlaceholder(L"Tile ID (decimal or 0x...)");
        assets->addWidget(_tileId, 27.0f);
        addButton(assets, L"Select / add tile", 30.0f, [this]() {
            uint32_t tileId = 0u;
            if (!parseUint32(_tileId->getText(), tileId)) {
                _host.setStatusText(L"Tile ID must be a 32-bit number.");
                return;
            }
            const bool added = _document->model().ensureTileAsset(tileId);
            _document->model().setSelectedTileId(tileId);
            _document->model().setTool(
                ayt::ay2d::editor::PaintTool::Pencil);
            if (added) _document->changed();
            refresh();
            _host.requestRepaint();
        });
        auto* assetsScroll = new ayt::ui::ScrollView();
        assetsScroll->setId("tilemap_workspace_assets_scroll");
        assetsScroll->setVerticalScrollBarVisibility(
            ayt::ui::ScrollView::ScrollBarVisibility::Auto);
        assetsScroll->setHorizontalScrollBarVisibility(
            ayt::ui::ScrollView::ScrollBarVisibility::Hidden);
        assetsScroll->setContentOwned(assets);
        ayt::ui::BoxSlotLimits assetsLimits;
        assetsLimits.minWidth = 248.0f;
        assetsLimits.maxWidth = 520.0f;
        body->addWidget(assetsScroll, 300.0f, assetsLimits);

        auto* leftSplitter = new ayt::ui::SplitterHandle();
        leftSplitter->setId("tilemap_workspace_left_splitter");
        leftSplitter->setOrientation(
            ayt::ui::SplitterHandle::Orientation::Horizontal);
        body->addWidget(leftSplitter,
                        ayt::ui::SplitterHandle::kDefaultWidth);

        auto* canvasPanel = new ayt::ui::Panel();
        canvasPanel->setId("tilemap_workspace_canvas_panel");
        canvasPanel->setPadding(0.0f, 0.0f, 0.0f, 0.0f);
        _canvas = new EditorTilemapCanvas(_document->model());
        _canvas->setAtlasTextures(&_atlasTextures);
        _canvas->setOnEdited([this]() {
            _document->changed();
            refresh();
            _host.requestRepaint();
        });
        _canvas->setOnTilePicked([this](uint32_t tileId) {
            _document->model().setSelectedTileId(tileId);
            _document->model().setTool(
                ayt::ay2d::editor::PaintTool::Pencil);
            refresh();
            _host.requestRepaint();
        });
        _canvas->setOnViewChanged([this](float zoom) {
            _zoomPercent = zoom;
            updateSummary();
        });
        canvasPanel->addChild(_canvas);
        ayt::ui::BoxSlotLimits canvasLimits;
        canvasLimits.minWidth = 360.0f;
        body->addWidget(canvasPanel, 0.0f, canvasLimits);

        auto* rightSplitter = new ayt::ui::SplitterHandle();
        rightSplitter->setId("tilemap_workspace_right_splitter");
        rightSplitter->setOrientation(
            ayt::ui::SplitterHandle::Orientation::Horizontal);
        body->addWidget(rightSplitter,
                        ayt::ui::SplitterHandle::kDefaultWidth);

        auto* inspector = new ayt::ui::VBox();
        inspector->setId("tilemap_workspace_inspector");
        inspector->setSpacing(5.0f);
        inspector->setPadding(5.0f, 5.0f, 5.0f, 5.0f);
        inspector->addWidget(makeLabel(L"Render Layers", 14), 24.0f);
        _layerList = new ayt::ui::ListView();
        _layerList->setId("tilemap_workspace_layer_list");
        _layerList->setItemHeight(25.0f);
        _layerList->setOnSelectionChanged([this](int index) {
            if (_syncing || index < 0) return;
            if (_document->model().setActiveLayer(
                    static_cast<size_t>(index))) {
                refresh();
                _host.requestRepaint();
            }
        });
        inspector->addWidget(_layerList, 112.0f);
        auto* layerButtons = new ayt::ui::HBox();
        layerButtons->setSpacing(3.0f);
        addIconButton(
            layerButtons, "tilemap_layer_add", "plus.svg", L"+",
            L"Add render layer", [this]() {
                const size_t number =
                    _document->model().document().layerCount() + 1u;
                mutate(_document->model().addLayer(
                    "Layer " + std::to_string(number)));
            });
        addIconButton(
            layerButtons, "tilemap_layer_remove", "minus.svg", L"−",
            L"Remove active render layer", [this]() {
                mutate(_document->model().removeLayer(activeLayer()));
            });
        addIconButton(
            layerButtons, "tilemap_layer_up", "arrow-up.svg", L"↑",
            L"Move active layer up", [this]() {
                mutate(_document->model().moveLayer(activeLayer(), 1));
            });
        addIconButton(
            layerButtons, "tilemap_layer_down", "arrow-down.svg", L"↓",
            L"Move active layer down", [this]() {
                mutate(_document->model().moveLayer(activeLayer(), -1));
            });
        _layerVisibility = addIconButton(
            layerButtons, "tilemap_layer_visibility", "eye.svg", L"V",
            L"Show or hide active layer", [this]() {
                const size_t layer = activeLayer();
                const auto& layers = _document->model().document().layers();
                if (layer < layers.size()) {
                    mutate(_document->model().setLayerVisible(
                        layer, !layers[layer].visible));
                }
            });
        inspector->addWidget(layerButtons, 32.0f);
        _layerName = new ayt::ui::TextInput();
        _layerName->setId("tilemap_workspace_layer_name");
        _layerName->setPlaceholder(L"Active layer name");
        auto* layerNameRow = new ayt::ui::HBox();
        layerNameRow->setSpacing(3.0f);
        layerNameRow->addWidget(_layerName, 0.0f);
        addIconButton(
            layerNameRow, "tilemap_layer_rename", "edit.svg", L"R",
            L"Rename active layer", [this]() {
                mutate(_document->model().renameLayer(
                    activeLayer(), encodeUtf8(_layerName->getText())));
            });
        inspector->addWidget(layerNameRow, 32.0f);
        inspector->addWidget(makeLabel(L"Selection", 14), 24.0f);
        auto* selectionButtons = new ayt::ui::HBox();
        selectionButtons->setSpacing(3.0f);
        addIconButton(
            selectionButtons, "tilemap_selection_flip_h",
            "flip-horizontal.svg", L"H", L"Flip selection horizontally",
            [this]() { mutate(_document->model().flipSelectionHorizontal()); });
        addIconButton(
            selectionButtons, "tilemap_selection_flip_v",
            "flip-vertical.svg", L"V", L"Flip selection vertically",
            [this]() { mutate(_document->model().flipSelectionVertical()); });
        addIconButton(
            selectionButtons, "tilemap_selection_rotate",
            "rotate-clockwise.svg", L"R", L"Rotate selection clockwise",
            [this]() { mutate(_document->model().rotateSelectionClockwise()); });
        addIconButton(
            selectionButtons, "tilemap_selection_stamp",
            "rubber-stamp.svg", L"S", L"Save selection as a reusable Stamp",
            [this]() { saveSelectionAsStamp(); });
        inspector->addWidget(selectionButtons, 32.0f);
        inspector->addWidget(makeLabel(L"Tile Animation", 14), 24.0f);
        _animationFrameList = new ayt::ui::ListView();
        _animationFrameList->setId("tilemap_animation_frames");
        _animationFrameList->setItemHeight(24.0f);
        _animationFrameList->setOnSelectionChanged([this](int index) {
            if (_syncing || index < 0) return;
            const auto frames = selectedAnimationFrames();
            if (index >= static_cast<int>(frames.size())) return;
            _animationFrameIndex = index;
            _animationTile->setText(std::to_wstring(
                frames[static_cast<size_t>(index)].tileId));
            _animationDuration->setText(std::to_wstring(
                frames[static_cast<size_t>(index)].durationMs));
        });
        inspector->addWidget(_animationFrameList, 72.0f);
        auto* animationButtons = new ayt::ui::HBox();
        animationButtons->setSpacing(3.0f);
        addButton(animationButtons, L"+", 34.0f,
                  [this]() { addAnimationFrame(); });
        addButton(animationButtons, L"−", 34.0f,
                  [this]() { removeAnimationFrame(); });
        addButton(animationButtons, L"↑", 34.0f,
                  [this]() { moveAnimationFrame(-1); });
        addButton(animationButtons, L"↓", 34.0f,
                  [this]() { moveAnimationFrame(1); });
        _animationPreview = addButton(
            animationButtons, L"Pause", 72.0f, [this]() {
                _canvas->setAnimationPreviewEnabled(
                    !_canvas->animationPreviewEnabled());
                refresh();
                _host.requestRepaint();
            });
        inspector->addWidget(animationButtons, 30.0f);
        auto* animationEdit = new ayt::ui::HBox();
        animationEdit->setSpacing(3.0f);
        _animationTile = new ayt::ui::TextInput();
        _animationTile->setPlaceholder(L"Tile");
        animationEdit->addWidget(_animationTile, 72.0f);
        _animationDuration = new ayt::ui::TextInput();
        _animationDuration->setPlaceholder(L"Duration ms");
        animationEdit->addWidget(_animationDuration, 92.0f);
        addButton(animationEdit, L"Apply", 62.0f,
                  [this]() { applyAnimationFrame(); });
        inspector->addWidget(animationEdit, 30.0f);

        inspector->addWidget(makeLabel(L"Auto Tile Terrain", 14), 24.0f);
        _terrainList = new ayt::ui::ListView();
        _terrainList->setId("tilemap_terrain_list");
        _terrainList->setItemHeight(24.0f);
        _terrainList->setOnSelectionChanged([this](int index) {
            if (_syncing || index < 0
                || index >= static_cast<int>(_terrainIds.size())) return;
            const uint32_t id = _terrainIds[static_cast<size_t>(index)];
            _document->model().setSelectedTerrainId(id);
            _editingNewTerrain = false;
            _terrainRuleIndex = -1;
            if (const auto* terrain =
                    _document->model().document().terrainDefinition(id)) {
                _terrainRuleDraft = terrain->rules;
                _terrainDraftSourceId = id;
            }
            refresh();
        });
        inspector->addWidget(_terrainList, 72.0f);
        auto* terrainIdentity = new ayt::ui::HBox();
        terrainIdentity->setSpacing(3.0f);
        _terrainId = new ayt::ui::TextInput();
        _terrainId->setPlaceholder(L"ID");
        terrainIdentity->addWidget(_terrainId, 56.0f);
        _terrainName = new ayt::ui::TextInput();
        _terrainName->setPlaceholder(L"Terrain name");
        terrainIdentity->addWidget(_terrainName, 0.0f);
        inspector->addWidget(terrainIdentity, 30.0f);
        _terrainFallback = new ayt::ui::TextInput();
        _terrainFallback->setPlaceholder(L"Fallback tile ID");
        inspector->addWidget(_terrainFallback, 28.0f);
        _terrainRuleList = new ayt::ui::ListView();
        _terrainRuleList->setId("tilemap_terrain_rule_list");
        _terrainRuleList->setItemHeight(24.0f);
        _terrainRuleList->setOnSelectionChanged([this](int index) {
            if (_syncing || index < 0
                || index >= static_cast<int>(_terrainRuleDraft.size())) return;
            _terrainRuleIndex = index;
            const auto& rule =
                _terrainRuleDraft[static_cast<size_t>(index)];
            _terrainRuleMatchMask = rule.matchMask;
            _terrainRuleNeighborMask = rule.neighborMask;
            _terrainRuleTile->setText(std::to_wstring(rule.tileId));
            syncTerrainNeighborButtons();
        });
        inspector->addWidget(_terrainRuleList, 72.0f);
        auto addNeighbor = [this](ayt::ui::HBox* row,
                                  const wchar_t* label, uint8_t bit) {
            auto* button = addButton(row, label, 0.0f, [this, bit]() {
                cycleTerrainNeighbor(bit);
            });
            _terrainNeighborButtons[bit] = button;
        };
        auto* terrainNorth = new ayt::ui::HBox();
        terrainNorth->setSpacing(3.0f);
        addNeighbor(terrainNorth, L"NW *", 7u);
        addNeighbor(terrainNorth, L"N *", 0u);
        addNeighbor(terrainNorth, L"NE *", 1u);
        inspector->addWidget(terrainNorth, 29.0f);
        auto* terrainMiddle = new ayt::ui::HBox();
        terrainMiddle->setSpacing(3.0f);
        addNeighbor(terrainMiddle, L"W *", 6u);
        terrainMiddle->addWidget(makeLabel(L"CENTER", 10), 0.0f);
        addNeighbor(terrainMiddle, L"E *", 2u);
        inspector->addWidget(terrainMiddle, 29.0f);
        auto* terrainSouth = new ayt::ui::HBox();
        terrainSouth->setSpacing(3.0f);
        addNeighbor(terrainSouth, L"SW *", 5u);
        addNeighbor(terrainSouth, L"S *", 4u);
        addNeighbor(terrainSouth, L"SE *", 3u);
        inspector->addWidget(terrainSouth, 29.0f);
        auto* terrainRuleEdit = new ayt::ui::HBox();
        terrainRuleEdit->setSpacing(3.0f);
        _terrainRuleTile = new ayt::ui::TextInput();
        _terrainRuleTile->setPlaceholder(L"Output tile");
        terrainRuleEdit->addWidget(_terrainRuleTile, 0.0f);
        addButton(terrainRuleEdit, L"New", 48.0f,
                  [this]() { beginTerrainRule(); });
        addButton(terrainRuleEdit, L"Apply", 54.0f,
                  [this]() { applyTerrainRule(); });
        addButton(terrainRuleEdit, L"−", 34.0f,
                  [this]() { removeTerrainRule(); });
        inspector->addWidget(terrainRuleEdit, 30.0f);
        auto* terrainActions = new ayt::ui::HBox();
        terrainActions->setSpacing(3.0f);
        addButton(terrainActions, L"New Terrain", 92.0f,
                  [this]() { beginTerrain(); });
        addButton(terrainActions, L"Save Terrain", 92.0f,
                  [this]() { saveTerrain(); });
        addButton(terrainActions, L"Delete", 58.0f,
                  [this]() { deleteTerrain(); });
        inspector->addWidget(terrainActions, 30.0f);
        auto* terrainHint = makeLabel(
            L"Neighbor button: * ignore → 1 same → 0 different", 10);
        terrainHint->setWordWrap(true);
        inspector->addWidget(terrainHint, 32.0f);
        inspector->addWidget(makeLabel(L"Map Shadow Color · #RRGGBBAA", 13),
                             23.0f);
        _shadowColor = new ayt::ui::TextInput();
        _shadowColor->setId("tilemap_workspace_shadow_color");
        _shadowColor->setPlaceholder(L"0x00000080");
        inspector->addWidget(_shadowColor, 27.0f);
        addButton(inspector, L"Apply shadow color", 29.0f, [this]() {
            uint32_t rgba = 0u;
            if (!parseRgba(_shadowColor->getText(), rgba)) {
                _host.setStatusText(
                    L"Shadow color must be #RRGGBBAA or 0xRRGGBBAA.");
                return;
            }
            mutate(_document->model().setShadowColor(rgba));
        })->setId("tilemap_workspace_shadow_color_apply");
        inspector->addWidget(makeLabel(L"Selected Tile Collision", 13), 23.0f);
        _collisionFlags = new ayt::ui::TextInput();
        _collisionFlags->setId("tilemap_workspace_collision_flags");
        _collisionFlags->setPlaceholder(L"Flags: 0 or 0x...");
        inspector->addWidget(_collisionFlags, 27.0f);
        addButton(inspector, L"Apply collision flags", 29.0f, [this]() {
            uint32_t flags = 0u;
            if (!parseUint32(_collisionFlags->getText(), flags)) {
                _host.setStatusText(
                    L"Collision flags must be a 32-bit number.");
                return;
            }
            mutate(_document->model().setCollisionFlags(
                _document->model().selectedTileId(), flags));
        });
        auto* help = makeLabel(
            L"Wheel: zoom\nMiddle drag or Space + left drag: pan\n"
            L"Right click: pick visible tile", 11);
        help->setWordWrap(true);
        inspector->addWidget(help, 60.0f);
        auto* inspectorScroll = new ayt::ui::ScrollView();
        inspectorScroll->setId("tilemap_workspace_inspector_scroll");
        inspectorScroll->setVerticalScrollBarVisibility(
            ayt::ui::ScrollView::ScrollBarVisibility::Auto);
        inspectorScroll->setHorizontalScrollBarVisibility(
            ayt::ui::ScrollView::ScrollBarVisibility::Hidden);
        inspectorScroll->setContentOwned(inspector);
        ayt::ui::BoxSlotLimits inspectorLimits;
        inspectorLimits.minWidth = 238.0f;
        inspectorLimits.maxWidth = 440.0f;
        body->addWidget(inspectorScroll, 280.0f, inspectorLimits);

        loadSavedAtlasImages();
        refresh();
    }
    ~TilemapWorkspaceView() override {
        _document->setSavePathProvider({});
        clearTooltips();
        if (_root != nullptr) ayt::ui::destroyWidgetTree(_root);
    }
    ayt::ui::Widget* rootWidget() noexcept override { return _root; }
    ayt::ui::Widget* releaseRootWidget() noexcept override {
        auto* result = _root; _root = nullptr; return result;
    }
    IEditorCommandTarget* commandTarget() noexcept override { return this; }
    IEditorViewInputTarget* inputTarget() noexcept override { return this; }
    void tick(float) override {
        if (!_importCommitPending) return;
        auto activeScope = ayt::ui::UIManager::pushActive(_host.uiManager());
        _importCommitPending = false;
        commitAtlasImport();
    }
    void prepareForUiShutdown() override {
        auto activeScope = ayt::ui::UIManager::pushActive(_host.uiManager());
        _importCommitPending = false;
        _document->setSavePathProvider({});
        clearTooltips();
        if (_importCancel != nullptr) _importCancel->setOnClicked({});
        if (_importCommit != nullptr) _importCommit->setOnClicked({});
        if (_skipEmpty != nullptr) _skipEmpty->setOnClicked({});
        if (_importChoose != nullptr) _importChoose->setOnClicked({});
        if (_importModeButton != nullptr) _importModeButton->setOnClicked({});
        if (_importRegionUndo != nullptr) _importRegionUndo->setOnClicked({});
        if (_importRegionClear != nullptr) _importRegionClear->setOnClicked({});
        if (_stampSelect != nullptr) _stampSelect->setOnClicked({});
        if (_stampDelete != nullptr) _stampDelete->setOnClicked({});
        for (ayt::ui::TextInput* input : _importInputs) {
            if (input != nullptr) input->setOnTextChanged({});
        }
        if (_importPreview != nullptr) {
            _importPreview->setOnRectangleSelected({});
        }
        if (_importModal != nullptr && _importModal->isOpen()) {
            _importModal->closeModal();
        }
        _importModal.reset();
        for (ayt::ui::Button* button : _shadowQuadrantButtons) {
            if (button != nullptr) button->setOnClicked({});
        }
        if (_shadowDialogCancel != nullptr) {
            _shadowDialogCancel->setOnClicked({});
        }
        if (_shadowDialogApply != nullptr) {
            _shadowDialogApply->setOnClicked({});
        }
        if (_shadowModal != nullptr && _shadowModal->isOpen()) {
            _shadowModal->closeModal();
        }
        _shadowModal.reset();
        if (_canvas != nullptr) {
            _canvas->setOnEdited({});
            _canvas->setOnTilePicked({});
            _canvas->setOnViewChanged({});
        }
        if (_atlasPicker != nullptr) {
            _atlasPicker->setOnTileSelected({});
            _atlasPicker->setOnStatus({});
            _atlasPicker->setOnRectangleSelected({});
        }
        if (_tileList != nullptr) _tileList->setOnSelectionChanged({});
        if (_atlasSelector != nullptr) {
            _atlasSelector->setOnSelectionChanged({});
        }
        if (_stampSelector != nullptr) {
            _stampSelector->setOnSelectionChanged({});
        }
        if (_layerList != nullptr) _layerList->setOnSelectionChanged({});
        if (_animationFrameList != nullptr) {
            _animationFrameList->setOnSelectionChanged({});
        }
        if (_terrainList != nullptr) {
            _terrainList->setOnSelectionChanged({});
        }
        if (_terrainRuleList != nullptr) {
            _terrainRuleList->setOnSelectionChanged({});
        }
        for (ayt::ui::Button* button : _buttons) {
            if (button != nullptr) button->setOnClicked({});
        }
        _canvas = nullptr;
        _atlasPicker = nullptr;
        _tileList = nullptr;
        _layerList = nullptr;
        _animationFrameList = nullptr;
        _terrainList = nullptr;
        _terrainRuleList = nullptr;
        _tileId = nullptr;
        _layerName = nullptr;
        _collisionFlags = nullptr;
        _shadowColor = nullptr;
        _animationTile = nullptr;
        _animationDuration = nullptr;
        _terrainId = nullptr;
        _terrainName = nullptr;
        _terrainFallback = nullptr;
        _terrainRuleTile = nullptr;
        _summary = nullptr;
        _documentPath = nullptr;
        _documentPathTooltip = nullptr;
        _atlasSelector = nullptr;
        _save = nullptr;
        _saveAs = nullptr;
        _selection = nullptr;
        _pencil = nullptr;
        _eraser = nullptr;
        _fill = nullptr;
        _rectangle = nullptr;
        _terrain = nullptr;
        _stamp = nullptr;
        _shadow = nullptr;
        _shadowClear = nullptr;
        _shadowAdvanced = nullptr;
        _shadowQuadrantButtons.fill(nullptr);
        _shadowDialogStatus = nullptr;
        _shadowDialogCancel = nullptr;
        _shadowDialogApply = nullptr;
        _grid = nullptr;
        _collision = nullptr;
        _shadowVisibility = nullptr;
        _frame = nullptr;
        _layerVisibility = nullptr;
        _animationPreview = nullptr;
        _terrainNeighborButtons.fill(nullptr);
        _importPreview = nullptr;
        _importSourceLabel = nullptr;
        _importInfo = nullptr;
        _importTileWidth = nullptr;
        _importTileHeight = nullptr;
        _importMarginX = nullptr;
        _importMarginY = nullptr;
        _importSpacingX = nullptr;
        _importSpacingY = nullptr;
        _importFirstTileId = nullptr;
        _importFolder = nullptr;
        _importChoose = nullptr;
        _importCancel = nullptr;
        _importCommit = nullptr;
        _skipEmpty = nullptr;
        _importModeButton = nullptr;
        _importRegionUndo = nullptr;
        _importRegionClear = nullptr;
        _stampSelector = nullptr;
        _stampSelect = nullptr;
        _stampDelete = nullptr;
        _importGridInputs.clear();
        _importInputs.clear();
        _buttons.clear();
    }
    bool handlesCommand(const std::string& id) const override {
        return id == "file.save" || id == "file.save_as"
            || id == "edit.undo" || id == "edit.redo"
            || id == "edit.delete";
    }
    bool canExecuteCommand(const std::string& id) const override {
        if (id == "edit.undo") return _document->model().canUndo();
        if (id == "edit.redo") return _document->model().canRedo();
        if (id == "edit.delete") {
            return _document->model().selection().valid;
        }
        return (id == "file.save" || id == "file.save_as")
            && _document->hasSavePathProvider();
    }
    bool executeCommand(const std::string& id) override {
        if (!canExecuteCommand(id)) return false;
        bool changed = false;
        if (id == "edit.undo") {
            changed = _document->model().undo();
            if (changed) _document->changed();
        } else if (id == "edit.redo") {
            changed = _document->model().redo();
            if (changed) _document->changed();
        } else if (id == "edit.delete") {
            changed = _document->model().deleteSelection();
            if (changed) _document->changed();
        } else {
            return saveDocument(id == "file.save_as");
        }
        if (changed) {
            refresh();
            _host.requestRepaint();
        }
        return changed;
    }

    bool onPointerDown(float, float, int) override { return false; }
    bool onPointerMove(float, float) override { return false; }
    bool onPointerUp(float, float, int) override { return false; }
    bool onWheel(float, float, float) override { return false; }
    bool onKeyDown(int keyCode) override {
        if (keyCode == ayt::ui::UIKey_Control) {
            _ctrlDown = true;
            return false;
        }
        if (keyCode == ayt::ui::UIKey_Shift) {
            _shiftDown = true;
            return false;
        }
        if (textInputFocused()) return false;
        if (_ctrlDown && keyCode == ayt::ui::UIKey_C) {
            _host.setStatusText(_document->model().copySelection()
                ? L"Tilemap selection copied." : L"Nothing selected.");
            return true;
        }
        if (_ctrlDown && keyCode == ayt::ui::UIKey_X) {
            mutate(_document->model().cutSelection());
            return true;
        }
        if (_ctrlDown && keyCode == ayt::ui::UIKey_V) {
            mutate(_document->model().pasteSelection());
            return true;
        }
        switch (keyCode) {
        case ayt::ui::UIKey_M:
            setTool(ayt::ay2d::editor::PaintTool::Selection);
            return true;
        case ayt::ui::UIKey_P:
            setTool(ayt::ay2d::editor::PaintTool::Pencil);
            return true;
        case ayt::ui::UIKey_E:
            setTool(ayt::ay2d::editor::PaintTool::Eraser);
            return true;
        case ayt::ui::UIKey_F:
            setTool(ayt::ay2d::editor::PaintTool::FloodFill);
            return true;
        case ayt::ui::UIKey_R:
            setTool(ayt::ay2d::editor::PaintTool::Rectangle);
            return true;
        case ayt::ui::UIKey_T:
            if (_document->model().document().terrainDefinition(
                    _document->model().selectedTerrainId()) != nullptr) {
                setTool(ayt::ay2d::editor::PaintTool::Terrain);
            } else {
                _host.setStatusText(
                    L"Create or select an auto-tile terrain first.");
            }
            return true;
        case ayt::ui::UIKey_S:
            if (_document->model().selectedStampId() != 0u) {
                setTool(ayt::ay2d::editor::PaintTool::Stamp);
            } else {
                _host.setStatusText(
                    L"Choose a saved Stamp or create one from Source Sheet.");
            }
            return true;
        case ayt::ui::UIKey_H:
            setShadowBrush(ayt::ay2d::editor::ShadowMask_All);
            return true;
        case ayt::ui::UIKey_Space:
            if (_canvas != nullptr) _canvas->setSpacePan(true);
            return true;
        case ayt::ui::UIKey_Home:
            if (_canvas != nullptr) _canvas->frameDocument();
            _host.requestRepaint();
            return true;
        case ayt::ui::UIKey_Left:
            mutate(_document->model().moveSelection(-1, 0));
            return true;
        case ayt::ui::UIKey_Right:
            mutate(_document->model().moveSelection(1, 0));
            return true;
        case ayt::ui::UIKey_Up:
            mutate(_document->model().moveSelection(0, 1));
            return true;
        case ayt::ui::UIKey_Down:
            mutate(_document->model().moveSelection(0, -1));
            return true;
        default:
            return false;
        }
    }
    void onKeyUp(int keyCode) override {
        if (keyCode == ayt::ui::UIKey_Control) _ctrlDown = false;
        if (keyCode == ayt::ui::UIKey_Shift) _shiftDown = false;
        if (keyCode == ayt::ui::UIKey_Space && _canvas != nullptr) {
            _canvas->setSpacePan(false);
            _host.requestRepaint();
        }
    }
    bool hasPointerCapture() const noexcept override {
        return _canvas != nullptr && _canvas->editingGestureActive();
    }
    ayt::ui::UiCursorHint cursorHint(float x, float y) const override {
        if (_canvas == nullptr
            || !_canvas->getWorldBounds().contains({x, y})) {
            return ayt::ui::UiCursorHint::Default;
        }
        return _canvas->getCursorHint();
    }

private:
    static ayt::ui::TextLabel* makeLabel(const std::wstring& text,
                                         int size)
    {
        auto* label = new ayt::ui::TextLabel();
        label->setText(text);
        label->setFontSize(size);
        return label;
    }

    ayt::ui::Button* addButton(ayt::ui::HBox* parent,
                               const std::wstring& text, float width,
                               std::function<void()> clicked)
    {
        auto* button = new ayt::ui::Button();
        button->setText(text);
        button->setPadding(7.0f, 2.0f, 7.0f, 2.0f);
        button->setOnClicked(std::move(clicked));
        parent->addWidget(button, width);
        _buttons.push_back(button);
        return button;
    }

    ayt::ui::Button* addButton(ayt::ui::VBox* parent,
                               const std::wstring& text, float height,
                               std::function<void()> clicked)
    {
        auto* button = new ayt::ui::Button();
        button->setText(text);
        button->setPadding(7.0f, 2.0f, 7.0f, 2.0f);
        button->setOnClicked(std::move(clicked));
        parent->addWidget(button, height);
        _buttons.push_back(button);
        return button;
    }

    ayt::ui::Button* addIconButton(
        ayt::ui::ToolBar* parent, const char* id,
        const std::filesystem::path& iconName,
        const std::wstring& fallbackText,
        const std::wstring& accessibleLabel,
        std::function<void()> clicked)
    {
        auto* button = parent->addButton(fallbackText, std::move(clicked));
        button->setId(id);
        button->setAccessibilityLabel(accessibleLabel);
        button->setPadding(6.0f, 4.0f, 6.0f, 4.0f);
        button->setSize({32.0f, 28.0f});
        std::string error;
        if (auto icon = ayt::ui::SvgDocument::loadFromFile(
                _iconRoot / iconName, &error)) {
            button->setText(L"");
            button->setIconDocument(std::move(icon));
            button->setIconSize(18.0f);
            button->setIconColor(inactiveIconColor());
        }
        _buttons.push_back(button);
        auto activeScope = ayt::ui::UIManager::pushActive(_host.uiManager());
        if (auto* tooltip = ayt::ui::Tooltip::attachTo(button)) {
            tooltip->setText(accessibleLabel);
            _tooltips.push_back(tooltip);
        }
        return button;
    }

    ayt::ui::Button* addIconButton(
        ayt::ui::HBox* parent, const char* id,
        const std::filesystem::path& iconName,
        const std::wstring& fallbackText,
        const std::wstring& accessibleLabel,
        std::function<void()> clicked)
    {
        auto* button = new ayt::ui::Button();
        button->setId(id);
        button->setText(fallbackText);
        button->setAccessibilityLabel(accessibleLabel);
        button->setPadding(6.0f, 4.0f, 6.0f, 4.0f);
        button->setOnClicked(std::move(clicked));
        std::string error;
        if (auto icon = ayt::ui::SvgDocument::loadFromFile(
                _iconRoot / iconName, &error)) {
            button->setText(L"");
            button->setIconDocument(std::move(icon));
            button->setIconSize(18.0f);
            button->setIconColor(inactiveIconColor());
        }
        parent->addWidget(button, 32.0f);
        _buttons.push_back(button);
        auto activeScope = ayt::ui::UIManager::pushActive(_host.uiManager());
        if (auto* tooltip = ayt::ui::Tooltip::attachTo(button)) {
            tooltip->setText(accessibleLabel);
            _tooltips.push_back(tooltip);
        }
        return button;
    }

    static ayt::math::FVector4 inactiveIconColor() noexcept {
        return {0.82f, 0.85f, 0.90f, 1.0f};
    }

    static ayt::math::FVector4 activeIconColor() noexcept {
        return {0.28f, 0.66f, 1.0f, 1.0f};
    }

    static void setIconState(ayt::ui::Button* button, bool active,
                             const std::wstring& label)
    {
        if (button == nullptr) return;
        button->setIconColor(
            active ? activeIconColor() : inactiveIconColor());
        button->setAccessibilityLabel(
            active ? label + L", active" : label);
    }

    void clearTooltips() noexcept {
        if (_tooltips.empty()) return;
        auto activeScope = ayt::ui::UIManager::pushActive(_host.uiManager());
        for (ayt::ui::Tooltip* tooltip : _tooltips) {
            if (tooltip == nullptr) continue;
            tooltip->detach();
            ayt::ui::destroyWidgetTree(tooltip);
        }
        _tooltips.clear();
    }

    void setShadowBrush(uint8_t mask)
    {
        _document->model().setSelectedShadowMask(mask);
        setTool(ayt::ay2d::editor::PaintTool::Shadow);
    }

    void refreshShadowBrushDialog()
    {
        static constexpr std::array<const wchar_t*, 4> quadrantNames{
            L"Top-left", L"Top-right", L"Bottom-left", L"Bottom-right"};
        static constexpr std::array<uint8_t, 4> quadrantBits{
            ayt::ay2d::editor::ShadowMask_TopLeft,
            ayt::ay2d::editor::ShadowMask_TopRight,
            ayt::ay2d::editor::ShadowMask_BottomLeft,
            ayt::ay2d::editor::ShadowMask_BottomRight};
        for (size_t index = 0u; index < quadrantBits.size(); ++index) {
            ayt::ui::Button* button = _shadowQuadrantButtons[index];
            if (button == nullptr) continue;
            const bool included = (_pendingShadowMask
                                   & quadrantBits[index]) != 0u;
            button->setText(included ? L"ON" : L"OFF");
            button->setAccessibilityLabel(
                std::wstring(quadrantNames[index]) + L" quadrant, "
                + (included ? L"included" : L"excluded"));
        }
        if (_shadowDialogStatus != nullptr) {
            if (_pendingShadowMask == ayt::ay2d::editor::ShadowMask_All) {
                _shadowDialogStatus->setText(L"Brush: full-cell shadow");
            } else if (_pendingShadowMask
                       == ayt::ay2d::editor::ShadowMask_None) {
                _shadowDialogStatus->setText(L"Brush: clear shadow");
            } else {
                _shadowDialogStatus->setText(L"Brush: custom quadrants");
            }
        }
        _host.requestRepaint();
    }

    void buildShadowBrushDialog()
    {
        if (_shadowModal != nullptr) return;
        _shadowModal = std::make_unique<ayt::ui::Modal>();
        _shadowModal->setId("tilemap_shadow_brush_dialog");
        _shadowModal->setSize({360.0f, 278.0f});
        _shadowModal->setDismissOnDimmerClick(false);

        auto* root = new ayt::ui::VBox();
        root->setStyleId("panel_default");
        root->setPadding(16.0f, 14.0f, 16.0f, 14.0f);
        root->setSpacing(8.0f);
        root->addWidget(makeLabel(L"Advanced Shadow Brush", 17), 28.0f);
        auto* note = makeLabel(
            L"Click a quadrant to include or exclude that part of the cell.",
            11);
        note->setWordWrap(true);
        root->addWidget(note, 36.0f);

        static constexpr std::array<const char*, 4> quadrantIds{
            "tilemap_shadow_quadrant_tl", "tilemap_shadow_quadrant_tr",
            "tilemap_shadow_quadrant_bl", "tilemap_shadow_quadrant_br"};
        static constexpr std::array<uint8_t, 4> quadrantBits{
            ayt::ay2d::editor::ShadowMask_TopLeft,
            ayt::ay2d::editor::ShadowMask_TopRight,
            ayt::ay2d::editor::ShadowMask_BottomLeft,
            ayt::ay2d::editor::ShadowMask_BottomRight};
        for (size_t rowIndex = 0u; rowIndex < 2u; ++rowIndex) {
            auto* row = new ayt::ui::HBox();
            row->setSpacing(6.0f);
            for (size_t columnIndex = 0u; columnIndex < 2u;
                 ++columnIndex) {
                const size_t index = rowIndex * 2u + columnIndex;
                auto* button = new ayt::ui::Button();
                button->setId(quadrantIds[index]);
                button->setPadding(8.0f, 6.0f, 8.0f, 6.0f);
                button->setOnClicked([this, bit = quadrantBits[index]]() {
                    _pendingShadowMask ^= bit;
                    refreshShadowBrushDialog();
                });
                row->addWidget(button, 0.0f);
                _shadowQuadrantButtons[index] = button;
            }
            root->addWidget(row, 48.0f);
        }

        _shadowDialogStatus = makeLabel(L"Brush: full-cell shadow", 11);
        _shadowDialogStatus->setId("tilemap_shadow_brush_status");
        root->addWidget(_shadowDialogStatus, 22.0f);

        auto* footer = new ayt::ui::HBox();
        footer->setSpacing(8.0f);
        footer->addWidget(makeLabel(L"", 11), 0.0f);
        _shadowDialogCancel = new ayt::ui::Button();
        _shadowDialogCancel->setId("tilemap_shadow_brush_cancel");
        _shadowDialogCancel->setText(L"Cancel");
        _shadowDialogCancel->setPadding(7.0f, 2.0f, 7.0f, 2.0f);
        _shadowDialogCancel->setOnClicked([this]() {
            if (_shadowModal != nullptr) _shadowModal->closeModal();
        });
        footer->addWidget(_shadowDialogCancel, 86.0f);
        _shadowDialogApply = new ayt::ui::Button();
        _shadowDialogApply->setId("tilemap_shadow_brush_apply");
        _shadowDialogApply->setText(L"Use Brush");
        _shadowDialogApply->setPadding(7.0f, 2.0f, 7.0f, 2.0f);
        _shadowDialogApply->setOnClicked([this]() {
            setShadowBrush(_pendingShadowMask);
            if (_shadowModal != nullptr) _shadowModal->closeModal();
        });
        footer->addWidget(_shadowDialogApply, 104.0f);
        root->addWidget(footer, 31.0f);
        _shadowModal->setContentOwned(root);
    }

    void openShadowBrushDialog()
    {
        ayt::ui::UIManager* ui = _host.uiManager();
        if (ui == nullptr) {
            _host.setStatusText(
                L"Advanced shadow editing requires an AYUI host.");
            return;
        }
        auto activeScope = ayt::ui::UIManager::pushActive(ui);
        buildShadowBrushDialog();
        _pendingShadowMask = _document->model().selectedShadowMask();
        refreshShadowBrushDialog();
        const ayt::math::FVector2 client = ui->getClientSize();
        ayt::math::FVector2 dialogSize{360.0f, 278.0f};
        constexpr float kOuterMargin = 24.0f;
        if (client.x > 0.0f) {
            dialogSize.x = std::min(
                dialogSize.x, std::max(1.0f, client.x - kOuterMargin * 2.0f));
        }
        if (client.y > 0.0f) {
            dialogSize.y = std::min(
                dialogSize.y, std::max(1.0f, client.y - kOuterMargin * 2.0f));
        }
        _shadowModal->setSize(dialogSize);
        _shadowModal->setPosition({
            std::round(std::max(0.0f, (client.x - dialogSize.x) * 0.5f)),
            std::round(std::max(0.0f, (client.y - dialogSize.y) * 0.5f))});
        _shadowModal->openModal();
        _host.requestRepaint();
    }

    ayt::ui::TextInput* addImportField(
        ayt::ui::VBox* parent, const std::wstring& labelText,
        const char* id, bool gridField = true)
    {
        auto* row = new ayt::ui::HBox();
        row->setSpacing(5.0f);
        row->addWidget(makeLabel(labelText, 11), 88.0f);
        auto* input = new ayt::ui::TextInput();
        input->setId(id);
        input->setOnTextChanged([this](const std::wstring&) {
            if (!_syncingImport) refreshImportPlan();
        });
        row->addWidget(input, 0.0f);
        parent->addWidget(row, 27.0f);
        _importInputs.push_back(input);
        if (gridField) _importGridInputs.push_back(input);
        return input;
    }

    void buildAtlasImportDialog()
    {
        if (_importModal != nullptr) return;
        _importModal = std::make_unique<ayt::ui::Modal>();
        _importModal->setId("tilemap_atlas_import_dialog");
        _importModal->setSize({930.0f, 650.0f});
        _importModal->setDismissOnDimmerClick(false);

        auto* root = new ayt::ui::VBox();
        root->setStyleId("panel_default");
        root->setPadding(14.0f, 12.0f, 14.0f, 12.0f);
        root->setSpacing(8.0f);
        root->addWidget(makeLabel(L"Import Tile Sheet", 17), 28.0f);
        auto* sourceRow = new ayt::ui::HBox();
        sourceRow->setSpacing(8.0f);
        _importSourceLabel = makeLabel(L"No source image selected", 11);
        sourceRow->addWidget(_importSourceLabel, 0.0f);
        _importChoose = new ayt::ui::Button();
        _importChoose->setText(L"Choose PNG…");
        _importChoose->setPadding(7.0f, 2.0f, 7.0f, 2.0f);
        _importChoose->setOnClicked([this]() { chooseAtlasImage(); });
        sourceRow->addWidget(_importChoose, 132.0f);
        root->addWidget(sourceRow, 29.0f);

        auto* middle = new ayt::ui::HBox();
        middle->setSpacing(10.0f);
        auto* previewPanel = new ayt::ui::Panel();
        previewPanel->setPadding(0.0f, 0.0f, 0.0f, 0.0f);
        _importPreview = new EditorTileAtlasPicker();
        _importPreview->setOnRectangleSelected(
            [this](const EditorTileAtlasPicker::SourceRect& rectangle) {
                if (_importMode != AtlasImportMode::FreeRegions) return;
                _manualRegions.push_back({
                    "Region " + std::to_string(_manualRegions.size() + 1u),
                    rectangle.x, rectangle.y,
                    rectangle.width, rectangle.height});
                refreshImportPlan();
            });
        previewPanel->addChild(_importPreview);
        middle->addWidget(previewPanel, 0.0f);

        auto* settings = new ayt::ui::VBox();
        settings->setSpacing(5.0f);
        settings->addWidget(makeLabel(L"Slicing Mode", 13), 23.0f);
        _importModeButton = new ayt::ui::Button();
        _importModeButton->setId("tilemap_import_mode");
        _importModeButton->setText(L"Mode: Grid");
        _importModeButton->setPadding(7.0f, 2.0f, 7.0f, 2.0f);
        _importModeButton->setOnClicked([this]() { cycleImportMode(); });
        settings->addWidget(_importModeButton, 29.0f);
        _importTileWidth = addImportField(settings, L"Tile width", "tilemap_import_tile_w");
        _importTileHeight = addImportField(settings, L"Tile height", "tilemap_import_tile_h");
        _importMarginX = addImportField(settings, L"Margin X", "tilemap_import_margin_x");
        _importMarginY = addImportField(settings, L"Margin Y", "tilemap_import_margin_y");
        _importSpacingX = addImportField(settings, L"Spacing X", "tilemap_import_spacing_x");
        _importSpacingY = addImportField(settings, L"Spacing Y", "tilemap_import_spacing_y");
        _importFirstTileId = addImportField(settings, L"First Tile ID", "tilemap_import_first_id", false);
        _importFolder = addImportField(settings, L"Virtual folder", "tilemap_import_folder", false);
        auto* regionButtons = new ayt::ui::HBox();
        regionButtons->setSpacing(5.0f);
        _importRegionUndo = new ayt::ui::Button();
        _importRegionUndo->setId("tilemap_import_region_undo");
        _importRegionUndo->setText(L"Undo Region");
        _importRegionUndo->setPadding(7.0f, 2.0f, 7.0f, 2.0f);
        _importRegionUndo->setOnClicked([this]() {
            if (_manualRegions.empty()) return;
            _manualRegions.pop_back();
            if (_importPreview != nullptr) {
                _importPreview->clearRectangleSelection();
            }
            refreshImportPlan();
        });
        regionButtons->addWidget(_importRegionUndo, 0.0f);
        _importRegionClear = new ayt::ui::Button();
        _importRegionClear->setText(L"Clear");
        _importRegionClear->setPadding(7.0f, 2.0f, 7.0f, 2.0f);
        _importRegionClear->setOnClicked([this]() {
            _manualRegions.clear();
            if (_importPreview != nullptr) {
                _importPreview->clearRectangleSelection();
            }
            refreshImportPlan();
        });
        regionButtons->addWidget(_importRegionClear, 70.0f);
        settings->addWidget(regionButtons, 29.0f);
        _skipEmpty = new ayt::ui::Button();
        _skipEmpty->setPadding(7.0f, 2.0f, 7.0f, 2.0f);
        _skipEmpty->setOnClicked([this]() {
            _skipTransparent = !_skipTransparent;
            refreshImportPlan();
        });
        settings->addWidget(_skipEmpty, 29.0f);
        _importInfo = makeLabel(L"Choose an image to preview its grid.", 11);
        _importInfo->setWordWrap(true);
        settings->addWidget(_importInfo, 0.0f);
        middle->addWidget(settings, 260.0f);
        root->addWidget(middle, 0.0f);

        auto* footer = new ayt::ui::HBox();
        footer->setSpacing(8.0f);
        footer->addWidget(makeLabel(
            L"Preview and commit use the same exact source pixels.", 11),
            0.0f);
        _importCancel = new ayt::ui::Button();
        _importCancel->setText(L"Cancel");
        _importCancel->setPadding(7.0f, 2.0f, 7.0f, 2.0f);
        _importCancel->setOnClicked([this]() {
            if (_importModal != nullptr) _importModal->closeModal();
        });
        footer->addWidget(_importCancel, 96.0f);
        _importCommit = new ayt::ui::Button();
        _importCommit->setId("tilemap_import_commit");
        _importCommit->setText(L"Import");
        _importCommit->setPadding(7.0f, 2.0f, 7.0f, 2.0f);
        _importCommit->setEnabled(false);
        _importCommit->setOnClicked([this]() {
            if (_importCommitPending) return;
            _importCommitPending = true;
            _importCommit->setEnabled(false);
            _importCommit->setText(L"Importing…");
            _host.requestRepaint();
        });
        footer->addWidget(_importCommit, 132.0f);
        root->addWidget(footer, 31.0f);
        _importModal->setContentOwned(root);
    }

    void openAtlasImport()
    {
        ayt::ui::UIManager* ui = _host.uiManager();
        if (ui == nullptr) {
            _host.setStatusText(L"Tile-sheet import requires an AYUI host.");
            return;
        }
        auto activeScope = ayt::ui::UIManager::pushActive(ui);
        buildAtlasImportDialog();
        if (_pendingAtlasPath.empty()) chooseAtlasImage();
        const ayt::math::FVector2 client = ui->getClientSize();
        ayt::math::FVector2 dialogSize{930.0f, 650.0f};
        constexpr float kOuterMargin = 24.0f;
        if (client.x > 0.0f) {
            dialogSize.x = std::min(
                dialogSize.x, std::max(1.0f, client.x - kOuterMargin * 2.0f));
        }
        if (client.y > 0.0f) {
            dialogSize.y = std::min(
                dialogSize.y, std::max(1.0f, client.y - kOuterMargin * 2.0f));
        }
        _importModal->setSize(dialogSize);
        _importModal->setPosition({
            std::round(std::max(0.0f, (client.x - dialogSize.x) * 0.5f)),
            std::round(std::max(0.0f, (client.y - dialogSize.y) * 0.5f))});
        _importModal->openModal();
        _host.requestRepaint();
    }

    void setImportMode(AtlasImportMode mode)
    {
        if (mode == AtlasImportMode::Metadata && !_detectedMetadata) {
            mode = AtlasImportMode::Grid;
        }
        _importMode = mode;
        const bool editableGrid = mode == AtlasImportMode::Grid;
        for (ayt::ui::TextInput* input : _importGridInputs) {
            if (input != nullptr) input->setReadOnly(!editableGrid);
        }
        const bool freeRegions = mode == AtlasImportMode::FreeRegions;
        if (_importRegionUndo != nullptr) {
            _importRegionUndo->setEnabled(
                freeRegions && !_manualRegions.empty());
        }
        if (_importRegionClear != nullptr) {
            _importRegionClear->setEnabled(
                freeRegions && !_manualRegions.empty());
        }
        if (_importPreview != nullptr) {
            _importPreview->setRectangleSelectionEnabled(freeRegions, false);
        }
        if (_importModeButton != nullptr) {
            if (mode == AtlasImportMode::Metadata) {
                _importModeButton->setText(L"Mode: Metadata");
            } else if (freeRegions) {
                _importModeButton->setText(L"Mode: Free Regions");
            } else {
                _importModeButton->setText(L"Mode: Grid");
            }
        }
        refreshImportPlan();
    }

    void cycleImportMode()
    {
        if (_importMode == AtlasImportMode::Grid) {
            setImportMode(AtlasImportMode::FreeRegions);
        } else if (_importMode == AtlasImportMode::FreeRegions
                   && _detectedMetadata) {
            setImportMode(AtlasImportMode::Metadata);
        } else {
            setImportMode(AtlasImportMode::Grid);
        }
    }

    void chooseAtlasImage()
    {
        const std::string path = _host.chooseImageFile();
        if (path.empty()) return;
        std::string error;
        EditorAuthoringImage image = _host.loadAuthoringImage(path, &error);
        if (!image) {
            _host.setStatusText(L"Tile-sheet load failed: "
                + ayt::ui::decodeUtf8Text(error));
            return;
        }
        _pendingAtlasPath = path;
        _pendingAtlasImage = std::move(image);
        _manualRegions.clear();
        const uint32_t suggested =
            ayt::ay2d::editor::suggestAtlasTileSize(
                _pendingAtlasImage.width, _pendingAtlasImage.height,
                _document->model().document().tileWidth());
        uint32_t firstTileId = 1u;
        const auto& assets = _document->model().document().tileAssets();
        if (!assets.empty() && assets.rbegin()->first
            != (std::numeric_limits<uint32_t>::max)()) {
            firstTileId = assets.rbegin()->first + 1u;
        }
        const std::filesystem::path source = std::filesystem::u8path(path);
        _pendingAtlasName = source.stem().string();
        if (_pendingAtlasName.empty()) _pendingAtlasName = "Imported";
        _detectedMetadata = discoverAtlasMetadata(
            source, _pendingAtlasImage.width, _pendingAtlasImage.height);
        const bool metadataGrid = _detectedMetadata
            && _detectedMetadata.parsed.metadata.layout
                == ayt::ay2d::editor::TileAtlasLayout::Grid;
        _syncingImport = true;
        _importTileWidth->setText(std::to_wstring(metadataGrid
            ? _detectedMetadata.parsed.metadata.tileWidth : suggested));
        _importTileHeight->setText(std::to_wstring(metadataGrid
            ? _detectedMetadata.parsed.metadata.tileHeight : suggested));
        _importMarginX->setText(std::to_wstring(metadataGrid
            ? _detectedMetadata.parsed.metadata.marginX : 0u));
        _importMarginY->setText(std::to_wstring(metadataGrid
            ? _detectedMetadata.parsed.metadata.marginY : 0u));
        _importSpacingX->setText(std::to_wstring(metadataGrid
            ? _detectedMetadata.parsed.metadata.spacingX : 0u));
        _importSpacingY->setText(std::to_wstring(metadataGrid
            ? _detectedMetadata.parsed.metadata.spacingY : 0u));
        _importFirstTileId->setText(std::to_wstring(firstTileId));
        _importFolder->setText(ayt::ui::decodeUtf8Text(
            "Tiles/" + _pendingAtlasName));
        _importSourceLabel->setText(
            ayt::ui::decodeUtf8Text(source.filename().string()) + L"  ·  "
            + std::to_wstring(_pendingAtlasImage.width) + L" × "
            + std::to_wstring(_pendingAtlasImage.height) + L" px"
            + (_detectedMetadata.parsed.recognized
                ? L"  ·  " + ayt::ui::decodeUtf8Text(
                    _detectedMetadata.path.filename().string())
                : std::wstring{}));
        _syncingImport = false;
        setImportMode(_detectedMetadata
            ? AtlasImportMode::Metadata : AtlasImportMode::Grid);
        if (_detectedMetadata) {
            _host.setStatusText(L"Recognized atlas metadata: "
                + ayt::ui::decodeUtf8Text(
                    _detectedMetadata.path.filename().string()));
        } else if (_detectedMetadata.parsed.recognized) {
            _host.setStatusText(L"Companion metadata is incompatible: "
                + ayt::ui::decodeUtf8Text(
                    _detectedMetadata.parsed.error));
        } else {
            _host.setStatusText(
                L"No matching metadata found. Review the grid or use Free Regions.");
        }
    }

    bool readImportUInt(ayt::ui::TextInput* input, uint32_t& value) const
    {
        return input != nullptr && parseUint32(input->getText(), value);
    }

    uint32_t nextAtlasId() const noexcept
    {
        const auto& atlases = _document->model().document().tileAtlases();
        if (atlases.empty()) return 1u;
        return atlases.rbegin()->first == (std::numeric_limits<uint32_t>::max)()
            ? 0u : atlases.rbegin()->first + 1u;
    }

    void refreshImportPlan()
    {
        if (_importInfo == nullptr || _importCommit == nullptr
            || _importPreview == nullptr || !_pendingAtlasImage) return;
        ayt::ay2d::editor::TileAtlasImportRequest request;
        request.atlasId = nextAtlasId();
        request.name = _pendingAtlasName;
        request.sourcePath = _pendingAtlasPath;
        request.imageWidth = _pendingAtlasImage.width;
        request.imageHeight = _pendingAtlasImage.height;
        request.folder = encodeUtf8(_importFolder->getText());
        request.skipTransparent = _skipTransparent;
        const bool parsed = request.atlasId != 0u
            && readImportUInt(_importFirstTileId, request.firstTileId)
            && (_importMode != AtlasImportMode::Grid
                || (readImportUInt(_importTileWidth, request.tileWidth)
                    && readImportUInt(_importTileHeight, request.tileHeight)
                    && readImportUInt(_importMarginX, request.marginX)
                    && readImportUInt(_importMarginY, request.marginY)
                    && readImportUInt(_importSpacingX, request.spacingX)
                    && readImportUInt(_importSpacingY, request.spacingY)));
        _skipEmpty->setText(_skipTransparent
            ? L"Skip Empty: On" : L"Skip Empty: Off");
        const bool freeRegions = _importMode == AtlasImportMode::FreeRegions;
        if (_importRegionUndo != nullptr) {
            _importRegionUndo->setEnabled(
                freeRegions && !_manualRegions.empty());
        }
        if (_importRegionClear != nullptr) {
            _importRegionClear->setEnabled(
                freeRegions && !_manualRegions.empty());
        }
        if (!parsed) {
            _pendingImportPlan = {};
            _importCommit->setText(L"Import 0 Tiles");
            _importCommit->setEnabled(false);
            _importInfo->setText(L"Enter valid unsigned grid values.");
            return;
        }
        if (_importMode == AtlasImportMode::Metadata) {
            _pendingImportPlan =
                ayt::ay2d::editor::planTileAtlasMetadataImport(
                    request, _detectedMetadata.parsed.metadata,
                    *_pendingAtlasImage.bgraPixels);
        } else if (freeRegions) {
            _pendingImportPlan =
                ayt::ay2d::editor::planTileAtlasRegionImport(
                    request, _manualRegions,
                    *_pendingAtlasImage.bgraPixels);
        } else {
            _pendingImportPlan = ayt::ay2d::editor::planTileAtlasImport(
                request, *_pendingAtlasImage.bgraPixels);
        }
        std::vector<EditorTileAtlasPicker::Cell> cells;
        cells.reserve(_pendingImportPlan.tiles.size());
        for (const auto& tile : _pendingImportPlan.tiles) {
            cells.push_back({tile.tileId, tile.sourceX, tile.sourceY,
                             tile.sourceWidth, tile.sourceHeight});
        }
        _importPreview->setAtlas(_pendingAtlasImage.texture,
                                 _pendingImportPlan.source,
                                 std::move(cells));
        _importPreview->setRectangleSelectionEnabled(freeRegions, false);
        const bool valid = static_cast<bool>(_pendingImportPlan);
        _importCommit->setEnabled(valid);
        _importCommit->setText(L"Import "
            + std::to_wstring(_pendingImportPlan.tiles.size()) + L" Tiles");
        if (!valid) {
            _importInfo->setText(
                ayt::ui::decodeUtf8Text(_pendingImportPlan.error));
        } else {
            if (_pendingImportPlan.source.layout
                == ayt::ay2d::editor::TileAtlasLayout::Grid) {
                _importInfo->setText(
                    std::to_wstring(_pendingImportPlan.source.columns) + L" × "
                    + std::to_wstring(_pendingImportPlan.source.rows)
                    + L" grid  ·  "
                    + std::to_wstring(_pendingImportPlan.tiles.size())
                    + (_skipTransparent ? L" non-empty tiles" : L" tiles"));
            } else {
                _importInfo->setText(
                    std::to_wstring(_pendingImportPlan.tiles.size())
                    + L" exact source regions"
                    + (freeRegions ? L"  ·  drag to add another" : L""));
            }
        }
        _host.requestRepaint();
    }

    void commitAtlasImport()
    {
        refreshImportPlan();
        if (!_pendingImportPlan) return;
        const uint32_t atlasId = _pendingImportPlan.source.atlasId;
        const uint32_t firstTileId = _pendingImportPlan.tiles.front().tileId;
        const size_t count = _pendingImportPlan.tiles.size();
        if (!_document->model().importTileAtlas(
                _pendingImportPlan.source, _pendingImportPlan.folder,
                _pendingImportPlan.tiles)) {
            _host.setStatusText(
                L"Import rejected: an atlas or Tile ID already exists.");
            return;
        }
        _atlasImages[atlasId] = _pendingAtlasImage;
        _atlasTextures[atlasId] = _pendingAtlasImage.texture;
        _document->model().setSelectedTileId(firstTileId);
        _document->model().setTool(ayt::ay2d::editor::PaintTool::Pencil);
        _document->changed();
        if (_importModal != nullptr) _importModal->closeModal();
        refresh();
        _host.requestRepaint();
        _host.setStatusText(L"Imported " + std::to_wstring(count)
            + L" tiles. Select directly from the Source Sheet.");
    }

    std::string resolvedAtlasPath(const std::string& sourcePath) const
    {
        std::filesystem::path path = std::filesystem::u8path(sourcePath);
        if (path.is_relative() && !_document->path().empty()) {
            const std::filesystem::path documentPath =
                std::filesystem::u8path(_document->path());
            const std::filesystem::path assetRoot =
                tilemapAssetRoot(documentPath);
            std::error_code error;
            const std::filesystem::path assetRelative = assetRoot / path;
            if (!assetRoot.empty()
                && std::filesystem::is_regular_file(assetRelative, error)
                && !error) {
                path = assetRelative;
            } else {
                path = documentPath.parent_path() / path;
            }
        }
        return std::filesystem::absolute(path).lexically_normal().string();
    }

    void loadSavedAtlasImages()
    {
        std::wstring failures;
        for (const auto& [atlasId, source] :
             _document->model().document().tileAtlases()) {
            std::string error;
            EditorAuthoringImage image = _host.loadAuthoringImage(
                resolvedAtlasPath(source.sourcePath), &error);
            if (!image) {
                if (!failures.empty()) failures += L"; ";
                failures += ayt::ui::decodeUtf8Text(source.name);
                continue;
            }
            _atlasTextures[atlasId] = image.texture;
            _atlasImages[atlasId] = std::move(image);
        }
        if (!failures.empty()) {
            _host.setStatusText(L"Atlas source unavailable (using fallback): "
                                + failures);
        }
    }

    std::vector<ayt::ay2d::editor::TileAnimationFrame>
    selectedAnimationFrames() const
    {
        const auto& animations = _document->model().document().animations();
        const auto found = animations.find(
            _document->model().selectedTileId());
        return found == animations.end()
            ? std::vector<ayt::ay2d::editor::TileAnimationFrame>{}
            : found->second;
    }

    void addAnimationFrame()
    {
        auto frames = selectedAnimationFrames();
        frames.push_back({_document->model().selectedTileId(), 100u});
        _animationFrameIndex = static_cast<int>(frames.size() - 1u);
        mutate(_document->model().setAnimation(
            _document->model().selectedTileId(), std::move(frames)));
    }

    void removeAnimationFrame()
    {
        auto frames = selectedAnimationFrames();
        if (_animationFrameIndex < 0
            || _animationFrameIndex >= static_cast<int>(frames.size())) {
            _host.setStatusText(L"Select an animation frame to remove.");
            return;
        }
        frames.erase(frames.begin() + _animationFrameIndex);
        if (_animationFrameIndex >= static_cast<int>(frames.size())) {
            _animationFrameIndex = static_cast<int>(frames.size()) - 1;
        }
        mutate(_document->model().setAnimation(
            _document->model().selectedTileId(), std::move(frames)));
    }

    void moveAnimationFrame(int direction)
    {
        auto frames = selectedAnimationFrames();
        const int target = _animationFrameIndex + direction;
        if (_animationFrameIndex < 0
            || _animationFrameIndex >= static_cast<int>(frames.size())
            || target < 0 || target >= static_cast<int>(frames.size())) return;
        std::swap(frames[static_cast<size_t>(_animationFrameIndex)],
                  frames[static_cast<size_t>(target)]);
        _animationFrameIndex = target;
        mutate(_document->model().setAnimation(
            _document->model().selectedTileId(), std::move(frames)));
    }

    void applyAnimationFrame()
    {
        uint32_t tileId = 0u;
        uint32_t duration = 0u;
        if (!parseUint32(_animationTile->getText(), tileId)
            || !parseUint32(_animationDuration->getText(), duration)
            || duration == 0u) {
            _host.setStatusText(
                L"Animation frame needs a tile ID and non-zero duration.");
            return;
        }
        auto frames = selectedAnimationFrames();
        const ayt::ay2d::editor::TileAnimationFrame frame{tileId, duration};
        if (_animationFrameIndex >= 0
            && _animationFrameIndex < static_cast<int>(frames.size())) {
            frames[static_cast<size_t>(_animationFrameIndex)] = frame;
        } else {
            frames.push_back(frame);
            _animationFrameIndex = static_cast<int>(frames.size() - 1u);
        }
        mutate(_document->model().setAnimation(
            _document->model().selectedTileId(), std::move(frames)));
    }

    void syncAnimationEditor()
    {
        const auto frames = selectedAnimationFrames();
        if (_animationFrameIndex >= static_cast<int>(frames.size())) {
            _animationFrameIndex = static_cast<int>(frames.size()) - 1;
        }
        std::vector<std::wstring> labels;
        for (size_t index = 0u; index < frames.size(); ++index) {
            labels.push_back(std::to_wstring(index + 1u) + L". Tile "
                + std::to_wstring(frames[index].tileId) + L" · "
                + std::to_wstring(frames[index].durationMs) + L" ms");
        }
        _animationFrameList->setItems(labels);
        _animationFrameList->setSelectedIndex(_animationFrameIndex);
        if (_animationFrameIndex >= 0) {
            const auto& frame = frames[static_cast<size_t>(_animationFrameIndex)];
            _animationTile->setText(std::to_wstring(frame.tileId));
            _animationDuration->setText(std::to_wstring(frame.durationMs));
        } else {
            _animationTile->setText(std::to_wstring(
                _document->model().selectedTileId()));
            _animationDuration->setText(L"100");
        }
        _animationPreview->setText(
            _canvas->animationPreviewEnabled() ? L"Pause" : L"Play");
    }

    void syncTerrainNeighborButtons()
    {
        constexpr std::array<const wchar_t*, 8> labels{
            L"N", L"NE", L"E", L"SE", L"S", L"SW", L"W", L"NW"};
        for (uint8_t bit = 0u; bit < 8u; ++bit) {
            ayt::ui::Button* button = _terrainNeighborButtons[bit];
            if (button == nullptr) continue;
            const uint8_t value = static_cast<uint8_t>(1u << bit);
            const wchar_t state = (_terrainRuleMatchMask & value) == 0u
                ? L'*' : (_terrainRuleNeighborMask & value) != 0u
                    ? L'1' : L'0';
            button->setText(std::wstring(labels[bit]) + L" " + state);
        }
    }

    void syncTerrainRuleList()
    {
        constexpr std::array<const wchar_t*, 8> names{
            L"N", L"NE", L"E", L"SE", L"S", L"SW", L"W", L"NW"};
        if (_terrainRuleIndex >= static_cast<int>(_terrainRuleDraft.size())) {
            _terrainRuleIndex = static_cast<int>(_terrainRuleDraft.size()) - 1;
        }
        std::vector<std::wstring> items;
        for (size_t index = 0u; index < _terrainRuleDraft.size(); ++index) {
            std::wostringstream label;
            label << index + 1u << L". ";
            const auto& rule = _terrainRuleDraft[index];
            for (size_t bit = 0u; bit < names.size(); ++bit) {
                const uint8_t value = static_cast<uint8_t>(1u << bit);
                const wchar_t state = (rule.matchMask & value) == 0u
                    ? L'*' : (rule.neighborMask & value) != 0u ? L'1' : L'0';
                if (bit > 0u) label << L' ';
                label << names[bit] << state;
            }
            label << L" → " << rule.tileId;
            items.push_back(label.str());
        }
        _terrainRuleList->setItems(items);
        _terrainRuleList->setSelectedIndex(_terrainRuleIndex);
        if (_terrainRuleIndex >= 0) {
            const auto& rule =
                _terrainRuleDraft[static_cast<size_t>(_terrainRuleIndex)];
            _terrainRuleMatchMask = rule.matchMask;
            _terrainRuleNeighborMask = rule.neighborMask;
            _terrainRuleTile->setText(std::to_wstring(rule.tileId));
        } else {
            _terrainRuleMatchMask = 0u;
            _terrainRuleNeighborMask = 0u;
            _terrainRuleTile->setText(std::to_wstring(
                _document->model().selectedTileId()));
        }
        syncTerrainNeighborButtons();
    }

    void syncTerrainEditor()
    {
        const auto& document = _document->model().document();
        _terrainIds.clear();
        std::vector<std::wstring> items;
        int selectedIndex = -1;
        for (const auto& [id, terrain] : document.terrains()) {
            if (!_editingNewTerrain
                && id == _document->model().selectedTerrainId()) {
                selectedIndex = static_cast<int>(_terrainIds.size());
            }
            _terrainIds.push_back(id);
            items.push_back(ayt::ui::decodeUtf8Text(terrain.name)
                + L" · ID " + std::to_wstring(id));
        }
        _terrainList->setItems(items);
        _terrainList->setSelectedIndex(selectedIndex);
        const auto* selected = _editingNewTerrain ? nullptr
            : document.terrainDefinition(
                _document->model().selectedTerrainId());
        if (selected != nullptr) {
            _terrainId->setText(std::to_wstring(selected->terrainId));
            _terrainName->setText(ayt::ui::decodeUtf8Text(selected->name));
            _terrainFallback->setText(
                std::to_wstring(selected->fallbackTileId));
            if (_terrainDraftSourceId != selected->terrainId) {
                _terrainRuleDraft = selected->rules;
                _terrainRuleIndex = -1;
                _terrainDraftSourceId = selected->terrainId;
            }
        } else if (!_editingNewTerrain) {
            beginTerrain(false);
        }
        syncTerrainRuleList();
    }

    void beginTerrain(bool announce = true)
    {
        uint32_t nextId = 1u;
        const auto& document = _document->model().document();
        while (document.terrainDefinition(nextId) != nullptr) ++nextId;
        _editingNewTerrain = true;
        _terrainDraftSourceId = 0u;
        _terrainRuleDraft.clear();
        _terrainRuleIndex = -1;
        _terrainId->setText(std::to_wstring(nextId));
        _terrainName->setText(L"Terrain " + std::to_wstring(nextId));
        _terrainFallback->setText(std::to_wstring(
            _document->model().selectedTileId()));
        syncTerrainRuleList();
        if (announce) {
            _host.setStatusText(
                L"New terrain: add visual neighbor rules, then save.");
        }
    }

    void beginTerrainRule()
    {
        _terrainRuleIndex = -1;
        _terrainRuleMatchMask = 0u;
        _terrainRuleNeighborMask = 0u;
        _terrainRuleTile->setText(std::to_wstring(
            _document->model().selectedTileId()));
        _terrainRuleList->setSelectedIndex(-1);
        syncTerrainNeighborButtons();
    }

    void cycleTerrainNeighbor(uint8_t bit)
    {
        const uint8_t value = static_cast<uint8_t>(1u << bit);
        if ((_terrainRuleMatchMask & value) == 0u) {
            _terrainRuleMatchMask = static_cast<uint8_t>(
                _terrainRuleMatchMask | value);
            _terrainRuleNeighborMask = static_cast<uint8_t>(
                _terrainRuleNeighborMask | value);
        } else if ((_terrainRuleNeighborMask & value) != 0u) {
            _terrainRuleNeighborMask = static_cast<uint8_t>(
                _terrainRuleNeighborMask & ~value);
        } else {
            _terrainRuleMatchMask = static_cast<uint8_t>(
                _terrainRuleMatchMask & ~value);
        }
        syncTerrainNeighborButtons();
    }

    void applyTerrainRule()
    {
        uint32_t tileId = 0u;
        if (!parseUint32(_terrainRuleTile->getText(), tileId)) {
            _host.setStatusText(L"Terrain output tile must be a number.");
            return;
        }
        ayt::ay2d::editor::AutoTileRule rule{
            _terrainRuleMatchMask, _terrainRuleNeighborMask, tileId};
        if (_terrainRuleIndex >= 0
            && _terrainRuleIndex < static_cast<int>(_terrainRuleDraft.size())) {
            _terrainRuleDraft[static_cast<size_t>(_terrainRuleIndex)] = rule;
        } else {
            _terrainRuleDraft.push_back(rule);
            _terrainRuleIndex = static_cast<int>(_terrainRuleDraft.size() - 1u);
        }
        _syncing = true;
        syncTerrainRuleList();
        _syncing = false;
    }

    void removeTerrainRule()
    {
        if (_terrainRuleIndex < 0
            || _terrainRuleIndex >= static_cast<int>(_terrainRuleDraft.size())) {
            _host.setStatusText(L"Select a terrain rule to remove.");
            return;
        }
        _terrainRuleDraft.erase(
            _terrainRuleDraft.begin() + _terrainRuleIndex);
        if (_terrainRuleIndex >= static_cast<int>(_terrainRuleDraft.size())) {
            _terrainRuleIndex = static_cast<int>(_terrainRuleDraft.size()) - 1;
        }
        _syncing = true;
        syncTerrainRuleList();
        _syncing = false;
    }

    void saveTerrain()
    {
        uint32_t id = 0u;
        uint32_t fallback = 0u;
        if (!parseUint32(_terrainId->getText(), id) || id == 0u
            || !parseUint32(_terrainFallback->getText(), fallback)) {
            _host.setStatusText(
                L"Terrain ID must be non-zero and fallback must be a tile ID.");
            return;
        }
        ayt::ay2d::editor::TerrainDefinition terrain;
        terrain.terrainId = id;
        terrain.name = encodeUtf8(_terrainName->getText());
        terrain.fallbackTileId = fallback;
        terrain.rules = _terrainRuleDraft;
        if (terrain.name.empty()) {
            _host.setStatusText(L"Terrain needs a name.");
            return;
        }
        const bool changed =
            _document->model().setTerrainDefinition(std::move(terrain));
        _document->model().setSelectedTerrainId(id);
        _editingNewTerrain = false;
        _terrainDraftSourceId = id;
        mutate(changed);
        setTool(ayt::ay2d::editor::PaintTool::Terrain);
    }

    void deleteTerrain()
    {
        const uint32_t id = _document->model().selectedTerrainId();
        if (id == 0u || !_document->model().removeTerrainDefinition(id)) {
            _host.setStatusText(L"Select a saved terrain to delete.");
            return;
        }
        _editingNewTerrain = false;
        _terrainDraftSourceId = 0u;
        _terrainRuleDraft.clear();
        _terrainRuleIndex = -1;
        _document->changed();
        refresh();
        _host.requestRepaint();
    }

    void selectTile(uint32_t tileId)
    {
        _selectingStamp = false;
        if (_atlasPicker != nullptr) {
            _atlasPicker->setRectangleSelectionEnabled(false);
        }
        if (_document->model().selectedTileId() != tileId) {
            _animationFrameIndex = -1;
        }
        _document->model().setSelectedTileId(tileId);
        _document->model().setTool(ayt::ay2d::editor::PaintTool::Pencil);
        refresh();
        _host.requestRepaint();
    }

    uint32_t nextStampId() const noexcept
    {
        const auto& stamps = _document->model().document().tileStamps();
        if (stamps.empty()) return 1u;
        return stamps.rbegin()->first == (std::numeric_limits<uint32_t>::max)()
            ? 0u : stamps.rbegin()->first + 1u;
    }

    void toggleStampSelection()
    {
        const auto* source = _document->model().document().tileAtlas(
            _shownAtlasId);
        if (source == nullptr || source->layout
            != ayt::ay2d::editor::TileAtlasLayout::Grid) {
            _host.setStatusText(
                L"Stamp selection requires a regular-grid Source Sheet.");
            return;
        }
        _selectingStamp = !_selectingStamp;
        _atlasPicker->setRectangleSelectionEnabled(_selectingStamp, true);
        _stampSelect->setText(_selectingStamp ? L"Cancel" : L"Select");
        _host.setStatusText(_selectingStamp
            ? L"Drag across Source Sheet cells to create a reusable Stamp."
            : L"Stamp selection cancelled.");
        _host.requestRepaint();
    }

    void createStampFromSourceRectangle(
        const EditorTileAtlasPicker::SourceRect& rectangle)
    {
        if (!_selectingStamp) return;
        const auto& document = _document->model().document();
        const auto* source = document.tileAtlas(_shownAtlasId);
        if (source == nullptr || source->layout
            != ayt::ay2d::editor::TileAtlasLayout::Grid
            || source->tileWidth == 0u || source->tileHeight == 0u) return;
        const uint32_t pitchX = source->tileWidth + source->spacingX;
        const uint32_t pitchY = source->tileHeight + source->spacingY;
        const uint32_t originColumn = rectangle.x <= source->marginX ? 0u
            : (rectangle.x - source->marginX) / pitchX;
        const uint32_t originRow = rectangle.y <= source->marginY ? 0u
            : (rectangle.y - source->marginY) / pitchY;
        ayt::ay2d::editor::TileStampDefinition stamp;
        stamp.stampId = nextStampId();
        stamp.name = "Stamp " + std::to_string(stamp.stampId);
        const uint64_t right = static_cast<uint64_t>(rectangle.x)
            + rectangle.width;
        const uint64_t bottom = static_cast<uint64_t>(rectangle.y)
            + rectangle.height;
        for (const auto& [tileId, asset] : document.tileAssets()) {
            if (asset.atlasId != _shownAtlasId
                || asset.sourceX < rectangle.x || asset.sourceY < rectangle.y
                || static_cast<uint64_t>(asset.sourceX) + asset.sourceWidth
                    > right
                || static_cast<uint64_t>(asset.sourceY) + asset.sourceHeight
                    > bottom) continue;
            const uint32_t column = (asset.sourceX - source->marginX) / pitchX;
            const uint32_t row = (asset.sourceY - source->marginY) / pitchY;
            stamp.cells.push_back({
                static_cast<int32_t>(column) - static_cast<int32_t>(originColumn),
                // Source-sheet Y grows downward while Tilemap world Y grows
                // upward. Keep the selected top-left cell as the anchor and
                // preserve the source block's visible row order.
                static_cast<int32_t>(originRow) - static_cast<int32_t>(row),
                tileId});
        }
        if (stamp.stampId == 0u || stamp.cells.size() < 2u) {
            _host.setStatusText(
                L"Select at least two imported cells; skipped cells may remain holes.");
            return;
        }
        if (!_document->model().setTileStamp(std::move(stamp))) {
            _host.setStatusText(L"The selected cells could not form a Stamp.");
            return;
        }
        _document->model().setTool(ayt::ay2d::editor::PaintTool::Stamp);
        _selectingStamp = false;
        _atlasPicker->setRectangleSelectionEnabled(false);
        _document->changed();
        refresh();
        _host.requestRepaint();
        _host.setStatusText(L"Reusable Stamp created. Click the map to place it.");
    }

    void saveSelectionAsStamp()
    {
        const uint32_t stampId = nextStampId();
        if (stampId == 0u) {
            _host.setStatusText(L"No free Stamp ID is available.");
            return;
        }
        if (!_document->model().saveSelectionAsStamp(
                stampId, "Selection " + std::to_string(stampId))) {
            _host.setStatusText(
                L"Select a region containing imported tile assets first.");
            return;
        }
        _document->model().setTool(ayt::ay2d::editor::PaintTool::Stamp);
        _document->changed();
        refresh();
        _host.requestRepaint();
        _host.setStatusText(
            L"Selection saved as a reusable Stamp. Undo removes it.");
    }

    void deleteSelectedStamp()
    {
        const uint32_t stampId = _document->model().selectedStampId();
        if (stampId == 0u) return;
        if (_document->model().removeTileStamp(stampId)) {
            _document->model().setTool(ayt::ay2d::editor::PaintTool::Pencil);
            _document->changed();
            refresh();
            _host.requestRepaint();
            _host.setStatusText(L"Stamp removed. Undo restores it.");
        }
    }

    void syncSourcePicker()
    {
        if (_atlasPicker == nullptr) return;
        const auto& document = _document->model().document();
        _atlasIds.clear();
        std::vector<std::wstring> atlasLabels;
        for (const auto& [candidateId, candidate] : document.tileAtlases()) {
            _atlasIds.push_back(candidateId);
            std::wstring layout;
            if (candidate.layout == ayt::ay2d::editor::TileAtlasLayout::Grid) {
                layout = std::to_wstring(candidate.columns) + L" × "
                    + std::to_wstring(candidate.rows);
            } else {
                const size_t count = static_cast<size_t>(std::count_if(
                    document.tileAssets().begin(), document.tileAssets().end(),
                    [candidateId](const auto& pair) {
                        return pair.second.atlasId == candidateId;
                    }));
                layout = std::to_wstring(count) + L" regions";
            }
            atlasLabels.push_back(ayt::ui::decodeUtf8Text(candidate.name)
                + L"  ·  " + layout);
        }
        if (atlasLabels.empty()) atlasLabels.push_back(L"No tile sheet imported");
        if (_atlasSelector != nullptr) _atlasSelector->setItems(atlasLabels);
        uint32_t atlasId = 0u;
        if (const auto* selected = document.tileAsset(
                _document->model().selectedTileId())) {
            atlasId = selected->atlasId;
        }
        if (atlasId == 0u && document.tileAtlases().contains(_shownAtlasId)) {
            atlasId = _shownAtlasId;
        }
        if (atlasId == 0u && !document.tileAtlases().empty()) {
            atlasId = document.tileAtlases().begin()->first;
        }
        const auto* source = document.tileAtlas(atlasId);
        const auto texture = _atlasTextures.find(atlasId);
        if (source == nullptr || texture == _atlasTextures.end()) {
            _shownAtlasId = 0u;
            _selectingStamp = false;
            _atlasPicker->clearAtlas();
            if (_atlasSelector != nullptr) {
                _atlasSelector->setSelectedIndex(
                    document.tileAtlases().empty() ? 0 : -1);
            }
            return;
        }
        std::vector<EditorTileAtlasPicker::Cell> cells;
        for (const auto& [tileId, asset] : document.tileAssets()) {
            if (asset.atlasId != atlasId) continue;
            cells.push_back({tileId, asset.sourceX, asset.sourceY,
                             asset.sourceWidth, asset.sourceHeight});
        }
        _shownAtlasId = atlasId;
        const auto atlasIndex = std::find(
            _atlasIds.begin(), _atlasIds.end(), atlasId);
        if (_atlasSelector != nullptr) {
            _atlasSelector->setSelectedIndex(atlasIndex == _atlasIds.end()
                ? -1 : static_cast<int>(atlasIndex - _atlasIds.begin()));
        }
        _atlasPicker->setAtlas(texture->second, *source, std::move(cells));
        _atlasPicker->setSelectedTileId(
            _document->model().selectedTileId());
    }

    size_t activeLayer() const noexcept {
        return _document->model().document().activeLayerIndex();
    }

    void mutate(bool changed) {
        if (!changed) return;
        _document->changed();
        refresh();
        _host.requestRepaint();
    }

    void setTool(ayt::ay2d::editor::PaintTool tool) {
        if (_canvas != nullptr && _canvas->editingGestureActive()) return;
        if (tool == ayt::ay2d::editor::PaintTool::Terrain
            && _document->model().document().terrainDefinition(
                _document->model().selectedTerrainId()) == nullptr) {
            _host.setStatusText(
                L"Create or select an auto-tile terrain first.");
            return;
        }
        if (tool == ayt::ay2d::editor::PaintTool::Shadow
            && _canvas != nullptr) {
            _canvas->setShowShadows(true);
        }
        _document->model().setTool(tool);
        refresh();
        _host.requestRepaint();
    }

    bool textInputFocused() const noexcept {
        const ayt::ui::UIManager* ui = _host.uiManager();
        const ayt::ui::Widget* focused = ui != nullptr
            ? ui->getFocusedWidget() : nullptr;
        return focused != nullptr && focused->isTextEditingWidget();
    }

    void updateSummary() {
        if (_summary == nullptr) return;
        const auto& document = _document->model().document();
        _summary->setText(std::to_wstring(document.cols()) + L" × "
            + std::to_wstring(document.rows()) + L" cells  ·  "
            + std::to_wstring(document.layerCount()) + L" layers  ·  "
            + std::to_wstring(static_cast<int>(std::round(_zoomPercent)))
            + L"%");
        updateDocumentPath();
    }

    void updateDocumentPath() {
        if (_documentPath == nullptr) return;
        const bool unsaved = _document->path().empty();
        const std::wstring text = unsaved
            ? L"File: Not saved yet — Ctrl+S to choose a location"
            : L"File: " + ayt::ui::decodeUtf8Text(_document->path());
        _documentPath->setText(text);
        if (_documentPathTooltip != nullptr) {
            _documentPathTooltip->setText(unsaved
                ? L"This tilemap has no file yet. Save chooses a location; "
                  L"the default folder is Assets/tilemaps."
                : text);
        }
    }

    bool saveDocument(bool saveAs) {
        std::string error;
        bool saved = false;
        if (saveAs) {
            if (!_document->hasSavePathProvider()) return false;
            const std::string selected = showSavePath(true);
            if (selected.empty()) return false;
            saved = _document->saveAs(selected, &error);
        } else {
            saved = _document->save(&error);
        }
        if (!saved) {
            if (error != "Tilemap save was canceled.") {
                _host.setStatusText(
                    L"Tilemap save failed: "
                    + ayt::ui::decodeUtf8Text(error));
            }
            return false;
        }

        std::wstring status = L"Tilemap saved to "
            + ayt::ui::decodeUtf8Text(_document->path());
        if (!_document->lastSaveNotice().empty()) {
            status += L". ";
            status += ayt::ui::decodeUtf8Text(
                _document->lastSaveNotice());
        } else {
            status += L" and runtime asset cooked.";
        }
        _host.setStatusText(status);
        refresh();
        _host.requestRepaint();
        return true;
    }

    std::string showSavePath(bool saveAs) {
        if (auto* provider = dynamic_cast<
                IEditorDocumentSavePathProvider*>(&_host)) {
            const std::string selected =
                provider->chooseDocumentSavePath(*_document, saveAs);
            if (!selected.empty()) return selected;
        }
        return showTilemapSaveDialog(_host.projectRoot(), _document->path());
    }

    void refresh() {
        if (_summary == nullptr) return;
        _syncing = true;
        const auto& model = _document->model();
        const auto& document = model.document();
        updateSummary();

        const bool pencil = model.tool()
            == ayt::ay2d::editor::PaintTool::Pencil;
        const bool selection = model.tool()
            == ayt::ay2d::editor::PaintTool::Selection;
        const bool eraser = model.tool()
            == ayt::ay2d::editor::PaintTool::Eraser;
        const bool fill = model.tool()
            == ayt::ay2d::editor::PaintTool::FloodFill;
        const bool rectangle = model.tool()
            == ayt::ay2d::editor::PaintTool::Rectangle;
        const bool terrain = model.tool()
            == ayt::ay2d::editor::PaintTool::Terrain;
        const bool stamp = model.tool()
            == ayt::ay2d::editor::PaintTool::Stamp;
        const bool shadow = model.tool()
            == ayt::ay2d::editor::PaintTool::Shadow;
        setIconState(_selection, selection, L"Selection (M)");
        setIconState(_pencil, pencil, L"Pencil (P)");
        setIconState(_eraser, eraser, L"Eraser (E)");
        setIconState(_fill, fill, L"Flood Fill (F)");
        setIconState(_rectangle, rectangle, L"Rectangle (R)");
        setIconState(_terrain, terrain, L"Auto terrain brush (T)");
        setIconState(_stamp, stamp, L"Stamp (S)");
        const uint8_t shadowMask = model.selectedShadowMask();
        setIconState(
            _shadow,
            shadow && shadowMask == ayt::ay2d::editor::ShadowMask_All,
            L"Full shadow (H)");
        setIconState(
            _shadowClear,
            shadow && shadowMask == ayt::ay2d::editor::ShadowMask_None,
            L"Clear shadow");
        setIconState(
            _shadowAdvanced,
            shadow && shadowMask != ayt::ay2d::editor::ShadowMask_All
                && shadowMask != ayt::ay2d::editor::ShadowMask_None,
            L"Advanced shadow quadrants");
        setIconState(_grid, _canvas->showGrid(), L"Grid overlay");
        setIconState(_collision, _canvas->showCollision(),
                     L"Collision overlay");
        setIconState(_shadowVisibility, _canvas->showShadows(),
                     L"Shadow overlay");

        _tileIds.clear();
        std::vector<std::wstring> tileLabels;
        int selectedTileIndex = -1;
        for (const auto& [tileId, asset] : document.tileAssets()) {
            if (tileId == model.selectedTileId()) {
                selectedTileIndex = static_cast<int>(_tileIds.size());
            }
            _tileIds.push_back(tileId);
            tileLabels.push_back(std::to_wstring(tileId) + L"   "
                + ayt::ui::decodeUtf8Text(asset.name));
        }
        _tileList->setItems(tileLabels);
        _tileList->setSelectedIndex(selectedTileIndex);
        _tileId->setText(std::to_wstring(model.selectedTileId()));
        syncSourcePicker();

        _stampIds.clear();
        _stampIds.push_back(0u);
        std::vector<std::wstring> stampLabels{L"Single tile"};
        int selectedStampIndex = 0;
        for (const auto& [stampId, definition] : document.tileStamps()) {
            _stampIds.push_back(stampId);
            stampLabels.push_back(ayt::ui::decodeUtf8Text(definition.name)
                + L"  ·  " + std::to_wstring(definition.cells.size())
                + L" cells");
            if (stampId == model.selectedStampId()) {
                selectedStampIndex = static_cast<int>(_stampIds.size() - 1u);
            }
        }
        _stampSelector->setItems(stampLabels);
        _stampSelector->setSelectedIndex(selectedStampIndex);
        _stampDelete->setEnabled(model.selectedStampId() != 0u);
        const auto* shownSource = document.tileAtlas(_shownAtlasId);
        _stampSelect->setEnabled(shownSource != nullptr
            && shownSource->layout
                == ayt::ay2d::editor::TileAtlasLayout::Grid);
        _stampSelect->setText(_selectingStamp ? L"Cancel" : L"Select");

        std::vector<std::wstring> layerLabels;
        for (size_t index = 0u; index < document.layerCount(); ++index) {
            const auto& layer = document.layers()[index];
            layerLabels.push_back(
                (layer.visible ? L"[on]  " : L"[off] ")
                + std::to_wstring(index + 1u) + L"   "
                + ayt::ui::decodeUtf8Text(layer.name));
        }
        _layerList->setItems(layerLabels);
        _layerList->setSelectedIndex(
            static_cast<int>(document.activeLayerIndex()));
        if (document.activeLayerIndex() < document.layers().size()) {
            _layerName->setText(ayt::ui::decodeUtf8Text(
                document.layers()[document.activeLayerIndex()].name));
            setIconState(
                _layerVisibility,
                document.layers()[document.activeLayerIndex()].visible,
                L"Active layer visibility");
        }
        _collisionFlags->setText(std::to_wstring(
            document.collisionFlagsFor(model.selectedTileId())));
        _shadowColor->setText(rgbaText(document.shadowColor()));
        syncAnimationEditor();
        syncTerrainEditor();
        _syncing = false;
    }

    std::shared_ptr<TilemapWorkspaceDocument> _document;
    IEditorHostServices& _host;
    std::filesystem::path _iconRoot;
    ayt::ui::Widget* _root = nullptr;
    ayt::ui::TextLabel* _summary = nullptr;
    ayt::ui::TextLabel* _documentPath = nullptr;
    ayt::ui::Tooltip* _documentPathTooltip = nullptr;
    ayt::ui::ComboBox* _atlasSelector = nullptr;
    ayt::ui::ComboBox* _stampSelector = nullptr;
    EditorTilemapCanvas* _canvas = nullptr;
    EditorTileAtlasPicker* _atlasPicker = nullptr;
    ayt::ui::ListView* _tileList = nullptr;
    ayt::ui::ListView* _layerList = nullptr;
    ayt::ui::ListView* _animationFrameList = nullptr;
    ayt::ui::ListView* _terrainList = nullptr;
    ayt::ui::ListView* _terrainRuleList = nullptr;
    ayt::ui::TextInput* _tileId = nullptr;
    ayt::ui::TextInput* _layerName = nullptr;
    ayt::ui::TextInput* _collisionFlags = nullptr;
    ayt::ui::TextInput* _shadowColor = nullptr;
    ayt::ui::TextInput* _animationTile = nullptr;
    ayt::ui::TextInput* _animationDuration = nullptr;
    ayt::ui::TextInput* _terrainId = nullptr;
    ayt::ui::TextInput* _terrainName = nullptr;
    ayt::ui::TextInput* _terrainFallback = nullptr;
    ayt::ui::TextInput* _terrainRuleTile = nullptr;
    ayt::ui::Button* _pencil = nullptr;
    ayt::ui::Button* _save = nullptr;
    ayt::ui::Button* _saveAs = nullptr;
    ayt::ui::Button* _selection = nullptr;
    ayt::ui::Button* _eraser = nullptr;
    ayt::ui::Button* _fill = nullptr;
    ayt::ui::Button* _rectangle = nullptr;
    ayt::ui::Button* _terrain = nullptr;
    ayt::ui::Button* _stamp = nullptr;
    ayt::ui::Button* _shadow = nullptr;
    ayt::ui::Button* _shadowClear = nullptr;
    ayt::ui::Button* _shadowAdvanced = nullptr;
    ayt::ui::Button* _stampSelect = nullptr;
    ayt::ui::Button* _stampDelete = nullptr;
    ayt::ui::Button* _grid = nullptr;
    ayt::ui::Button* _collision = nullptr;
    ayt::ui::Button* _shadowVisibility = nullptr;
    ayt::ui::Button* _frame = nullptr;
    ayt::ui::Button* _layerVisibility = nullptr;
    ayt::ui::Button* _animationPreview = nullptr;
    std::array<ayt::ui::Button*, 8> _terrainNeighborButtons{};
    std::unique_ptr<ayt::ui::Modal> _shadowModal;
    std::array<ayt::ui::Button*, 4> _shadowQuadrantButtons{};
    ayt::ui::TextLabel* _shadowDialogStatus = nullptr;
    ayt::ui::Button* _shadowDialogCancel = nullptr;
    ayt::ui::Button* _shadowDialogApply = nullptr;
    uint8_t _pendingShadowMask = ayt::ay2d::editor::ShadowMask_All;
    std::unique_ptr<ayt::ui::Modal> _importModal;
    EditorTileAtlasPicker* _importPreview = nullptr;
    ayt::ui::TextLabel* _importSourceLabel = nullptr;
    ayt::ui::TextLabel* _importInfo = nullptr;
    ayt::ui::TextInput* _importTileWidth = nullptr;
    ayt::ui::TextInput* _importTileHeight = nullptr;
    ayt::ui::TextInput* _importMarginX = nullptr;
    ayt::ui::TextInput* _importMarginY = nullptr;
    ayt::ui::TextInput* _importSpacingX = nullptr;
    ayt::ui::TextInput* _importSpacingY = nullptr;
    ayt::ui::TextInput* _importFirstTileId = nullptr;
    ayt::ui::TextInput* _importFolder = nullptr;
    ayt::ui::Button* _importChoose = nullptr;
    ayt::ui::Button* _importCancel = nullptr;
    ayt::ui::Button* _importCommit = nullptr;
    ayt::ui::Button* _skipEmpty = nullptr;
    ayt::ui::Button* _importModeButton = nullptr;
    ayt::ui::Button* _importRegionUndo = nullptr;
    ayt::ui::Button* _importRegionClear = nullptr;
    std::vector<ayt::ui::TextInput*> _importGridInputs;
    std::vector<ayt::ui::TextInput*> _importInputs;
    std::map<uint32_t, EditorAuthoringImage> _atlasImages;
    std::map<uint32_t, ayt::ui::ImageTextureHandle> _atlasTextures;
    EditorAuthoringImage _pendingAtlasImage;
    ayt::ay2d::editor::TileAtlasImportPlan _pendingImportPlan;
    std::string _pendingAtlasPath;
    std::string _pendingAtlasName;
    AtlasMetadataDiscovery _detectedMetadata;
    std::vector<ayt::ay2d::editor::TileAtlasRegionRequest> _manualRegions;
    AtlasImportMode _importMode = AtlasImportMode::Grid;
    uint32_t _shownAtlasId = 0u;
    std::vector<ayt::ui::Button*> _buttons;
    std::vector<ayt::ui::Tooltip*> _tooltips;
    std::vector<uint32_t> _tileIds;
    std::vector<uint32_t> _atlasIds;
    std::vector<uint32_t> _stampIds;
    std::vector<uint32_t> _terrainIds;
    std::vector<ayt::ay2d::editor::AutoTileRule> _terrainRuleDraft;
    uint32_t _terrainDraftSourceId = 0u;
    int _terrainRuleIndex = -1;
    uint8_t _terrainRuleMatchMask = 0u;
    uint8_t _terrainRuleNeighborMask = 0u;
    int _animationFrameIndex = -1;
    float _zoomPercent = 100.0f;
    bool _syncing = false;
    bool _syncingImport = false;
    bool _importCommitPending = false;
    bool _skipTransparent = true;
    bool _selectingStamp = false;
    bool _editingNewTerrain = false;
    bool _ctrlDown = false;
    bool _shiftDown = false;
};

class TimedAssetDocument final
    : public IEditorDocument, public IEditorTimelineSource {
public:
    ~TimedAssetDocument() override { releaseAudio(); }

    bool initialize(const EditorOpenRequest& request, bool audio,
                    std::string& error)
    {
        _path = request.resourcePath;
        _title = std::filesystem::path(_path).filename().string();
        _type = audio ? "ayeditor.timeline.audio.document"
                      : "ayeditor.timeline.animation.document";
        _isAudio = audio;
        _sidecarPath = _path + ".timeline.json";
        if (_path.empty() || !std::filesystem::is_regular_file(_path)) {
            error = "Timeline asset does not exist: " + _path;
            return false;
        }
        if (audio) {
            _audioResource = std::make_unique<ayt::resource::Audio>();
            if (_audioResource->load(_path)) {
                _duration = _audioResource->getDuration();
                buildWaveform();
                attachAudio();
            }
            if (_duration <= 0.0) _duration = wavDuration(_path);
            _tracks.push_back({"audio", _title,
                EditorTimelineTrackKind::Audio, 0.0, _duration, true});
            _clips.push_back({"clip.1", "audio", _path, 0.0, _duration,
                0.0, 1.0f});
        } else {
            ayt::resource::Animation resource;
            if (!resource.load(_path)) {
                error = "Could not load animation timeline: " + _path;
                return false;
            }
            _duration = resource.getDuration();
            for (std::uint32_t index = 0; index < resource.getTrackCount(); ++index) {
                std::string name = resource.getTrackNodeName(index);
                const char* property = resource.getTrackProperty(index);
                if (property != nullptr && *property != '\0') {
                    name += " / ";
                    name += property;
                }
                const std::string trackId = "animation." + std::to_string(index);
                _tracks.push_back({trackId, std::move(name),
                    EditorTimelineTrackKind::Animation, 0.0, _duration, true});
                const float* times = resource.getTrackTimes(index);
                const float* values = resource.getTrackFloatValues(index);
                for (std::uint32_t key = 0;
                     key < resource.getTrackKeyframeCount(index); ++key) {
                    _keyframes.push_back({"key." + std::to_string(++_nextItemId),
                        trackId, times != nullptr ? times[key] : 0.0,
                        values != nullptr ? values[key] : 0.0});
                }
            }
            for (std::uint32_t index = 0; index < resource.getNotifyCount(); ++index) {
                _tracks.push_back({"event." + std::to_string(index),
                    resource.getNotifyName(index), EditorTimelineTrackKind::Event,
                    resource.getNotifyTime(index),
                    resource.getNotifyTime(index), true});
            }
        }
        if (_tracks.empty()) {
            _tracks.push_back({"timeline", _title,
                audio ? EditorTimelineTrackKind::Audio
                      : EditorTimelineTrackKind::Animation,
                0.0, _duration, true});
        }
        if (std::filesystem::is_regular_file(_sidecarPath)
            && !loadSidecar(error)) return false;
        _dirty = false;
        return true;
    }
    const std::string& typeId() const noexcept override { return _type; }
    const std::string& path() const noexcept override { return _path; }
    const std::string& title() const noexcept override { return _title; }
    bool isDirty() const noexcept override { return _dirty; }
    uint64_t revision() const noexcept override { return _revision; }
    bool save(std::string* error) override {
        if (!writeSequence(_sidecarPath, error)) return false;
        _dirty = false;
        return true;
    }
    double timelineDurationSeconds() const noexcept override { return _duration; }
    double timelinePositionSeconds() const noexcept override { return _position; }
    bool setTimelinePositionSeconds(double seconds) override {
        const double clamped = std::clamp(seconds, 0.0, _duration);
        if (std::abs(clamped - _position) < 1.0e-9) return false;
        _position = clamped;
        if (_playing && _isAudio) startAudioAtPlayhead();
        return true;
    }
    std::vector<EditorTimelineTrack> timelineTracks() const override {
        return _tracks;
    }
    std::vector<EditorTimelineKeyframe> timelineKeyframes() const override {
        return _keyframes;
    }
    std::vector<EditorTimelineClip> timelineClips() const override {
        return _clips;
    }
    std::vector<float> timelineWaveformPeaks(
        const std::string& trackId) const override {
        return trackId == "audio" ? _waveform : std::vector<float>{};
    }
    bool timelinePlaying() const noexcept override { return _playing; }
    void timelinePlay() override {
        if (_duration <= 0.0) return;
        if (_position >= _duration) _position = 0.0;
        _playing = true;
        if (_isAudio) startAudioAtPlayhead();
    }
    void timelinePause() override {
        _playing = false;
        stopAudioVoice();
    }
    void timelineStop() override {
        _playing = false;
        stopAudioVoice();
        _position = 0.0;
    }
    void timelineTick(double seconds) override {
        if (!_playing || seconds <= 0.0) return;
        bool audioClock = false;
        if (_audioEngine != nullptr && _audioVoice != ayt::audio::InvalidVoice) {
            const ayt::audio::VoiceRuntime* voice =
                _audioEngine->voiceAt(_audioVoice - 1u);
            if (voice != nullptr && voice->playing && _audioSampleRate != 0u) {
                _position = _playingClipStart
                    + static_cast<double>(_audioEngine->voicePositionFrames(
                        _audioVoice)) / static_cast<double>(_audioSampleRate)
                    - _playingSourceOffset;
                audioClock = true;
            }
        }
        if (!audioClock) _position += seconds;
        if (_position >= _duration) {
            _position = _duration;
            _playing = false;
            stopAudioVoice();
        }
    }
    bool timelineAddKeyframe(const std::string& trackId, double time,
                             double value) override {
        if (!hasTrack(trackId)) return false;
        beginEdit();
        _keyframes.push_back({"key." + std::to_string(++_nextItemId), trackId,
            std::clamp(time, 0.0, _duration), value});
        sortItems();
        return true;
    }
    bool timelineMoveKeyframe(const std::string& id, double time) override {
        auto it = std::find_if(_keyframes.begin(), _keyframes.end(),
            [&id](const auto& key) { return key.id == id; });
        if (it == _keyframes.end()) return false;
        beginEdit();
        it->timeSeconds = std::clamp(time, 0.0, _duration);
        sortItems();
        return true;
    }
    bool timelineRemoveKeyframe(const std::string& id) override {
        const auto it = std::find_if(_keyframes.begin(), _keyframes.end(),
            [&id](const auto& key) { return key.id == id; });
        if (it == _keyframes.end()) return false;
        beginEdit();
        _keyframes.erase(it);
        return true;
    }
    bool timelineAddClip(const EditorTimelineClip& value) override {
        if (!hasTrack(value.trackId) || value.durationSeconds <= 0.0) return false;
        beginEdit();
        EditorTimelineClip clip = value;
        if (clip.id.empty()) clip.id = "clip." + std::to_string(++_nextItemId);
        clip.startSeconds = std::max(0.0, clip.startSeconds);
        clip.sourceOffsetSeconds = std::max(0.0, clip.sourceOffsetSeconds);
        clip.gain = std::clamp(clip.gain, 0.0f, 8.0f);
        _duration = std::max(_duration,
            clip.startSeconds + clip.durationSeconds);
        _clips.push_back(std::move(clip));
        sortItems();
        return true;
    }
    bool timelineMoveClip(const std::string& id, double start) override {
        auto it = std::find_if(_clips.begin(), _clips.end(),
            [&id](const auto& clip) { return clip.id == id; });
        if (it == _clips.end()) return false;
        beginEdit();
        it->startSeconds = std::max(0.0, start);
        _duration = std::max(_duration,
            it->startSeconds + it->durationSeconds);
        sortItems();
        return true;
    }
    bool timelineRemoveClip(const std::string& id) override {
        const auto it = std::find_if(_clips.begin(), _clips.end(),
            [&id](const auto& clip) { return clip.id == id; });
        if (it == _clips.end()) return false;
        beginEdit();
        _clips.erase(it);
        return true;
    }
    bool timelineCanUndo() const noexcept override { return !_undo.empty(); }
    bool timelineCanRedo() const noexcept override { return !_redo.empty(); }
    bool timelineUndo() override {
        if (_undo.empty()) return false;
        _redo.push_back(snapshot());
        restore(_undo.back());
        _undo.pop_back();
        markChanged();
        return true;
    }
    bool timelineRedo() override {
        if (_redo.empty()) return false;
        _undo.push_back(snapshot());
        restore(_redo.back());
        _redo.pop_back();
        markChanged();
        return true;
    }
private:
    struct State {
        double duration = 0.0;
        std::vector<EditorTimelineKeyframe> keyframes;
        std::vector<EditorTimelineClip> clips;
    };

    bool hasTrack(const std::string& id) const {
        return std::any_of(_tracks.begin(), _tracks.end(),
            [&id](const auto& track) { return track.id == id; });
    }
    State snapshot() const { return {_duration, _keyframes, _clips}; }
    void restore(const State& value) {
        _duration = value.duration;
        _keyframes = value.keyframes;
        _clips = value.clips;
        _position = std::min(_position, _duration);
    }
    void beginEdit() {
        _undo.push_back(snapshot());
        if (_undo.size() > 128u) _undo.erase(_undo.begin());
        _redo.clear();
        markChanged();
    }
    void markChanged() { _dirty = true; ++_revision; }
    void sortItems() {
        std::stable_sort(_keyframes.begin(), _keyframes.end(),
            [](const auto& a, const auto& b) {
                return a.timeSeconds < b.timeSeconds;
            });
        std::stable_sort(_clips.begin(), _clips.end(),
            [](const auto& a, const auto& b) {
                return a.startSeconds < b.startSeconds;
            });
    }
    bool writeSequence(const std::string& path, std::string* error) const {
        try {
            nlohmann::json document = {
                {"schemaVersion", 1}, {"source", _path},
                {"durationSeconds", _duration},
                {"keyframes", nlohmann::json::array()},
                {"clips", nlohmann::json::array()}};
            for (const auto& key : _keyframes) {
                document["keyframes"].push_back({{"id", key.id},
                    {"track", key.trackId}, {"time", key.timeSeconds},
                    {"value", key.value}});
            }
            for (const auto& clip : _clips) {
                document["clips"].push_back({{"id", clip.id},
                    {"track", clip.trackId}, {"source", clip.sourcePath},
                    {"start", clip.startSeconds},
                    {"duration", clip.durationSeconds},
                    {"sourceOffset", clip.sourceOffsetSeconds},
                    {"gain", clip.gain}});
            }
            const std::filesystem::path destination(path);
            if (!destination.parent_path().empty()) {
                std::filesystem::create_directories(destination.parent_path());
            }
            std::ofstream output(destination, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("could not open output");
            output << document.dump(2) << '\n';
            if (!output) throw std::runtime_error("write failed");
            if (error != nullptr) error->clear();
            return true;
        } catch (const std::exception& exception) {
            if (error != nullptr) *error = exception.what();
            return false;
        }
    }
    bool loadSidecar(std::string& error) {
        try {
            std::ifstream input(_sidecarPath, std::ios::binary);
            nlohmann::json document;
            input >> document;
            std::vector<EditorTimelineKeyframe> keys;
            std::vector<EditorTimelineClip> clips;
            for (const auto& key : document.value(
                     "keyframes", nlohmann::json::array())) {
                EditorTimelineKeyframe value;
                value.id = key.value("id", "key." + std::to_string(++_nextItemId));
                value.trackId = key.value("track", std::string{});
                value.timeSeconds = key.value("time", 0.0);
                value.value = key.value("value", 0.0);
                if (hasTrack(value.trackId)) keys.push_back(std::move(value));
            }
            for (const auto& clip : document.value(
                     "clips", nlohmann::json::array())) {
                EditorTimelineClip value;
                value.id = clip.value("id", "clip." + std::to_string(++_nextItemId));
                value.trackId = clip.value("track", std::string{});
                value.sourcePath = clip.value("source", std::string{});
                value.startSeconds = clip.value("start", 0.0);
                value.durationSeconds = clip.value("duration", 0.0);
                value.sourceOffsetSeconds = clip.value("sourceOffset", 0.0);
                value.gain = clip.value("gain", 1.0f);
                if (hasTrack(value.trackId) && value.durationSeconds > 0.0)
                    clips.push_back(std::move(value));
            }
            _duration = std::max(_duration,
                document.value("durationSeconds", _duration));
            _keyframes = std::move(keys);
            _clips = std::move(clips);
            sortItems();
            return true;
        } catch (const std::exception& exception) {
            error = "Could not load timeline sidecar: ";
            error += exception.what();
            return false;
        }
    }
    void buildWaveform() {
        if (_audioResource == nullptr || _audioResource->getData() == nullptr
            || _audioResource->getChannels() == 0u) return;
        const std::uint32_t bits = _audioResource->getBitsPerSample();
        const std::uint32_t channels = _audioResource->getChannels();
        const std::size_t bytesPerSample = bits / 8u;
        if ((bits != 16u && bits != 32u) || bytesPerSample == 0u) return;
        const std::size_t frames = _audioResource->getDataSize()
            / (bytesPerSample * channels);
        if (frames == 0u) return;
        constexpr std::size_t buckets = 96u;
        _waveform.reserve(buckets * 2u);
        const std::uint8_t* bytes = _audioResource->getData();
        for (std::size_t bucket = 0; bucket < buckets; ++bucket) {
            const std::size_t begin = bucket * frames / buckets;
            const std::size_t end = std::max(begin + 1u,
                (bucket + 1u) * frames / buckets);
            float minimum = 1.0f;
            float maximum = -1.0f;
            for (std::size_t frame = begin; frame < std::min(end, frames); ++frame) {
                float sample = 0.0f;
                const std::uint8_t* source = bytes
                    + frame * channels * bytesPerSample;
                if (bits == 16u) {
                    std::int16_t value = 0;
                    std::memcpy(&value, source, sizeof(value));
                    sample = static_cast<float>(value) / 32768.0f;
                } else {
                    std::memcpy(&sample, source, sizeof(sample));
                }
                minimum = std::min(minimum, sample);
                maximum = std::max(maximum, sample);
            }
            _waveform.push_back(std::clamp(minimum, -1.0f, 1.0f));
            _waveform.push_back(std::clamp(maximum, -1.0f, 1.0f));
        }
    }
    void attachAudio() {
        if (_audioResource == nullptr || _audioClip != ayt::audio::InvalidClipId)
            return;
        _audioSubsystem = ayt::audio::AudioSubSystem::findRegistered();
        _audioEngine = _audioSubsystem != nullptr ? _audioSubsystem->engine() : nullptr;
        if (_audioEngine == nullptr || !_audioEngine->isInitialized()) return;
        _audioClip = _audioSubsystem->registerClipFromResource(_audioResource.get());
        _audioSampleRate = _audioResource->getSampleRate();
    }
    void stopAudioVoice() {
        if (_audioEngine != nullptr && _audioEngine->isInitialized()
            && _audioVoice != ayt::audio::InvalidVoice) {
            _audioEngine->stop(_audioVoice);
        }
        _audioVoice = ayt::audio::InvalidVoice;
    }
    void startAudioAtPlayhead() {
        stopAudioVoice();
        attachAudio();
        if (_audioEngine == nullptr || _audioClip == ayt::audio::InvalidClipId)
            return;
        const auto clip = std::find_if(_clips.begin(), _clips.end(),
            [this](const EditorTimelineClip& value) {
                return value.trackId == "audio"
                    && _position >= value.startSeconds
                    && _position < value.startSeconds + value.durationSeconds;
            });
        if (clip == _clips.end()) return;
        _audioVoice = _audioEngine->play(_audioClip, ayt::audio::AudioBus::Music,
            {}, clip->gain, false);
        if (_audioVoice == ayt::audio::InvalidVoice || _audioSampleRate == 0u)
            return;
        _playingClipStart = clip->startSeconds;
        _playingSourceOffset = clip->sourceOffsetSeconds;
        const double sourceSeconds = clip->sourceOffsetSeconds
            + (_position - clip->startSeconds);
        (void)_audioEngine->setVoicePositionFrames(_audioVoice,
            static_cast<std::uint64_t>(sourceSeconds * _audioSampleRate));
    }
    void releaseAudio() {
        stopAudioVoice();
        if (_audioSubsystem != nullptr && _audioEngine != nullptr
            && _audioSubsystem->engine() == _audioEngine
            && _audioEngine->isInitialized()
            && _audioClip != ayt::audio::InvalidClipId) {
            _audioEngine->releaseClip(_audioClip);
        }
        _audioClip = ayt::audio::InvalidClipId;
        _audioEngine = nullptr;
        _audioSubsystem = nullptr;
    }
    static double wavDuration(const std::string& path)
    {
        std::ifstream input(path, std::ios::binary);
        char riff[4]{};
        std::uint32_t ignored = 0;
        char wave[4]{};
        input.read(riff, 4);
        input.read(reinterpret_cast<char*>(&ignored), 4);
        input.read(wave, 4);
        if (!input || std::string(riff, 4) != "RIFF"
            || std::string(wave, 4) != "WAVE") return 0.0;
        std::uint32_t byteRate = 0;
        std::uint32_t dataBytes = 0;
        while (input) {
            char id[4]{};
            std::uint32_t size = 0;
            input.read(id, 4);
            input.read(reinterpret_cast<char*>(&size), 4);
            if (!input) break;
            if (std::string(id, 4) == "fmt " && size >= 12u) {
                input.seekg(4, std::ios::cur);
                input.seekg(4, std::ios::cur);
                input.read(reinterpret_cast<char*>(&byteRate), 4);
                input.seekg(static_cast<std::streamoff>(size - 12u), std::ios::cur);
            } else if (std::string(id, 4) == "data") {
                dataBytes = size;
                input.seekg(size, std::ios::cur);
            } else {
                input.seekg(size, std::ios::cur);
            }
            if ((size & 1u) != 0u) input.seekg(1, std::ios::cur);
            if (byteRate != 0u && dataBytes != 0u) break;
        }
        return byteRate == 0u ? 0.0
            : static_cast<double>(dataBytes) / byteRate;
    }
    std::string _type;
    std::string _path;
    std::string _title;
    std::string _sidecarPath;
    double _duration = 0.0;
    double _position = 0.0;
    bool _playing = false;
    bool _isAudio = false;
    bool _dirty = false;
    std::uint64_t _revision = 1u;
    std::uint64_t _nextItemId = 0u;
    std::vector<EditorTimelineTrack> _tracks;
    std::vector<EditorTimelineKeyframe> _keyframes;
    std::vector<EditorTimelineClip> _clips;
    std::vector<float> _waveform;
    std::vector<State> _undo;
    std::vector<State> _redo;
    std::unique_ptr<ayt::resource::Audio> _audioResource;
    ayt::audio::AudioSubSystem* _audioSubsystem = nullptr;
    ayt::audio::AudioEngine* _audioEngine = nullptr;
    ayt::audio::AudioClipId _audioClip = ayt::audio::InvalidClipId;
    ayt::audio::VoiceHandle _audioVoice = ayt::audio::InvalidVoice;
    std::uint32_t _audioSampleRate = 0u;
    double _playingClipStart = 0.0;
    double _playingSourceOffset = 0.0;
};

class TimedAssetView final : public IEditorView, public IEditorCommandTarget {
public:
    explicit TimedAssetView(std::shared_ptr<TimedAssetDocument> document)
        : _document(std::move(document))
    {
        auto* root = new ayt::ui::VBox();
        _root = root;
        root->setSpacing(8.0f);
        root->setPadding(12.0f, 10.0f, 12.0f, 10.0f);
        auto* heading = new ayt::ui::TextLabel();
        heading->setText(ayt::ui::decodeUtf8Text(_document->title()));
        heading->setFontSize(15);
        root->addWidget(heading, 28.0f);
        auto* summary = new ayt::ui::TextLabel();
        summary->setText(L"Duration: "
            + std::to_wstring(_document->timelineDurationSeconds())
            + L" s   Tracks: "
            + std::to_wstring(_document->timelineTracks().size()));
        root->addWidget(summary, 26.0f);
        auto* note = new ayt::ui::TextLabel();
        note->setText(L"Open Timeline to edit clips, keyframes and waveform timing.");
        root->addWidget(note, 28.0f);
        auto* actions = new ayt::ui::HBox();
        actions->setSpacing(6.0f);
        addButton(actions, L"Add Key", [this]() {
            const auto tracks = _document->timelineTracks();
            if (!tracks.empty()) (void)_document->timelineAddKeyframe(
                tracks.front().id, _document->timelinePositionSeconds(), 0.0);
        });
        addButton(actions, L"Add Clip", [this]() {
            const auto tracks = _document->timelineTracks();
            if (tracks.empty()) return;
            EditorTimelineClip clip;
            clip.trackId = tracks.front().id;
            clip.sourcePath = _document->path();
            clip.startSeconds = _document->timelinePositionSeconds();
            clip.durationSeconds = std::max(0.1,
                _document->timelineDurationSeconds() * 0.25);
            (void)_document->timelineAddClip(clip);
        });
        addButton(actions, L"Undo", [this]() {
            (void)_document->timelineUndo();
        });
        addButton(actions, L"Redo", [this]() {
            (void)_document->timelineRedo();
        });
        addButton(actions, L"Save", [this]() {
            std::string error;
            (void)_document->save(&error);
        });
        root->addWidget(actions, 28.0f);
    }
    ~TimedAssetView() override {
        if (_root != nullptr) ayt::ui::destroyWidgetTree(_root);
    }
    ayt::ui::Widget* rootWidget() noexcept override { return _root; }
    ayt::ui::Widget* releaseRootWidget() noexcept override {
        auto* result = _root; _root = nullptr; return result;
    }
    IEditorCommandTarget* commandTarget() noexcept override { return this; }
    bool handlesCommand(const std::string& id) const override {
        return id == "file.save" || id == "edit.undo" || id == "edit.redo";
    }
    bool canExecuteCommand(const std::string& id) const override {
        if (id == "edit.undo") return _document->timelineCanUndo();
        if (id == "edit.redo") return _document->timelineCanRedo();
        return id == "file.save" && _document->isDirty();
    }
    bool executeCommand(const std::string& id) override {
        if (id == "edit.undo") return _document->timelineUndo();
        if (id == "edit.redo") return _document->timelineRedo();
        if (id == "file.save") {
            std::string error;
            return _document->save(&error);
        }
        return false;
    }
private:
    static void addButton(ayt::ui::HBox* row, const std::wstring& text,
                          std::function<void()> callback) {
        auto* button = new ayt::ui::Button();
        button->setText(text);
        button->setOnClicked(std::move(callback));
        row->addWidget(button, 76.0f);
    }
    std::shared_ptr<TimedAssetDocument> _document;
    ayt::ui::Widget* _root = nullptr;
};

class AudioToolView final : public IEditorView {
public:
    explicit AudioToolView(std::function<void()> openAudio)
        : _openAudio(std::move(openAudio))
    {
        auto* root = new ayt::ui::VBox();
        _root = root;
        root->setSpacing(6.0f);
        root->setPadding(12.0f, 10.0f, 12.0f, 10.0f);
        auto* heading = new ayt::ui::TextLabel();
        heading->setText(L"Audio Mixer");
        heading->setFontSize(15);
        root->addWidget(heading, 26.0f);
        _status = new ayt::ui::TextLabel();
        _status->setFontSize(12);
        root->addWidget(_status, 22.0f);
        auto* transport = new ayt::ui::HBox();
        transport->setSpacing(6.0f);
        addButton(transport, L"Pause", [this]() {
            if (auto* value = engine()) value->pause();
        });
        addButton(transport, L"Resume", [this]() {
            if (auto* value = engine()) value->resume();
        });
        addButton(transport, L"Stop All", [this]() {
            if (auto* value = engine()) value->stopAll();
        });
        addButton(transport, L"Full Editor", [this]() {
            if (_openAudio) _openAudio();
        });
        root->addWidget(transport, 28.0f);
        addGain(root, L"Master", ayt::audio::AudioBus::Master, true);
        addGain(root, L"Music", ayt::audio::AudioBus::Music, false);
        addGain(root, L"SFX", ayt::audio::AudioBus::Sfx, false);
        addGain(root, L"UI", ayt::audio::AudioBus::Ui, false);
        addGain(root, L"Voice", ayt::audio::AudioBus::Voice, false);
        tick(0.0f);
    }
    ~AudioToolView() override {
        if (_root != nullptr) ayt::ui::destroyWidgetTree(_root);
    }
    ayt::ui::Widget* rootWidget() noexcept override { return _root; }
    ayt::ui::Widget* releaseRootWidget() noexcept override {
        auto* result = _root; _root = nullptr; return result;
    }
    void tick(float) override {
        auto* value = engine();
        const std::wstring status = value == nullptr
            ? L"Audio subsystem is unavailable."
            : std::wstring(value->isPaused() ? L"Paused" : L"Running")
                + L"   Active voices: "
                + std::to_wstring(value->activeVoiceCount());
        if (_status->getText() != status) _status->setText(status);
        if (value == nullptr) return;
        _updating = true;
        for (GainControl& control : _controls) {
            const float gain = control.master
                ? value->mixer().getMasterGain()
                : value->mixer().getBusGain(control.bus);
            if (std::abs(control.slider->getValue() - gain) > 0.001f) {
                control.slider->setValue(gain);
            }
            control.value->setText(std::to_wstring(
                static_cast<int>(std::round(gain * 100.0f))) + L"%");
        }
        _updating = false;
    }
    bool wantsBackgroundTick() const noexcept override { return true; }
private:
    struct GainControl {
        ayt::audio::AudioBus bus;
        bool master;
        ayt::ui::Slider* slider;
        ayt::ui::TextLabel* value;
    };
    static ayt::audio::AudioEngine* engine() {
        ayt::audio::AudioSubSystem* subsystem =
            ayt::audio::AudioSubSystem::findRegistered();
        return subsystem != nullptr ? subsystem->engine() : nullptr;
    }
    static void addButton(ayt::ui::HBox* row, const std::wstring& text,
                          std::function<void()> clicked) {
        auto* button = new ayt::ui::Button();
        button->setText(text);
        button->setOnClicked(std::move(clicked));
        row->addWidget(button, 82.0f);
    }
    void addGain(ayt::ui::VBox* root, const std::wstring& name,
                 ayt::audio::AudioBus bus, bool master) {
        auto* row = new ayt::ui::HBox();
        row->setSpacing(6.0f);
        auto* label = new ayt::ui::TextLabel();
        label->setText(name);
        row->addWidget(label, 64.0f);
        auto* slider = new ayt::ui::Slider();
        slider->setValueRange(0.0f, 1.5f);
        slider->setValue(1.0f);
        slider->setOnValueChanged([this, bus, master](float gain) {
            if (_updating) return;
            if (auto* value = engine()) {
                if (master) value->setMasterGain(gain);
                else value->setBusGain(bus, gain);
            }
        });
        row->addWidget(slider, 0.0f);
        auto* amount = new ayt::ui::TextLabel();
        amount->setText(L"100%");
        row->addWidget(amount, 48.0f);
        root->addWidget(row, 26.0f);
        _controls.push_back({bus, master, slider, amount});
    }
    std::function<void()> _openAudio;
    ayt::ui::Widget* _root = nullptr;
    ayt::ui::TextLabel* _status = nullptr;
    std::vector<GainControl> _controls;
    bool _updating = false;
};

class TimelineToolView final : public IEditorView {
public:
    explicit TimelineToolView(IEditorHostServices& host) : _host(host)
    {
        auto* root = new ayt::ui::VBox();
        _root = root;
        root->setSpacing(5.0f);
        root->setPadding(12.0f, 10.0f, 12.0f, 10.0f);
        auto* transport = new ayt::ui::HBox();
        transport->setSpacing(6.0f);
        addButton(transport, L"Play", [this]() {
            if (auto* value = source()) value->timelinePlay();
        });
        addButton(transport, L"Pause", [this]() {
            if (auto* value = source()) value->timelinePause();
        });
        addButton(transport, L"Stop", [this]() {
            if (auto* value = source()) value->timelineStop();
        });
        root->addWidget(transport, 28.0f);
        auto* edit = new ayt::ui::HBox();
        edit->setSpacing(4.0f);
        addButton(edit, L"Key +", [this]() { addKeyframe(); }, 58.0f);
        addButton(edit, L"Clip +", [this]() { addClip(); }, 58.0f);
        addButton(edit, L"< 0.1", [this]() { nudgeSelection(-0.1); }, 58.0f);
        addButton(edit, L"0.1 >", [this]() { nudgeSelection(0.1); }, 58.0f);
        addButton(edit, L"Delete", [this]() { deleteSelection(); }, 62.0f);
        addButton(edit, L"Undo", [this]() {
            if (auto* value = source()) (void)value->timelineUndo();
        }, 58.0f);
        addButton(edit, L"Redo", [this]() {
            if (auto* value = source()) (void)value->timelineRedo();
        }, 58.0f);
        addButton(edit, L"Save", [this]() { saveSource(); }, 58.0f);
        root->addWidget(edit, 28.0f);
        _label = new ayt::ui::TextLabel();
        _label->setFontSize(12);
        root->addWidget(_label, 22.0f);
        _ruler = new ayt::ui::TextLabel();
        _ruler->setFontSize(11);
        root->addWidget(_ruler, 20.0f);
        _playhead = new ayt::ui::Slider();
        _playhead->setValueRange(0.0f, 1.0f);
        _playhead->setOnValueChanged([this](float seconds) {
            if (_updating) return;
            if (auto* value = source()) {
                (void)value->setTimelinePositionSeconds(seconds);
            }
        });
        root->addWidget(_playhead, 24.0f);
        _waveform = new ayt::ui::TextLabel();
        _waveform->setFontSize(11);
        root->addWidget(_waveform, 20.0f);
        for (std::size_t index = 0; index < 8u; ++index) {
            auto* track = new ayt::ui::TextLabel();
            track->setFontSize(11);
            root->addWidget(track, 20.0f);
            _trackLabels.push_back(track);
        }
        tick(0.0f);
    }
    ~TimelineToolView() override {
        if (_root != nullptr) ayt::ui::destroyWidgetTree(_root);
    }
    ayt::ui::Widget* rootWidget() noexcept override { return _root; }
    ayt::ui::Widget* releaseRootWidget() noexcept override {
        auto* result = _root; _root = nullptr; return result;
    }
    void tick(float dt) override {
        IEditorTimelineSource* timeline = source();
        std::string sourceTitle;
        for (const EditorDocumentRecord& record :
             _host.workspace().documents().records()) {
            if (record.editorId == kEditorTimelineToolExtensionId) continue;
            if (dynamic_cast<IEditorTimelineSource*>(record.document.get())
                == timeline) {
                sourceTitle = record.document->title();
                break;
            }
        }
        if (timeline != nullptr) timeline->timelineTick(dt);
        const std::wstring text = timeline == nullptr
            ? L"No open document exposes timeline data."
            : ayt::ui::decodeUtf8Text(sourceTitle) + L"   "
                + std::to_wstring(timeline->timelinePositionSeconds()) + L" / "
                + std::to_wstring(timeline->timelineDurationSeconds()) + L" s"
                + (timeline->timelinePlaying() ? L"   Playing" : L"");
        if (_label->getText() != text) {
            _label->setText(text);
            _host.requestRepaint();
        }
        const double duration = timeline != nullptr
            ? timeline->timelineDurationSeconds() : 0.0;
        _ruler->setText(duration > 0.0
            ? L"0 s                 " + std::to_wstring(duration * 0.25)
                + L"                 " + std::to_wstring(duration * 0.5)
                + L"                 " + std::to_wstring(duration * 0.75)
                + L"                 " + std::to_wstring(duration) + L" s"
            : L"0 s");
        _updating = true;
        _playhead->setValueRange(0.0f,
            static_cast<float>(std::max(0.001, duration)));
        _playhead->setValue(static_cast<float>(timeline != nullptr
            ? timeline->timelinePositionSeconds() : 0.0));
        _updating = false;
        const std::vector<EditorTimelineTrack> tracks = timeline != nullptr
            ? timeline->timelineTracks() : std::vector<EditorTimelineTrack>{};
        const std::vector<EditorTimelineKeyframe> keys = timeline != nullptr
            ? timeline->timelineKeyframes()
            : std::vector<EditorTimelineKeyframe>{};
        const std::vector<EditorTimelineClip> clips = timeline != nullptr
            ? timeline->timelineClips() : std::vector<EditorTimelineClip>{};
        std::wstring waveformText;
        if (timeline != nullptr) {
            for (const EditorTimelineTrack& track : tracks) {
                const std::vector<float> peaks =
                    timeline->timelineWaveformPeaks(track.id);
                if (peaks.empty()) continue;
                static constexpr wchar_t levels[] = L"_▁▂▃▄▅▆▇█";
                waveformText = L"Waveform  ";
                const std::size_t pairs = peaks.size() / 2u;
                const std::size_t shown = std::min<std::size_t>(64u, pairs);
                for (std::size_t index = 0; index < shown; ++index) {
                    const std::size_t sourceIndex = index * pairs / shown;
                    const float amplitude = std::max(
                        std::abs(peaks[sourceIndex * 2u]),
                        std::abs(peaks[sourceIndex * 2u + 1u]));
                    waveformText.push_back(levels[static_cast<std::size_t>(
                        std::clamp(amplitude, 0.0f, 1.0f) * 8.0f)]);
                }
                break;
            }
        }
        _waveform->setText(waveformText);
        _waveform->setVisible(!waveformText.empty());
        for (std::size_t index = 0; index < _trackLabels.size(); ++index) {
            if (index >= tracks.size()) {
                _trackLabels[index]->setText(L"");
                _trackLabels[index]->setVisible(false);
                continue;
            }
            const EditorTimelineTrack& track = tracks[index];
            const wchar_t* marker = track.kind == EditorTimelineTrackKind::Audio
                ? L"♪" : (track.kind == EditorTimelineTrackKind::Event
                    ? L"◆" : L"●");
            _trackLabels[index]->setText(std::wstring(marker) + L"  "
                + ayt::ui::decodeUtf8Text(track.name) + L"   ["
                + std::to_wstring(track.startSeconds) + L" – "
                + std::to_wstring(track.endSeconds) + L"]   keys "
                + std::to_wstring(std::count_if(keys.begin(), keys.end(),
                    [&track](const auto& key) {
                        return key.trackId == track.id;
                    })) + L"   clips "
                + std::to_wstring(std::count_if(clips.begin(), clips.end(),
                    [&track](const auto& clip) {
                        return clip.trackId == track.id;
                    })));
            _trackLabels[index]->setVisible(true);
        }
    }
    bool wantsBackgroundTick() const noexcept override { return true; }
private:
    IEditorTimelineSource* source() const {
        for (const EditorDocumentRecord& record :
             _host.workspace().documents().records()) {
            if (record.editorId == kEditorTimelineToolExtensionId) continue;
            if (auto* value = dynamic_cast<IEditorTimelineSource*>(
                    record.document.get())) return value;
        }
        return nullptr;
    }
    void addKeyframe() {
        IEditorTimelineSource* value = source();
        if (value == nullptr) return;
        const auto tracks = value->timelineTracks();
        if (tracks.empty()) return;
        if (value->timelineAddKeyframe(tracks.front().id,
                value->timelinePositionSeconds(), 0.0)) {
            const auto keys = value->timelineKeyframes();
            if (!keys.empty()) _selectedItemId = keys.back().id;
            _host.requestRepaint();
        }
    }
    void addClip() {
        IEditorTimelineSource* value = source();
        if (value == nullptr) return;
        const auto tracks = value->timelineTracks();
        if (tracks.empty()) return;
        EditorTimelineClip clip;
        clip.trackId = tracks.front().id;
        clip.startSeconds = value->timelinePositionSeconds();
        clip.durationSeconds = std::max(0.1,
            value->timelineDurationSeconds() * 0.25);
        if (value->timelineAddClip(clip)) {
            const auto clips = value->timelineClips();
            if (!clips.empty()) _selectedItemId = clips.back().id;
            _host.requestRepaint();
        }
    }
    void nudgeSelection(double delta) {
        IEditorTimelineSource* value = source();
        if (value == nullptr || _selectedItemId.empty()) return;
        const auto keys = value->timelineKeyframes();
        const auto key = std::find_if(keys.begin(), keys.end(), [this](const auto& item) {
            return item.id == _selectedItemId;
        });
        if (key != keys.end()) {
            (void)value->timelineMoveKeyframe(key->id, key->timeSeconds + delta);
            return;
        }
        const auto clips = value->timelineClips();
        const auto clip = std::find_if(clips.begin(), clips.end(), [this](const auto& item) {
            return item.id == _selectedItemId;
        });
        if (clip != clips.end())
            (void)value->timelineMoveClip(clip->id, clip->startSeconds + delta);
    }
    void deleteSelection() {
        IEditorTimelineSource* value = source();
        if (value == nullptr || _selectedItemId.empty()) return;
        if (!value->timelineRemoveKeyframe(_selectedItemId))
            (void)value->timelineRemoveClip(_selectedItemId);
        _selectedItemId.clear();
    }
    void saveSource() {
        for (const EditorDocumentRecord& record :
             _host.workspace().documents().records()) {
            if (dynamic_cast<IEditorTimelineSource*>(record.document.get())
                != source()) continue;
            std::string error;
            if (!record.document->save(&error)) {
                _host.setStatusText(L"Timeline save failed: "
                    + ayt::ui::decodeUtf8Text(error));
            } else {
                _host.setStatusText(L"Timeline sequence saved.");
            }
            break;
        }
    }
    static void addButton(ayt::ui::HBox* row, const std::wstring& text,
                          std::function<void()> clicked,
                          float width = 72.0f) {
        auto* button = new ayt::ui::Button();
        button->setText(text);
        button->setOnClicked(std::move(clicked));
        row->addWidget(button, width);
    }
    IEditorHostServices& _host;
    ayt::ui::Widget* _root = nullptr;
    ayt::ui::TextLabel* _label = nullptr;
    ayt::ui::TextLabel* _ruler = nullptr;
    ayt::ui::Slider* _playhead = nullptr;
    ayt::ui::TextLabel* _waveform = nullptr;
    std::vector<ayt::ui::TextLabel*> _trackLabels;
    std::string _selectedItemId;
    bool _updating = false;
};

bool add(EditorExtensionRegistry& registry, EditorDescriptor descriptor,
         std::string* error)
{
    std::string localError;
    if (registry.registerEditor(std::move(descriptor), &localError)) return true;
    if (error != nullptr) *error = std::move(localError);
    return false;
}

} // namespace

bool registerEditorBuiltInExtensions(
    EditorExtensionRegistry& registry,
    EditorBuiltInExtensionConfig config,
    std::string* error)
{
    EditorDescriptor tilemap;
    tilemap.id = kEditorTilemapExtensionId;
    tilemap.displayName = L"Tilemap";
    tilemap.surfaceKind = EditorSurfaceKind::Document;
    tilemap.openPolicy = EditorOpenPolicy::PerResource;
    tilemap.defaultDockSlot = EditorDockSlot::Center;
    tilemap.extensions = {".aytilemap", ".aytilemap.json"};
    tilemap.assetTypes = {"Tilemap"};
    tilemap.createDocument = [](const EditorOpenRequest& request,
                                std::string& localError) {
        auto document = std::make_shared<TilemapWorkspaceDocument>();
        return document->initialize(request, localError)
            ? std::static_pointer_cast<IEditorDocument>(document) : nullptr;
    };
    tilemap.createView = [](const std::shared_ptr<IEditorDocument>& document,
                            IEditorHostServices& host) {
        auto tilemapDocument = std::dynamic_pointer_cast<
            TilemapWorkspaceDocument>(document);
        return tilemapDocument != nullptr
            ? std::unique_ptr<IEditorView>(
                std::make_unique<TilemapWorkspaceView>(tilemapDocument, host))
            : nullptr;
    };
    if (!add(registry, std::move(tilemap), error)) return false;

    auto addTimedAsset = [&registry, error](
        const char* id, const wchar_t* displayName, bool isAudio,
        std::vector<std::string> extensions,
        std::vector<std::string> assetTypes) {
        EditorDescriptor descriptor;
        descriptor.id = id;
        descriptor.displayName = displayName;
        descriptor.surfaceKind = EditorSurfaceKind::Document;
        descriptor.openPolicy = EditorOpenPolicy::PerResource;
        descriptor.defaultDockSlot = EditorDockSlot::Center;
        descriptor.extensions = std::move(extensions);
        descriptor.assetTypes = std::move(assetTypes);
        descriptor.createDocument = [isAudio](const EditorOpenRequest& request,
                                              std::string& localError) {
            auto document = std::make_shared<TimedAssetDocument>();
            return document->initialize(request, isAudio, localError)
                ? std::static_pointer_cast<IEditorDocument>(document) : nullptr;
        };
        descriptor.createView = [](
            const std::shared_ptr<IEditorDocument>& document,
            IEditorHostServices&) {
            auto timed = std::dynamic_pointer_cast<TimedAssetDocument>(document);
            return timed != nullptr
                ? std::unique_ptr<IEditorView>(
                    std::make_unique<TimedAssetView>(std::move(timed)))
                : nullptr;
        };
        return add(registry, std::move(descriptor), error);
    };
    if (!addTimedAsset(kEditorAnimationTimelineExtensionId, L"Animation",
            false, {".ayanm", ".ayanim"}, {"Animation"})) return false;
    if (!addTimedAsset(kEditorAudioTimelineExtensionId, L"Audio",
            true, {".ayaudio", ".wav", ".ogg", ".mp3", ".flac"},
            {"Audio"})) return false;

    EditorDescriptor audio;
    audio.id = kEditorAudioToolExtensionId;
    audio.displayName = L"Audio Mixer";
    audio.surfaceKind = EditorSurfaceKind::ToolPanel;
    audio.openPolicy = EditorOpenPolicy::Singleton;
    audio.defaultDockSlot = EditorDockSlot::Bottom;
    audio.createDocument = [](const EditorOpenRequest&, std::string&) {
        return std::make_shared<ToolDocument>(
            "ayeditor.tool.audio.document", "Audio Mixer");
    };
    audio.createView = [openAudio = std::move(config.openAudioMixer)](
        const std::shared_ptr<IEditorDocument>&, IEditorHostServices&) {
        return std::unique_ptr<IEditorView>(
            std::make_unique<AudioToolView>(openAudio));
    };
    if (!add(registry, std::move(audio), error)) return false;

    EditorDescriptor timeline;
    timeline.id = kEditorTimelineToolExtensionId;
    timeline.displayName = L"Timeline";
    timeline.surfaceKind = EditorSurfaceKind::ToolPanel;
    timeline.openPolicy = EditorOpenPolicy::Singleton;
    timeline.defaultDockSlot = EditorDockSlot::Bottom;
    timeline.createDocument = [](const EditorOpenRequest&, std::string&) {
        return std::make_shared<ToolDocument>(
            "ayeditor.tool.timeline.document", "Timeline");
    };
    timeline.createView = [](const std::shared_ptr<IEditorDocument>&,
                             IEditorHostServices& host) {
        return std::unique_ptr<IEditorView>(
            std::make_unique<TimelineToolView>(host));
    };
    return add(registry, std::move(timeline), error);
}

} // namespace ayt::editor
