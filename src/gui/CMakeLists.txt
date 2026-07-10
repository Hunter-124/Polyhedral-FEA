# SPDX-License-Identifier: AGPL-3.0-or-later
# Desktop GUI: GLFW + Dear ImGui (docking) + OpenGL — the same stack and
# theme family as the owner's AI-CAD application (ADR-0006).

include(FetchContent)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.5-docking
)
FetchContent_MakeAvailable(glfw imgui)

# ImGui has no CMake build; compile it as a third-party static lib
# (deliberately without our warning flags).
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

add_executable(polymesh-gui
    main.cpp
    theme.cpp
    widgets.cpp
    viewport.cpp
    scene.cpp
)
target_link_libraries(polymesh-gui
    PRIVATE geom mesh adapt fea imgui_lib OpenGL::GL polymesh_warnings
)
