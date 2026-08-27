# SPDX-License-Identifier: BSD-3-Clause
# Desktop GUI presentation layer only (GLFW + Dear ImGui + OpenGL).
# Physics, meshing, and solve live in src/* and src/pipeline — never here.
# Test Lab talks to polymesh_testlab only via docs/dag/interfaces.md files.

include(FetchContent)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
# GLFW picks its own backend per OS; only Linux needs an override here.
# Headless CI and many Linux boxes lack wayland-scanner, and X11 alone is
# enough for the desktop app, so the Linux build pins X11 and drops Wayland
# to avoid a hard configure failure (GitHub Actions). macOS (Cocoa) and
# Windows (Win32) must keep their native backend: an X11 request there names
# a platform whose headers the machine does not have.
if(UNIX AND NOT APPLE)
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.5-docking
)
FetchContent_MakeAvailable(glfw imgui)

add_library(imgui_lib STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui_lib PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui_lib PUBLIC glfw)

find_package(OpenGL REQUIRED)

# Windows system OpenGL is 1.1 only — load 3.x entry points with glad.
# Linux keeps GL_GLEXT_PROTOTYPES (CI has no glad package).
set(POLYMESH_GUI_GL_LIBS OpenGL::GL)
set(POLYMESH_GUI_GL_COMPAT_INCLUDE "")
if(WIN32)
    find_package(glad CONFIG REQUIRED)
    target_compile_definitions(imgui_lib PUBLIC IMGUI_IMPL_OPENGL_LOADER_GLAD)
    target_link_libraries(imgui_lib PUBLIC glad::glad)
    list(APPEND POLYMESH_GUI_GL_LIBS glad::glad)
    message(STATUS "GUI OpenGL: glad loader (Windows)")
elseif(APPLE)
    # macOS has no <GL/gl.h>. Its desktop GL 3.2+ core entry points are
    # declared, with prototypes and no loader, by <OpenGL/gl3.h>, and every
    # symbol viewport.cpp uses is 3.2 core. Map the two Linux spellings onto
    # the Apple framework headers in the build tree instead of threading a
    # third #ifdef through the renderer. file(GENERATE) rewrites only on a
    # content change, so this does not dirty the GUI on every configure.
    set(POLYMESH_GUI_GL_COMPAT_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/gl_compat")
    file(GENERATE OUTPUT "${POLYMESH_GUI_GL_COMPAT_INCLUDE}/GL/gl.h"
         CONTENT "#pragma once\n#include <OpenGL/gl3.h>\n")
    file(GENERATE OUTPUT "${POLYMESH_GUI_GL_COMPAT_INCLUDE}/GL/glext.h"
         CONTENT "#pragma once\n#include <OpenGL/gl3ext.h>\n")
    # Apple deprecated OpenGL in 10.14; the framework still ships and works.
    # Without this every GL call in the build log is a deprecation warning.
    add_compile_definitions(GL_SILENCE_DEPRECATION)
    message(STATUS "GUI OpenGL: OpenGL.framework gl3 core (macOS)")
else()
    message(STATUS "GUI OpenGL: system headers + GLEXT prototypes")
endif()

# ImGui-free campaign file parsers (also linked by tests/testlab_selfcheck).
add_library(gui_testlab_data STATIC
    testlab_data.cpp
)
target_include_directories(gui_testlab_data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gui_testlab_data
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE polymesh_warnings
)

add_executable(polymesh-gui
    main.cpp
    theme.cpp
    widgets.cpp
    viewport.cpp
    cinema.cpp
    live_view.cpp
    gui_settings.cpp
    process_runner.cpp
    testlab_panel.cpp
)
target_include_directories(polymesh-gui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${POLYMESH_GUI_GL_COMPAT_INCLUDE}
)
target_link_libraries(polymesh-gui
    PRIVATE
        polymesh::pipeline
        gui_testlab_data
        imgui_lib
        ${POLYMESH_GUI_GL_LIBS}
        polymesh_warnings
        polymesh_perf
)
target_compile_definitions(polymesh-gui PRIVATE POLYMESH_VERSION="${PROJECT_VERSION}")
if(UNIX AND NOT APPLE)
    set_target_properties(polymesh-gui PROPERTIES
        INSTALL_RPATH "\$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()

install(TARGETS polymesh-gui RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES
    "${CMAKE_SOURCE_DIR}/packaging/linux/io.github.hunter124.PolyMesh.desktop"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install(FILES
    "${CMAKE_SOURCE_DIR}/packaging/linux/io.github.hunter124.PolyMesh.metainfo.xml"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
install(FILES
    "${CMAKE_SOURCE_DIR}/packaging/linux/io.github.hunter124.PolyMesh.svg"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
install(FILES
    "${glfw_SOURCE_DIR}/LICENSE.md"
    DESTINATION "${POLYMESH_INSTALL_DOCDIR}"
    RENAME GLFW-LICENSE.txt)
install(FILES
    "${imgui_SOURCE_DIR}/LICENSE.txt"
    DESTINATION "${POLYMESH_INSTALL_DOCDIR}"
    RENAME Dear-ImGui-LICENSE.txt)

# Advisor activation cinema: the network drawing and `cinema advisor` need the
# inference module. Optional exactly as in apps/cli — a GUI configured with
# POLYMESH_WITH_ADVISOR=OFF still builds, and the cinema panel then says on
# screen that this build carries no inference module rather than drawing an
# empty graph that looks like a loaded one.
if(TARGET polymesh::advisor)
    target_link_libraries(polymesh-gui PRIVATE polymesh::advisor)
    polymesh_stage_onnxruntime(polymesh-gui)
endif()
