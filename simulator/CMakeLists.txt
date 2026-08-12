cmake_minimum_required(VERSION 3.16)
project(m4_sim CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(M4SIM_SANITIZE "Enable AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(M4SIM_BUILD_NATIVE_SMOKE "Build the lightweight host/native backend smoke" ON)

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
target_link_libraries(m4_deterministic_backend INTERFACE
  m4_reader_core
  m4_platform_ports
)

add_library(m4_board_model INTERFACE)
target_include_directories(m4_board_model INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(m4_board_model INTERFACE m4_reader_core)

add_executable(m4_simulator
  scenarios/scenarios.cpp
  scenarios/main.cpp
)
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

# TtfReader is deliberately Arduino-free. Compile the production parser itself
# into a host regression target so outline compatibility changes are exercised
# without QEMU, an SD fixture, or distro-specific font files.
add_executable(m4_ttf_reader_compound_tests
  tests/ttf_reader_compound.cpp
  ../firmware/lib/TtfReader/src/TtfReader.cpp
)
target_include_directories(m4_ttf_reader_compound_tests PRIVATE
  ../firmware/lib/TtfReader/src
)
add_test(NAME m4_ttf_reader_compound_tests COMMAND m4_ttf_reader_compound_tests)

add_executable(m4_board_contract tools/board_contract.cpp)
target_link_libraries(m4_board_contract PRIVATE m4_board_model)

add_executable(m4_issue_contracts tests/issue_contracts.cpp)
target_link_libraries(m4_issue_contracts PRIVATE
  m4_reader_core
  m4_deterministic_backend
)
add_test(NAME m4_issue_contracts COMMAND m4_issue_contracts)

if(M4SIM_BUILD_NATIVE_SMOKE)
  add_executable(m4_native_smoke native/main.cpp)
  target_link_libraries(m4_native_smoke PRIVATE
    m4_reader_core
    m4_platform_ports
  )
  add_test(NAME m4_native_smoke COMMAND m4_native_smoke)
endif()
