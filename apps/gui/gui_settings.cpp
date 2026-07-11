// SPDX-License-Identifier: BSD-3-Clause
#include "gui_settings.hpp"

#include "process_runner.hpp"

namespace polymesh::gui {

std::filesystem::path GuiSettings::resolved_testlab_binary() const {
    const auto found = find_testlab_binary(
        testlab_binary.empty() ? std::filesystem::path{} : std::filesystem::path{testlab_binary});
    if (!found.empty()) {
        return found;
    }
    // Bare name: execvp / CreateProcess will search PATH.
    return testlab_binary.empty() ? std::filesystem::path{"polymesh_testlab"}
                                  : std::filesystem::path{testlab_binary};
}

std::filesystem::path GuiSettings::campaigns_root_path() const {
    return std::filesystem::path{campaigns_root};
}

} // namespace polymesh::gui
