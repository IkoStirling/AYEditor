// Test_EditorImporter.cpp - Phase 1 ED-01 unit tests.
//
// Covers the AYEditor Importer + ImportDialog surface. Goals:
//   * Confirm unsupported extension → clean error result, no crash.
//   * Confirm missing file → "does not exist" error.
//   * Confirm ImportDialog::importFromPath is a one-line pass-through
//     to Importer::importFile (same return shape, same error text).
//
// We deliberately do NOT drive a real FBX through the converter
// pipeline in this iteration. FBXConverter loads the FBX SDK + parses
// + spawns shaderc — that takes seconds + depends on disk artifacts.
// The error-path tests prove the contract; the success path will be
// covered manually with `D:/Projects/suzanne.fbx` during the
// end-to-end verification step (see plan §Verification).

#include "AYTest.h"
#include "AYEditor/Importer.h"
#include "AYEditor/EditorAssetImportQueue.h"
#include "AYEditor/ImportDialog.h"
#include "AYIO/File.h"
#include <AYIO/Path.h>

#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace ayt::editor;

namespace {

// Tiny helper: write `bytes` to `path` (creating parents). We avoid
// AYFile APIs here because the AYIO migration is mid-flight and the
// simple ofstream path is the least surprising one for a unit test.
bool writeStubFile(const std::string& path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return f.good();
}

} // namespace

TEST_SUITE(AYEditor_Importer)

TEST_CASE(EditorAssetImportQueueReportsFailureAndSupportsRetry)
{
    EditorAssetImportQueue queue;
    const auto id = queue.enqueue(
        "D:/no/such/path/missing.fbx", "D:/tmp/ayeditor-import-queue");
    CHECK(id != 0u);
    for (int attempt = 0; attempt < 200 && queue.busy(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        (void)queue.poll();
    }
    const EditorAssetImportJob* failed = queue.find(id);
    CHECK(failed != nullptr);
    CHECK(failed != nullptr && failed->state == EditorAssetImportJobState::Failed);
    CHECK(failed != nullptr && !failed->message.empty());
    CHECK_FLOAT_EQ(queue.overallProgress(), 1.0f, 1e-5f);

    const auto retryId = queue.retry(id);
    CHECK(retryId != 0u);
    CHECK(retryId != id);
    CHECK(queue.busy());
    while (queue.busy()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        (void)queue.poll();
    }
    const EditorAssetImportJob* retried = queue.find(retryId);
    CHECK(retried != nullptr);
    CHECK(retried != nullptr && retried->force);
    CHECK(retried != nullptr && retried->state == EditorAssetImportJobState::Failed);
}

TEST_CASE(extension_of_lowercases_and_strips_dot)
{
    CHECK_TRUE(Importer::extensionOf("foo.fbx")      == "fbx");
    CHECK_TRUE(Importer::extensionOf("Foo.FBX")      == "fbx");
    CHECK_TRUE(Importer::extensionOf("a/b/c.Png")    == "png");
    CHECK_TRUE(Importer::extensionOf("noext")        == "");
    CHECK_TRUE(Importer::extensionOf("trailing.")    == "");
}

TEST_CASE(is_supported_extension_matches_factory_table)
{
    // Mirror AYResource::isImportSupportedExtension().
    CHECK_TRUE(Importer::isSupportedExtension("a.fbx"));
    CHECK_TRUE(Importer::isSupportedExtension("a.glb"));
    CHECK_TRUE(Importer::isSupportedExtension("a.gltf"));
    CHECK_TRUE(Importer::isSupportedExtension("a.png"));
    CHECK_TRUE(Importer::isSupportedExtension("a.bmp"));
    CHECK_TRUE(Importer::isSupportedExtension("a.tga"));
    CHECK_TRUE(Importer::isSupportedExtension("a.dds"));
    CHECK_TRUE(Importer::isSupportedExtension("a.jpg"));
    CHECK_TRUE(Importer::isSupportedExtension("a.jpeg"));
    CHECK_TRUE(Importer::isSupportedExtension("a.wav"));
    CHECK_FALSE(Importer::isSupportedExtension("a.mp4"));
    CHECK_FALSE(Importer::isSupportedExtension("a"));
}

TEST_CASE(import_empty_sourcepath_returns_error)
{
    Importer::Result r = Importer::importFile("", "C:/tmp/cache");
    CHECK_FALSE(r.success);
    CHECK_TRUE(!r.errorMessage.empty());
    CHECK_TRUE(r.errorMessage.find("empty") != std::string::npos);
}

TEST_CASE(import_missing_file_returns_error)
{
    Importer::Result r =
        Importer::importFile("D:/no/such/path/missing.fbx", "D:/tmp/cache");
    CHECK_FALSE(r.success);
    CHECK_TRUE(r.errorMessage.find("does not exist") != std::string::npos);
}

TEST_CASE(import_animation_missing_file_returns_error)
{
    Importer::Result r = Importer::importAnimationFile(
        "D:/no/such/path/missing_animation.fbx", "D:/tmp/cache");
    CHECK_FALSE(r.success);
    CHECK_TRUE(r.errorMessage.find("does not exist") != std::string::npos);
}

TEST_CASE(import_unsupported_extension_returns_error_with_supported_list)
{
    // Create a real on-disk file with an unsupported extension; the
    // extension check runs *after* the file-exists check.
    const std::string path = (std::filesystem::temp_directory_path()
        / "ayeditor_test_unsupported.mp4").string();
    CHECK_TRUE(writeStubFile(path, "fake video bytes"));

    Importer::Result r = Importer::importFile(path, "D:/tmp/cache");
    CHECK_FALSE(r.success);
    CHECK_TRUE(r.errorMessage.find("unsupported") != std::string::npos);
    CHECK_TRUE(r.errorMessage.find("Supported:")   != std::string::npos);

    ayt::io::File::remove(path);
}

TEST_CASE(import_dialog_is_thin_passthrough_to_importer)
{
    // Calling through `ImportDialog::importFromPath` with an empty
    // source must produce the same error shape as a direct call to
    // `Importer::importFile`. The dialog layer is one line; the test
    // pins that contract so a future menu refactor can't silently
    // change the return semantics.
    Importer::Result direct =
        Importer::importFile("", "D:/tmp/cache");
    Importer::Result viaDialog =
        ImportDialog::importFromPath("", "D:/tmp/cache");
    CHECK_FALSE(direct.success);
    CHECK_FALSE(viaDialog.success);
    CHECK_TRUE(direct.errorMessage == viaDialog.errorMessage);
}

TEST_SUITE_END
