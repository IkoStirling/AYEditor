#pragma once

#include "AYEditor/EditorVersion.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ayt::editor {

inline constexpr std::string_view kEditorProjectDescriptorFile =
    "project.ayproject.json";
inline constexpr std::uint32_t kEditorProjectDescriptorSchemaVersion = 2u;

struct EditorProjectRunDescriptor {
    std::string executable;
    std::string workingDirectory = ".";
    std::vector<std::string> arguments;
};

struct EditorProjectUiDescriptor {
    // Portable path below the project asset root. Empty keeps legacy
    // per-World `ui` compatibility active.
    std::string flow;
    // Optional named entry point. Empty selects the Flow document default.
    std::string entry;
};

struct EditorProjectWorldDescriptor {
    std::string id;
    std::string scene;
    // Legacy single-layout shorthand. New projects select a Context from the
    // project-level UI Flow instead.
    std::string ui;
    std::string uiContext;
    std::vector<std::string> tilemaps;
};

// Tool-facing project contract. Runtime callbacks and module factories remain
// in GameProject C++; this file gives all editor hosts one stable place to find
// content roots, authoring entry points, World resources, and the runnable app.
struct EditorProjectDescriptor {
    std::uint32_t schemaVersion = 0u;
    std::string id;
    std::string displayName;
    std::string engineProfile;
    std::string assetRoot = "Assets";
    std::string gameAssembly;
    std::string gameCodeRoot;
    // Optional authoring default: "2D", "3D", or "Auto". A per-scene
    // .ayeditor/workspace.json entry still has higher precedence.
    std::string defaultSceneView = "Auto";
    EditorProjectUiDescriptor ui;
    std::string startupWorld;
    std::vector<EditorProjectWorldDescriptor> worlds;
    EditorProjectRunDescriptor run;
    std::string sourcePath;
    // Appended to preserve offsets of the established tool-facing fields.
    // This is a portable asset-root-relative GameFlow reference.
    std::string startupFlow;
    // Optional typed action/guard contract used by editor and CI validation
    // when project-owned C++ handlers are not loaded.
    std::string gameFlowContract;

    explicit operator bool() const noexcept;
    bool validate(std::string* error = nullptr) const;
    bool serialize(std::string& jsonText,
                   std::string* error = nullptr) const;
    bool save(const std::string& projectRoot,
              std::string* error = nullptr) const;
    const EditorProjectWorldDescriptor* findWorld(
        std::string_view worldId) const noexcept;

    static EditorProjectDescriptor load(
        const std::string& projectRoot,
        std::string* error = nullptr);
};

// Accept a project directory or its project.ayproject.json file, validate the
// descriptor, and return the normalized absolute project root. This is the
// shared boundary for command-line, Open Project and Recent Projects hosts.
std::string resolveEditorProjectRoot(
    const std::string& selectedPath,
    std::string* error = nullptr);

// Resolves the project startup World into one absolute Scene path without
// mutating editor state. `projectDescriptorPresent` deliberately remains true
// for malformed descriptors so product sessions never fall back to demo-only
// reference content after a project has been selected.
struct EditorProjectStartupSceneResolution {
    bool projectDescriptorPresent = false;
    std::string assetRootPath;
    std::string scenePath;
    std::string error;

    explicit operator bool() const noexcept {
        return projectDescriptorPresent && error.empty()
            && !scenePath.empty();
    }
};

EditorProjectStartupSceneResolution resolveEditorProjectStartupScene(
    const std::string& projectRoot);

} // namespace ayt::editor
