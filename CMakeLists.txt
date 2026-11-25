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
    src/nes/ppu_new.c
    src/nes/apu.c
    src/nes/nes.c
    src/nes/state.c
)
target_include_directories(btdd_core PUBLIC src)
if(NOT WIN32 OR NOT MSVC)
    find_library(BTDD_LIBM m)
    if(BTDD_LIBM)
        target_link_libraries(btdd_core PUBLIC ${BTDD_LIBM})
    endif()
endif()
btdd_warnings(btdd_core)

# Display layer without SDL: picture from the PPU, palette, PNG (M2)
add_library(btdd_screen STATIC
    src/platform/screen.c
    src/platform/png.c
    src/platform/rewind.c
    src/platform/wide.c
)
target_link_libraries(btdd_screen PUBLIC btdd_core)
btdd_warnings(btdd_screen)

add_executable(mods_test tests/mods/mods_test.c)
target_link_libraries(mods_test PRIVATE btdd_screen btdd_mods)
btdd_warnings(mods_test)

# M13 netplay core (no SDL, no sockets) and its two-peer test over a simulated link
add_library(btdd_net STATIC src/net/netplay.c src/net/joincode.c)
target_link_libraries(btdd_net PUBLIC btdd_core)
btdd_warnings(btdd_net)
# join-code path on one PC: two real UDP sockets on 127.0.0.1 (src/platform/udp.c is plain sockets, no SDL)
add_executable(joincode_test tests/net/joincode_test.c src/platform/udp.c)
target_include_directories(joincode_test PRIVATE src/platform)
target_link_libraries(joincode_test PRIVATE btdd_net)
if(WIN32)
    target_link_libraries(joincode_test PRIVATE ws2_32)
endif()
btdd_warnings(joincode_test)
add_executable(nettest tools/nettest/nettest_main.c)
target_link_libraries(nettest PRIVATE btdd_net)
btdd_warnings(nettest)

add_executable(rewind_test tests/state/rewind_test.c)
target_link_libraries(rewind_test PRIVATE btdd_screen)
btdd_warnings(rewind_test)

add_executable(btdd_shot tools/shot/shot_main.c)
target_link_libraries(btdd_shot PRIVATE btdd_screen btdd_mods)
btdd_warnings(btdd_shot)

add_executable(btdd src/main.c)
target_link_libraries(btdd PRIVATE btdd_core)
btdd_warnings(btdd)

# Coverage recorder, snapshots, warp/god mode, explorer (tools only; not part of the game exe)
# M7 mods (our own code hooked at ROM routine addresses; src/mods/mods.h)
add_library(btdd_mods STATIC
    src/mods/mods.c
    src/mods/mod_lives.c
    src/mods/mod_nolimit.c
    src/mods/mod_wide.c
    src/mods/mod_cheats.c
)
target_link_libraries(btdd_mods PUBLIC btdd_core)
btdd_warnings(btdd_mods)

add_library(btdd_cov STATIC
    src/cov/coverage.c
    src/cov/snapshot.c
    src/cov/tracker.c
    src/cov/play.c
)
target_link_libraries(btdd_cov PUBLIC btdd_core)
btdd_warnings(btdd_cov)

add_executable(btdd_explore src/explore_main.c)
target_link_libraries(btdd_explore PRIVATE btdd_core btdd_cov)
btdd_warnings(btdd_explore)

add_executable(btdd_static src/static_main.c)
target_link_libraries(btdd_static PRIVATE btdd_core btdd_cov)
btdd_warnings(btdd_static)

add_executable(btdd_headless src/headless_main.c)
target_link_libraries(btdd_headless PRIVATE btdd_core btdd_cov btdd_mods)
btdd_warnings(btdd_headless)

add_executable(btdd_apu_check tools/apu_check/apu_check_main.c)
target_link_libraries(btdd_apu_check PRIVATE btdd_core)
btdd_warnings(btdd_apu_check)

add_executable(apu_synth_test tests/apu/apu_synth_test.c)
target_link_libraries(apu_synth_test PRIVATE btdd_core)
btdd_warnings(apu_synth_test)

add_executable(nestest_check tests/cpu/nestest.c)
target_link_libraries(nestest_check PRIVATE btdd_core)
btdd_warnings(nestest_check)

add_executable(cpu_call_test tests/cpu/call_test.c)
target_link_libraries(cpu_call_test PRIVATE btdd_core)
btdd_warnings(cpu_call_test)

# M5: the generator in C (same output as recomp/gen.py), the step the shipped exe runs at first start.
add_executable(btdd_gen recomp/gen.c)
target_link_libraries(btdd_gen PRIVATE btdd_core)
btdd_warnings(btdd_gen)

# Sources of the headless runner without src/runtime/cpu.c: recomp/native_cpu.c compiles cpu.c into itself
# with cpu_run renamed and provides a cpu_run with native dispatch.
# The list is collected from btdd_headless and the libraries it links (recursively), so merged trees need no
# edit here. Must come after those targets are defined.
function(btdd_collect_sources target out)
    get_target_property(_src ${target} SOURCES)
    get_target_property(_dir ${target} SOURCE_DIR)
    set(_all)
    foreach(f IN LISTS _src)
        if(IS_ABSOLUTE "${f}")
            list(APPEND _all "${f}")
        else()
            list(APPEND _all "${_dir}/${f}")
        endif()
    endforeach()
    get_target_property(_libs ${target} LINK_LIBRARIES)
    foreach(l IN LISTS _libs)
        if(TARGET ${l})
            get_target_property(_type ${l} TYPE)
            if(_type STREQUAL "STATIC_LIBRARY" OR _type STREQUAL "OBJECT_LIBRARY")
                btdd_collect_sources(${l} _sub)
                list(APPEND _all ${_sub})
            endif()
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _all)
    set(${out} ${_all} PARENT_SCOPE)
endfunction()
btdd_collect_sources(btdd_headless BTDD_NATIVE_CORE)
list(FILTER BTDD_NATIVE_CORE EXCLUDE REGEX "/src/runtime/cpu\\.c$")
list(APPEND BTDD_NATIVE_CORE recomp/native_cpu.c)
# Trees whose Cpu has insn_hook/tick_hook (coverage + APU merged): generated code must call them (native_rt.h).
file(READ src/runtime/cpu.h BTDD_CPU_H)
set(BTDD_CPU_HOOK_DEFS)
if(BTDD_CPU_H MATCHES "tick_hook" AND BTDD_CPU_H MATCHES "insn_hook")
    set(BTDD_CPU_HOOK_DEFS BTDD_CPU_HOOKS)
endif()

# M5 test exe with the generated code linked in (ROM-derived, kept outside the repo, e.g.
# -DBTDD_NATIVE_GEN=C:/.../Gamefiles/Movies/m5/generated).
set(BTDD_NATIVE_GEN "" CACHE PATH "Folder with recomp/gen.py output; adds the btdd_native target")
if(BTDD_NATIVE_GEN)
    file(GLOB BTDD_NATIVE_SOURCES "${BTDD_NATIVE_GEN}/native_*.c")
    add_executable(btdd_native ${BTDD_NATIVE_CORE} ${BTDD_NATIVE_SOURCES})
    target_include_directories(btdd_native PRIVATE src recomp)
    target_compile_definitions(btdd_native PRIVATE BTDD_NATIVE_STATIC ${BTDD_CPU_HOOK_DEFS})
    if(MSVC)
        target_compile_definitions(btdd_native PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endif()

# M5 load-time recompiler: btdd_jit generates C from the player's ROM at first start, compiles it to a DLL in
# %LOCALAPPDATA%\btdd\cache and loads it. With BTDD_LIBTCC=ON the compiler is libtcc, used as a separate,
# dynamically linked DLL (LGPL) from BTDD_TINYCC_DIR (default ../tools/tinycc: libtcc.h, libtcc.dll, its import
# library, and tcc's include/ + lib/). Without it the compiler command comes from the BTDD_CC environment
# variable (development only).
option(BTDD_LIBTCC "Compile generated code with libtcc (M5)" OFF)
set(BTDD_TINYCC_DIR "${CMAKE_SOURCE_DIR}/../tools/tinycc" CACHE PATH "TinyCC folder (libtcc.h, libtcc.dll)")
add_executable(btdd_jit ${BTDD_NATIVE_CORE} recomp/native_jit.c recomp/gen.c)
btdd_warnings(btdd_jit)
target_include_directories(btdd_jit PRIVATE src recomp)
target_compile_definitions(btdd_jit PRIVATE ${BTDD_CPU_HOOK_DEFS})
target_compile_definitions(btdd_jit PRIVATE BTDD_GEN_NO_MAIN "BTDD_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
if(MSVC)
    target_compile_definitions(btdd_jit PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(BTDD_LIBTCC)
    find_path(LIBTCC_INCLUDE libtcc.h PATHS "${BTDD_TINYCC_DIR}" "${BTDD_TINYCC_DIR}/libtcc" NO_DEFAULT_PATH)
    find_library(LIBTCC_LIB NAMES libtcc tcc PATHS "${BTDD_TINYCC_DIR}" "${BTDD_TINYCC_DIR}/libtcc" NO_DEFAULT_PATH)
    if(NOT LIBTCC_INCLUDE OR NOT LIBTCC_LIB)
        message(FATAL_ERROR "BTDD_LIBTCC: libtcc.h / libtcc import library not found under ${BTDD_TINYCC_DIR}")
    endif()
    target_include_directories(btdd_jit PRIVATE "${LIBTCC_INCLUDE}")
    target_link_libraries(btdd_jit PRIVATE "${LIBTCC_LIB}")
    target_compile_definitions(btdd_jit PRIVATE BTDD_LIBTCC "BTDD_TCC_LIB_DIR=\"${BTDD_TINYCC_DIR}\"")
    # btdd_apu_check with native code compiled by libtcc (checks the APU tick under native dispatch)
    btdd_collect_sources(btdd_apu_check BTDD_APU_JIT_SOURCES)
    list(FILTER BTDD_APU_JIT_SOURCES EXCLUDE REGEX "/src/runtime/cpu\\.c$")
    add_executable(btdd_apu_check_jit ${BTDD_APU_JIT_SOURCES} recomp/native_cpu.c recomp/native_jit.c recomp/gen.c)
    target_include_directories(btdd_apu_check_jit PRIVATE src recomp "${LIBTCC_INCLUDE}")
    target_link_libraries(btdd_apu_check_jit PRIVATE "${LIBTCC_LIB}")
    target_compile_definitions(btdd_apu_check_jit PRIVATE BTDD_GEN_NO_MAIN "BTDD_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\""
        ${BTDD_CPU_HOOK_DEFS} BTDD_LIBTCC "BTDD_TCC_LIB_DIR=\"${BTDD_TINYCC_DIR}\"")
    if(MSVC)
        target_compile_definitions(btdd_apu_check_jit PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    # libtcc stays a separate DLL (LGPL): put it next to the exe so the loader finds it
    if(EXISTS "${BTDD_TINYCC_DIR}/libtcc.dll")
        add_custom_command(TARGET btdd_jit POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${BTDD_TINYCC_DIR}/libtcc.dll" "$<TARGET_FILE_DIR:btdd_jit>")
    endif()
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

    # M13: UPnP port mapping for hosting online games (miniupnpc, BSD licence)
    set(UPNPC_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(UPNPC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(UPNPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(miniupnpc
        GIT_REPOSITORY https://github.com/miniupnp/miniupnp.git
        GIT_TAG        miniupnpc_2_3_3
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  miniupnpc
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(miniupnpc)

    # M2: the game in a window
    add_executable(btdd_play src/platform/play_main.c src/platform/config.c src/platform/video.c src/platform/gpu.c
                             src/platform/menu.c src/platform/udp.c src/platform/upnp.c)
    target_link_libraries(btdd_play PRIVATE libminiupnpc-static)
    target_compile_definitions(btdd_play PRIVATE MINIUPNP_STATICLIB)
    target_link_libraries(btdd_play PRIVATE btdd_net)
    if(WIN32)
        target_link_libraries(btdd_play PRIVATE ws2_32)
    endif()
    # M10 prototype: offscreen renders of the GPU shader presets (PNG files for comparing display options)
    add_executable(btdd_gfxshot tools/gfxshot/gfxshot_main.c src/platform/gpu.c)
    target_link_libraries(btdd_gfxshot PRIVATE btdd_screen SDL3::SDL3-static)
    btdd_warnings(btdd_gfxshot)
    target_link_libraries(btdd_play PRIVATE btdd_screen SDL3::SDL3-static)
    btdd_warnings(btdd_play)
    # mods (M7) in the settings menu when this tree has them
    if(TARGET btdd_mods)
        target_link_libraries(btdd_play PRIVATE btdd_mods)
        target_compile_definitions(btdd_play PRIVATE BTDD_HAVE_MODS)
    endif()
endif()
