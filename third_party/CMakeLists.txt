# Vendored third-party code. Kept as sources rather than binaries so the whole
# app still builds from a clean checkout with nothing but Visual Studio.

add_library(rf_imgui STATIC
    nanosvg/nanosvg_impl.c
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_win32.cpp
    imgui/backends/imgui_impl_dx11.cpp
)
add_library(rf::imgui ALIAS rf_imgui)

target_include_directories(rf_imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/nanosvg)

target_compile_definitions(rf_imgui PUBLIC
    IMGUI_DEFINE_MATH_OPERATORS
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    IMGUI_DISABLE_DEMO_WINDOWS
    # The UI is drawn entirely with ImDrawList primitives, so 16-bit indices
    # would overflow on the gallery grid.
    ImDrawIdx=unsigned\ int)

target_link_libraries(rf_imgui PUBLIC d3d11 dxgi dxguid)

set_target_properties(rf_imgui PROPERTIES
    FOLDER "third_party"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Vendored code is not ours to fix warnings in.
target_compile_options(rf_imgui PRIVATE /W0)

# --- MinHook ----------------------------------------------------------------
# Used only by the injected game-capture hook, to detour IDXGISwapChain::Present
# inside the game. MIT licensed; see minhook/LICENSE.txt.
add_library(rf_minhook STATIC
    minhook/src/buffer.c
    minhook/src/hook.c
    minhook/src/trampoline.c
    minhook/src/hde/hde32.c
    minhook/src/hde/hde64.c
)
add_library(rf::minhook ALIAS rf_minhook)

target_include_directories(rf_minhook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/minhook/include)

set_target_properties(rf_minhook PROPERTIES
    FOLDER "third_party"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

target_compile_options(rf_minhook PRIVATE /W0)

add_library(rf_rnnoise STATIC
    rnnoise/src/denoise.c
    rnnoise/src/rnn.c
    rnnoise/src/pitch.c
    rnnoise/src/kiss_fft.c
    rnnoise/src/celt_lpc.c
    rnnoise/src/nnet.c
    rnnoise/src/nnet_default.c
    rnnoise/src/parse_lpcnet_weights.c
    rnnoise/src/rnnoise_tables.c
    rnnoise/src/rnnoise_data_init.c
    rnnoise/src/x86/x86cpu.c
    rnnoise/src/x86/x86_dnn_map.c
    rnnoise/src/x86/nnet_sse4_1.c
    rnnoise/src/x86/nnet_avx2.c
)
add_library(rf::rnnoise ALIAS rf_rnnoise)

target_include_directories(rf_rnnoise PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/rnnoise/include
                                      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/rnnoise/src)
target_compile_definitions(rf_rnnoise PRIVATE
    USE_WEIGHTS_FILE RNN_ENABLE_X86_RTCD CPU_INFO_BY_C OPUS_X86_MAY_HAVE_SSE2 _USE_MATH_DEFINES)
set_source_files_properties(rnnoise/src/x86/nnet_sse4_1.c PROPERTIES COMPILE_DEFINITIONS "__SSE4_1__")
set_source_files_properties(rnnoise/src/x86/nnet_avx2.c PROPERTIES
    COMPILE_OPTIONS "/arch:AVX2" COMPILE_DEFINITIONS "__FMA__")

set_target_properties(rf_rnnoise PROPERTIES
    FOLDER "third_party"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_options(rf_rnnoise PRIVATE /W0)

add_library(rf_speexdsp STATIC
    speexdsp/src/preprocess.c
    speexdsp/src/mdf.c
    speexdsp/src/filterbank.c
    speexdsp/src/fftwrap.c
    speexdsp/src/kiss_fft.c
    speexdsp/src/kiss_fftr.c
)
add_library(rf::speexdsp ALIAS rf_speexdsp)

target_include_directories(rf_speexdsp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/speexdsp/include
                                       PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/speexdsp/src)
target_compile_definitions(rf_speexdsp PRIVATE FLOATING_POINT USE_KISS_FFT EXPORT= _USE_MATH_DEFINES)

set_target_properties(rf_speexdsp PROPERTIES
    FOLDER "third_party"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_options(rf_speexdsp PRIVATE /W0)
