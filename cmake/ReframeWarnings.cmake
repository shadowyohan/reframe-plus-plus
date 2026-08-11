add_library(rf_flags INTERFACE)
add_library(rf::flags ALIAS rf_flags)

target_compile_definitions(rf_flags INTERFACE
    UNICODE _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    WINRT_LEAN_AND_MEAN)

target_compile_options(rf_flags INTERFACE
    /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /MP /EHsc
    /wd4324
    $<$<CONFIG:Release,RelWithDebInfo>:/O2 /Oi /Gy /GS- /fp:fast>)

target_link_options(rf_flags INTERFACE
    $<$<CONFIG:Release,RelWithDebInfo>:/OPT:REF /OPT:ICF /INCREMENTAL:NO>
    /DEBUG)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

function(rf_target_defaults tgt)
    target_link_libraries(${tgt} PUBLIC rf::flags)
    set_target_properties(${tgt} PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endfunction()
