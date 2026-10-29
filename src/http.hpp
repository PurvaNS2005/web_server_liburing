#pragma once
#include <cstddef>
#include <string>
#include <string_view>

namespace zerohttpd {
struct Request {
    std::string method, target, path;
    bool head = false;
    bool keep_alive = false;
};
enum class ParseState { incomplete, complete, error };
struct ParseResult {
    ParseState state = ParseState::incomplete;
    Request request;
    std::size_t consumed = 0;
    int status = 400;
};
ParseResult parse_request(std::string_view input, std::size_t limit);
std::string_view reason(int status);
std::string_view content_type(std::string_view path);
std::string response_header(int status, std::size_t length, std::string_view type,
                            bool keep_alive);
}
