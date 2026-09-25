cmake_minimum_required(VERSION 3.25)

project(ReframePlusPlus
        VERSION 2.1.0
        DESCRIPTION "Low-overhead GPU screen recorder with instant replay (NVIDIA + AMD + Intel)"
        LANGUAGES CXX C)

if(NOT WIN32)
    message(FATAL_ERROR "Reframe++ is a Windows-only project (D3D11 / DXGI / WGC / Media Foundation).")
endif()

set(RF_BUILD_NUMBER 1121)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

foreach(cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY${cfg} "${CMAKE_BINARY_DIR}/bin")
endforeach()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ReframeOptions)
include(ReframeWarnings)

add_subdirectory(third_party)
add_subdirectory(src)
add_subdirectory(apps)

if(RF_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT reframe-cli)

# installer
find_program(RF_ISCC ISCC
             PATHS "$ENV{ProgramFiles\(x86\)}/Inno Setup 6"
                   "$ENV{ProgramFiles}/Inno Setup 6"
                   "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6"
             DOC "Inno Setup command line compiler")

if(RF_ISCC)
    add_custom_target(installer
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_SOURCE_DIR}/build/installer"
        COMMAND "${RF_ISCC}"
                "/DBuildDir=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
                "/DAppVersion=${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
                "${CMAKE_SOURCE_DIR}/installer/reframe.iss"
        DEPENDS reframe
        COMMENT "Building the installer with Inno Setup"
        VERBATIM)
    set_target_properties(installer PROPERTIES FOLDER "packaging")
    message(STATUS "  Installer target                       : ON (${RF_ISCC})")
else()
    message(STATUS "  Installer target                       : OFF (Inno Setup not found)")
endif()

message(STATUS "")
message(STATUS "  Reframe++ ${PROJECT_VERSION}")
message(STATUS "  WGC capture (Windows.Graphics.Capture) : ${RF_ENABLE_WGC}")
message(STATUS "  NVENC (NVIDIA Video Codec SDK)         : ${RF_ENABLE_NVENC}")
message(STATUS "  AMF (AMD Advanced Media Framework)     : ${RF_ENABLE_AMF}")
message(STATUS "  Media Foundation fallback encoder      : ${RF_ENABLE_MF}")
message(STATUS "")
