cmake_minimum_required(VERSION 3.20)
# The release number: shown on the main menu and by --version. Releases are tagged v<this>.
project(AtomicBombermanModern VERSION 1.0 LANGUAGES CXX)

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

# The free asset set, made by code: graphics, sounds and data for people without the original game.
add_library(ab_free src/free/free_art.cpp src/free/free_sound.cpp src/free/free_data.cpp)
target_link_libraries(ab_free PUBLIC ab_game ab_resources)
ab_warnings(ab_free)

if(NOT ANDROID)
    add_executable(free_art_dump tools/diagnostics/free_art_dump.cpp)
    target_link_libraries(free_art_dump PRIVATE ab_free)
    ab_warnings(free_art_dump)
endif()

# Network play: sockets, protocol, server and client. No SDL: the dedicated
# server is built from this alone.
find_package(Threads REQUIRED)
add_library(ab_net src/net/socket.cpp src/net/protocol.cpp src/net/server.cpp src/net/client.cpp src/net/upnp.cpp src/net/crypto.cpp src/net/relay.cpp)
target_link_libraries(ab_net PUBLIC ab_game ab_resources Threads::Threads)
if(WIN32)
    target_link_libraries(ab_net PUBLIC ws2_32)
endif()
ab_warnings(ab_net)

# Dedicated server: no window, no SDL. (Not for Android: the app is the game alone.)
if(NOT ANDROID)
    add_executable(atomic_server src/server/main.cpp)
    target_link_libraries(atomic_server PRIVATE ab_net ab_free Threads::Threads)
    target_compile_definitions(atomic_server PRIVATE AB_VERSION="${PROJECT_VERSION}")
    ab_warnings(atomic_server)
    install(TARGETS atomic_server RUNTIME DESTINATION .)
endif()

# Front end: SDL3 window, input and audio + OpenGL 3.3 renderer. Optional.
# SDL3 is taken from the system when installed. With -DAB_FETCH_SDL3=ON it is
# downloaded and built as a static library instead (used by the release builds).
option(AB_FETCH_SDL3 "Download and build SDL3 if it is not installed" OFF)
option(AB_MACOS_APP "macOS: build the game as an application bundle (.app) instead of a plain program" OFF)
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND AND AB_FETCH_SDL3)
    include(FetchContent)
    if(ANDROID)
        # The Android activity (SDL's Java side) loads libSDL3.so and then the game's library.
        set(SDL_SHARED ON CACHE BOOL "" FORCE)
        set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    else()
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
    endif()
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.2.8
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(SDL3)
    set(SDL3_FOUND TRUE)
endif()
if(SDL3_FOUND)
    set(AB_APP_SOURCES
        src/app/main.cpp
        src/app/editor.cpp
        src/app/net_ui.cpp
        src/app/touch.cpp
        src/app/import_screen.cpp
        src/app/original_folder.cpp
        src/rendering/gl.cpp
        src/rendering/renderer.cpp
        src/rendering/sprites.cpp
        src/audio/audio.cpp)
    if(ANDROID)
        # On Android the game is a shared library named "main", started by SDL's activity
        # (see android/). Built by Gradle, which calls this file once per processor type.
        add_library(main SHARED ${AB_APP_SOURCES})
        set(AB_APP main)
    elseif(APPLE AND AB_MACOS_APP)
        # macOS: an application bundle, "Atomic Bomberman.app", with the program inside it
        # (Contents/MacOS/atomic, which still takes the command-line options).
        # -DAB_MACOS_ICON=FILE.icns gives it an icon; the release build makes that file
        # from packaging/macos/icon-1024.png (see .github/workflows/build.yml).
        set(AB_ICON)
        if(AB_MACOS_ICON)
            set(AB_ICON ${AB_MACOS_ICON})
            set_source_files_properties(${AB_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
            get_filename_component(AB_ICON_NAME ${AB_ICON} NAME)
        endif()
        add_executable(atomic MACOSX_BUNDLE ${AB_APP_SOURCES} ${AB_ICON})
        set(AB_APP atomic)
        set_target_properties(atomic PROPERTIES
            OUTPUT_NAME "Atomic Bomberman"
            MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info.plist.in
            MACOSX_BUNDLE_BUNDLE_NAME "Atomic Bomberman"
            MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.elraro.atomicbomberman"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_ICON_FILE "${AB_ICON_NAME}"
            MACOSX_BUNDLE_COPYRIGHT "GPL-3.0-or-later. A new program: it holds nothing of the original game.")
        install(TARGETS atomic BUNDLE DESTINATION .)
    else()
        add_executable(atomic ${AB_APP_SOURCES})
        set(AB_APP atomic)
        install(TARGETS atomic RUNTIME DESTINATION .)
    endif()
    target_link_libraries(${AB_APP} PRIVATE ab_game ab_resources ab_net ab_free SDL3::SDL3)
    target_compile_definitions(${AB_APP} PRIVATE AB_VERSION="${PROJECT_VERSION}")
    ab_warnings(${AB_APP})
else()
    message(STATUS "SDL3 not found: building the core and tests only (use -DAB_FETCH_SDL3=ON to download it)")
endif()

if(NOT ANDROID)
enable_testing()
# Developer tool for the movie decoder (not installed).
add_executable(mve_dump tools/diagnostics/mve_dump.cpp)
target_link_libraries(mve_dump PRIVATE ab_resources)
ab_warnings(mve_dump)

add_executable(ai_stats tools/diagnostics/ai_stats.cpp)
target_link_libraries(ai_stats PRIVATE ab_game)
ab_warnings(ai_stats)

# Developer tool for the network mode: a client without a window (not installed).
add_executable(net_bot tools/diagnostics/net_bot.cpp)
target_link_libraries(net_bot PRIVATE ab_net)
ab_warnings(net_bot)

add_executable(ab_tests tests/unit/test_core.cpp)
target_link_libraries(ab_tests PRIVATE ab_game ab_resources ab_free)
ab_warnings(ab_tests)
add_test(NAME core COMMAND ab_tests)

# Network mode: codec tests, and a server with several clients over the loopback interface.
add_executable(ab_net_tests tests/unit/test_net.cpp)
target_link_libraries(ab_net_tests PRIVATE ab_net ab_free)
ab_warnings(ab_net_tests)
add_test(NAME net COMMAND ab_net_tests)
endif()
