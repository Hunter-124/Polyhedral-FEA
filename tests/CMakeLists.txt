# SPDX-License-Identifier: AGPL-3.0-or-later
add_executable(polymesh_tests
    test_tri_surface.cpp
    test_stl.cpp
    test_poly_mesh.cpp
    test_material.cpp
    test_sizing_field.cpp
    test_reference_case.cpp
    test_backend.cpp
    test_quadrature.cpp
    test_shape.cpp
    test_tier0.cpp
    test_cantilever.cpp
    test_mms_convergence.cpp
    test_lame_cylinder.cpp
    support/structured_mesh.cpp
    support/mms.cpp
)
target_include_directories(polymesh_tests PRIVATE .)
target_link_libraries(polymesh_tests
    PRIVATE geom mesh adapt fea bench_harness Catch2::Catch2WithMain polymesh_warnings
)

include(Catch)
# Working directory = repo root so the bench harness can load bench/reference/.
catch_discover_tests(polymesh_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
