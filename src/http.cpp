#include "http.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <ctime>
#include <vector>

namespace zerohttpd {
namespace {
std::string lower(std::string_view value) {
    std::string out(value);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
std::string_view trim(std::string_view v) {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
    return v;
}
bool token(std::string_view v) {
    if (v.empty()) return false;
    for (unsigned char c : v)
        if (!std::isalnum(c) && std::string_view("!#$%&'*+-.^_`|~").find(c) == std::string_view::npos)
            return false;
    return true;
}
bool has_token(std::string_view list, std::string_view wanted) {
    while (!list.empty()) {
        auto comma = list.find(',');
        if (trim(list.substr(0, comma)) == wanted) return true;
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
    }
    return false;
}
int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
// Decode once, then reject dot-dot components before any filesystem access.
bool decode_path(std::string_view target, std::string& path) {
    if (target.empty() || target.front() != '/' || target.find('#') != target.npos) return false;
    auto query = target.find('?');
    target = target.substr(0, query);
    std::string decoded;
    for (std::size_t i = 0; i < target.size(); ++i) {
        unsigned char c = target[i];
        if (c == '%') {
            if (i + 2 >= target.size() || hex(target[i+1]) < 0 || hex(target[i+2]) < 0) return false;
            c = static_cast<unsigned char>((hex(target[i+1]) << 4) | hex(target[i+2]));
            i += 2;
        }
        if (c < 32 || c == 127 || c == '\\') return false;
        decoded += static_cast<char>(c);
    }
    // Spaces encoded as %20 are valid file names, but control bytes are not.
    path.clear();
    std::size_t start = 1;
    while (start <= decoded.size()) {
        auto end = decoded.find('/', start);
        auto part = std::string_view(decoded).substr(start, end == decoded.npos ? end : end-start);
        if (part == "..") return false;
        if (!part.empty() && part != ".") {
            if (!path.empty()) path += '/';
            path += part;
        }
        if (end == decoded.npos) break;
        start = end + 1;
    }
    if (decoded.back() == '/' || path.empty()) {
        if (!path.empty()) path += '/';
        path += "index.html";
    }
    return true;
}
}

ParseResult parse_request(std::string_view input, std::size_t limit) {
    ParseResult result;
    auto end = input.find("\r\n\r\n");
    if (end == input.npos) {
        if (input.size() >= limit) { result.state = ParseState::error; result.status = 431; }
        return result;
    }
    result.state = ParseState::error;
    result.consumed = end + 4;
    if (result.consumed > limit) { result.status = 431; return result; }
    auto line_end = input.find("\r\n");
    auto line = input.substr(0, line_end);
    auto a = line.find(' '), b = line.find(' ', a == line.npos ? 0 : a + 1);
    if (a == line.npos || b == line.npos || line.find(' ', b + 1) != line.npos) return result;
    auto method = line.substr(0, a), target = line.substr(a+1, b-a-1), version = line.substr(b+1);
    if (!token(method)) return result;
    if (version != "HTTP/1.1" && version != "HTTP/1.0") { result.status = 505; return result; }
    for (unsigned char c : target) if (c <= 32 || c == 127) return result;
    bool host = false, length_seen = false, close = false, persist = false;
    std::size_t pos = line_end + 2;
    while (pos < end) {
        auto next = input.find("\r\n", pos);
        auto header = input.substr(pos, next-pos);
        auto colon = header.find(':');
        if (colon == header.npos || !token(header.substr(0, colon))) return result;
        auto name = lower(header.substr(0, colon));
        auto value = trim(header.substr(colon+1));
        for (unsigned char c : value) if ((c < 32 && c != '\t') || c == 127) return result;
        if (name == "host") {
            if (host || value.empty() || value.find_first_of(" \t,/@\\") != value.npos) return result;
            host = true;
        } else if (name == "content-length") {
            if (length_seen || value.empty()) return result;
            length_seen = true;
            unsigned long long length = 0;
            auto parsed = std::from_chars(value.data(), value.data()+value.size(), length);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data()+value.size() || length != 0) return result;
        } else if (name == "transfer-encoding") {
            return result; // No request bodies; close so unread bytes cannot become a request.
        } else if (name == "expect") {
            result.status = 417; return result;
        } else if (name == "connection") {
            auto lowered = lower(value);
            close |= has_token(lowered, "close");
            persist |= has_token(lowered, "keep-alive");
        }
        pos = next + 2;
    }
    if (version == "HTTP/1.1" && !host) return result;
    result.request.method = std::string(method);
    result.request.target = std::string(target);
    result.request.head = method == "HEAD";
    if (method != "GET" && method != "HEAD") { result.status = 405; return result; }
    if (!decode_path(target, result.request.path)) { result.status = 400; return result; }
    result.request.keep_alive = !close && (version == "HTTP/1.1" || persist);
    result.state = ParseState::complete;
    return result;
}

std::string_view reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 417: return "Expectation Failed";
    case 431: return "Request Header Fields Too Large";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    default: return "Internal Server Error";
    }
}
std::string_view content_type(std::string_view path) {
    auto dot = path.rfind('.');
    auto ext = dot == path.npos ? "" : lower(path.substr(dot));
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js") return "application/javascript";
    if (ext == ".txt") return "text/plain; charset=utf-8";
    if (ext == ".json") return "application/json";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".ico") return "image/x-icon";
    return "application/octet-stream";
}
std::string response_header(int status, std::size_t length, std::string_view type, bool keep_alive) {
    std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    char date[64];
    std::strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &utc);
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason(status)) + "\r\n";
    out += "Server: zerohttpd/1.0\r\nDate: " + std::string(date) + "\r\n";
    out += "Content-Length: " + std::to_string(length) + "\r\nContent-Type: " + std::string(type);
    out += "\r\nConnection: " + std::string(keep_alive ? "keep-alive" : "close");
    out += "\r\nX-Content-Type-Options: nosniff\r\n";
    if (status == 405) out += "Allow: GET, HEAD\r\n";
    out += "\r\n";
    return out;
}
}
