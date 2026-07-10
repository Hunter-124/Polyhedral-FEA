# SPDX-License-Identifier: AGPL-3.0-or-later
add_executable(polymesh main.cpp)
target_link_libraries(polymesh PRIVATE geom mesh adapt fea polymesh_warnings)
