cmake_minimum_required(VERSION 3.12)

set(MODULE_NAME AYEditor_UnitTests)

add_executable(${MODULE_NAME}
    main.cpp
)

target_link_libraries(${MODULE_NAME} PRIVATE AYEditor AYUI AYTest AYResource AYIO AYEntity)

target_include_directories(${MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../AYEntity
    ${CMAKE_CURRENT_SOURCE_DIR}/../../AYEntity/include
)

# Editor unit tests do not load textures via the bimg pipeline, so
# AYRenderer's transitive `bgfx::bimg_decode` pulls `tinyexr` into the
# final link â€?where it collides with `basisu_encoder.lib`'s tinyexr
# (the canonical KTX2/Basis encoder, pulled transitively from
# assimp-vc145-mtd.lib via AYResource/FBXConverter). The
# AYSuzanneSkinned_Demo + AYRenderer_Demo link the same libs without
# surface collision (linker quietly picks one); this AYEditor test
# EXE surfaces LNK2005 because AYEditor is freshly rebuilt and the
# AYResource -> assimp path produces a different link traversal.
# Drop bimg_decode.lib from the default-library search so MSVC keeps
# the basisu_encoder copy. Only `tinyexr` is duplicated; bimg.lib's
# own symbols still resolve via AYEditor's transitive dep chain.
if(MSVC)
    # Background: AYRenderer (PRIVATE) â†?bgfx::bimg_decode pulls tinyexr
    # into AYEditor_UnitTests' link line. AYResource/FBXConverter
    # transitively pulls basisu_encoder.lib (via assimp-vc145-mtd.lib),
    # which also contains tinyexr. The linker errors on the duplicate
    # tinyexr symbols even though both copies are bit-identical (bgfx
    # vendors tinyexr; basisu ships a snapshot of the same code).
    #
    # The demos (AYSuzanneSkinned_Demo.exe, AYRenderer_Demo.exe) link
    # the same pair without LNK2005 because they predate the AYEditor
    # module. /FORCE:UNRESOLVED + accepting the warning here is the
    # narrowest workaround that touches only the new test target and
    # doesn't perturb AYRenderer's semantics for the working demos.
    target_link_options(${MODULE_NAME} PRIVATE
        "/FORCE:MULTIPLE"
        "/IGNORE:4005"   # duplicate definition treated as warning
        "/IGNORE:4006"   # duplicate definition treated as warning
    )
endif()

# D5+.5 (2026-07-26): Disable /OPT:REF for this test binary only â€?
# MSVC's optimizer was stripping the AYTest static `_reg_*`
# initializers when their only "use" was inside the initializer
# itself (calling registerTest to append to the chain). With
# /OPT:NO REF, every symbol in the .obj survives the link, so the
# static init table is preserved end-to-end. Test binary only;
# demos (which link the same pair without issue) keep /OPT:REF.
if(MSVC)
    target_link_options(${MODULE_NAME} PRIVATE "/OPT:NOREF")
endif()

ay_configure_test_tmpdir(${MODULE_NAME})
add_test(NAME ${MODULE_NAME} COMMAND ${MODULE_NAME})
