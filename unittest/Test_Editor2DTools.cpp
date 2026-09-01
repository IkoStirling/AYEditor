#include <AYEditor/Editor2DViewportModel.h>
#include <AYEditor/EditorTilemapDocument.h>
#include <AYTest.h>

#include <cstdio>
#include <string>

using namespace ayt::editor;

TEST_SUITE(Editor2DToolsTests)

TEST_CASE(ViewportRoundTripGridPanAndAnchoredZoom)
{
    Editor2DViewportModel viewport;
    viewport.setViewport(800u, 400u);
    viewport.setCamera({10.0f, 20.0f}, 200.0f);
    viewport.setGrid(32.0f, 16.0f, {-16.0f, -8.0f});

    const ayt::math::FVector2 screen{700.0f, 100.0f};
    const ayt::math::FVector2 world = viewport.screenToWorld(screen);
    const ayt::math::FVector2 roundTrip = viewport.worldToScreen(world);
    CHECK_FLOAT_EQ(roundTrip.x, screen.x, 1e-5f);
    CHECK_FLOAT_EQ(roundTrip.y, screen.y, 1e-5f);

    const EditorTileCell cell = viewport.worldToCell({17.0f, 25.0f});
    CHECK_INT_EQ(cell.x, 1);
    CHECK_INT_EQ(cell.y, 2);
    const ayt::math::FVector2 snapped = viewport.snapToGrid({17.0f, 25.0f});
    CHECK_FLOAT_EQ(snapped.x, 16.0f, 1e-6f);
    CHECK_FLOAT_EQ(snapped.y, 24.0f, 1e-6f);

    const ayt::math::FVector2 anchorBefore = viewport.screenToWorld(screen);
    viewport.zoomAt(2.0f, screen);
    const ayt::math::FVector2 anchorAfter = viewport.screenToWorld(screen);
    CHECK_FLOAT_EQ(anchorAfter.x, anchorBefore.x, 1e-5f);
    CHECK_FLOAT_EQ(anchorAfter.y, anchorBefore.y, 1e-5f);
    CHECK_FLOAT_EQ(viewport.verticalWorldSize(), 100.0f, 1e-6f);
}

TEST_CASE(TilemapDocumentPaintFillMetadataAndRoundTrip)
{
    const char* path = "editor_2d_tools.aytilemap.json";
    EditorTilemapDocument document;
    CHECK_TRUE(document.create(3u, 2u, 32u, 16u, 0u));
    CHECK_TRUE(document.paint(1u, 0u, 9u));
    CHECK_TRUE(document.paint(1u, 1u, 9u));
    CHECK_INT_EQ(document.floodFill(0u, 0u, 5u), 2u);
    CHECK_INT_EQ(document.tileAt(0u, 1u), 5u);
    CHECK_INT_EQ(document.tileAt(2u, 1u), 0u);
    document.setCollisionFlags(9u, 0x11u);
    document.setAnimation(5u, {{10u, 100u}, {11u, 150u}});
    CHECK_TRUE(document.save(path));
    CHECK_FALSE(document.dirty());

    EditorTilemapDocument loaded;
    std::string error;
    CHECK_TRUE(loaded.load(path, &error));
    CHECK_TRUE(error.empty());
    CHECK_INT_EQ(loaded.cols(), 3u);
    CHECK_INT_EQ(loaded.rows(), 2u);
    CHECK_INT_EQ(loaded.tileAt(0u, 0u), 5u);
    CHECK_INT_EQ(loaded.tileAt(1u, 1u), 9u);
    CHECK_INT_EQ(loaded.collisionFlags().at(9u), 0x11u);
    CHECK_INT_EQ(static_cast<uint32_t>(loaded.animations().at(5u).size()), 2u);
    CHECK_INT_EQ(loaded.animations().at(5u)[1].tileId, 11u);
    CHECK_INT_EQ(loaded.animations().at(5u)[1].durationMs, 150u);
    CHECK_FALSE(loaded.dirty());
    std::remove(path);
}

TEST_CASE(TilemapDocumentRejectsInvalidDimensionsAndOutOfBoundsPaint)
{
    EditorTilemapDocument document;
    CHECK_FALSE(document.create(0u, 2u, 16u, 16u));
    CHECK_TRUE(document.create(2u, 2u, 16u, 16u));
    CHECK_FALSE(document.paint(2u, 0u, 1u));
    CHECK_INT_EQ(document.floodFill(99u, 99u, 1u), 0u);
}

TEST_SUITE_END
