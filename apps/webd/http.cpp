// SPDX-License-Identifier: BSD-3-Clause
#include "http.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace polymesh::webd {
namespace {

constexpr std::size_t kMaxHeaderBytes = 64U * 1024U;

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

std::string_view reason_phrase(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 202:
        return "Accepted";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 409:
        return "Conflict";
    case 413:
        return "Payload Too Large";
    case 500:
        return "Internal Server Error";
    default:
        return "Error";
    }
}

std::string strip_query(std::string_view target) {
    const auto query = target.find('?');
    return std::string(target.substr(0, query));
}

/// Percent-decodes the path portion of a request target, leaving the query
/// string byte-for-byte. Every route and the static handler read the decoded
/// path, so an id or filename carrying a reserved character ('+' in
/// `plate+hole.step`, a space, a '#') survives the round trip a browser's
/// `encodeURIComponent` puts it through. Returns false on a malformed escape
/// or an encoded NUL, which are rejected as a bad request rather than
/// silently repaired.
bool decode_target_path(std::string& target) {
    const auto query = target.find('?');
    const std::string_view path(target.data(),
                                query == std::string::npos ? target.size() : query);
    std::string decoded;
    decoded.reserve(path.size());
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] != '%') {
            decoded.push_back(path[i]);
            continue;
        }
        if (i + 2 >= path.size()) {
            return false;
        }
        const auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f') {
                return c - 'a' + 10;
            }
            if (c >= 'A' && c <= 'F') {
                return c - 'A' + 10;
            }
            return -1;
        };
        const int hi = nibble(path[i + 1]);
        const int lo = nibble(path[i + 2]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        const int value = hi * 16 + lo;
        if (value == 0) {
            return false;
        }
        decoded.push_back(static_cast<char>(value));
        i += 2;
    }
    if (query != std::string::npos) {
        decoded.append(target, query, std::string::npos);
    }
    target = std::move(decoded);
    return true;
}

bool path_within(const std::filesystem::path& child, const std::filesystem::path& root) {
    auto child_it = child.begin();
    auto root_it = root.begin();
    for (; root_it != root.end(); ++root_it, ++child_it) {
        if (child_it == child.end() || *child_it != *root_it) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string_view HttpRequest::header(std::string_view name) const {
    const auto it = headers.find(lower(name));
    return it == headers.end() ? std::string_view{} : std::string_view(it->second);
}

HttpConnection::HttpConnection(int fd, bool omit_body) : fd_(fd), omit_body_(omit_body) {}

HttpConnection::~HttpConnection() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

bool HttpConnection::send_all(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t sent = 0;
    while (sent < size) {
        const ssize_t result = ::send(fd_, bytes + sent, size - sent, MSG_NOSIGNAL);
        if (result > 0) {
            sent += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

bool HttpConnection::send_response(const HttpResponse& response) {
    std::string head = "HTTP/1.1 " + std::to_string(response.status) + " " +
                       std::string(reason_phrase(response.status)) + "\r\n";
    head += "Content-Type: " + response.content_type + "\r\n";
    head += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    head += "Connection: close\r\n";
    for (const auto& [name, value] : response.headers) {
        head += name + ": " + value + "\r\n";
    }
    head += "\r\n";
    if (!send_all(head.data(), head.size())) {
        return false;
    }
    return omit_body_ || response.body.empty() ||
           send_all(response.body.data(), response.body.size());
}

bool HttpConnection::begin_sse() {
    static constexpr std::string_view kHeader = "HTTP/1.1 200 OK\r\n"
                                                "Content-Type: text/event-stream\r\n"
                                                "Cache-Control: no-cache\r\n"
                                                "X-Accel-Buffering: no\r\n"
                                                "Connection: close\r\n\r\n";
    return send_all(kHeader.data(), kHeader.size());
}

bool HttpConnection::send_sse(std::string_view event, std::string_view data) {
    if (omit_body_) {
        return true;
    }
    std::string message;
    message.reserve(event.size() + data.size() + 16);
    message += "event: ";
    message += event;
    message += "\ndata: ";
    message += data;
    message += "\n\n";
    return send_all(message.data(), message.size());
}

bool HttpConnection::send_sse_comment(std::string_view comment) {
    if (omit_body_) {
        return true;
    }
    std::string message = ": ";
    message += comment;
    message += "\n\n";
    return send_all(message.data(), message.size());
}

bool HttpConnection::send_file(int status, std::string_view content_type,
                               const std::string& path,
                               std::vector<std::pair<std::string, std::string>> headers) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
        return send_response(json_error(404, "file not found"));
    }
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " +
                       std::string(reason_phrase(status)) + "\r\n";
    head += "Content-Type: ";
    head += content_type;
    head += "\r\nContent-Length: " + std::to_string(size) + "\r\nConnection: close\r\n";
    for (const auto& [name, value] : headers) {
        head += name + ": " + value + "\r\n";
    }
    head += "\r\n";
    if (!send_all(head.data(), head.size())) {
        return false;
    }
    if (omit_body_) {
        return true;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    std::array<char, 64U * 1024U> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 && !send_all(buffer.data(), static_cast<std::size_t>(count))) {
            return false;
        }
    }
    return true;
}

HttpServer::HttpServer(std::string host, std::uint16_t port, HttpHandler handler)
    : host_(std::move(host)), port_(port), handler_(std::move(handler)) {}

HttpServer::~HttpServer() { stop(); }

void HttpServer::run() {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* addresses = nullptr;
    const std::string port_text = std::to_string(port_);
    const int lookup = ::getaddrinfo(host_.c_str(), port_text.c_str(), &hints, &addresses);
    if (lookup != 0) {
        throw std::runtime_error("cannot resolve listen address: " +
                                 std::string(::gai_strerror(lookup)));
    }
    for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
        const int candidate =
            ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate < 0) {
            continue;
        }
        const int one = 1;
        (void)::setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (::bind(candidate, address->ai_addr, address->ai_addrlen) == 0 &&
            ::listen(candidate, SOMAXCONN) == 0) {
            listen_fd_ = candidate;
            break;
        }
        ::close(candidate);
    }
    ::freeaddrinfo(addresses);
    if (listen_fd_ < 0) {
        throw std::runtime_error("cannot bind " + host_ + ":" + port_text + ": " +
                                 std::strerror(errno));
    }
    std::printf("polymesh-webd listening on %s:%u\n", host_.c_str(),
                static_cast<unsigned int>(port_));
    std::fflush(stdout);

    while (!stopping_.load(std::memory_order_relaxed)) {
        const int client = ::accept(listen_fd_, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (stopping_.load(std::memory_order_relaxed)) {
                break;
            }
            continue;
        }
        std::thread([this, client] { serve_connection(client); }).detach();
    }
}

void HttpServer::stop() {
    stopping_.store(true, std::memory_order_relaxed);
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

void HttpServer::serve_connection(int fd) const {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(8192);
    std::array<std::uint8_t, 8192> buffer{};
    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos && bytes.size() <= kMaxHeaderBytes) {
        const ssize_t count = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (count <= 0) {
            ::close(fd);
            return;
        }
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + count);
        const std::string_view view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        header_end = view.find("\r\n\r\n");
    }
    HttpConnection connection(fd);
    if (header_end == std::string::npos || header_end > kMaxHeaderBytes) {
        connection.send_response(
            json_error(400, "request headers are too large or incomplete"));
        return;
    }

    HttpRequest request;
    const std::string_view header_text(reinterpret_cast<const char*>(bytes.data()),
                                       header_end);
    const auto first_line_end = header_text.find("\r\n");
    if (first_line_end == std::string_view::npos) {
        connection.send_response(json_error(400, "invalid HTTP request line"));
        return;
    }
    const std::string_view request_line = header_text.substr(0, first_line_end);
    const auto first_space = request_line.find(' ');
    const auto second_space = first_space == std::string_view::npos
                                  ? std::string_view::npos
                                  : request_line.find(' ', first_space + 1);
    if (first_space == std::string_view::npos || second_space == std::string_view::npos) {
        connection.send_response(json_error(400, "invalid HTTP request line"));
        return;
    }
    request.method = std::string(request_line.substr(0, first_space));
    request.target =
        std::string(request_line.substr(first_space + 1, second_space - first_space - 1));
    if (!decode_target_path(request.target)) {
        connection.send_response(
            json_error(400, "malformed percent-escape in request target"));
        return;
    }
    request.version = std::string(request_line.substr(second_space + 1));
    if (request.version != "HTTP/1.1" && request.version != "HTTP/1.0") {
        connection.send_response(json_error(400, "unsupported HTTP version"));
        return;
    }
    std::size_t cursor = first_line_end + 2;
    while (cursor < header_text.size()) {
        const auto end = header_text.find("\r\n", cursor);
        const auto line = header_text.substr(cursor, end == std::string_view::npos
                                                         ? header_text.size() - cursor
                                                         : end - cursor);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            connection.send_response(json_error(400, "invalid HTTP header"));
            return;
        }
        request.headers[lower(line.substr(0, colon))] = trim(line.substr(colon + 1));
        if (end == std::string_view::npos) {
            break;
        }
        cursor = end + 2;
    }

    std::size_t content_length = 0;
    if (const auto text = request.header("content-length"); !text.empty()) {
        const auto [end, error] =
            std::from_chars(text.data(), text.data() + text.size(), content_length);
        if (error != std::errc{} || end != text.data() + text.size()) {
            connection.send_response(json_error(400, "invalid Content-Length"));
            return;
        }
    }
    if (content_length > kMaxRequestBody) {
        connection.send_response(json_error(413, "request body exceeds the 256 MiB limit"));
        return;
    }
    const std::size_t body_start = header_end + 4;
    request.body.insert(request.body.end(),
                        bytes.begin() + static_cast<std::ptrdiff_t>(body_start), bytes.end());
    while (request.body.size() < content_length) {
        const std::size_t remaining = content_length - request.body.size();
        const ssize_t count = ::recv(fd, buffer.data(), std::min(buffer.size(), remaining), 0);
        if (count <= 0) {
            connection.send_response(
                json_error(400, "request body ended before Content-Length"));
            return;
        }
        request.body.insert(request.body.end(), buffer.begin(), buffer.begin() + count);
    }
    if (request.body.size() > content_length) {
        request.body.resize(content_length);
    }
    if (request.method == "HEAD") {
        request.head = true;
        request.method = "GET";
    }
    connection.set_omit_body(request.head);
    try {
        handler_(request, connection);
    } catch (const std::exception& error) {
        connection.send_response(json_error(500, error.what()));
    } catch (...) {
        connection.send_response(json_error(500, "unhandled server error"));
    }
}

HttpResponse json_error(int status, std::string message) {
    HttpResponse response;
    response.status = status;
    std::string escaped;
    escaped.reserve(message.size());
    for (const char c : message) {
        switch (c) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20U) {
                static constexpr char kHex[] = "0123456789abcdef";
                escaped += "\\u00";
                escaped += kHex[(static_cast<unsigned char>(c) >> 4U) & 0x0fU];
                escaped += kHex[static_cast<unsigned char>(c) & 0x0fU];
            } else {
                escaped += c;
            }
            break;
        }
    }
    response.body = "{\"ok\":false,\"error\":\"" + escaped + "\"}";
    return response;
}

std::string mime_type(std::string_view path) {
    const std::string extension = lower(std::filesystem::path(path).extension().string());
    if (extension == ".html") {
        return "text/html; charset=utf-8";
    }
    if (extension == ".css") {
        return "text/css; charset=utf-8";
    }
    if (extension == ".js") {
        return "text/javascript; charset=utf-8";
    }
    if (extension == ".svg") {
        return "image/svg+xml";
    }
    if (extension == ".png") {
        return "image/png";
    }
    if (extension == ".ttf") {
        return "font/ttf";
    }
    if (extension == ".woff2") {
        return "font/woff2";
    }
    if (extension == ".json") {
        return "application/json; charset=utf-8";
    }
    return "application/octet-stream";
}

bool serve_static(const HttpRequest& request, HttpConnection& connection,
                  const std::string& web_root) {
    if (request.method != "GET") {
        return false;
    }
    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(web_root, error);
    if (error || !std::filesystem::is_directory(root)) {
        connection.send_response(json_error(404, "web root is unavailable"));
        return true;
    }
    std::string target = strip_query(request.target);
    if (target.empty() || target == "/") {
        target = "/index.html";
    }
    while (!target.empty() && target.front() == '/') {
        target.erase(target.begin());
    }
    auto candidate = std::filesystem::weakly_canonical(root / target, error);
    if (error || !path_within(candidate, root) ||
        !std::filesystem::is_regular_file(candidate)) {
        error.clear();
        candidate = std::filesystem::weakly_canonical(root / "index.html", error);
    }
    if (error || !path_within(candidate, root) ||
        !std::filesystem::is_regular_file(candidate)) {
        connection.send_response(json_error(404, "static file not found"));
        return true;
    }
    connection.send_file(200, mime_type(candidate.string()), candidate.string());
    return true;
}

} // namespace polymesh::webd
