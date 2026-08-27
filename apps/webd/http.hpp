// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace polymesh::webd {

inline constexpr std::size_t kMaxRequestBody = 256U * 1024U * 1024U;

struct HttpRequest {
    std::string method;
    std::string target;
    std::string version;
    std::map<std::string, std::string, std::less<>> headers;
    std::vector<std::uint8_t> body;
    bool head = false;

    [[nodiscard]] std::string_view header(std::string_view name) const;
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

class HttpConnection {
  public:
    explicit HttpConnection(int fd, bool omit_body = false);
    ~HttpConnection();
    HttpConnection(const HttpConnection&) = delete;
    HttpConnection& operator=(const HttpConnection&) = delete;

    bool send_response(const HttpResponse& response);
    bool begin_sse();
    bool send_sse(std::string_view event, std::string_view data);
    bool send_sse_comment(std::string_view comment);
    bool send_file(int status, std::string_view content_type, const std::string& path,
                   std::vector<std::pair<std::string, std::string>> headers = {});
    void set_omit_body(bool omit) { omit_body_ = omit; }

  private:
    int fd_ = -1;
    bool omit_body_ = false;
    bool send_all(const void* data, std::size_t size);
};

using HttpHandler = std::function<void(const HttpRequest&, HttpConnection&)>;

class HttpServer {
  public:
    HttpServer(std::string host, std::uint16_t port, HttpHandler handler);
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void run();
    void stop();

  private:
    std::string host_;
    std::uint16_t port_ = 0;
    HttpHandler handler_;
    std::atomic<bool> stopping_{false};
    int listen_fd_ = -1;

    void serve_connection(int fd) const;
};

[[nodiscard]] HttpResponse json_error(int status, std::string message);
[[nodiscard]] std::string mime_type(std::string_view path);
[[nodiscard]] bool serve_static(const HttpRequest& request, HttpConnection& connection,
                                const std::string& web_root);

} // namespace polymesh::webd
