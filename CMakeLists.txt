cmake_minimum_required(VERSION 3.20)
project(AtomicBombermanModern LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Windows release builds link the C++ runtime statically so the .exe runs without a redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Warning flags for this project's own targets (not for a fetched SDL3).
function(ab_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
endfunction()

# Gameplay core: no SDL, no OpenGL, no platform code.
add_library(ab_game
    src/game/values.cpp
    src/game/world.cpp
    src/game/ai.cpp
    src/game/roulette.cpp
    src/game/match.cpp
)
target_include_directories(ab_game PUBLIC src)
ab_warnings(ab_game)

# Readers for the original game's data files (legacy formats stay out of the core).
add_library(ab_resources src/resources/scheme_file.cpp src/resources/ani_file.cpp src/resources/asset_import.cpp
    src/resources/settings.cpp src/resources/help_file.cpp src/resources/campaign_file.cpp src/resources/mve_file.cpp)
target_link_libraries(ab_resources PUBLIC ab_game)
ab_warnings(ab_resources)

# Network play: sockets, protocol, server and client. No SDL: the dedicated
# server is built from this alone.
add_library(ab_net src/net/socket.cpp src/net/protocol.cpp src/net/server.cpp src/net/client.cpp)
target_link_libraries(ab_net PUBLIC ab_game ab_resources)
if(WIN32)
    target_link_libraries(ab_net PUBLIC ws2_32)
endif()
ab_warnings(ab_net)

# Front end: SDL3 window, input and audio + OpenGL 3.3 renderer. Optional.
# SDL3 is taken from the system when installed. With -DAB_FETCH_SDL3=ON it is
# downloaded and built as a static library instead (used by the release builds).
option(AB_FETCH_SDL3 "Download and build SDL3 if it is not installed" OFF)
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND AND AB_FETCH_SDL3)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.2.8
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3)
    set(SDL3_FOUND TRUE)
endif()
if(SDL3_FOUND)
    add_executable(atomic
        src/app/main.cpp
        src/app/editor.cpp
        src/rendering/gl.cpp
        src/rendering/renderer.cpp
        src/rendering/sprites.cpp
        src/audio/audio.cpp)
    target_link_libraries(atomic PRIVATE ab_game ab_resources SDL3::SDL3)
    ab_warnings(atomic)
    install(TARGETS atomic RUNTIME DESTINATION .)
else()
    message(STATUS "SDL3 not found: building the core and tests only (use -DAB_FETCH_SDL3=ON to download it)")
endif()

enable_testing()
# Developer tool for the movie decoder (not installed).
add_executable(mve_dump tools/diagnostics/mve_dump.cpp)
target_link_libraries(mve_dump PRIVATE ab_resources)
ab_warnings(mve_dump)

add_executable(ab_tests tests/unit/test_core.cpp)
target_link_libraries(ab_tests PRIVATE ab_game ab_resources)
ab_warnings(ab_tests)
add_test(NAME core COMMAND ab_tests)

# Network mode: codec tests, and a server with several clients over the loopback interface.
add_executable(ab_net_tests tests/unit/test_net.cpp)
target_link_libraries(ab_net_tests PRIVATE ab_net)
ab_warnings(ab_net_tests)
add_test(NAME net COMMAND ab_net_tests)
