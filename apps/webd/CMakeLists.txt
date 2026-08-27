# SPDX-License-Identifier: BSD-3-Clause
add_executable(polymesh-webd
    main.cpp
    http.cpp
    jobs.cpp
    encode.cpp
)
target_link_libraries(polymesh-webd
    PRIVATE polymesh::geom polymesh::fea polymesh::pipeline
            nlohmann_json::nlohmann_json polymesh_warnings polymesh_perf
)
target_compile_definitions(polymesh-webd PRIVATE POLYMESH_VERSION="${PROJECT_VERSION}")
if(UNIX)
    set_target_properties(polymesh-webd PROPERTIES
        INSTALL_RPATH "\$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()

install(TARGETS polymesh-webd RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if(TARGET polymesh::advisor)
    target_link_libraries(polymesh-webd PRIVATE polymesh::advisor)
    target_compile_definitions(polymesh-webd PRIVATE POLYMESH_WITH_ADVISOR)
    polymesh_stage_onnxruntime(polymesh-webd)
endif()
