#include "AYTest.h"

#include "AYEditor/EditorAssetTilePresenter.h"

using namespace ayt::editor;

TEST_SUITE(AYEditor_AssetTilePresenter)

TEST_CASE(asset_tile_presenter_preserves_full_name_and_native_marker)
{
    EditorAssetRecord record;
    record.id = 42;
    record.name = "character.v2.aymesh";
    record.type = EditorAssetType::Mesh;

    const EditorAssetTilePresentation tile =
        EditorAssetTilePresenter{}.present(record);
    CHECK(tile.assetId == 42);
    CHECK(tile.fullFileName == L"character.v2.aymesh");
    CHECK(tile.typeAbbreviation == L"MESH");
    CHECK(tile.category == EditorAssetTileCategory::Geometry);
    CHECK(tile.showEngineResourceMarker);
    CHECK(!tile.folder);
}

TEST_CASE(asset_tile_presenter_groups_related_types_by_strip_color)
{
    EditorAssetRecord mesh;
    mesh.name = "hero.aymesh";
    mesh.type = EditorAssetType::Mesh;
    EditorAssetRecord source;
    source.name = "hero.fbx";
    source.type = EditorAssetType::SourceModel;

    const auto meshTile = EditorAssetTilePresenter{}.present(mesh);
    const auto sourceTile = EditorAssetTilePresenter{}.present(source);
    CHECK(meshTile.typeAbbreviation == L"MESH");
    CHECK(sourceTile.typeAbbreviation == L"MODEL");
    CHECK(meshTile.category == sourceTile.category);
    CHECK(meshTile.categoryColor.x == sourceTile.categoryColor.x);
    CHECK(meshTile.categoryColor.y == sourceTile.categoryColor.y);
    CHECK(meshTile.categoryColor.z == sourceTile.categoryColor.z);
    CHECK(meshTile.showEngineResourceMarker);
    CHECK(!sourceTile.showEngineResourceMarker);
}

TEST_CASE(asset_tile_presenter_marks_native_suffixes_case_insensitively)
{
    CHECK(EditorAssetTilePresenter::isEngineNativeFileName("Hero.AYMESH"));
    CHECK(EditorAssetTilePresenter::isEngineNativeFileName("walk.ayanm"));
    CHECK(EditorAssetTilePresenter::isEngineNativeFileName("hero.ayrig"));
    CHECK(EditorAssetTilePresenter::isEngineNativeFileName("legacy.aysmap"));
    CHECK(EditorAssetTilePresenter::isEngineNativeFileName("editor.ui.json"));
    CHECK(EditorAssetTilePresenter::isEngineNativeFileName("lit.phoskia"));
    CHECK(!EditorAssetTilePresenter::isEngineNativeFileName("albedo.png"));
    CHECK(!EditorAssetTilePresenter::isEngineNativeFileName("hero.fbx"));
}

TEST_CASE(asset_tile_presenter_describes_folders_without_native_marker)
{
    EditorAssetEntry entry;
    entry.folder = true;
    entry.folderPath = "Assets/character.aymesh";
    entry.displayName = "character.aymesh";

    const EditorAssetTilePresentation tile =
        EditorAssetTilePresenter{}.present(entry);
    CHECK(tile.folder);
    CHECK(tile.assetId == 0);
    CHECK(tile.fullFileName == L"character.aymesh");
    CHECK(tile.typeAbbreviation == L"DIR");
    CHECK(tile.category == EditorAssetTileCategory::Folder);
    CHECK(!tile.showEngineResourceMarker);
}

TEST_CASE(asset_tile_presenter_abbreviates_every_asset_type)
{
    CHECK(std::wstring(EditorAssetTilePresenter::typeAbbreviation(
              EditorAssetType::Material)) == L"MAT");
    CHECK(std::wstring(EditorAssetTilePresenter::typeAbbreviation(
              EditorAssetType::Texture)) == L"TEX");
    CHECK(std::wstring(EditorAssetTilePresenter::typeAbbreviation(
              EditorAssetType::Scene)) == L"SCN");
    CHECK(std::wstring(EditorAssetTilePresenter::typeAbbreviation(
              EditorAssetType::Animation)) == L"ANIM");
    CHECK(std::wstring(EditorAssetTilePresenter::typeAbbreviation(
              EditorAssetType::Skeleton)) == L"SKEL");
    CHECK(std::wstring(EditorAssetTilePresenter::typeAbbreviation(
              EditorAssetType::SkeletonMapping)) == L"RIG");
    CHECK(std::wstring(EditorAssetTilePresenter::typeAbbreviation(
              EditorAssetType::Script)) == L"SCR");
    CHECK(std::wstring(EditorAssetTilePresenter::typeAbbreviation(
              EditorAssetType::Shader)) == L"SHDR");
    CHECK(std::wstring(EditorAssetTilePresenter::typeAbbreviation(
              EditorAssetType::Audio)) == L"AUD");
    CHECK(std::wstring(EditorAssetTilePresenter::typeAbbreviation(
              EditorAssetType::UiLayout)) == L"UI");
    CHECK(std::wstring(EditorAssetTilePresenter::typeAbbreviation(
              EditorAssetType::Tilemap)) == L"MAP");
    CHECK(std::wstring(EditorAssetTilePresenter::typeAbbreviation(
              EditorAssetType::Unknown)) == L"FILE");
}

TEST_SUITE_END
