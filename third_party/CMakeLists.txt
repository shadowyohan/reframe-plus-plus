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
