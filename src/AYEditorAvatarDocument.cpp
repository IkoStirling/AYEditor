#include <AYAssetFormat/AssetFormat.h>
#include <AYEditor/EditorAvatarDocument.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <json.hpp>
#include <set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif
namespace ayt::editor {
namespace {
using Json = nlohmann::json;
std::filesystem::path native(const std::string &text) {
  return std::filesystem::u8path(text);
}
std::string utf8(const std::filesystem::path &p) {
  auto s = p.generic_u8string();
  return {s.begin(), s.end()};
}
std::filesystem::path bindingPath(const std::filesystem::path &p) {
  auto result = p;
  result += ".editor.json";
  return result;
}
bool failure(std::string *error, const std::string &message) {
  if (error)
    *error = message;
  return false;
}
std::filesystem::path resolveBundle(const std::filesystem::path &preset) {
  const auto metadata = bindingPath(preset);
  if (!std::filesystem::exists(metadata)) {
    auto bundle = preset;
    bundle.replace_extension(".avb");
    if (!std::filesystem::exists(bundle))
      throw std::runtime_error("Preset has no bundle association. Open its "
                               ".avb and save the preset first.");
    return bundle;
  }
  if (std::filesystem::file_size(metadata) > 65536)
    throw std::runtime_error("Avatar editor binding exceeds 64 KiB");
  std::ifstream input(metadata, std::ios::binary);
  if (!input)
    throw std::runtime_error("Cannot read Avatar editor binding");
  std::string bytes((std::istreambuf_iterator<char>(input)), {});
  std::set<std::string> keys;
  auto json = Json::parse(
      bytes, [&](int depth, Json::parse_event_t event, Json &value) {
        if (depth > 8)
          throw std::runtime_error("Avatar editor binding nesting limit");
        if (event == Json::parse_event_t::key &&
            !keys.insert(value.get<std::string>()).second)
          throw std::runtime_error("Duplicate Avatar editor binding field");
        return true;
      });
  if (!json.is_object() || json.size() != 3 ||
      json.value("format", std::string{}) != "AYEditorAvatarBinding" ||
      !json.contains("version") || !json["version"].is_number_integer() ||
      json["version"] != 1 || !json.contains("bundle") ||
      !json["bundle"].is_string())
    throw std::runtime_error("Invalid Avatar editor binding");
  auto text = json["bundle"].get<std::string>();
  if (text.empty() || text.find('\0') != std::string::npos)
    throw std::runtime_error("Invalid bundle path in Avatar editor binding");
  auto bundle = native(text);
  return bundle.is_absolute() ? bundle : preset.parent_path() / bundle;
}
void writeBinding(const std::filesystem::path &preset,
                  const std::string &bundleText) {
  const auto bundle = std::filesystem::absolute(native(bundleText));
  auto relative = bundle.lexically_relative(
      std::filesystem::absolute(preset).parent_path());
  const auto content =
      Json{{"format", "AYEditorAvatarBinding"},
           {"version", 1},
           {"bundle", utf8(relative.empty() ? bundle : relative)}}
          .dump(2);
  auto destination = bindingPath(preset), temporary = destination;
  temporary += ".tmp-" +
               std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count());
  try {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << content << '\n';
    output.close();
    if (!output)
      throw std::runtime_error("Cannot write Avatar editor binding");
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      throw std::runtime_error("Cannot publish Avatar editor binding");
#else
    std::filesystem::rename(temporary, destination);
#endif
  } catch (...) {
    std::error_code ec;
    std::filesystem::remove(temporary, ec);
    throw;
  }
}
} // namespace
bool EditorAvatarDocument::initialize(const EditorOpenRequest &request,
                                      std::string &error) {
  try {
    if (request.resourcePath.empty()) {
      error.clear();
      return true;
    }
    const auto source = std::filesystem::absolute(native(request.resourcePath))
                            .lexically_normal();
    bool isBundle = ayt::asset_format::matchesPath(
        utf8(source), ayt::asset_format::Id::AvatarBundle);
    if (!isBundle && !ayt::asset_format::matchesPath(
                         utf8(source), ayt::asset_format::Id::AvatarPreset))
      return failure(&error, "Choose an .avb bundle or .avp preset.");
    auto bundle = isBundle ? source : resolveBundle(source);
    auto presetFile = source;
    if (isBundle)
      presetFile.replace_extension(".avp");
    auto catalog = std::make_unique<avatar::Catalog>();
    avatar::Preset preset;
    auto status = avatar::loadBundle(bundle, *catalog, preset);
    if (!status)
      return failure(&error, status.message);
    auto creator = std::make_unique<avatar::Creator>(*catalog, preset);
    if (!isBundle || std::filesystem::exists(presetFile)) {
      status = creator->load(presetFile);
      if (!status)
        return failure(&error, status.message);
      // Loading the initial preset is a baseline, never an undoable edit.
      creator = std::make_unique<avatar::Creator>(*catalog, creator->preset());
    }
    auto observed = avatar::encodePreset(creator->preset());
    _creator.reset();
    _catalog = std::move(catalog);
    _creator = std::move(creator);
    _bundlePath = utf8(bundle);
    // The authoring document's writable identity is always the preset. The
    // recovery host restores into document.path(); never expose the read-only
    // bundle there, even when the open request came from an .avb resource.
    _path = _presetPath = utf8(presetFile);
    _title = utf8(presetFile.filename());
    _saved = _observed = std::move(observed);
    _diagnostic.clear();
    ++_revision;
    ++_appearanceRevision;
    error.clear();
    return true;
  } catch (const std::exception &e) {
    return failure(&error, e.what());
  }
}
bool EditorAvatarDocument::isDirty() const noexcept {
  try {
    return _creator && avatar::encodePreset(_creator->preset()) != _saved;
  } catch (...) {
    return true;
  }
}
void EditorAvatarDocument::contentChanged() {
  if (!_creator)
    return;
  auto next = avatar::encodePreset(_creator->preset());
  if (next != _observed) {
    _observed = std::move(next);
    ++_revision;
    ++_appearanceRevision;
  }
  _diagnostic.clear();
}
bool EditorAvatarDocument::setParameter(const std::string &id, float value) {
  if (!_creator)
    return false;
  auto status = _creator->setParameter(id, value);
  if (!status) {
    _diagnostic = status.message;
    return false;
  }
  contentChanged();
  return true;
}
bool EditorAvatarDocument::writePresetAndBinding(const std::string &text,
                                                 std::string *error) const {
  if (!_creator)
    return failure(error, "Open an Avatar bundle first.");
  try {
    // Each file publishes atomically. This is not a two-file transaction;
    // a failed save remains dirty and reports its error for retry.
    writeBinding(native(text), _bundlePath);
    auto status = _creator->save(native(text));
    if (!status)
      return failure(error, status.message);
    if (error)
      error->clear();
    return true;
  } catch (const std::exception &e) {
    return failure(error, e.what());
  }
}
bool EditorAvatarDocument::save(std::string *error) {
  if (!writePresetAndBinding(_presetPath, error))
    return false;
  _saved = avatar::encodePreset(_creator->preset());
  _diagnostic = "Saved parameters: " + _presetPath;
  ++_revision;
  return true;
}
bool EditorAvatarDocument::saveAs(const std::string &text, std::string *error) {
  if (!ayt::asset_format::matchesPath(text,
                                      ayt::asset_format::Id::AvatarPreset))
    return failure(error, "Choose an .avp preset path.");
  if (!writePresetAndBinding(text, error))
    return false;
  _path = _presetPath =
      utf8(std::filesystem::absolute(native(text)).lexically_normal());
  _title = utf8(native(_path).filename());
  _saved = avatar::encodePreset(_creator->preset());
  _diagnostic = "Saved parameters: " + _presetPath;
  ++_revision;
  return true;
}
bool EditorAvatarDocument::reload(std::string *error) {
  std::string message;
  // Preserve the live Creator identity: its UI panel borrows this session.
  if (!_creator)
    return failure(error, "Open an Avatar bundle first.");
  auto status = _creator->load(native(_presetPath));
  if (!status)
    return failure(error, status.message);
  _saved = avatar::encodePreset(_creator->preset());
  contentChanged();
  _diagnostic = "Loaded parameters: " + _presetPath;
  if (error)
    error->clear();
  return true;
}
bool EditorAvatarDocument::writeRecoveryCopy(const std::string &text,
                                             std::string *error) const {
  return writePresetAndBinding(text, error);
}
bool EditorAvatarDocument::handlesCommand(const std::string &id) const {
  return id == "edit.undo" || id == "edit.redo" || id == "file.save" ||
         id == "file.save_as" || id == "file.reload";
}
bool EditorAvatarDocument::canExecuteCommand(const std::string &id) const {
  if (!_creator || _creator->isEditing())
    return false;
  if (id == "edit.undo")
    return _creator->canUndo();
  if (id == "edit.redo")
    return _creator->canRedo();
  return handlesCommand(id) &&
         (id != "file.reload" || std::filesystem::exists(native(_presetPath)));
}
bool EditorAvatarDocument::executeCommand(const std::string &id) {
  if (!canExecuteCommand(id))
    return false;
  if (id == "edit.undo" || id == "edit.redo") {
    bool changed = id == "edit.undo" ? _creator->undo() : _creator->redo();
    if (changed)
      contentChanged();
    return changed;
  }
  std::string error;
  bool result = false;
  if (id == "file.save")
    result = save(&error);
  else if (id == "file.reload")
    result = reload(&error);
  else if (id == "file.save_as") {
    auto chosen = _savePath ? _savePath(true) : std::string{};
    if (chosen.empty())
      return false;
    result = saveAs(chosen, &error);
  }
  if (!result)
    _diagnostic = error;
  return result;
}
} // namespace ayt::editor
