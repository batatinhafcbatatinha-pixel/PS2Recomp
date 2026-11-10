cmake_minimum_required(VERSION 3.20)

project(PS2Runtime VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(PS2X_ENABLE_RUNNER_UNITY_BUILD "Build ps2EntryRunner with CMake unity build" ON)
set(PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE 32 CACHE STRING "Unity build batch size for ps2EntryRunner")
option(PS2X_ENABLE_RUNNER_PCH "Precompile the heavy runtime headers for ps2EntryRunner" ON)
option(PS2X_ENABLE_SCCACHE "Use sccache as compiler launcher when available" ON)

option(PS2X_ENABLE_CODWAW_SUPPLEMENTS "Include private Final Fronts code supplements and address hooks (games/codwaw-ff sets it)" OFF)
option(PS2X_REQUIRE_GPU_RASTERIZER "Reject software rasterizer fallback" OFF)
option(PS2X_GPU_COMPUTE_SCANOUT "Decode and composite display scanout on the GPU" OFF)
option(PS2X_GPU_TEXTURE_HANDOFF "Present a shared GL texture without RGBA CPU roundtrip" OFF)
if(PS2X_GPU_TEXTURE_HANDOFF AND NOT PS2X_GPU_COMPUTE_SCANOUT)
    message(FATAL_ERROR "PS2X_GPU_TEXTURE_HANDOFF requires PS2X_GPU_COMPUTE_SCANOUT")
endif()

option(PS2X_ENABLE_RUNTIME_LOGS "Enable PS2 runtime logs" ON)
option(PS2X_ENABLE_AGRESSIVE_LOGS "Enable very verbose/agressive PS2 runtime logs" OFF)
option(PS2X_ENABLE_IOP_RPC_TRACE "Log unhandled IOP/SIF RPC trace suggestions" ON)
option(PS2X_STRICT_RETURN_DIAGNOSTICS "Route generated JR $ra returns through runtime branch diagnostics" OFF)
option(PS2X_SHOW_WINDOWS_CONSOLE "Show a console window for ps2EntryRunner on Windows release builds" ON)
option(PS2X_ENABLE_DEBUG_UI "Build the desktop runtime debug UI" ON)

if(PS2X_ENABLE_SCCACHE)
    find_program(PS2X_SCCACHE_PROGRAM sccache)

    if(PS2X_SCCACHE_PROGRAM)
        if(CMAKE_C_COMPILER)
            set(CMAKE_C_COMPILER_LAUNCHER "${PS2X_SCCACHE_PROGRAM}")
        endif()

        set(CMAKE_CXX_COMPILER_LAUNCHER "${PS2X_SCCACHE_PROGRAM}")
        message(STATUS "Using sccache: ${PS2X_SCCACHE_PROGRAM}")
    else()
        message(STATUS "sccache not found; continuing without compiler launcher")
    endif()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ReleaseMode.cmake")

set(PS2X_IS_VITA OFF)

if((CMAKE_C_COMPILER MATCHES "arm-vita-eabi") OR
    (CMAKE_CXX_COMPILER MATCHES "arm-vita-eabi"))
    set(PS2X_IS_VITA ON)
endif()

set(PS2X_IS_ANDROID OFF)

if(ANDROID)
    set(PS2X_IS_ANDROID ON)
endif()

option(PS2X_VITA_CREATE_PACKAGE "Create Vita eboot/vpk targets" ON)
set(PS2X_VITA_APP_NAME "PS2 Retro X" CACHE STRING "Display name for the Vita bubble")
set(PS2X_VITA_TITLEID "RANJ00001" CACHE STRING "9-character Vita title id")
set(PS2X_VITA_VERSION "01.00" CACHE STRING "Vita app version")
set(PS2X_DEFAULT_BOOT_ELF "" CACHE STRING "Guest ELF path passed directly to main() when argv is unavailable")

if(PS2X_IS_VITA)
    if(NOT DEFINED ENV{VITASDK})
        message(FATAL_ERROR "VITASDK is not defined. Configure inside the VitaSDK environment or pass the Vita toolchain file.")
    endif()

    set(PS2X_VITASDK "$ENV{VITASDK}")

    if(NOT EXISTS "${PS2X_VITASDK}/share/vita.cmake")
        message(FATAL_ERROR "Could not find vita.cmake under ${PS2X_VITASDK}/share.")
    endif()

    include("${PS2X_VITASDK}/share/vita.cmake" REQUIRED)
else()
    include(FetchContent)
    set(FETCHCONTENT_QUIET FALSE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_GAMES OFF CACHE BOOL "" FORCE)

    if(PS2X_IS_ANDROID)
        set(PLATFORM "Android" CACHE STRING "" FORCE)
    endif()

    FetchContent_Declare(
        raylib
        GIT_REPOSITORY "https://github.com/raysan5/raylib.git"
        GIT_TAG "5.5" # we will migrate to 4.2.0 later, trust me it will be better
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(raylib)

    if(PS2X_ENABLE_DEBUG_UI AND NOT PS2X_IS_ANDROID)
        FetchContent_Declare(
            imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG "v1.92.7-docking"
            GIT_SHALLOW TRUE
        )
        FetchContent_GetProperties(imgui)

        if(NOT imgui_POPULATED)
            FetchContent_Populate(imgui)
        endif()

        set(PS2X_IMGUI_SOURCE_DIR "${imgui_SOURCE_DIR}")

        add_library(imgui STATIC
            "${PS2X_IMGUI_SOURCE_DIR}/imgui.cpp"
            "${PS2X_IMGUI_SOURCE_DIR}/imgui_draw.cpp"
            "${PS2X_IMGUI_SOURCE_DIR}/imgui_tables.cpp"
            "${PS2X_IMGUI_SOURCE_DIR}/imgui_widgets.cpp"
            "${PS2X_IMGUI_SOURCE_DIR}/imgui_demo.cpp"
        )
        target_include_directories(imgui PUBLIC "${PS2X_IMGUI_SOURCE_DIR}")

        FetchContent_Declare(
            rlImGui
            GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
            GIT_TAG "Raylib_5_5"
            GIT_SHALLOW TRUE
        )
        FetchContent_GetProperties(rlImGui)

        if(NOT rlimgui_POPULATED)
            FetchContent_Populate(rlImGui)
        endif()

        set(PS2X_RLIMGUI_SOURCE_DIR "${rlimgui_SOURCE_DIR}")

        add_library(rlImGui STATIC
            "${PS2X_RLIMGUI_SOURCE_DIR}/rlImGui.cpp"
        )
        target_include_directories(rlImGui PUBLIC "${PS2X_RLIMGUI_SOURCE_DIR}")
        target_link_libraries(rlImGui PUBLIC raylib imgui)
    endif()
endif()

add_library(ps2_host_backend INTERFACE)

set(PS2X_DEFAULT_BOOT_ELF_DEFINE "")

if(PS2X_DEFAULT_BOOT_ELF)
    set(PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF}")
    string(REPLACE "\\" "\\\\" PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF_DEFINE}")
    string(REPLACE "\"" "\\\"" PS2X_DEFAULT_BOOT_ELF_DEFINE "${PS2X_DEFAULT_BOOT_ELF_DEFINE}")
endif()

if(PS2X_IS_VITA)
    set(PS2X_VITA_PREFIX "${PS2X_VITASDK}/arm-vita-eabi")
    set(PS2X_VITA_EXTRA_INCLUDE_DIRS "" CACHE STRING "Extra include directories for Vita host dependencies")
    set(PS2X_VITA_LIBRARY_DIR "${PS2X_VITA_PREFIX}/lib")
    set(PS2X_VITA_REQUIRED_LIBRARY_NAMES
        raylib
        SDL2
        OpenSLES
        taihen_stub
        SceAppMgr_stub
        SceCtrl_stub
        SceGxm_stub
        SceCommonDialog_stub
        SceLibKernel_stub
        SceAudio_stub
        SceTouch_stub
        SceHid_stub
        SceMotion_stub
        SceSysmodule_stub
        SceIofilemgr_stub
        SceNetCtl_stub
        SceNet_stub
        SceDisplay_stub
        SceAppUtil_stub
        SceAudioIn_stub
        ScePower_stub
        SceProcessmgr_stub
        SceIme_stub
        libIMGEGL_stub_weak
        libgpu_es4_ext_stub_weak
        libGLESv2_stub_weak
        libGL_stub_weak
        libGLESv1_CM_stub_weak
    )

    set(PS2X_VITA_INCLUDE_DIRS
        "${PS2X_VITA_PREFIX}/include"
    )

    foreach(PS2X_VITA_OPTIONAL_INCLUDE_DIR IN ITEMS
        "${PS2X_VITA_PREFIX}/include/raylib"
        "${PS2X_VITA_PREFIX}/include/SDL2")
        if(EXISTS "${PS2X_VITA_OPTIONAL_INCLUDE_DIR}")
            list(APPEND PS2X_VITA_INCLUDE_DIRS "${PS2X_VITA_OPTIONAL_INCLUDE_DIR}")
        endif()
    endforeach()

    if(PS2X_VITA_EXTRA_INCLUDE_DIRS)
        list(APPEND PS2X_VITA_INCLUDE_DIRS ${PS2X_VITA_EXTRA_INCLUDE_DIRS})
    endif()

    if(NOT EXISTS "${PS2X_VITA_PREFIX}/include/SDL2/SDL.h")
        message(FATAL_ERROR
            "Missing SDL2 headers under ${PS2X_VITA_PREFIX}/include/SDL2. "
            "Quenom/raylib-5.5-vita requires SDL2 with PVR support installed into VitaSDK.")
    endif()

    if(NOT EXISTS "${PS2X_VITA_LIBRARY_DIR}")
        message(FATAL_ERROR "Missing Vita library directory: ${PS2X_VITA_LIBRARY_DIR}")
    endif()

    function(ps2x_find_vita_library out_var library_name)
        string(MAKE_C_IDENTIFIER "${library_name}" library_id)
        set(cache_var "PS2X_VITA_LIB_${library_id}")

        find_library(${cache_var}
            NAMES "${library_name}" "${library_name}.a" "lib${library_name}.a"
            PATHS "${PS2X_VITA_LIBRARY_DIR}"
            NO_DEFAULT_PATH
        )

        if(NOT ${cache_var})
            message(FATAL_ERROR
                "Could not find Vita library '${library_name}' under ${PS2X_VITA_LIBRARY_DIR}. "
                "Install Quenom/raylib-5.5-vita and its SDL2/PVR dependencies into VitaSDK first.")
        endif()

        set(${out_var} "${${cache_var}}" PARENT_SCOPE)
    endfunction()

    set(PS2X_VITA_RESOLVED_LIBRARIES "")

    foreach(PS2X_VITA_LIBRARY_NAME IN LISTS PS2X_VITA_REQUIRED_LIBRARY_NAMES)
        ps2x_find_vita_library(PS2X_VITA_LIBRARY_PATH "${PS2X_VITA_LIBRARY_NAME}")
        list(APPEND PS2X_VITA_RESOLVED_LIBRARIES "${PS2X_VITA_LIBRARY_PATH}")
    endforeach()

    target_compile_definitions(ps2_host_backend INTERFACE PLATFORM_VITA)
    target_include_directories(ps2_host_backend INTERFACE ${PS2X_VITA_INCLUDE_DIRS})
    target_link_libraries(ps2_host_backend INTERFACE
        ${PS2X_VITA_RESOLVED_LIBRARIES}
        m
        c
        pthread
    )
else()
    target_link_libraries(ps2_host_backend INTERFACE raylib)

    if(PS2X_IS_ANDROID)
        target_link_libraries(ps2_host_backend INTERFACE log android)
    endif()
endif()

if(PS2X_IS_ANDROID)
    set(PS2X_ENABLE_FFMPEG_DEFAULT OFF)
else()
    set(PS2X_ENABLE_FFMPEG_DEFAULT ON)
endif()

option(PS2X_ENABLE_FFMPEG "Enable FFmpeg-backed MPEG video decoding" ${PS2X_ENABLE_FFMPEG_DEFAULT})

if(NOT PS2X_ENABLE_FFMPEG)
    message(STATUS "FFmpeg disabled; MPEG video decode falls back to stub frames")
    add_library(ffmpeg INTERFACE)
elseif(WIN32)
    include(ExternalProject)

    set(FFMPEG_PREBUILT_TAG "n7.1-241205")
    set(FFMPEG_PREBUILT_URL
        "https://github.com/System233/ffmpeg-msvc-prebuilt/releases/download/${FFMPEG_PREBUILT_TAG}/ffmpeg-${FFMPEG_PREBUILT_TAG}-lgpl-amd64-shared.zip")

    set(PS2X_FFMPEG_ROOT "" CACHE PATH "Existing extracted Windows FFmpeg package (no download)")
    set(FFMPEG_PREFIX_DIR "${CMAKE_BINARY_DIR}/ThirdParty/ffmpeg-prefix")
    if(PS2X_FFMPEG_ROOT)
        set(FFMPEG_SOURCE_DIR "${PS2X_FFMPEG_ROOT}")
        if(NOT EXISTS "${FFMPEG_SOURCE_DIR}/include/libavcodec/avcodec.h" OR
           NOT EXISTS "${FFMPEG_SOURCE_DIR}/bin/avcodec.lib")
            message(FATAL_ERROR "PS2X_FFMPEG_ROOT must contain include/libavcodec/avcodec.h and bin/avcodec.lib")
        endif()
    else()
        set(FFMPEG_SOURCE_DIR "${FFMPEG_PREFIX_DIR}/src/ffmpeg_external")
    endif()
    set(FFMPEG_INCLUDE_DIR "${FFMPEG_SOURCE_DIR}/include")
    set(FFMPEG_LIB_DIR "${FFMPEG_SOURCE_DIR}/bin")
    set(FFMPEG_BIN_DIR "${FFMPEG_SOURCE_DIR}/bin")

    if(PS2X_FFMPEG_ROOT)
        add_custom_target(ffmpeg_external)
    else()
    ExternalProject_Add(ffmpeg_external
        URL "${FFMPEG_PREBUILT_URL}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        PREFIX "${FFMPEG_PREFIX_DIR}"
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS
        "${FFMPEG_LIB_DIR}/avcodec.lib"
        "${FFMPEG_LIB_DIR}/avformat.lib"
        "${FFMPEG_LIB_DIR}/avutil.lib"
        "${FFMPEG_LIB_DIR}/swresample.lib"
        "${FFMPEG_LIB_DIR}/swscale.lib"
    )

    endif()

    # Link against the import libraries shipped with the Windows DLLs.
    add_library(ffmpeg_avcodec STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avcodec PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avcodec.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avcodec ffmpeg_external)

    add_library(ffmpeg_avformat STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avformat PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avformat.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avformat ffmpeg_external)

    add_library(ffmpeg_avutil STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_avutil PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/avutil.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_avutil ffmpeg_external)

    add_library(ffmpeg_swresample STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_swresample PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/swresample.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_swresample ffmpeg_external)

    add_library(ffmpeg_swscale STATIC IMPORTED GLOBAL)
    set_target_properties(ffmpeg_swscale PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/swscale.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
    )
    add_dependencies(ffmpeg_swscale ffmpeg_external)

    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE
        ffmpeg_avcodec
        ffmpeg_avformat
        ffmpeg_avutil
        ffmpeg_swresample
        ffmpeg_swscale
        bcrypt
        secur32
        ws2_32
        user32
        advapi32
        ole32
        shell32
    )

    set(PS2X_FFMPEG_BIN_DIR "${FFMPEG_BIN_DIR}" CACHE INTERNAL "Directory containing FFmpeg runtime DLLs")
    set(PS2X_COPY_FFMPEG_DLLS_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyFfmpegDlls.cmake"
        CACHE INTERNAL
        "Script used to stage FFmpeg runtime DLLs beside executables")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET GLOBAL
        libavcodec
        libavformat
        libavutil
        libswresample
        libswscale
    )

    add_library(ffmpeg INTERFACE)
    target_link_libraries(ffmpeg INTERFACE PkgConfig::FFMPEG)
endif()

function(ps2x_stage_ffmpeg_runtime_dlls target_name)
    if(NOT WIN32)
        return()
    endif()

    if(NOT PS2X_FFMPEG_BIN_DIR OR NOT PS2X_COPY_FFMPEG_DLLS_SCRIPT)
        return()
    endif()

    if(TARGET ffmpeg_external)
        add_dependencies(${target_name} ffmpeg_external)
    endif()

    add_custom_command(
        TARGET ${target_name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
        "-Dsource_dir=${PS2X_FFMPEG_BIN_DIR}"
        "-Ddest_dir=$<TARGET_FILE_DIR:${target_name}>"
        -P "${PS2X_COPY_FFMPEG_DLLS_SCRIPT}"
        VERBATIM
    )
endfunction()

# Keep textual engines/shaders in the build graph even when a compiler's
# localized include-output parser cannot discover their .inl dependencies.
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/vu/ps2_vu1_core.cpp"
    APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/vu/ps2_vu1_native.inl"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/vu/ps2_vu1_step.inl")
set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/gs/gs_gpu_backend.cpp"
    APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/gs/gs_gpu_shaders.inl"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/gs/gs_gpu_scanout.inl")

add_library(ps2_runtime STATIC
    src/lib/game_overrides.cpp
    src/lib/gs/ps2_gif_arbiter.cpp
    src/lib/ps2_audio.cpp
    src/lib/ps2_audio_vag.cpp
    src/lib/gs/ps2_gs_memory.cpp
    src/lib/gs/gs_frontend.cpp
    src/lib/gs/gs_cpu_backend.cpp
    src/lib/gs/gs_gpu_backend.cpp
    src/lib/gs/gs_swizzle.cpp
    src/lib/gs/gs_threaded_backend.cpp
    src/lib/gs/gpu/gs_gl.cpp
    src/lib/gs/gpu/gs_hw_backend.cpp
    src/lib/gs/gpu/gs_texture_store.cpp
    src/lib/ps2_iop_host.cpp
    src/lib/ps2_memory.cpp
    src/lib/ps2_ipu.cpp
    src/lib/ps2_pad.cpp
    src/lib/host_os.cpp
    src/lib/ps2_profiler.cpp
    src/lib/ps2_rom_device.cpp
    src/lib/ps2_vfs.cpp
    src/lib/ps2_runtime.cpp
    src/lib/ps2_hot.cpp
    src/lib/ps2_vif1_interpreter.cpp
    src/lib/vu/ps2_vu1_core.cpp
    src/lib/vu/ps2_vu1_upper.cpp
    src/lib/vu/ps2_vu1_lower.cpp
    src/lib/games_database.cpp
)

# Per-source definitions avoid invalidating the generated game's heavy PCH.
set_source_files_properties(src/lib/ps2_runtime.cpp PROPERTIES
    COMPILE_DEFINITIONS "PS2X_REQUIRE_GPU_RASTERIZER=$<BOOL:${PS2X_REQUIRE_GPU_RASTERIZER}>")
set_source_files_properties(src/lib/gs/gs_gpu_backend.cpp PROPERTIES
    COMPILE_DEFINITIONS "PS2X_GPU_COMPUTE_SCANOUT=$<BOOL:${PS2X_GPU_COMPUTE_SCANOUT}>;PS2X_GPU_TEXTURE_HANDOFF=$<BOOL:${PS2X_GPU_TEXTURE_HANDOFF}>")

if(PS2X_ENABLE_RUNTIME_LOGS)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2_RUNTIME_LOGS=1
    )
endif()

if(PS2X_ENABLE_AGRESSIVE_LOGS)
    target_compile_definitions(ps2_runtime PUBLIC
        AGRESSIVE_LOGS=1
    )
endif()

# Per-function enter/exit tracing in every recompiled function. Costly even with
# the stream disabled, so it is a separate opt-in for tracing sessions.
option(PS2X_FUNCTION_LOG_TRACKER "Trace entry/exit of every recompiled function" OFF)
if(PS2X_FUNCTION_LOG_TRACKER)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2_FUNCTION_LOG_TRACKER=1
    )
endif()

if(PS2X_ENABLE_IOP_RPC_TRACE)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_ENABLE_IOP_RPC_TRACE=1
    )
else()
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_ENABLE_IOP_RPC_TRACE=0
    )
endif()

if(PS2X_STRICT_RETURN_DIAGNOSTICS)
    target_compile_definitions(ps2_runtime PUBLIC
        PS2X_STRICT_RETURN_DIAGNOSTICS=1
    )
endif()

target_compile_definitions(ps2_runtime PRIVATE
    PS2X_HAS_FFMPEG=$<BOOL:${PS2X_ENABLE_FFMPEG}>
)

file(GLOB_RECURSE KERNEL_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel/*.cpp"
)

# Private, independently compiled overlay supplements. Keep them out of the
# runtime translation unit so ordinary runtime edits retain short build times.
if(PS2X_ENABLE_CODWAW_SUPPLEMENTS)
file(GLOB CODWAW_OVERLAY_SRC_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lib/codwaw_*_chunk_*.cpp"
)
endif()
target_compile_definitions(ps2_runtime PRIVATE PS2X_ENABLE_CODWAW_SUPPLEMENTS=$<BOOL:${PS2X_ENABLE_CODWAW_SUPPLEMENTS}>)
if(PS2X_ENABLE_CODWAW_SUPPLEMENTS)
    # The game folder whose project/ holds the supplements generated from your disc (scripts/prepare_game_code.py).
    set(PS2X_CODWAW_PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../codwaw-ff/project" CACHE PATH
        "Final Fronts project folder with the generated supplements")
    target_include_directories(ps2_runtime PRIVATE "${PS2X_CODWAW_PROJECT_DIR}")
endif()

if(MSVC AND CODWAW_OVERLAY_SRC_FILES)
    set_source_files_properties(${CODWAW_OVERLAY_SRC_FILES} PROPERTIES COMPILE_OPTIONS "/Ob1")
endif()

target_sources(ps2_runtime PRIVATE
    ${KERNEL_SRC_FILES}
    ${CODWAW_OVERLAY_SRC_FILES}
)

set(PS2X_RUNNER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/runner" CACHE PATH "Directory holding the generated recompiled sources")

# Optional directory of chunk files that #include the generated sources in groups
# (see make_chunks.py). CMake then sees ~2.5k sources instead of ~80k, which turns a
# ~17 minute generate step (and the per-build glob check) into seconds.
set(PS2X_GAME_CHUNK_DIR "" CACHE PATH "Directory of chunk files that include the generated sources")

if(PS2X_GAME_CHUNK_DIR)
    file(GLOB RUNNER_SRC_FILES CONFIGURE_DEPENDS
        "${PS2X_GAME_CHUNK_DIR}/*.cpp"
    )
else()
    file(GLOB RUNNER_SRC_FILES CONFIGURE_DEPENDS
        "${PS2X_RUNNER_DIR}/*.cpp"
    )
endif()

set(RUNNER_MAIN_CPP "${PS2X_RUNNER_DIR}/main.cpp")
set(ROOT_MAIN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

if(EXISTS "${RUNNER_MAIN_CPP}")
    list(APPEND RUNNER_SRC_FILES "${RUNNER_MAIN_CPP}")
elseif(EXISTS "${ROOT_MAIN_CPP}")
    list(APPEND RUNNER_SRC_FILES "${ROOT_MAIN_CPP}")
endif()

# Generated game code lives in its own static library so runtime edits do not
# recompile ~80k translated functions. PS2X_GAME_LIB_PREBUILT links a saved copy
# of that library instead of depending on the ps2_game_code target.
if(WIN32 AND NOT PS2X_IS_ANDROID AND NOT PS2X_IS_VITA)
    set(PS2X_SPLIT_GAME_CODE_DEFAULT ON)
else()
    set(PS2X_SPLIT_GAME_CODE_DEFAULT OFF)
endif()
option(PS2X_SPLIT_GAME_CODE "Build generated sources as the ps2_game_code static library" ${PS2X_SPLIT_GAME_CODE_DEFAULT})
set(PS2X_GAME_LIB_PREBUILT "" CACHE FILEPATH "Prebuilt ps2_game_code library to link instead of the ps2_game_code target")

if(PS2X_SPLIT_GAME_CODE)
    set(PS2X_GAME_SRC_FILES ${RUNNER_SRC_FILES})
    list(REMOVE_ITEM PS2X_GAME_SRC_FILES "${RUNNER_MAIN_CPP}" "${ROOT_MAIN_CPP}")
    set(PS2X_ENTRY_SRC_FILES ${RUNNER_SRC_FILES})
    list(REMOVE_ITEM PS2X_ENTRY_SRC_FILES ${PS2X_GAME_SRC_FILES})
    list(REMOVE_DUPLICATES PS2X_ENTRY_SRC_FILES)

    add_library(ps2_game_code STATIC ${PS2X_GAME_SRC_FILES})
    add_executable(ps2EntryRunner ${PS2X_ENTRY_SRC_FILES})
    set(PS2X_GAME_CODE_TARGET ps2_game_code)
    # Clang's loop rotation never finishes on one multi-megabyte translated function (Ben 10's FUN_0028b290: 6.9 MB,
    # 22k resumable labels; over 50 minutes at -O1). Without rotation the same -O1 takes 42 s, and -O0 instead made the
    # game far slower. A generated file over 2 MB, or a chunk that includes one, is compiled without loop rotation.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT MSVC)
        set(PS2X_HUGE_GAME_SOURCE_BYTES 2000000 CACHE STRING "Generated sources larger than this are compiled without loop rotation by Clang")
        foreach(src IN LISTS PS2X_GAME_SRC_FILES)
            set(huge FALSE)
            if(EXISTS "${src}")
                file(SIZE "${src}" bytes)
                if(bytes GREATER PS2X_HUGE_GAME_SOURCE_BYTES)
                    set(huge TRUE)
                elseif(PS2X_GAME_CHUNK_DIR)
                    get_filename_component(dir "${src}" DIRECTORY)
                    file(STRINGS "${src}" includes REGEX "^#include \"")
                    foreach(line IN LISTS includes)
                        string(REGEX REPLACE "^#include \"(.*)\"$" "\\1" rel "${line}")
                        if(EXISTS "${dir}/${rel}")
                            file(SIZE "${dir}/${rel}" bytes)
                            if(bytes GREATER PS2X_HUGE_GAME_SOURCE_BYTES)
                                set(huge TRUE)
                                break()
                            endif()
                        endif()
                    endforeach()
                endif()
            endif()
            if(huge)
                set_source_files_properties("${src}" PROPERTIES COMPILE_OPTIONS "-mllvm;-rotation-max-header-size=0")
            endif()
        endforeach()
    endif()
elseif(PS2X_IS_ANDROID)
    add_library(ps2EntryRunner SHARED
        ${RUNNER_SRC_FILES}
    )

    target_link_options(ps2EntryRunner PRIVATE "-Wl,--undefined=ANativeActivity_onCreate")
    set(PS2X_GAME_CODE_TARGET ps2EntryRunner)
else()
    add_executable(ps2EntryRunner
        ${RUNNER_SRC_FILES}
    )
    set(PS2X_GAME_CODE_TARGET ps2EntryRunner)
endif()

# Chunk files are already hand-made unity batches.
if(PS2X_ENABLE_RUNNER_UNITY_BUILD AND NOT PS2X_GAME_CHUNK_DIR)
    set_target_properties(${PS2X_GAME_CODE_TARGET} PROPERTIES
        UNITY_BUILD ON
        UNITY_BUILD_BATCH_SIZE "${PS2X_RUNNER_UNITY_BUILD_BATCH_SIZE}"
    )
endif()

# Optional frozen header snapshot for the generated game code (see
# codwaw-ff/scripts/sync_game_abi.py): runtime-only header edits then do not
# invalidate the precompiled header or the ~2.8k game-code objects.
set(PS2X_GAME_ABI_DIR "" CACHE PATH "Frozen runtime header snapshot used only by the game code")
if(PS2X_GAME_ABI_DIR AND EXISTS "${PS2X_GAME_ABI_DIR}/runtime-include/ps2_runtime.h")
    set(PS2X_GAME_HEADER_ROOT "${PS2X_GAME_ABI_DIR}/runtime-include")
else()
    set(PS2X_GAME_HEADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/include")
endif()

if(PS2X_ENABLE_RUNNER_PCH)
    set(PS2X_RUNNER_PCH_HEADERS
        "${PS2X_GAME_HEADER_ROOT}/ps2_runtime_macros.h"
        "${PS2X_GAME_HEADER_ROOT}/ps2_runtime.h"
    )

    foreach(PS2X_RUNNER_GENERATED_HEADER IN ITEMS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_recompiled_functions.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/ps2_recompiled_stubs.h")
        if(EXISTS "${PS2X_RUNNER_GENERATED_HEADER}")
            list(APPEND PS2X_RUNNER_PCH_HEADERS "${PS2X_RUNNER_GENERATED_HEADER}")
        endif()
    endforeach()

    list(APPEND PS2X_RUNNER_PCH_HEADERS
        "${PS2X_GAME_HEADER_ROOT}/ps2_syscalls.h"
        "${PS2X_GAME_HEADER_ROOT}/ps2_stubs.h"
    )

    target_precompile_headers(${PS2X_GAME_CODE_TARGET} PRIVATE ${PS2X_RUNNER_PCH_HEADERS})
endif()

if(PS2X_SPLIT_GAME_CODE)
    target_link_libraries(ps2_game_code PRIVATE ps2_runtime)
    target_include_directories(ps2_game_code PRIVATE "${PS2X_RUNNER_DIR}")
    if(NOT PS2X_GAME_HEADER_ROOT STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_include_directories(ps2_game_code BEFORE PRIVATE
            "${PS2X_GAME_ABI_DIR}/runtime-include"
            "${PS2X_GAME_ABI_DIR}/kernel"
            "${PS2X_GAME_ABI_DIR}/iop-include"
        )
    endif()
    if(MSVC)
        target_compile_options(ps2_game_code PRIVATE /FS /bigobj)
    endif()
    # The function-table initializer is ~400k lines of plain stores: optimizing it
    # only costs compile time.
    set(PS2X_GAME_REGISTER_SRC "${PS2X_GAME_CHUNK_DIR}/chunk_register.cpp")
    if(PS2X_GAME_CHUNK_DIR AND EXISTS "${PS2X_GAME_REGISTER_SRC}" AND MSVC)
        set_source_files_properties("${PS2X_GAME_REGISTER_SRC}" PROPERTIES
            SKIP_PRECOMPILE_HEADERS ON
            COMPILE_OPTIONS "/Od"
        )
    elseif(PS2X_GAME_CHUNK_DIR AND EXISTS "${PS2X_GAME_REGISTER_SRC}")
        set_source_files_properties("${PS2X_GAME_REGISTER_SRC}" PROPERTIES
            SKIP_PRECOMPILE_HEADERS ON
            COMPILE_OPTIONS "-O0"
        )
    endif()
    if(PS2X_GAME_LIB_PREBUILT)
        target_link_libraries(ps2EntryRunner PRIVATE "${PS2X_GAME_LIB_PREBUILT}")
    else()
        target_link_libraries(ps2EntryRunner PRIVATE ps2_game_code)
    endif()
endif()

# Native VU1 blocks generated by vu1n_gen.py from captured microcode (game data, not published).
# The blocks register themselves at static initialisation, so they are compiled into the executable.
set(PS2X_VU1N_DIR "" CACHE PATH "Folder with generated vu1n_part*.cpp (empty: exact engine only)")
if(PS2X_VU1N_DIR)
    file(GLOB PS2X_VU1N_PARTS CONFIGURE_DEPENDS "${PS2X_VU1N_DIR}/vu1n_part*.cpp")
    if(PS2X_VU1N_PARTS)
        target_sources(ps2EntryRunner PRIVATE ${PS2X_VU1N_PARTS})
        if(MSVC)
            set_source_files_properties(${PS2X_VU1N_PARTS} PROPERTIES COMPILE_OPTIONS "/bigobj")
        endif()
    endif()
endif()

if(PS2X_DEFAULT_BOOT_ELF_DEFINE)
    target_compile_definitions(ps2EntryRunner PRIVATE
        PS2X_DEFAULT_BOOT_ELF="${PS2X_DEFAULT_BOOT_ELF_DEFINE}"
    )
endif()

if(MSVC)
    target_compile_options(ps2EntryRunner PRIVATE /FS)
endif()

target_include_directories(ps2_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/lib/Kernel
)

target_link_libraries(ps2_runtime PUBLIC ps2_host_backend ffmpeg ps2_iop)
# The runtime and generated code use SSE4.1 intrinsics (_mm_extract_epi32/64, blends). MSVC accepts them
# unconditionally; GCC/Clang need the target feature enabled for every consumer of the headers.
if(NOT MSVC AND NOT PS2X_IS_ARM_TARGET AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64|i.86")
    target_compile_options(ps2_runtime PUBLIC -msse4.1)
endif()
if(WIN32)
    # Native GS renderer (gs/gpu): its own WGL context on the GS thread.
    target_link_libraries(ps2_runtime PRIVATE opengl32 gdi32 dbghelp synchronization)   # dbghelp: the sampling profiler (MinGW ignores its #pragma comment)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(OpenGL REQUIRED COMPONENTS OpenGL EGL)
    target_link_libraries(ps2_runtime PRIVATE OpenGL::EGL OpenGL::OpenGL ${CMAKE_DL_LIBS})
endif()
if(NOT WIN32)
    target_link_libraries(ps2_runtime PRIVATE ${CMAKE_DL_LIBS})  # ps2_hot.cpp: dlopen
endif()
target_link_libraries(ps2EntryRunner
    PRIVATE
    ps2_runtime
)
target_include_directories(ps2EntryRunner PRIVATE "${PS2X_RUNNER_DIR}")

if(PS2X_ENABLE_DEBUG_UI AND NOT PS2X_IS_VITA AND NOT PS2X_IS_ANDROID)
    target_sources(ps2EntryRunner PRIVATE
        src/lib/ps2_debug_panel.cpp
    )
    target_link_libraries(ps2EntryRunner PRIVATE rlImGui imgui)
    target_compile_definitions(ps2EntryRunner PRIVATE PS2X_ENABLE_DEBUG_UI=1)
endif()

ps2x_stage_ffmpeg_runtime_dlls(ps2EntryRunner)

if(PS2X_IS_VITA)
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d PARENTAL_LEVEL=1") 
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

    if(PS2X_VITA_CREATE_PACKAGE)
        set(PS2X_VITA_VPK_FILES "")
        # Third-party driver modules (PowerVR SGX via PVR_PSP2, gl4es4vita) staged by vita/setup.sh outside the
        # repository; they are not distributed with PS2Recomp.
        set(PS2X_VITA_MODULE_DIR "$ENV{VITASDK}/arm-vita-eabi/share/ps2x-vita-modules" CACHE PATH
            "Folder with the Vita .suprx modules staged by vita/setup.sh")

        foreach(PS2X_VITA_MODULE IN ITEMS
            libgpu_es4_ext.suprx
            libIMGEGL.suprx
            libGLESv2.suprx
            libpvrPSP2_WSEGL.suprx
            libGL.suprx)
            set(PS2X_VITA_MODULE_PATH "${PS2X_VITA_MODULE_DIR}/${PS2X_VITA_MODULE}")
            if(NOT EXISTS "${PS2X_VITA_MODULE_PATH}" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vita/module/${PS2X_VITA_MODULE}")
                set(PS2X_VITA_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/vita/module/${PS2X_VITA_MODULE}") # older local setups
            endif()

            if(EXISTS "${PS2X_VITA_MODULE_PATH}")
                list(APPEND PS2X_VITA_VPK_FILES FILE "${PS2X_VITA_MODULE_PATH}" "module/${PS2X_VITA_MODULE}")
            else()
                message(WARNING
                    "Missing ${PS2X_VITA_MODULE_PATH}; the vpk will rely on ur0:data/external/ for it")
            endif()
        endforeach()

        vita_create_self(eboot.bin ps2EntryRunner UNSAFE)
        vita_create_vpk(ps2EntryRunner.vpk ${PS2X_VITA_TITLEID} eboot.bin
            VERSION ${PS2X_VITA_VERSION}
            NAME ${PS2X_VITA_APP_NAME}
            ${PS2X_VITA_VPK_FILES}
        )
    endif()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    EnableFastReleaseMode(ps2_runtime)
    EnableFastReleaseMode(ps2EntryRunner)
    if(PS2X_SPLIT_GAME_CODE)
        EnableFastReleaseMode(ps2_game_code)
    endif()

    if(WIN32 AND NOT PS2X_SHOW_WINDOWS_CONSOLE)
        if(MSVC)
            target_link_options(ps2EntryRunner PRIVATE
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/SUBSYSTEM:WINDOWS>
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:/ENTRY:mainCRTStartup>
            )
        elseif(MINGW)
            target_link_options(ps2EntryRunner PRIVATE
                $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-mwindows>
            )
        endif()
    endif()
endif()

# Controller support files next to the executable (both zlib license, downloaded and checked, never committed):
# gamecontrollerdb.txt names the buttons of PlayStation pads, USB adapters and generic controllers, and on Windows
# SDL3.dll reads every pad that SDL knows. Without them the runner still works, with raylib's pads only.
if(NOT PS2X_IS_VITA AND NOT PS2X_IS_ANDROID)
    set(PS2X_PAD_DIR "${CMAKE_BINARY_DIR}/pad-support")
    set(PS2X_PAD_DB "${PS2X_PAD_DIR}/gamecontrollerdb.txt")
    if(NOT EXISTS "${PS2X_PAD_DB}")
        file(DOWNLOAD
            "https://raw.githubusercontent.com/mdqinc/SDL_GameControllerDB/22a98f6339abf4b6ca6cb061443a70e4f7ab252d/gamecontrollerdb.txt"
            "${PS2X_PAD_DB}"
            EXPECTED_HASH SHA256=0f17831b6a9e0e9fc2f52bf8ab1bef1d30ffd307d966c2118e556db345ea7e96
            STATUS PS2X_PAD_DB_STATUS)
        list(GET PS2X_PAD_DB_STATUS 0 PS2X_PAD_DB_CODE)
        if(NOT PS2X_PAD_DB_CODE EQUAL 0)
            message(WARNING "gamecontrollerdb.txt was not downloaded: ${PS2X_PAD_DB_STATUS}")
            file(REMOVE "${PS2X_PAD_DB}")
        endif()
    endif()
    if(EXISTS "${PS2X_PAD_DB}")
        add_custom_command(TARGET ps2EntryRunner POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PS2X_PAD_DB}" "$<TARGET_FILE_DIR:ps2EntryRunner>")
    endif()
    if(WIN32)
        set(PS2X_SDL3_ZIP "${PS2X_PAD_DIR}/SDL3-3.2.26-win32-x64.zip")
        if(NOT EXISTS "${PS2X_PAD_DIR}/SDL3.dll")
            file(DOWNLOAD
                "https://github.com/libsdl-org/SDL/releases/download/release-3.2.26/SDL3-3.2.26-win32-x64.zip"
                "${PS2X_SDL3_ZIP}"
                EXPECTED_HASH SHA256=866bd702f16cc767420ed401e8c5663679833a0de56892f03e4ef3e9c0a5a333
                STATUS PS2X_SDL3_STATUS)
            list(GET PS2X_SDL3_STATUS 0 PS2X_SDL3_CODE)
            if(PS2X_SDL3_CODE EQUAL 0)
                file(ARCHIVE_EXTRACT INPUT "${PS2X_SDL3_ZIP}" DESTINATION "${PS2X_PAD_DIR}" PATTERNS SDL3.dll LICENSE.txt)
            else()
                message(WARNING "SDL3.dll was not downloaded: ${PS2X_SDL3_STATUS}")
            endif()
            file(REMOVE "${PS2X_SDL3_ZIP}")
        endif()
        if(EXISTS "${PS2X_PAD_DIR}/SDL3.dll")
            add_custom_command(TARGET ps2EntryRunner POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PS2X_PAD_DIR}/SDL3.dll" "$<TARGET_FILE_DIR:ps2EntryRunner>"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PS2X_PAD_DIR}/LICENSE.txt" "$<TARGET_FILE_DIR:ps2EntryRunner>/SDL3-LICENSE.txt")
        endif()
    endif()
endif()

# Work around WinAPI vs raylib symbol clash for CloseWindow on x64
if(MSVC)
    # Recompiled guest recursion lives on the host stack, and inlined helpers make its frames large.
    target_link_options(ps2EntryRunner PRIVATE "/STACK:67108864")
    target_link_options(ps2EntryRunner PRIVATE "/FORCE:MULTIPLE")
    # Function address map for the built-in sampling profiler (PS2X_PROFILE=1).
    target_link_options(ps2EntryRunner PRIVATE "/MAP:$<TARGET_FILE_DIR:ps2EntryRunner>/ps2EntryRunner.map")
elseif(MINGW)
    # llvm-mingw: the same 64 MB stack, the same first-definition-wins rule as /FORCE:MULTIPLE
    # (sceSifSearchModuleByName is defined in both Stubs/SIF.cpp and Stubs/Services.cpp), and the map.
    target_link_options(ps2EntryRunner PRIVATE "-Wl,--stack,67108864" "-Wl,--allow-multiple-definition"
                        "-Wl,-Map,$<TARGET_FILE_DIR:ps2EntryRunner>/ps2EntryRunner.map")
endif()

# Vita: Work around for GCC 10's LTO plugin
if(PS2X_IS_VITA)
    target_link_options(ps2EntryRunner PRIVATE "-Wl,--allow-multiple-definition")
    target_sources(ps2EntryRunner PRIVATE src/lib/ps2_vita_runtime.cpp)
    set_source_files_properties(src/lib/ps2_vita_runtime.cpp PROPERTIES
        SKIP_UNITY_BUILD_INCLUSION TRUE
        COMPILE_OPTIONS "-fno-lto"
    )
endif()

if(PS2X_IS_ANDROID)
    target_sources(ps2EntryRunner PRIVATE src/lib/ps2_android_runtime.cpp)
endif()

install(TARGETS ps2_runtime ps2EntryRunner
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

if(WIN32 AND PS2X_ENABLE_FFMPEG)
    install(DIRECTORY "${FFMPEG_BIN_DIR}/"
        DESTINATION bin
        FILES_MATCHING
        PATTERN "*.dll"
    )
endif()

install(DIRECTORY include/
    DESTINATION include
)
