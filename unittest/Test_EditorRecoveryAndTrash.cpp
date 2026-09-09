#include "AYTest.h"

#include "AYEditor/EditorAssetOperations.h"
#include "AYEditor/EditorAssetTrash.h"
#include "AYEditor/EditorExtension.h"
#include "AYEditor/EditorRecoveryStore.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

namespace {

struct RecoveryTrashCleanup {
    std::filesystem::path root;
    ~RecoveryTrashCleanup() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

std::filesystem::path recoveryTrashRoot(const char* suffix)
{
    return std::filesystem::temp_directory_path()
        / (std::string("ayeditor_recovery_trash_") + suffix + "_"
           + std::to_string(std::chrono::steady_clock::now()
               .time_since_epoch().count()));
}

void writeRecoveryTrashFile(const std::filesystem::path& path,
                            const std::string& text)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

std::string readRecoveryTrashFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

class SelectiveRecoveryDocument final : public ayt::editor::IEditorDocument {
public:
    SelectiveRecoveryDocument(std::string path, std::string title,
                              std::string recoveryText)
        : _path(std::move(path)), _title(std::move(title)),
          _recoveryText(std::move(recoveryText)) {}

    const std::string& typeId() const noexcept override { return _type; }
    const std::string& path() const noexcept override { return _path; }
    const std::string& title() const noexcept override { return _title; }
    bool isDirty() const noexcept override { return true; }
    std::uint64_t revision() const noexcept override { return 1; }
    bool save(std::string* error) override {
        if (error != nullptr) error->clear();
        return true;
    }
    bool writeRecoveryCopy(const std::string& path,
                           std::string* error) const override {
        if (error != nullptr) error->clear();
        writeRecoveryTrashFile(path, _recoveryText);
        return std::filesystem::is_regular_file(path);
    }

private:
    std::string _type = "test.recovery";
    std::string _path;
    std::string _title;
    std::string _recoveryText;
};

} // namespace

TEST_SUITE(AYEditor_RecoveryAndTrash)

TEST_CASE(editor_recovery_restores_only_selected_documents)
{
    using namespace ayt::editor;
    RecoveryTrashCleanup cleanup{recoveryTrashRoot("selective")};
    const auto first = cleanup.root / "Assets/first.logia";
    const auto second = cleanup.root / "Assets/second.logia";
    writeRecoveryTrashFile(first, "first-old");
    writeRecoveryTrashFile(second, "second-old");
    SelectiveRecoveryDocument firstDocument(
        first.string(), "First", "first-recovered");
    SelectiveRecoveryDocument secondDocument(
        second.string(), "Second", "second-recovered");
    {
        EditorRecoveryStore crashed(cleanup.root.string());
        std::string error;
        CHECK(crashed.beginSession(&error));
        CHECK(crashed.autosave({&firstDocument, &secondDocument}).documents == 2u);
    }

    EditorRecoveryStore restarted(cleanup.root.string());
    std::string error;
    CHECK(restarted.beginSession(&error));
    CHECK(restarted.recoverableDocuments().size() == 2u);
    const EditorRecoveryResult selected = restarted.restorePrevious({1u});
    CHECK(selected);
    CHECK(selected.documents == 1u);
    CHECK(readRecoveryTrashFile(first) == "first-old");
    CHECK(readRecoveryTrashFile(second) == "second-recovered");
    CHECK(restarted.hasRecoverableSession());
    CHECK(restarted.recoverableDocuments().size() == 1u);
    CHECK(restarted.restorePrevious().documents == 1u);
    CHECK(readRecoveryTrashFile(first) == "first-recovered");
    CHECK_FALSE(restarted.hasRecoverableSession());
    restarted.markCleanShutdown();
}

TEST_CASE(editor_trash_lists_restores_and_batch_purges_transactions)
{
    using namespace ayt::editor;
    RecoveryTrashCleanup cleanup{recoveryTrashRoot("transactions")};
    const auto first = cleanup.root / "Assets/first.aymesh";
    const auto second = cleanup.root / "Assets/second.aymesh";
    writeRecoveryTrashFile(first, "first");
    writeRecoveryTrashFile(second, "second");
    EditorAssetRecord firstRecord;
    firstRecord.logicalPath = "Assets/first.aymesh";
    firstRecord.absolutePath = first.string();
    EditorAssetRecord secondRecord;
    secondRecord.logicalPath = "Assets/second.aymesh";
    secondRecord.absolutePath = second.string();

    EditorAssetTrash trash(cleanup.root.string());
    CHECK(trash.moveToTrash({firstRecord}).moved == 1u);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(trash.moveToTrash({secondRecord}).moved == 1u);
    CHECK(trash.transactions().size() == 2u);
    const std::string newest = trash.transactions().front().id;
    CHECK(trash.restore(newest).moved == 1u);
    CHECK(std::filesystem::is_regular_file(second));
    CHECK(trash.transactions().size() == 1u);
    CHECK(trash.purgeAll().moved == 1u);
    CHECK_FALSE(std::filesystem::exists(first));
    CHECK(trash.transactions().empty());

    const auto history = readEditorAssetOperationHistory(cleanup.root.string());
    CHECK(history.size() == 4u);
    CHECK(history[0].operation == "Purge");
    CHECK(history[1].operation == "Restore");
}

TEST_SUITE_END
