# SPDX-License-Identifier: BSD-3-Clause
add_executable(polymesh_tests
    test_tri_surface.cpp
    test_stl.cpp
    test_step.cpp
    test_cad_model.cpp
    test_cad_topology.cpp
    test_poly_mesh.cpp
    test_material.cpp
    test_sizing_field.cpp
    test_graded_sizing.cpp
    test_reference_case.cpp
    test_backend.cpp
    test_spmv.cpp
    test_quadrature.cpp
    test_shape.cpp
    test_hierarchical.cpp
    test_hp_assembly.cpp
    test_tier0.cpp
    test_cantilever.cpp
    test_iterative_solve.cpp
    test_mms_convergence.cpp
    test_lame_cylinder.cpp
    test_kirsch_plate.cpp
    test_kirsch_c5_graded.cpp
    test_goodier_cavity.cpp
    test_l_domain.cpp
    test_msh.cpp
    test_gui_pipeline.cpp
    test_product_mesh_tier1.cpp
    test_tet_fill.cpp
    test_local_refine.cpp
    test_features.cpp
    test_indicators.cpp
    test_zz.cpp
    test_vtu.cpp
    test_vem.cpp
    test_fe_vem_assembly.cpp
    test_element_tendency.cpp
    test_refinement_plan.cpp
    test_geometry_fidelity.cpp
    test_quality.cpp
    test_hex_vem_pipeline.cpp
    test_prism.cpp
    test_prism_fill.cpp
    test_graded_fill.cpp
    test_varyhedron_fill.cpp
    test_wall_project.cpp
    test_geogram_clip.cpp
    test_cvt_lloyd.cpp
    test_cvt_sites.cpp
    test_cvt_export.cpp
    test_cvt_poly_mesher.cpp
    test_mesh_conformity.cpp
    test_curved_mesh_quality.cpp
    test_pyramid.cpp
    test_transition_fill.cpp
    test_mixed_fill.cpp
    test_octa_fill.cpp
    test_p_elevate.cpp
    test_hp_driver.cpp
    test_d6_bench_smoke.cpp
    testlab_selfcheck.cpp
    test_probe_util.cpp

    support/structured_mesh.cpp
    support/mms.cpp
)
# Headless pipeline is tested via libpipeline — no GUI / display required.
target_include_directories(polymesh_tests PRIVATE .
    ${CMAKE_SOURCE_DIR}/apps/testlab
)
target_link_libraries(polymesh_tests
    PRIVATE geom mesh adapt fea bench_harness pipeline Catch2::Catch2WithMain polymesh_warnings
)


# gui_testlab_data is built under apps/gui when POLYMESH_WITH_GUI=ON. When GUI
# is off, compile the same unit into the test binary so selfcheck still runs.
if(TARGET gui_testlab_data)
    target_link_libraries(polymesh_tests PRIVATE gui_testlab_data)
else()
    target_sources(polymesh_tests PRIVATE ${CMAKE_SOURCE_DIR}/apps/gui/testlab_data.cpp)
    target_include_directories(polymesh_tests PRIVATE ${CMAKE_SOURCE_DIR}/apps/gui)
    target_link_libraries(polymesh_tests PRIVATE nlohmann_json::nlohmann_json)
endif()

include(Catch)
# Working directory = repo root so the bench harness can load bench/reference/.
catch_discover_tests(polymesh_tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
