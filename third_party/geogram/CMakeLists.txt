# SPDX-License-Identifier: BSD-3-Clause
# Vendored Geogram PSMs (Bruno Lévy / Inria) — see NOTICE + README.polymesh.md.
# Upstream sources keep their original copyright headers (BSD-3).

cmake_minimum_required(VERSION 3.24)

add_library(polymesh_geogram STATIC
    delaunay/Delaunay_psm.cpp
)
add_library(polymesh::geogram ALIAS polymesh_geogram)

target_include_directories(polymesh_geogram
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/delaunay
)

# Static link: GEOGRAM_API empty; avoid dllimport decorations.
target_compile_definitions(polymesh_geogram
    PUBLIC
        GEO_STATIC_LIBS
        POLYMESH_WITH_GEOGRAM=1
)

# Upstream recommends exact-rounding flags for predicates; apply only here.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(polymesh_geogram PRIVATE
        -frounding-math
        -ffp-contract=off
        -Wno-all
        -Wno-extra
        -Wno-pedantic
        -Wno-conversion
        -Wno-unused-parameter
        -Wno-sign-conversion
    )
endif()

# Do NOT attach polymesh_warnings (-Werror) to third_party amalgams.
set_target_properties(polymesh_geogram PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_EXTENSIONS OFF
)

if(UNIX AND NOT APPLE)
    # process_* / dlopen helpers inside the PSM may need libdl.
    target_link_libraries(polymesh_geogram PUBLIC ${CMAKE_DL_LIBS})
endif()

find_package(Threads)
if(Threads_FOUND)
    target_link_libraries(polymesh_geogram PUBLIC Threads::Threads)
endif()

# Optional lighter target: predicates-only (not linked into mesh by default).
add_library(polymesh_geogram_predicates STATIC
    predicates/Predicates_psm.cpp
)
target_include_directories(polymesh_geogram_predicates
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/predicates
)
target_compile_definitions(polymesh_geogram_predicates
    PUBLIC
        GEO_STATIC_LIBS
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(polymesh_geogram_predicates PRIVATE
        -frounding-math
        -ffp-contract=off
        -Wno-all
        -Wno-extra
        -Wno-pedantic
        -Wno-conversion
    )
endif()
set_target_properties(polymesh_geogram_predicates PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_EXTENSIONS OFF
)
