# SPDX-License-Identifier: BSD-3-Clause
# Desktop GUI presentation layer only (GLFW + Dear ImGui + OpenGL).
# Physics, meshing, and solve live in src/* and src/pipeline — never here.
# Test Lab talks to polymesh_testlab only via docs/dag/interfaces.md files.

include(FetchContent)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
# Headless CI and many Linux boxes lack wayland-scanner; X11 is enough for
# the desktop app and avoids a hard configure failure (GitHub Actions).
set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
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
# Linux/macOS keep GL_GLEXT_PROTOTYPES (CI has no glad package).
set(POLYMESH_GUI_GL_LIBS OpenGL::GL)
if(WIN32)
    find_package(glad CONFIG REQUIRED)
    target_compile_definitions(imgui_lib PUBLIC IMGUI_IMPL_OPENGL_LOADER_GLAD)
    target_link_libraries(imgui_lib PUBLIC glad::glad)
    list(APPEND POLYMESH_GUI_GL_LIBS glad::glad)
    message(STATUS "GUI OpenGL: glad loader (Windows)")
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
    gui_settings.cpp
    process_runner.cpp
    testlab_panel.cpp
)
target_include_directories(polymesh-gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(polymesh-gui
    PRIVATE
        pipeline
        gui_testlab_data
        imgui_lib
        ${POLYMESH_GUI_GL_LIBS}
        polymesh_warnings
        polymesh_perf
)
