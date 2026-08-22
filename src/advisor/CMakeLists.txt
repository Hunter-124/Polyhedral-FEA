# SPDX-License-Identifier: BSD-3-Clause
#
# Learned mesh advisor inference (ADR-0027).
#
# ONNX Runtime comes from the official prebuilt CPU release archive rather than
# the vcpkg port: the port builds ORT from source, which is a multi-hour,
# all-core build on a developer laptop for a dependency we only use as a
# single-threaded CPU inference engine. The archive is pinned by SHA-256, so the
# binary we link is reproducible.

add_library(polymesh_advisor_cost
    src/calibration.cpp
)
add_library(polymesh::advisor_cost ALIAS polymesh_advisor_cost)
target_include_directories(polymesh_advisor_cost PUBLIC include)
target_link_libraries(polymesh_advisor_cost PRIVATE polymesh_warnings)

if(NOT POLYMESH_WITH_ADVISOR)
    return()
endif()

include(FetchContent)

set(POLYMESH_ONNXRUNTIME_VERSION "1.28.0" CACHE STRING "Prebuilt ONNX Runtime version")

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_ort_archive "onnxruntime-win-x64-${POLYMESH_ONNXRUNTIME_VERSION}.zip")
    set(_ort_hash "SHA256=abef733dacbe2f571547a7150b479b5cb9cc0df22f96c24983a42cadb1b4f8bc")
    set(_ort_implib "lib/onnxruntime.lib")
    set(_ort_runtime "lib/onnxruntime.dll")
elseif(UNIX AND NOT APPLE AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_ort_archive "onnxruntime-linux-x64-${POLYMESH_ONNXRUNTIME_VERSION}.tgz")
    set(_ort_hash "")
    set(_ort_implib "")
    set(_ort_runtime "lib/libonnxruntime.so")
else()
    set(_ort_archive "")
endif()

if(NOT _ort_archive)
    message(FATAL_ERROR
        "POLYMESH_WITH_ADVISOR=ON but no prebuilt ONNX Runtime archive is pinned for this "
        "platform. Reconfigure with -DPOLYMESH_WITH_ADVISOR=OFF, or add the archive name and "
        "SHA-256 for this platform in src/advisor/CMakeLists.txt.")
endif()

# An empty URL_HASH value expands to zero arguments and makes URL_HASH swallow
# the next keyword, so the declaration is built conditionally rather than
# passing a hash we do not have.
set(_ort_url
    "https://github.com/microsoft/onnxruntime/releases/download/v${POLYMESH_ONNXRUNTIME_VERSION}/${_ort_archive}")
if(_ort_hash)
    FetchContent_Declare(onnxruntime
        URL "${_ort_url}"
        URL_HASH ${_ort_hash}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
else()
    message(WARNING
        "ONNX Runtime archive ${_ort_archive} is not SHA-256 pinned for this platform; "
        "the downloaded binary is not reproducible. Add its hash in src/advisor/CMakeLists.txt.")
    FetchContent_Declare(onnxruntime
        URL "${_ort_url}"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
endif()
FetchContent_MakeAvailable(onnxruntime)

# The release archive ships headers + import library only, with no CMake
# package config, so the imported target is declared here.
add_library(onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(onnxruntime PROPERTIES
    IMPORTED_LOCATION "${onnxruntime_SOURCE_DIR}/${_ort_runtime}"
    INTERFACE_INCLUDE_DIRECTORIES "${onnxruntime_SOURCE_DIR}/include"
)
if(_ort_implib)
    set_target_properties(onnxruntime PROPERTIES
        IMPORTED_IMPLIB "${onnxruntime_SOURCE_DIR}/${_ort_implib}")
endif()

add_library(polymesh_advisor
    src/advisor.cpp
)
add_library(polymesh::advisor ALIAS polymesh_advisor)
target_include_directories(polymesh_advisor PUBLIC include)
target_link_libraries(polymesh_advisor
    PUBLIC polymesh::pipeline
    PRIVATE onnxruntime nlohmann_json::nlohmann_json polymesh_warnings polymesh_perf
)
# ORT's C++ header is exception-based and warns under -Wconversion on GCC/Clang;
# it is a vendored third-party header, not ours to fix.
target_include_directories(polymesh_advisor SYSTEM PRIVATE "${onnxruntime_SOURCE_DIR}/include")

# Every executable that links advisor needs onnxruntime.dll beside it on
# Windows. The path goes through the cache because a CMake function body reads
# variables from its CALL site, not from this directory scope.
set(POLYMESH_ONNXRUNTIME_RUNTIME "${onnxruntime_SOURCE_DIR}/${_ort_runtime}"
    CACHE INTERNAL "Absolute path to the ONNX Runtime shared library")

function(polymesh_stage_onnxruntime target)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${POLYMESH_ONNXRUNTIME_RUNTIME}"
                    "$<TARGET_FILE_DIR:${target}>"
            COMMENT "Staging onnxruntime.dll next to ${target}"
            VERBATIM)
    endif()
endfunction()
