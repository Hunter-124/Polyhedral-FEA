# SPDX-License-Identifier: BSD-3-Clause
add_executable(polymesh main.cpp)
target_link_libraries(polymesh
    PRIVATE polymesh::geom polymesh::mesh polymesh::adapt polymesh::fea polymesh::pipeline
            polymesh::advisor_cost nlohmann_json::nlohmann_json polymesh_warnings polymesh_perf
)
target_compile_definitions(polymesh PRIVATE POLYMESH_VERSION="${PROJECT_VERSION}")
if(UNIX)
    set_target_properties(polymesh PROPERTIES
        INSTALL_RPATH "\$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()

add_custom_command(TARGET polymesh POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:polymesh>/../share/polymesh/calibration"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/tests/fixtures/parts/plate_hole.step"
            "$<TARGET_FILE_DIR:polymesh>/../share/polymesh/calibration/plate_hole.step"
    COMMENT "Staging the portable-cost calibration mesh"
    VERBATIM)

install(TARGETS polymesh RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if(TARGET polymesh::advisor)
    target_link_libraries(polymesh PRIVATE polymesh::advisor)
    target_compile_definitions(polymesh PRIVATE POLYMESH_WITH_ADVISOR)
    polymesh_stage_onnxruntime(polymesh)
endif()
