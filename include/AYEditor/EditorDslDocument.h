#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ayt::editor {

enum class EditorDslLanguage {
    Unknown = 0,
    Phoskia,
    Logia,
};

enum class EditorDslDiagnosticSeverity {
    Info = 0,
    Warning,
    Error,
};

struct EditorDslDiagnostic {
    EditorDslDiagnosticSeverity severity = EditorDslDiagnosticSeverity::Error;
    int line = 0;
    int column = 0;
    std::string message;
    std::string hint;
};

struct EditorDslCompileReport {
    bool success = false;
    EditorDslLanguage language = EditorDslLanguage::Unknown;
    std::string summary;
    std::vector<EditorDslDiagnostic> diagnostics;
    // Logia reports the generated Lua byte count. Phoskia's existing
    // frontend/backend API does not expose its generated .sc text, so this is
    // zero for Phoskia even after a successful compile.
    std::size_t generatedBytes = 0;
};

EditorDslLanguage editorDslLanguageFromPath(
    const std::string& path) noexcept;
const char* editorDslLanguageName(EditorDslLanguage language) noexcept;

// One source-backed editor document. The document owns normalized UTF-8 text,
// tracks the last saved snapshot, preserves an existing UTF-8 BOM/CRLF policy,
// and invokes the production Phoskia or Logia compiler in memory.
//
// UI widgets are intentionally absent: AYEditorSession may present this model
// in a DockCard without coupling file/compiler tests to AYUI input dispatch.
class EditorDslDocument final {
public:
    bool open(const std::string& absolutePath,
              const std::string& displayPath,
              std::string* error = nullptr);
    bool save(std::string* error = nullptr);

    void setSourceUtf8(std::string source);
    const std::string& sourceUtf8() const noexcept { return _source; }
    const std::string& absolutePath() const noexcept { return _absolutePath; }
    const std::string& displayPath() const noexcept { return _displayPath; }
    EditorDslLanguage language() const noexcept { return _language; }
    bool isDirty() const noexcept { return _source != _savedSource; }

    EditorDslCompileReport compile() const;

private:
    enum class LineEnding {
        Lf,
        CrLf,
    };

    std::string serializeForDisk() const;

    std::string _absolutePath;
    std::string _displayPath;
    std::string _source;
    std::string _savedSource;
    EditorDslLanguage _language = EditorDslLanguage::Unknown;
    LineEnding _lineEnding = LineEnding::Lf;
    bool _utf8Bom = false;
};

} // namespace ayt::editor
