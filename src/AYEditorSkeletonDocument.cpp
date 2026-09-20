#include "AYEditor/EditorSkeletonDocument.h"

#include <AYIO/File.h>
#include <AYResource/assetsDefs/IAnimation.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace ayt::editor {
namespace {

using Json = nlohmann::json;

std::string normalizedAbsolute(const std::string& path)
{
    if (path.empty()) return {};
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    return (error ? std::filesystem::path(path) : absolute)
        .lexically_normal().generic_string();
}

std::string lowerExtension(const std::filesystem::path& path)
{
    std::string result = path.extension().string();
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return result;
}

bool isProjectRelative(const std::filesystem::path& path) noexcept
{
    if (path.empty() || path.is_absolute()) return false;
    const auto first = path.begin();
    return first == path.end() || *first != std::filesystem::path("..");
}

std::string encodeProjectPath(const std::string& value,
                              const std::string& projectRoot)
{
    if (value.empty() || projectRoot.empty()) return value;
    std::error_code error;
    const auto relative = std::filesystem::relative(value, projectRoot, error);
    if (!error && isProjectRelative(relative)) {
        return relative.lexically_normal().generic_string();
    }
    return std::filesystem::path(value).lexically_normal().generic_string();
}

std::string resolveProjectPath(const std::string& value,
                               const std::string& projectRoot)
{
    if (value.empty()) return {};
    const std::filesystem::path path(value);
    return normalizedAbsolute((path.is_absolute() || projectRoot.empty())
        ? path.string() : (std::filesystem::path(projectRoot) / path).string());
}

bool sameFile(const std::string& left, const std::string& right)
{
    if (left.empty() || right.empty()) return false;
    std::error_code error;
    const bool equivalent = std::filesystem::equivalent(left, right, error);
    if (!error) return equivalent;
#if defined(_WIN32)
    std::string a = normalizedAbsolute(left);
    std::string b = normalizedAbsolute(right);
    std::transform(a.begin(), a.end(), a.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(b.begin(), b.end(), b.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return a == b;
#else
    return normalizedAbsolute(left) == normalizedAbsolute(right);
#endif
}

} // namespace

bool EditorSkeletonDocument::initialize(const EditorOpenRequest& request,
                                        std::string& error)
{
    if (!_core.open(request.resourcePath, &error)) return false;
    _openedResourcePath = normalizedAbsolute(request.resourcePath);
    const std::string extension = lowerExtension(request.resourcePath);
    _openedProfileExplicitly = extension == ".ayrig" || extension == ".aysmap";
    _path = _core.skeletonPath();
    const std::string display = request.displayPath.empty()
        ? _path : request.displayPath;
    _title = std::filesystem::path(display).filename().string();
    return true;
}

void EditorSkeletonDocument::configureProjectRoot(
    const std::string& projectRoot)
{
    _projectRoot = normalizedAbsolute(projectRoot);
    _bindingMetadataPath = _projectRoot.empty() ? std::string{}
        : (std::filesystem::path(_projectRoot) / ".ayeditor"
            / "skeleton-profile-bindings.json").generic_string();

    if (!_openedProfileExplicitly) {
        const std::string bound = resolveBoundProfilePath(_projectRoot, _path);
        if (!bound.empty()) {
            std::string ignored;
            (void)_core.open(bound, &ignored);
        }
    } else if (lowerExtension(_openedResourcePath) == ".ayrig") {
        (void)persistProfileBinding(nullptr);
    }
    discoverProfiles();
}

bool EditorSkeletonDocument::save(std::string* error)
{
    if (!_core.saveMapping(error)) return false;
    discoverProfiles();
    return persistProfileBinding(error);
}

bool EditorSkeletonDocument::reload(std::string* error)
{
    return _core.reload(error);
}

bool EditorSkeletonDocument::switchMappingProfile(
    const std::string& path, std::string* error)
{
    if (_core.isDirty()) {
        if (error != nullptr) {
            *error = "Save or undo mapping changes before switching profiles.";
        }
        return false;
    }
    ayt::anim::editor::RigProfileInfo profile;
    if (!ayt::anim::editor::SkeletonEditorCore::inspectRigProfile(
            path, profile, error)
        || (profile.kind != ayt::anim::editor::RigProfileKind::Mapping
            && profile.kind != ayt::anim::editor::RigProfileKind::Retarget)
        || !sameFile(profile.sourceSkeletonPath, _path)) {
        if (error != nullptr && error->empty()) {
            *error = "Selected RigProfile is not a mapping or retarget profile for this skeleton.";
        }
        return false;
    }
    const std::string animationPath = _core.animationPath();
    const float animationTime = _core.time();
    if (!_core.open(path, error)) return false;
    if (!animationPath.empty()) {
        std::string ignored;
        if (_core.attachAnimation(animationPath, &ignored)) {
            (void)_core.setTime(animationTime);
        }
    }
    return persistProfileBinding(error);
}

bool EditorSkeletonDocument::configureRetarget(
    const std::string& targetSkeletonPath, const std::string& platform,
    std::string* error)
{
    return _core.configureRetarget(targetSkeletonPath, platform, error);
}

bool EditorSkeletonDocument::clearRetarget()
{
    return _core.clearRetarget();
}

bool EditorSkeletonDocument::applyTemplate(
    const std::string& path,
    ayt::anim::editor::SkeletonTemplateApplyReport* report,
    std::string* error)
{
    return _core.applyRigTemplate(path, report, error);
}

bool EditorSkeletonDocument::previewTemplate(
    const std::string& path,
    ayt::anim::editor::SkeletonTemplateApplyReport* report,
    std::string* error)
{
    return _core.previewRigTemplate(path, report, error);
}

void EditorSkeletonDocument::discoverProfiles()
{
    _mappingProfiles.clear();
    _templates.clear();
    if (_projectRoot.empty()) return;

    std::error_code error;
    std::filesystem::recursive_directory_iterator it(
        _projectRoot,
        std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    for (; !error && it != end; it.increment(error)) {
        if (!it->is_regular_file(error)
            || lowerExtension(it->path()) != ".ayrig") {
            continue;
        }
        ayt::anim::editor::RigProfileInfo info;
        if (!ayt::anim::editor::SkeletonEditorCore::inspectRigProfile(
                it->path().string(), info, nullptr)) {
            continue;
        }
        if (info.kind == ayt::anim::editor::RigProfileKind::Template) {
            _templates.push_back(std::move(info));
        } else if ((info.kind == ayt::anim::editor::RigProfileKind::Mapping
                || info.kind == ayt::anim::editor::RigProfileKind::Retarget)
            && sameFile(info.sourceSkeletonPath, _path)) {
            _mappingProfiles.push_back(std::move(info));
        }
    }
    const auto byName = [](const auto& left, const auto& right) {
        return left.name == right.name ? left.path < right.path
            : left.name < right.name;
    };
    std::sort(_mappingProfiles.begin(), _mappingProfiles.end(), byName);
    std::sort(_templates.begin(), _templates.end(), byName);
}

bool EditorSkeletonDocument::persistProfileBinding(std::string* error) const
{
    if (_bindingMetadataPath.empty() || _core.mappingPath().empty()) {
        if (error != nullptr) error->clear();
        return true;
    }
    std::error_code existsError;
    if (!std::filesystem::exists(_core.mappingPath(), existsError)) {
        if (error != nullptr) error->clear();
        return true;
    }
    try {
        Json root = Json::object();
        const std::string existing = ayt::io::File::readAllText(
            _bindingMetadataPath);
        if (!existing.empty()) root = Json::parse(existing);
        root["type"] = "SkeletonProfileBindings";
        root["version"] = 1;
        ayt::anim::editor::RigProfileInfo profile;
        (void)ayt::anim::editor::SkeletonEditorCore::inspectRigProfile(
            _core.mappingPath(), profile, nullptr);
        root["bindings"][encodeProjectPath(_path, _projectRoot)] = {
            {"profile", encodeProjectPath(
                _core.mappingPath(), _projectRoot)},
            {"profileId", profile.id},
        };
        const std::string encoded = root.dump(2) + "\n";
        const std::filesystem::path metadata(_bindingMetadataPath);
        std::error_code directoryError;
        std::filesystem::create_directories(
            metadata.parent_path(), directoryError);
        if (directoryError || !ayt::io::File::atomicWrite(
                _bindingMetadataPath, encoded.data(), encoded.size())) {
            if (error != nullptr) {
                *error = "Unable to write skeleton profile binding metadata.";
            }
            return false;
        }
        if (error != nullptr) error->clear();
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) *error = exception.what();
        return false;
    }
}

std::string EditorSkeletonDocument::resolveBoundProfilePath(
    const std::string& projectRoot, const std::string& skeletonPath)
{
    const std::string rootPath = normalizedAbsolute(projectRoot);
    if (rootPath.empty() || skeletonPath.empty()) return {};
    const std::string metadataPath = (std::filesystem::path(rootPath)
        / ".ayeditor" / "skeleton-profile-bindings.json").generic_string();
    const std::string text = ayt::io::File::readAllText(metadataPath);
    if (text.empty()) return {};
    try {
        const Json root = Json::parse(text);
        const auto bindings = root.find("bindings");
        if (bindings == root.end() || !bindings->is_object()) return {};
        const std::string absoluteSkeleton = normalizedAbsolute(skeletonPath);
        const std::string key = encodeProjectPath(absoluteSkeleton, rootPath);
        auto value = bindings->find(key);
        if (value == bindings->end()) value = bindings->find(absoluteSkeleton);
        if (value == bindings->end()) return {};
        std::string storedPath;
        std::string storedId;
        if (value->is_string()) {
            storedPath = value->get<std::string>();
        } else if (value->is_object()) {
            storedPath = value->value("profile", std::string{});
            storedId = value->value("profileId", std::string{});
        } else {
            return {};
        }
        const std::string result = resolveProjectPath(storedPath, rootPath);
        std::error_code existsError;
        if (std::filesystem::exists(result, existsError)) {
            ayt::anim::editor::RigProfileInfo profile;
            if (storedId.empty()
                || (ayt::anim::editor::SkeletonEditorCore::inspectRigProfile(
                        result, profile, nullptr) && profile.id == storedId)) {
                return result;
            }
        }
        if (storedId.empty()) return {};
        existsError.clear();
        std::filesystem::recursive_directory_iterator it(
            rootPath, std::filesystem::directory_options::skip_permission_denied,
            existsError);
        const std::filesystem::recursive_directory_iterator end;
        for (; !existsError && it != end; it.increment(existsError)) {
            if (!it->is_regular_file(existsError)
                || lowerExtension(it->path()) != ".ayrig") {
                continue;
            }
            ayt::anim::editor::RigProfileInfo profile;
            if (ayt::anim::editor::SkeletonEditorCore::inspectRigProfile(
                    it->path().string(), profile, nullptr)
                && profile.id == storedId) {
                return profile.path;
            }
        }
        return {};
    } catch (...) {
        return {};
    }
}

bool EditorSkeletonDocument::handlesCommand(const std::string& commandId) const
{
    return commandId == "file.save" || commandId == "edit.undo"
        || commandId == "edit.redo";
}

bool EditorSkeletonDocument::canExecuteCommand(const std::string& commandId) const
{
    if (commandId == "file.save") return _core.isDirty();
    if (commandId == "edit.undo") return _core.canUndo();
    if (commandId == "edit.redo") return _core.canRedo();
    return false;
}

bool EditorSkeletonDocument::executeCommand(const std::string& commandId)
{
    if (!canExecuteCommand(commandId)) return false;
    if (commandId == "file.save") return save();
    if (commandId == "edit.undo") return _core.undo();
    if (commandId == "edit.redo") return _core.redo();
    return false;
}

double EditorSkeletonDocument::timelineDurationSeconds() const noexcept
{
    return _core.duration();
}

double EditorSkeletonDocument::timelinePositionSeconds() const noexcept
{
    return _core.time();
}

bool EditorSkeletonDocument::setTimelinePositionSeconds(double seconds)
{
    return _core.setTime(static_cast<float>(seconds));
}

std::vector<EditorTimelineTrack> EditorSkeletonDocument::timelineTracks() const
{
    std::vector<EditorTimelineTrack> result;
    const ayt::resource::IAnimation* animation = _core.animation();
    if (animation == nullptr) return result;
    result.reserve(animation->getTrackCount());
    for (std::uint32_t index = 0; index < animation->getTrackCount(); ++index) {
        const char* node = animation->getTrackNodeName(index);
        const char* property = animation->getTrackProperty(index);
        EditorTimelineTrack track;
        track.id = "track-" + std::to_string(index);
        track.name = (node != nullptr ? node : "<unnamed>");
        if (property != nullptr && *property != '\0') {
            track.name += ".";
            track.name += property;
        }
        track.kind = EditorTimelineTrackKind::Animation;
        track.startSeconds = 0.0;
        track.endSeconds = animation->getDuration();
        result.push_back(std::move(track));
    }
    return result;
}

std::vector<EditorTimelineKeyframe>
EditorSkeletonDocument::timelineKeyframes() const
{
    std::vector<EditorTimelineKeyframe> result;
    const ayt::resource::IAnimation* animation = _core.animation();
    if (animation == nullptr) return result;
    const float ticksPerSecond = std::max(animation->getTicksPerSecond(), 1.0e-5f);
    for (std::uint32_t track = 0; track < animation->getTrackCount(); ++track) {
        const std::uint32_t count = animation->getTrackKeyframeCount(track);
        const float* times = animation->getTrackTimes(track);
        for (std::uint32_t key = 0; key < count && times != nullptr; ++key) {
            result.push_back({"track-" + std::to_string(track) + "-key-"
                + std::to_string(key), "track-" + std::to_string(track),
                static_cast<double>(times[key] / ticksPerSecond), 0.0});
        }
    }
    return result;
}

bool EditorSkeletonDocument::timelinePlaying() const noexcept
{
    return _core.isPlaying();
}

void EditorSkeletonDocument::timelinePlay() { _core.play(); }
void EditorSkeletonDocument::timelinePause() { _core.pause(); }
void EditorSkeletonDocument::timelineStop() { _core.stop(); }
void EditorSkeletonDocument::timelineTick(double seconds) {
    _core.tick(static_cast<float>(seconds));
}
bool EditorSkeletonDocument::timelineCanUndo() const noexcept { return _core.canUndo(); }
bool EditorSkeletonDocument::timelineCanRedo() const noexcept { return _core.canRedo(); }
bool EditorSkeletonDocument::timelineUndo() { return _core.undo(); }
bool EditorSkeletonDocument::timelineRedo() { return _core.redo(); }

} // namespace ayt::editor
