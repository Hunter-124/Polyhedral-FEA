// SPDX-License-Identifier: BSD-3-Clause
#include "bench/reference_case.hpp"

#include <nlohmann/json.hpp>

#include <format>
#include <fstream>

namespace polymesh::bench {

ReferenceCase parse_reference(const std::string& json_text) {
    try {
        const auto j = nlohmann::json::parse(json_text);
        return ReferenceCase{
            .name = j.at("name").get<std::string>(),
            .citation = j.at("citation").get<std::string>(),
            .values = j.at("values").get<std::map<std::string, double>>(),
        };
    } catch (const nlohmann::json::exception& e) {
        throw BenchError(std::format("malformed reference JSON: {}", e.what()));
    }
}

ReferenceCase load_reference(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw BenchError(std::format("cannot open reference case {}", path.string()));
    }
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return parse_reference(text);
}

} // namespace polymesh::bench
