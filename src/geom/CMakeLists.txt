# SPDX-License-Identifier: BSD-3-Clause
add_library(polymesh_geom
    src/tri_surface.cpp
    src/stl.cpp
    src/step.cpp
    src/cad_model.cpp
    src/cad_topology.cpp
    src/cad_geometry_features.cpp
    src/features.cpp
    src/signal_fft.cpp
    src/indicators.cpp
)
add_library(polymesh::geom ALIAS polymesh_geom)
target_include_directories(polymesh_geom PUBLIC include)
target_link_libraries(polymesh_geom PUBLIC Eigen3::Eigen PRIVATE polymesh_warnings polymesh_perf)

if(POLYMESH_OPENMP_FOUND)
    # Parallel inward ray casting in estimate_local_thickness (indicators.cpp).
    target_link_libraries(polymesh_geom PUBLIC OpenMP::OpenMP_CXX)
    target_compile_definitions(polymesh_geom PUBLIC POLYMESH_WITH_OPENMP)
endif()

if(POLYMESH_WITH_OCC)
    # Optional B-rep/STEP path (ADR-0001). Requires OpenCASCADE (OCCT).
    # Components cover STEP read (DataExchange) + BRepMesh (ModelingAlgorithms)
    # and the transitive CAF/Visualization deps of modern TKDESTEP.
    find_package(OpenCASCADE
        COMPONENTS
            FoundationClasses
            ModelingData
            ModelingAlgorithms
            Visualization
            ApplicationFramework
            DataExchange
    )
    if(NOT OpenCASCADE_FOUND)
        message(FATAL_ERROR
            "POLYMESH_WITH_OCC=ON but OpenCASCADE was not found.\n"
            "  Install OCCT development packages, or set -DOpenCASCADE_DIR=...\n"
            "  or reconfigure with -DPOLYMESH_WITH_OCC=OFF (default).")
    endif()

    target_compile_definitions(polymesh_geom PUBLIC POLYMESH_WITH_OCC)
    target_include_directories(polymesh_geom SYSTEM PRIVATE ${OpenCASCADE_INCLUDE_DIR})
    # Imported TK* targets carry INTERFACE_INCLUDE_DIRECTORIES and transitive
    # link deps. TKDESTEP is OCCT 7.7+; Ubuntu 24.04 still ships 7.6 TKSTEP*.
    set(_occ_libs
        TKMesh
        TKXSBase
        TKBRep
        TKTopAlgo
        TKMath
        TKernel
        TKG3d
        TKG2d
        TKGeomBase
        TKGeomAlgo
        TKShHealing
    )
    set(_tkdestep_loc "")
    if(TARGET TKDESTEP)
        get_target_property(_tkdestep_loc TKDESTEP IMPORTED_LOCATION)
        if(NOT _tkdestep_loc)
            get_target_property(_tkdestep_loc TKDESTEP IMPORTED_LOCATION_RELEASE)
        endif()
    endif()
    if(_tkdestep_loc)
        list(APPEND _occ_libs TKDESTEP)
        if(TARGET TKDE)
            list(APPEND _occ_libs TKDE)
        endif()
    else()
        list(APPEND _occ_libs TKSTEP TKSTEP209 TKSTEPAttr TKSTEPBase)
        if(TARGET TKXDESTEP)
            list(APPEND _occ_libs TKXDESTEP)
        endif()
    endif()
    target_link_libraries(polymesh_geom PRIVATE ${_occ_libs})
    message(STATUS "OpenCASCADE STEP support enabled (POLYMESH_WITH_OCC)")
endif()
