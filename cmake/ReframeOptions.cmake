

option(RF_BUILD_TESTS "Build unit tests"                                ON)
option(RF_ENABLE_WGC   "Enable Windows.Graphics.Capture backend"        ON)
option(RF_ENABLE_MF    "Enable Media Foundation encoder backend"        ON)
option(RF_ENABLE_NVENC "Enable NVENC encoder backend (needs NV Codec SDK)" ON)
option(RF_ENABLE_AMF   "Enable AMF encoder backend (needs AMF SDK headers)" ON)

set(RF_NVCODEC_SDK_DIR "" CACHE PATH
    "Path to the NVIDIA Video Codec SDK (folder containing Interface/nvEncodeAPI.h)")
set(RF_AMF_SDK_DIR "" CACHE PATH
    "Path to the AMF SDK (folder containing amf/public/include)")




if(RF_ENABLE_WGC)
    include(CheckIncludeFileCXX)
    set(CMAKE_REQUIRED_FLAGS "/std:c++20 /await:strict /EHsc")
    check_include_file_cxx("winrt/Windows.Graphics.Capture.h" RF_HAVE_CPPWINRT)
    unset(CMAKE_REQUIRED_FLAGS)
    if(NOT RF_HAVE_CPPWINRT)
        message(WARNING "C++/WinRT headers not found in the Windows SDK - disabling WGC backend.")
        set(RF_ENABLE_WGC OFF CACHE BOOL "" FORCE)
    endif()
endif()





if(RF_ENABLE_NVENC)
    find_path(RF_NVENC_INCLUDE_DIR nvEncodeAPI.h
              HINTS "${RF_NVCODEC_SDK_DIR}/Interface" "${RF_NVCODEC_SDK_DIR}"
                    "$ENV{NVCODEC_SDK}/Interface"
              PATHS "${CMAKE_SOURCE_DIR}/third_party/nvcodec/Interface"
              NO_CMAKE_SYSTEM_PATH)
    if(NOT RF_NVENC_INCLUDE_DIR)
        message(STATUS "nvEncodeAPI.h not found - NVENC backend compiles as a stub "
                       "(set RF_NVCODEC_SDK_DIR to enable it).")
        set(RF_ENABLE_NVENC OFF CACHE BOOL "" FORCE)
    endif()
endif()




if(RF_ENABLE_AMF)
    find_path(RF_AMF_INCLUDE_DIR core/Factory.h
              HINTS "${RF_AMF_SDK_DIR}/amf/public/include" "${RF_AMF_SDK_DIR}"
                    "$ENV{AMF_SDK}/amf/public/include"
              PATHS "${CMAKE_SOURCE_DIR}/third_party/amf/amf/public/include"
              NO_CMAKE_SYSTEM_PATH)
    if(NOT RF_AMF_INCLUDE_DIR)
        message(STATUS "AMF headers not found - AMF backend compiles as a stub "
                       "(set RF_AMF_SDK_DIR to enable it).")
        set(RF_ENABLE_AMF OFF CACHE BOOL "" FORCE)
    endif()
endif()
