# --- the injected game-capture hook -----------------------------------------
# A standalone DLL: it runs inside somebody else's process, so it links nothing
# but the Windows SDK, MinHook and the static CRT. Anything else would mean the
# game has to be able to find our runtime DLLs, which it cannot.
# Shaders for the in-game overlay, compiled to bytecode headers at build time.
# Doing it here rather than with D3DCompile at runtime keeps d3dcompiler_47.dll
# out of the game's process - the hook should add as little as possible to
# somebody else's address space.
find_program(RF_FXC fxc
             HINTS "$ENV{WindowsSdkVerBinPath}/x64"
             PATHS "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin"
             PATH_SUFFIXES "10.0.26100.0/x64" "10.0.22621.0/x64" "10.0.22000.0/x64"
             DOC "HLSL compiler from the Windows SDK")
if(NOT RF_FXC)
    message(FATAL_ERROR "fxc.exe not found - it ships with the Windows SDK and is needed for the in-game overlay shaders.")
endif()

set(RF_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/rf/capture/hook/OverlayQuad.hlsl")
set(RF_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
foreach(stage vs ps)
    if(stage STREQUAL "vs")
        set(entry VsMain)
        set(profile vs_4_0)
    else()
        set(entry PsMain)
        set(profile ps_4_0)
    endif()
    add_custom_command(
        OUTPUT "${RF_SHADER_DIR}/OverlayQuad_${stage}.h"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RF_SHADER_DIR}"
        COMMAND "${RF_FXC}" /nologo /T ${profile} /E ${entry}
                /Fh "${RF_SHADER_DIR}/OverlayQuad_${stage}.h"
                /Vn kOverlayQuad_${stage} "${RF_HLSL}"
        DEPENDS "${RF_HLSL}"
        COMMENT "Compiling the in-game overlay ${stage} shader"
        VERBATIM)
    list(APPEND RF_SHADER_HEADERS "${RF_SHADER_DIR}/OverlayQuad_${stage}.h")
endforeach()

add_library(reframe-hook SHARED
    rf/capture/hook/HookMain.cpp
    rf/capture/hook/HookGl.cpp
    rf/capture/hook/HookProtocol.h
    ${RF_SHADER_HEADERS}
)
target_link_libraries(reframe-hook PRIVATE rf::minhook d3d11 dxgi dxguid)
target_include_directories(reframe-hook PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/rf/capture/hook
                                                ${RF_SHADER_DIR})
set_target_properties(reframe-hook PROPERTIES
    OUTPUT_NAME "reframe-hook64"
    FOLDER "engine"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- assets compiled into the binary ----------------------------------------
file(GLOB RF_ASSET_FILES
     "${CMAKE_SOURCE_DIR}/assets/icons/*.svg"
     "${CMAKE_SOURCE_DIR}/assets/fonts/*.otf"
     "${CMAKE_SOURCE_DIR}/assets/fonts/*.ttf")

set(RF_EMBEDDED_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/EmbeddedAssets.cpp")

add_custom_command(
    OUTPUT "${RF_EMBEDDED_CPP}"
    COMMAND ${CMAKE_COMMAND}
            -DASSET_DIR=${CMAKE_SOURCE_DIR}/assets
            -DOUTPUT=${RF_EMBEDDED_CPP}
            -P ${CMAKE_SOURCE_DIR}/cmake/GenerateEmbeddedAssets.cmake
    DEPENDS ${RF_ASSET_FILES} "${CMAKE_SOURCE_DIR}/cmake/GenerateEmbeddedAssets.cmake"
    COMMENT "Embedding icons and fonts into the binary"
    VERBATIM)

add_library(reframe_core STATIC
    ${RF_EMBEDDED_CPP}
    rf/core/Lang.cpp
    rf/core/Log.cpp
    rf/core/Media.cpp
    rf/core/Paths.cpp
    rf/core/Status.cpp
    rf/core/Strings.cpp
    rf/core/Time.cpp

    rf/gpu/ColorConverter.cpp
    rf/gpu/CursorPainter.cpp
    rf/gpu/FrameBridge.cpp
    rf/gpu/D3DDevice.cpp
    rf/gpu/GpuInfo.cpp
    rf/gpu/Quirks.cpp
    rf/gpu/VendorApi.cpp

    rf/capture/CaptureFactory.cpp
    rf/capture/DxgiDuplCapture.cpp
    rf/capture/GameCapture.cpp
    rf/capture/GameWatcher.cpp
    rf/capture/WgcCapture.cpp

    rf/encode/AmfEncoder.cpp
    rf/encode/EncoderFactory.cpp
    rf/encode/MfEncoder.cpp
    rf/encode/NvencEncoder.cpp

    rf/audio/AacEncoder.cpp
    rf/audio/AppAudioTracks.cpp
    rf/audio/MaxineSetup.cpp
    rf/audio/NoiseSuppressor.cpp
    rf/audio/WasapiCapture.cpp
    rf/mux/Mp4Muxer.cpp
    rf/mux/TrackNames.cpp
    rf/player/VideoPlayer.cpp
    rf/replay/ReplayBuffer.cpp

    rf/engine/Recorder.cpp
    rf/engine/Settings.cpp

    rf/gallery/Gallery.cpp

    rf/ui/Assets.cpp
    rf/ui/Hud.cpp
    rf/ui/Overlay.cpp
    rf/ui/Screens.cpp
    rf/ui/Widgets.cpp
)

add_library(rf::core ALIAS reframe_core)
rf_target_defaults(reframe_core)

target_include_directories(reframe_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Everything below is linked explicitly rather than via #pragma comment so the
# dependency set is visible in one place.
target_link_libraries(reframe_core PUBLIC
    rf::imgui
    rf::rnnoise rf::speexdsp
    d3d11 dxgi dxguid dcomp
    mfplat mfuuid mf mfreadwrite
    avrt ole32 oleaut32 shlwapi propsys dwmapi)

if(RF_ENABLE_WGC)
    target_compile_definitions(reframe_core PUBLIC RF_HAS_WGC)
    target_link_libraries(reframe_core PUBLIC windowsapp)
    # C++/WinRT needs strict await + RTTI-free exception handling.
    target_compile_options(reframe_core PUBLIC /await:strict)
endif()

if(RF_ENABLE_NVENC)
    target_compile_definitions(reframe_core PUBLIC RF_HAS_NVENC)
    target_include_directories(reframe_core PRIVATE ${RF_NVENC_INCLUDE_DIR})
endif()

if(RF_ENABLE_AMF)
    target_compile_definitions(reframe_core PUBLIC RF_HAS_AMF)
    target_include_directories(reframe_core PRIVATE ${RF_AMF_INCLUDE_DIR})
endif()

# Mirror the folder layout in the Visual Studio solution explorer.
source_group("generated" FILES ${RF_EMBEDDED_CPP})
get_target_property(_rf_sources reframe_core SOURCES)
list(REMOVE_ITEM _rf_sources ${RF_EMBEDDED_CPP})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${_rf_sources})
set_target_properties(reframe_core PROPERTIES FOLDER "engine")

# The hook is a separate file shipped next to the executable, not linked into
# it - but nothing must be able to build the app without also building the hook,
# or game capture silently has no DLL to inject.
add_dependencies(reframe_core reframe-hook)
