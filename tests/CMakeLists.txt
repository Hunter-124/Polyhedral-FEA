# SPDX-License-Identifier: BSD-3-Clause
add_executable(polymesh_tests
    test_tri_surface.cpp
    test_stl.cpp
    test_step.cpp
    test_poly_mesh.cpp
    test_material.cpp
    test_sizing_field.cpp
    test_reference_case.cpp
    test_backend.cpp
    test_quadrature.cpp
    test_shape.cpp
    test_tier0.cpp
    test_cantilever.cpp
    test_iterative_solve.cpp
    test_mms_convergence.cpp
    test_lame_cylinder.cpp
    test_kirsch_plate.cpp
    test_goodier_cavity.cpp
    test_l_domain.cpp
    test_msh.cpp
    test_gui_pipeline.cpp
    test_product_mesh_tier1.cpp
    test_tet_fill.cpp
    test_features.cpp
    test_indicators.cpp
    test_zz.cpp
    test_vtu.cpp
    test_vem.cpp
    test_quality.cpp
    test_hex_vem_pipeline.cpp
    test_prism.cpp
    test_graded_fill.cpp
    test_pyramid.cpp
    test_transition_fill.cpp
    support/structured_mesh.cpp
    support/mms.cpp
)
# Headless pipeline is tested via libpipeline — no GUI / display required.
target_include_directories(polymesh_tests PRIVATE .)
target_link_libraries(polymesh_tests
    PRIVATE geom mesh adapt fea bench_harness pipeline Catch2::Catch2WithMain polymesh_warnings
)

include(Catch)
# Working directory = repo root so the bench harness can load bench/reference/.
catch_discover_tests(polymesh_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
