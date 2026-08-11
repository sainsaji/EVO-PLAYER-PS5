# =============================================================================
# EVO Player - CMake build
#
# WHY THERE ARE TWO BUILD SYSTEMS
#   The per-project Makefiles are canonical. They match how the PS5 Payload SDK
#   samples are written, they are what ProsperoPlayer uses, and they are the
#   shortest path from "edit a file" to "ELF on the console".
#
#   This CMake build exists mainly to produce compile_commands.json, which is
#   what clangd and VS Code IntelliSense need to resolve headers and offer
#   navigation. It builds the same sources with the same cross toolchain, so
#   the database it emits is accurate.
#
#   It is also the integration point for anything that genuinely wants CMake
#   (SDL2, mesa) once those get pulled into a project.
#
# CONFIGURE (inside the dev container):
#   $CMAKE -S . -B build/cmake -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
#   $CMAKE --build build/cmake
#
# $CMAKE is the SDK's prospero-cmake wrapper, which sets CMAKE_TOOLCHAIN_FILE
# to toolchain/prospero.cmake for you. Plain `cmake` will build for the host
# and is wrong here. scripts/gen-compile-commands.sh does this correctly.
# =============================================================================

cmake_minimum_required(VERSION 3.16)

project(EVOPlayer
    VERSION     0.1.0
    DESCRIPTION "PS5 12.70 homebrew media player and development environment"
    LANGUAGES   C ASM)

# -----------------------------------------------------------------------------
# Refuse to configure for the host. Building these sources with the host
# compiler produces Linux binaries that will never run on a PS5, and the
# resulting compile_commands.json would point IntelliSense at glibc headers.
# -----------------------------------------------------------------------------
if(NOT PROSPERO)
    message(FATAL_ERROR
        "This project must be configured with the PS5 cross toolchain.\n"
        "Use the SDK wrapper:\n"
        "    $CMAKE -S . -B build/cmake -G Ninja\n"
        "or pass the toolchain file explicitly:\n"
        "    cmake -S . -B build/cmake -G Ninja \\\n"
        "      -DCMAKE_TOOLCHAIN_FILE=$ENV{PS5_PAYLOAD_SDK}/toolchain/prospero.cmake\n"
        "Easiest: ./scripts/gen-compile-commands.sh")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Keep every payload in one predictable place.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/elf")

add_compile_options(-Wall -Wextra -Wno-unused-parameter -g -O2)

# -----------------------------------------------------------------------------
# Shared helpers (the notification wrapper) and the hand-written SCE headers.
# -----------------------------------------------------------------------------
set(EVO_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/projects/common")
add_library(evo_common STATIC "${EVO_COMMON_DIR}/src/evo_notify.c")
target_include_directories(evo_common PUBLIC "${EVO_COMMON_DIR}/include")

# The prebuilt ports prefix from pacbrew-repo (SDL2, FFmpeg, ...).
set(EVO_HB_PREFIX "$ENV{PS5_PAYLOAD_SDK}/target/user/homebrew")

# -----------------------------------------------------------------------------
# evo_payload(<name> [LIBS ...])
# Declares a payload built from projects/<name>/main.c.
# -----------------------------------------------------------------------------
function(evo_payload name)
    cmake_parse_arguments(ARG "" "" "LIBS;SOURCES" ${ARGN})

    set(srcs ${ARG_SOURCES})
    if(NOT srcs)
        set(srcs "${CMAKE_CURRENT_SOURCE_DIR}/projects/${name}/main.c")
    endif()

    add_executable(${name} ${srcs})
    set_target_properties(${name} PROPERTIES OUTPUT_NAME "${name}.elf")
    target_link_libraries(${name} PRIVATE evo_common ${ARG_LIBS})

    # `ninja test_<name>` deploys to the console, mirroring `make test`.
    # PS5_DEPLOY is set by prospero.cmake.
    add_custom_target(test_${name}
        USES_TERMINAL
        COMMAND ${PS5_DEPLOY} "$<TARGET_FILE:${name}>"
        DEPENDS ${name}
        COMMENT "Deploying ${name}.elf to $ENV{PS5_HOST}")
endfunction()

# -----------------------------------------------------------------------------
# The project suite. Library names match the SDK's SCE stubs in
# $PS5_PAYLOAD_SDK/target/lib (built from sce_stubs/*.c).
# -----------------------------------------------------------------------------
evo_payload(hello_world)
evo_payload(system_info)
# SceSystemService is for sceSystemServiceHideSplashScreen(), without which
# the splash stays composited over the test pattern.
evo_payload(videoout_test  LIBS SceVideoOut SceUserService SceSystemService)
evo_payload(audioout_test  LIBS SceAudioOut SceUserService m)
evo_payload(gpu_test       LIBS SceGnmDriver)
# Keep this list in step with projects/decoder_test/Makefile - the probe links
# the VideoOut/AudioOut stubs as its dynlib control (3/3 resolved is what proves
# the probe itself works) and calls sceUserService directly.
evo_payload(decoder_test   LIBS SceSysmodule SceVideoOut SceAudioOut
                                SceUserService pthread)
evo_payload(avplayer_test  LIBS SceSysmodule)
evo_payload(videodec2_test LIBS SceSysmodule)
evo_payload(createdecoder_test LIBS SceSysmodule)

# Phase 6 carries its own H.264 bitstream, linked into .rodata by test_stream.S
# with .incbin - hence the extra source, the ASM language above, and the include
# directory that lets the assembler find the .h264 next to the .S.
evo_payload(decodeframe_test LIBS SceSysmodule
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/projects/decodeframe_test/main.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/projects/decodeframe_test/test_stream.S")
target_include_directories(decodeframe_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/projects/decodeframe_test")
evo_payload(codecdump_test LIBS SceSysmodule)
evo_payload(avplayer_probe LIBS SceSysmodule
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/projects/avplayer_probe/main.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/projects/avplayer_probe/test_clip.S")
target_include_directories(avplayer_probe PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/projects/avplayer_probe")
evo_payload(slotcheck)
evo_payload(yuv_gpu_test)

message(STATUS "EVO Player: PS5 SDK  = $ENV{PS5_PAYLOAD_SDK}")
message(STATUS "EVO Player: sysroot  = ${CMAKE_SYSROOT}")
message(STATUS "EVO Player: ports    = ${EVO_HB_PREFIX}")
