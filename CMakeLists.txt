cmake_minimum_required(VERSION 3.20)
project(btdd_port C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(BTDD_SDL "Fetch SDL3 and define the btdd_sdl_probe target (M2 groundwork)" ON)

# Warnings for every target of ours (not for fetched libraries)
function(btdd_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
    if(WIN32 AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endfunction()

# Reference interpreter and hardware layer (no platform code)
add_library(btdd_core STATIC
    src/rom/rom.c
    src/rom/sha1.c
    src/runtime/cpu.c
    src/runtime/fm2.c
    src/nes/ppu.c
    src/nes/nes.c
)
target_include_directories(btdd_core PUBLIC src)
btdd_warnings(btdd_core)

add_executable(btdd src/main.c)
target_link_libraries(btdd PRIVATE btdd_core)
btdd_warnings(btdd)

# Coverage recorder, snapshots, warp/god mode, explorer (tools only; not part of the game exe)
add_library(btdd_cov STATIC
    src/cov/coverage.c
    src/cov/snapshot.c
    src/cov/tracker.c
    src/cov/play.c
)
target_link_libraries(btdd_cov PUBLIC btdd_core)
btdd_warnings(btdd_cov)

add_executable(btdd_headless src/headless_main.c)
target_link_libraries(btdd_headless PRIVATE btdd_core btdd_cov)
btdd_warnings(btdd_headless)

add_executable(nestest_check tests/cpu/nestest.c)
target_link_libraries(nestest_check PRIVATE btdd_core)
btdd_warnings(nestest_check)

add_executable(cpu_call_test tests/cpu/call_test.c)
target_link_libraries(cpu_call_test PRIVATE btdd_core)
btdd_warnings(cpu_call_test)

# Room for M5 (load-time recompiler): libtcc compiles generated C in-process. Not fetched yet; when it is,
# set BTDD_LIBTCC=ON, provide a target named libtcc (FetchContent or a prebuilt import) and link it to the
# recompiler target. Generated code links against btdd_core (Cpu, cpu_bus_read/write, cpu_add_cycles, cpu_call).
option(BTDD_LIBTCC "Build the load-time recompiler with embedded libtcc (M5; not available yet)" OFF)
if(BTDD_LIBTCC)
    message(FATAL_ERROR "BTDD_LIBTCC: libtcc integration is planned for M5 and not set up yet")
endif()

if(BTDD_SDL)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG        release-3.4.20  # f00dc919ca35b9756316e86c4777392de442f9b2
        GIT_SHALLOW    TRUE
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(SDL3)

    # Only target that links SDL for now; not part of the default build ("cmake --build ... --target btdd_sdl_probe").
    add_executable(btdd_sdl_probe EXCLUDE_FROM_ALL src/platform/sdl_probe.c)
    target_link_libraries(btdd_sdl_probe PRIVATE SDL3::SDL3-static)
    btdd_warnings(btdd_sdl_probe)
endif()
