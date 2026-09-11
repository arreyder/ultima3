cmake_minimum_required(VERSION 3.16)
project(Ultima3Portable LANGUAGES C)

include(CTest)
option(U3_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" -c "from PIL import Image"
    RESULT_VARIABLE U3_PIL_RESULT ERROR_QUIET)
if(NOT U3_PIL_RESULT EQUAL 0)
    message(FATAL_ERROR "Asset conversion needs Pillow. On Ubuntu install python3-pil and set -DPython3_EXECUTABLE=/usr/bin/python3")
endif()

add_library(u3bitmap STATIC Sources/U3Bitmap.c)
target_include_directories(u3bitmap PUBLIC Sources)
target_compile_features(u3bitmap PUBLIC c_std_11)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(u3bitmap PRIVATE -Wall -Wextra -Werror)
    if(U3_SANITIZERS)
        target_compile_options(u3bitmap PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(u3bitmap PUBLIC -fsanitize=address,undefined)
    endif()
endif()

file(GLOB_RECURSE U3_ASSET_INPUTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Resources/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/Images/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/Cursors/*")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/assets/manifest.json"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/export-portable-assets.py"
        --source "${CMAKE_CURRENT_SOURCE_DIR}" --output "${CMAKE_CURRENT_BINARY_DIR}/assets"
    DEPENDS ${U3_ASSET_INPUTS} LICENSE README.md scripts/export-portable-assets.py scripts/resource_fork.py
    VERBATIM)
add_custom_target(u3assets ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/assets/manifest.json")

if(BUILD_TESTING)
    add_executable(u3-bitmap-test tests/bitmap-test.c)
    target_link_libraries(u3-bitmap-test PRIVATE u3bitmap)
    # The existing test uses assert for both operations and verification.
    if(MSVC)
        target_compile_options(u3-bitmap-test PRIVATE /UNDEBUG)
    else()
        target_compile_options(u3-bitmap-test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME bitmap COMMAND u3-bitmap-test)
    add_test(NAME resource-fork COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_resource_fork.py")
    add_test(NAME portable-assets COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_portable_assets.py"
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/assets")
endif()
