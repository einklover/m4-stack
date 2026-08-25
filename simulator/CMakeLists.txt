cmake_minimum_required(VERSION 3.16)
project(m4_sim CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
option(M4SIM_SANITIZE "Enable AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(M4SIM_BUILD_NATIVE_SMOKE "Build the lightweight host/native backend smoke" ON)
set(M4SIM_REAL_CJK_FONT "" CACHE FILEPATH "Optional real Noto CJK TTC/OTC fixture")
add_compile_options(-Wall -Wextra)
if(M4SIM_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()
enable_testing()
add_library(m4_reader_core INTERFACE)
target_include_directories(m4_reader_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
add_library(m4_platform_ports INTERFACE)
target_include_directories(m4_platform_ports INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
add_library(m4_deterministic_backend INTERFACE)
target_include_directories(m4_deterministic_backend INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(m4_deterministic_backend INTERFACE m4_reader_core m4_platform_ports)
add_library(m4_board_model INTERFACE)
target_include_directories(m4_board_model INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(m4_board_model INTERFACE m4_reader_core)
add_executable(m4_simulator scenarios/scenarios.cpp scenarios/main.cpp)
target_link_libraries(m4_simulator PRIVATE m4_deterministic_backend)
add_executable(m4_board_tests tests/board_model.cpp)
target_link_libraries(m4_board_tests PRIVATE m4_board_model)
add_test(NAME m4_board_tests COMMAND m4_board_tests)
add_executable(m4_peripheral_policies tests/peripheral_policies.cpp)
target_link_libraries(m4_peripheral_policies PRIVATE m4_board_model)
add_test(NAME m4_peripheral_policies COMMAND m4_peripheral_policies)
add_executable(m4_sd_image_tests tests/sd_image.cpp)
target_link_libraries(m4_sd_image_tests PRIVATE m4_board_model)
add_test(NAME m4_sd_image_tests COMMAND m4_sd_image_tests)
# Reader-menu layout is pure policy shared by drawing and touch hit-testing.
# Exercise all Murphy M4 orientations on the host before device/QEMU validation.
add_executable(m4_reader_menu_layout_tests tests/reader_menu_layout.cpp)
target_include_directories(m4_reader_menu_layout_tests PRIVATE ../firmware/src)
add_test(NAME m4_reader_menu_layout_tests COMMAND m4_reader_menu_layout_tests)
add_executable(m4_track_a_geometry_tests tests/track_a_geometry.cpp)
target_include_directories(m4_track_a_geometry_tests PRIVATE ../firmware/src)
add_test(NAME m4_track_a_geometry_tests COMMAND m4_track_a_geometry_tests)
# Runtime font parsers are deliberately Arduino-free. Compile the production
# sources into host regression targets so compatibility changes are exercised
# without QEMU, an SD fixture, or distro-specific font files.
add_executable(m4_ttf_reader_compound_tests
  tests/ttf_reader_compound.cpp
  ../firmware/lib/TtfReader/src/TtfReader.cpp
  ../firmware/lib/TtfReader/src/TtfReaderFace.cpp
)
target_include_directories(m4_ttf_reader_compound_tests PRIVATE ../firmware/lib/TtfReader/src)
add_test(NAME m4_ttf_reader_compound_tests COMMAND m4_ttf_reader_compound_tests)
add_executable(m4_cff_reader_tests
  tests/cff_reader.cpp
  ../firmware/lib/TtfReader/src/CffReader.cpp
  ../firmware/lib/TtfReader/src/CffType2.cpp
  ../firmware/lib/TtfReader/src/CffRaster.cpp
)
target_include_directories(m4_cff_reader_tests PRIVATE ../firmware/lib/TtfReader/src)
add_test(NAME m4_cff_reader_tests COMMAND m4_cff_reader_tests)
if(M4SIM_REAL_CJK_FONT)
  if(NOT EXISTS "${M4SIM_REAL_CJK_FONT}")
    message(FATAL_ERROR "M4SIM_REAL_CJK_FONT does not exist: ${M4SIM_REAL_CJK_FONT}")
  endif()
  add_executable(m4_cff_real_cjk_tests
    tests/cff_reader_real_noto.cpp
    ../firmware/lib/TtfReader/src/CffReader.cpp
    ../firmware/lib/TtfReader/src/CffType2.cpp
    ../firmware/lib/TtfReader/src/CffRaster.cpp
  )
  target_include_directories(m4_cff_real_cjk_tests PRIVATE ../firmware/lib/TtfReader/src)
  # NotoSansCJK-Regular.ttc face 2 is the Simplified Chinese face on
  # Debian/Ubuntu packages. Keep the fixture outside the repository.
  add_test(NAME m4_cff_real_cjk_tests COMMAND m4_cff_real_cjk_tests "${M4SIM_REAL_CJK_FONT}" 2)
endif()
# Generic production-parser probe for real Chinese fonts downloaded by CI.
# It detects CFF1 vs glyf/loca at runtime, supports TTC/OTC face offsets, and
# rasterizes a shared Simplified-Chinese sample at multiple pixel sizes.
add_executable(m4_web_chinese_font_tests
  tests/chinese_font_web_matrix.cpp
  ../firmware/lib/TtfReader/src/TtfReader.cpp
  ../firmware/lib/TtfReader/src/TtfReaderFace.cpp
  ../firmware/lib/TtfReader/src/CffReader.cpp
  ../firmware/lib/TtfReader/src/CffType2.cpp
  ../firmware/lib/TtfReader/src/CffRaster.cpp
)
target_include_directories(m4_web_chinese_font_tests PRIVATE ../firmware/lib/TtfReader/src)
# Uses the same reference-bbox scale policy as TtfEpdFont.  The Downloads
# folder is intentionally an external fixture; pass M4_FONT_SAMPLE_DIR to
# register this matrix as a CTest when it is available.
add_executable(m4_font_visual_normalization_tests
  tests/font_visual_normalization.cpp
  ../firmware/lib/TtfReader/src/TtfReader.cpp
  ../firmware/lib/TtfReader/src/TtfReaderFace.cpp
  ../firmware/lib/TtfReader/src/CffReader.cpp
  ../firmware/lib/TtfReader/src/CffType2.cpp
  ../firmware/lib/TtfReader/src/CffRaster.cpp
)
target_include_directories(m4_font_visual_normalization_tests PRIVATE
  ../firmware/lib/TtfReader/src ../firmware/lib/EpdFont)
if(M4_FONT_SAMPLE_DIR)
  add_test(NAME m4_font_visual_normalization_tests
           COMMAND m4_font_visual_normalization_tests "${M4_FONT_SAMPLE_DIR}")
endif()

# Exercise the actual streamed runtime-font cache/render path across a face
# switch. Host shims provide only Arduino/SD types; TtfEpdFont and both outline
# backends are the production sources.
add_executable(m4_runtime_ttf_switch_tests
  tests/runtime_ttf_switch.cpp
  ../firmware/lib/EpdFont/TtfEpdFont.cpp
  ../firmware/lib/EpdFont/EpdFont.cpp
  ../firmware/lib/Utf8/Utf8.cpp
  ../firmware/lib/TtfReader/src/TtfReader.cpp
  ../firmware/lib/TtfReader/src/TtfReaderFace.cpp
  ../firmware/lib/TtfReader/src/CffReader.cpp
  ../firmware/lib/TtfReader/src/CffType2.cpp
  ../firmware/lib/TtfReader/src/CffRaster.cpp
)
target_include_directories(m4_runtime_ttf_switch_tests PRIVATE
  tests/runtime_font_shims
  ../firmware/src
  ../firmware/lib/EpdFont
  ../firmware/lib/Utf8
  ../firmware/lib/TtfReader/src)
if(M4_FONT_SAMPLE_DIR)
  add_test(NAME m4_runtime_ttf_switch_tests
           COMMAND m4_runtime_ttf_switch_tests
             "${M4_FONT_SAMPLE_DIR}/FZYTJW.TTF"
             "${M4_FONT_SAMPLE_DIR}/思源宋体SC-VF.ttf")
endif()
add_executable(m4_board_contract tools/board_contract.cpp)
target_link_libraries(m4_board_contract PRIVATE m4_board_model)
add_executable(m4_issue_contracts tests/issue_contracts.cpp)
target_link_libraries(m4_issue_contracts PRIVATE m4_reader_core m4_deterministic_backend)
add_test(NAME m4_issue_contracts COMMAND m4_issue_contracts)
add_executable(m4_gfx_refresh_policy_tests ../firmware/tests/native_app/test_gfx_refresh_policy.cpp)
add_test(NAME m4_gfx_refresh_policy_tests COMMAND m4_gfx_refresh_policy_tests)
add_executable(m4_runtime_ui_font_policy_tests ../firmware/tests/native_app/test_runtime_ui_font_policy.cpp)
target_include_directories(m4_runtime_ui_font_policy_tests PRIVATE ../firmware/src)
add_test(NAME m4_runtime_ui_font_policy_tests COMMAND m4_runtime_ui_font_policy_tests)
add_executable(m4_provider_shelf_cache_tests ../firmware/tests/native_app/test_provider_shelf_cache.cpp)
target_include_directories(m4_provider_shelf_cache_tests PRIVATE ../firmware/src)
add_test(NAME m4_provider_shelf_cache_tests COMMAND m4_provider_shelf_cache_tests)
# Host font tests avoid firmware/lib/EpdFont/EpdFont.cpp (Arduino/Utf8) and
# instead link a tiny Arduino-free vtable TU. Derived faces still override
# getGlyph / glyphAdvanceX / loadGlyphBitmap. -fno-rtti matches murphy_m4.
add_executable(m4_native_grid_font_tests
  tests/native_grid_font.cpp
  tests/epd_font_host_vtable.cpp
)
target_include_directories(m4_native_grid_font_tests PRIVATE ../firmware/src ../firmware/lib/EpdFont)
target_compile_options(m4_native_grid_font_tests PRIVATE -fno-rtti)
add_test(NAME m4_native_grid_font_tests
         COMMAND m4_native_grid_font_tests
                 "${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src/fontdata/m4_native_grid_15x16.bin")
# Builtin native-grid may only scale by integer N (Kronecker). Nominal reader
# sizes snap (12–20→1x, 21–39→2x, 40+→3x). Chrome is 1x/2x/2x (16/32/32).
add_executable(m4_scaled_epd_font_tests
  tests/scaled_epd_font.cpp
  tests/epd_font_host_vtable.cpp
)
target_include_directories(m4_scaled_epd_font_tests PRIVATE ../firmware/src ../firmware/lib/EpdFont)
target_compile_options(m4_scaled_epd_font_tests PRIVATE -fno-rtti)
add_test(NAME m4_scaled_epd_font_tests
         COMMAND m4_scaled_epd_font_tests
                 "${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src/fontdata/m4_native_grid_15x16.bin")

# Replay the production SSD1677 driver against the register-level simulator.
# The Arduino/SPI/ESP GPIO shims are test-only and keep this target independent
# of PlatformIO while exercising the real driver command stream.
add_executable(m4_ssd1677_driver_replay
  tests/driver_replay/ssd1677_driver_replay.cpp
  tests/driver_replay/HostEpdBus.cpp
  ../firmware/open-m4-sdk/libs/display/FreeInkDisplay/src/driver/Ssd1677Driver.cpp
)
target_compile_definitions(m4_ssd1677_driver_replay PRIVATE FREEINK_DEVICE_MURPHY_M4=1)
target_include_directories(m4_ssd1677_driver_replay PRIVATE
  tests/driver_replay/include
  tests/driver_replay
  ../firmware/open-m4-sdk/libs/hardware/BoardConfig/include
  ../firmware/open-m4-sdk/libs/display/FreeInkDisplay/src
  ../firmware/open-m4-sdk/libs/display/FreeInkDisplay/src/bus
  ../firmware/open-m4-sdk/libs/display/FreeInkDisplay/src/driver
)
target_link_libraries(m4_ssd1677_driver_replay PRIVATE m4_reader_core m4_deterministic_backend)
add_test(NAME m4_ssd1677_driver_replay COMMAND m4_ssd1677_driver_replay)
if(M4SIM_BUILD_NATIVE_SMOKE)
  add_executable(m4_native_smoke native/main.cpp)
  target_link_libraries(m4_native_smoke PRIVATE m4_reader_core m4_platform_ports)
  add_test(NAME m4_native_smoke COMMAND m4_native_smoke)
endif()
