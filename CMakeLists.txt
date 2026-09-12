cmake_minimum_required(VERSION 3.18)
project(Ultima3Portable LANGUAGES C)

include(CTest)
option(U3_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
option(U3_BUILD_GAME_CORE "Compile the preserved game for the Linux backend (Clang required)" OFF)
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

if(U3_BUILD_GAME_CORE)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "The legacy game uses Pascal string literals. Configure a new build directory with -DCMAKE_C_COMPILER=clang")
    endif()
    add_library(u3game-core OBJECT
        Sources/UltimaAutocombat.c Sources/UltimaSpellCombat.c
        Sources/UltimaNewMap.c Sources/UltimaMisc.c Sources/UltimaMain.c
        Sources/UltimaNew.c Sources/UltimaMacIF.c Sources/UltimaGraphics.c
        Sources/UltimaDngn.c Sources/UltimaText.c Sources/CarbonShunts.c
        Sources/U3RendererLegacy.m Sources/UltimaIncludes.m)
    set_source_files_properties(Sources/U3RendererLegacy.m Sources/UltimaIncludes.m
        PROPERTIES LANGUAGE C COMPILE_FLAGS "-x c")
    target_include_directories(u3game-core PRIVATE Sources)
    target_compile_options(u3game-core PRIVATE -fpascal-strings
        -include "${CMAKE_CURRENT_SOURCE_DIR}/Sources/Linux/U3LegacyTypes.h")
    target_compile_definitions(u3game-core PRIVATE U3_LINUX=1 TARGET_CARBON=1)
    target_link_libraries(u3game-core PRIVATE u3bitmap)
    add_library(u3legacy-memory STATIC Sources/Linux/U3LegacyMemory.c)
    target_link_libraries(u3legacy-memory PUBLIC u3bitmap)

    find_path(U3_JSON_INCLUDE_DIR json-c/json.h
        HINTS "${CMAKE_SOURCE_DIR}/build/deps/sysroot/usr/include" REQUIRED)
    find_library(U3_JSON_LIBRARY NAMES json-c libjson-c.so.5 REQUIRED)

    find_path(U3_SYSROOT_INCLUDE_DIR SDL2/SDL.h
        HINTS "${CMAKE_SOURCE_DIR}/build/deps/sysroot/usr/include" REQUIRED)
    find_path(U3_SYSROOT_ARCH_INCLUDE_DIR x86_64-linux-gnu/SDL2/_real_SDL_config.h
        HINTS "${CMAKE_SOURCE_DIR}/build/deps/sysroot/usr/include" REQUIRED)
    set(U3_SYSROOT_ARCH_REAL_DIR "${U3_SYSROOT_ARCH_INCLUDE_DIR}/x86_64-linux-gnu")

    find_library(U3_SDL2_LIBRARY NAMES SDL2-2.0 libSDL2-2.0.so.0 SDL2
        HINTS "/usr/lib/x86_64-linux-gnu" REQUIRED)
    find_library(U3_FLUIDSYNTH_LIBRARY NAMES fluidsynth libfluidsynth.so.3
        HINTS "/usr/lib/x86_64-linux-gnu" REQUIRED)
    find_library(U3_M_LIBRARY NAMES m REQUIRED)

    add_library(u3linux-assets STATIC Sources/Linux/U3LinuxAssets.c)
    target_include_directories(u3linux-assets PRIVATE "${U3_JSON_INCLUDE_DIR}")
    target_link_libraries(u3linux-assets PUBLIC u3bitmap "${U3_JSON_LIBRARY}")
    target_compile_options(u3linux-assets PRIVATE -Wall -Wextra -Werror)

    add_library(u3linux-io STATIC Sources/Linux/U3IOLinux.c)
    target_include_directories(u3linux-io PUBLIC Sources Sources/Linux)
    target_link_libraries(u3linux-io PUBLIC u3linux-assets u3legacy-memory u3bitmap)
    target_compile_options(u3linux-io PRIVATE -Wall -Wextra -Werror)

    add_library(u3linux-audio STATIC Sources/Linux/U3LinuxAudio.c)
    target_include_directories(u3linux-audio PUBLIC Sources Sources/Linux
        "${U3_SYSROOT_INCLUDE_DIR}" "${U3_SYSROOT_ARCH_REAL_DIR}")
    target_link_libraries(u3linux-audio PUBLIC u3bitmap "${U3_SDL2_LIBRARY}" "${U3_FLUIDSYNTH_LIBRARY}" "${U3_M_LIBRARY}")
    target_compile_options(u3linux-audio PRIVATE -Wall -Wextra -Werror)

    add_library(u3linux-carbon STATIC Sources/Linux/U3LinuxCarbon.c)
    target_include_directories(u3linux-carbon PUBLIC Sources Sources/Linux
        "${U3_SYSROOT_INCLUDE_DIR}" "${U3_SYSROOT_ARCH_REAL_DIR}")
    target_link_libraries(u3linux-carbon PUBLIC u3linux-assets u3bitmap "${U3_SDL2_LIBRARY}")
    target_compile_options(u3linux-carbon PRIVATE -Wall -Wextra -Werror -fpascal-strings)

    add_library(u3linux-platform STATIC Sources/Linux/U3LinuxPlatform.c)
    target_include_directories(u3linux-platform PUBLIC Sources Sources/Linux)
    target_link_libraries(u3linux-platform PUBLIC u3bitmap)
    target_compile_options(u3linux-platform PRIVATE -Wall -Wextra -Werror)

    add_library(u3linux-video STATIC Sources/Linux/U3LinuxVideo.c)
    target_include_directories(u3linux-video PUBLIC Sources Sources/Linux
        "${U3_SYSROOT_INCLUDE_DIR}" "${U3_SYSROOT_ARCH_REAL_DIR}")
    target_link_libraries(u3linux-video PUBLIC u3linux-assets u3bitmap "${U3_SDL2_LIBRARY}")
    target_compile_options(u3linux-video PRIVATE -Wall -Wextra -Werror -fpascal-strings)

    add_executable(ultima3 Sources/Linux/main.c)
    target_include_directories(ultima3 PRIVATE Sources Sources/Linux
        "${U3_SYSROOT_INCLUDE_DIR}" "${U3_SYSROOT_ARCH_REAL_DIR}")
    target_compile_options(ultima3 PRIVATE -fpascal-strings)
    target_link_libraries(ultima3 PRIVATE
        u3game-core
        u3linux-video
        u3linux-carbon
        u3linux-platform
        u3linux-audio
        u3linux-io
        u3linux-assets
        u3legacy-memory
        u3bitmap
        "${U3_SDL2_LIBRARY}"
        "${U3_FLUIDSYNTH_LIBRARY}"
        "${U3_JSON_LIBRARY}"
        "${U3_M_LIBRARY}")
    add_dependencies(ultima3 u3assets)

    if(BUILD_TESTING)
        add_executable(u3-legacy-memory-test tests/legacy-memory-test.c)
        target_compile_options(u3-legacy-memory-test PRIVATE -fpascal-strings -UNDEBUG)
        target_link_libraries(u3-legacy-memory-test PRIVATE u3legacy-memory)
        add_test(NAME legacy-memory COMMAND u3-legacy-memory-test)

        add_executable(u3-linux-assets-test tests/linux-assets-test.c)
        target_compile_options(u3-linux-assets-test PRIVATE -UNDEBUG)
        target_link_libraries(u3-linux-assets-test PRIVATE u3linux-assets)
        add_dependencies(u3-linux-assets-test u3assets)
        add_test(NAME linux-runtime-assets COMMAND u3-linux-assets-test "${CMAKE_CURRENT_BINARY_DIR}/assets")

        add_executable(u3-linux-io-test tests/linux-io-test.c)
        target_compile_options(u3-linux-io-test PRIVATE -UNDEBUG -fpascal-strings)
        target_link_libraries(u3-linux-io-test PRIVATE u3linux-io)
        add_dependencies(u3-linux-io-test u3assets)
        add_test(NAME linux-io COMMAND u3-linux-io-test "${CMAKE_CURRENT_BINARY_DIR}/assets")

        add_executable(u3-linux-audio-test tests/linux-audio-test.c)
        target_compile_options(u3-linux-audio-test PRIVATE -UNDEBUG)
        target_link_libraries(u3-linux-audio-test PRIVATE u3linux-audio)
        add_dependencies(u3-linux-audio-test u3assets)
        add_test(NAME linux-audio COMMAND u3-linux-audio-test "${CMAKE_CURRENT_BINARY_DIR}/assets")
        set_tests_properties(linux-audio PROPERTIES ENVIRONMENT "SDL_AUDIODRIVER=dummy")

        add_executable(u3-linux-platform-test tests/linux-platform-test.c)
        target_compile_options(u3-linux-platform-test PRIVATE -UNDEBUG)
        target_link_libraries(u3-linux-platform-test PRIVATE u3linux-platform)
        add_test(NAME linux-platform COMMAND u3-linux-platform-test)

        add_executable(u3-linux-carbon-test tests/linux-carbon-test.c)
        target_compile_options(u3-linux-carbon-test PRIVATE -UNDEBUG -fpascal-strings)
        target_link_libraries(u3-linux-carbon-test PRIVATE u3linux-carbon)
        add_dependencies(u3-linux-carbon-test u3assets)
        add_test(NAME linux-carbon COMMAND u3-linux-carbon-test "${CMAKE_CURRENT_BINARY_DIR}/assets")

        add_executable(u3-linux-video-test tests/linux-video-test.c)
        target_include_directories(u3-linux-video-test PRIVATE "${U3_SYSROOT_INCLUDE_DIR}")
        target_compile_options(u3-linux-video-test PRIVATE -UNDEBUG -fpascal-strings)
        target_link_libraries(u3-linux-video-test PRIVATE u3linux-video)
        add_dependencies(u3-linux-video-test u3assets)
        add_test(NAME linux-video COMMAND u3-linux-video-test "${CMAKE_CURRENT_BINARY_DIR}/assets")
        set_tests_properties(linux-video PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy")

        add_test(NAME linux-boot-render COMMAND ultima3)
        set_tests_properties(linux-boot-render PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;U3_ASSETS_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/assets;U3_STARTUP_RENDER_CHECK=${CMAKE_CURRENT_BINARY_DIR}/test-startup.bmp")
    endif()
endif()

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
