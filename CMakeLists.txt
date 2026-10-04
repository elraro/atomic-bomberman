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

enable_testing()
add_executable(ab_tests tests/unit/test_core.cpp)
target_link_libraries(ab_tests PRIVATE ab_game)
add_test(NAME core COMMAND ab_tests)
