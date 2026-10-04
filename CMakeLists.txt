cmake_minimum_required(VERSION 3.20)
project(AtomicBombermanModern LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow)

# Gameplay core: no SDL, no OpenGL, no platform code.
add_library(ab_game
    src/game/values.cpp
    src/game/world.cpp
)
target_include_directories(ab_game PUBLIC src)

# Readers for the original game's data files (legacy formats stay out of the core).
add_library(ab_resources src/resources/scheme_file.cpp src/resources/ani_file.cpp)
target_link_libraries(ab_resources PUBLIC ab_game)

# Front end: SDL3 window/input + OpenGL 3.3 renderer. Optional.
find_package(SDL3 CONFIG QUIET)
find_package(OpenGL QUIET)
if(SDL3_FOUND AND OpenGL_FOUND)
    add_executable(atomic src/app/main.cpp src/rendering/renderer.cpp src/rendering/sprites.cpp src/audio/audio.cpp)
    target_link_libraries(atomic PRIVATE ab_game ab_resources SDL3::SDL3 OpenGL::GL)
else()
    message(STATUS "SDL3 or OpenGL not found: building the core and tests only")
endif()

enable_testing()
add_executable(ab_tests tests/unit/test_core.cpp)
target_link_libraries(ab_tests PRIVATE ab_game ab_resources)
add_test(NAME core COMMAND ab_tests)
