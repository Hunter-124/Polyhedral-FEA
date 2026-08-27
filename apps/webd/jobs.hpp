// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "http.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace polymesh::webd {

struct ServerOptions {
    std::string examples_dir;
    std::string advisor_dir;
    double max_mem_gb = 0.0;
    std::size_t max_elems = 0;
    std::size_t max_dof = 0;
};

class JobService {
  public:
    explicit JobService(ServerOptions options);
    ~JobService();
    JobService(const JobService&) = delete;
    JobService& operator=(const JobService&) = delete;

    [[nodiscard]] bool advisor_available() const;
    bool handle(const HttpRequest& request, HttpConnection& connection);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace polymesh::webd
