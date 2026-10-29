#include "http.hpp"
#include "cache.hpp"
#include "config.hpp"
#include <iostream>
#include <random>
#include <stdexcept>

using namespace zerohttpd;
static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static ParseResult parse(std::string_view input) { return parse_request(input, 16384); }
static void http_tests() {
    std::string request = "GET /folder/file.txt?q=1 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    for (std::size_t n = 0; n < request.size(); ++n)
        check(parse(std::string_view(request).substr(0,n)).state == ParseState::incomplete, "fragmented header");
    auto good = parse(request + request);
    check(good.state == ParseState::complete && good.consumed == request.size(), "pipeline framing");
    check(good.request.path == "folder/file.txt" && good.request.keep_alive, "query and persistence");
    check(parse("HEAD / HTTP/1.0\r\n\r\n").request.head, "HEAD detection");
    check(!parse("GET / HTTP/1.0\r\n\r\n").request.keep_alive, "HTTP/1.0 default close");
    check(parse("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n").request.keep_alive, "HTTP/1.0 opt in");
    check(!parse("GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive, close\r\n\r\n").request.keep_alive, "close wins");
    check(parse("GET /space%20name.txt HTTP/1.1\r\nHost: x\r\n\r\n").request.path == "space name.txt", "URL decoding");
    check(parse("GET / HTTP/2.0\r\nHost: x\r\n\r\n").status == 505, "version rejection");
    check(parse("POST / HTTP/1.1\r\nHost: x\r\n\r\n").status == 405, "method rejection");
    for (const char* bad : {
        "\r\n\r\n", "GET / HTTP/1.1\r\n\r\n", "GET / HTTP/1.1\r\nHost: x\r\nHost: y\r\n\r\n",
        "GET / HTTP/1.1\r\nHost : x\r\n\r\n", "GET / HTTP/1.1\r\nHost: x\r\n folded\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\na",
        "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n",
        "GET /../secret HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET /%2e%2e/secret HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET /a%2f..%2f..%2fsecret HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET /%00 HTTP/1.1\r\nHost: x\r\n\r\n", "GET /%zz HTTP/1.1\r\nHost: x\r\n\r\n",
        "GET http://host/ HTTP/1.1\r\nHost: x\r\n\r\n", "GET  / HTTP/1.1\r\nHost: x\r\n\r\n"
    }) check(parse(bad).state == ParseState::error, bad);
    check(parse_request(std::string(256,'x'),256).status == 431, "incomplete size limit");
    check(parse_request(request,32).status == 431, "complete size limit");
    check(content_type("THING.PNG") == "image/png", "case-insensitive MIME");
    check(content_type("unknown.xyz") == "application/octet-stream", "MIME fallback");
    std::mt19937 random(20261028);
    for (int i = 0; i < 20000; ++i) {
        std::string bytes(random()%512, '\0');
        for (auto& c : bytes) c = static_cast<char>(random()%256);
        if (i%2) bytes += "\r\n\r\n";
        auto result = parse_request(bytes,256);
        check(result.consumed <= bytes.size(), "parser consumed outside input");
    }
}
static void cache_tests() {
    FileCache cache(6);
    FileVersion one{}; one.size = 3; one.inode = 1;
    FileVersion two = one; two.inode = 2;
    auto data = std::make_shared<Bytes>(3,'a');
    cache.put("a",one,data); cache.put("b",one,data);
    check(cache.used() == 6, "cache accounting");
    auto held = cache.get("a",one);
    cache.put("c",one,data);
    check(!cache.get("b",one) && cache.get("a",one), "LRU eviction order");
    check(!cache.get("a",two), "changed inode invalidates cache");
    check(held && (*held)[0] == 'a', "in-flight body survives eviction");
    cache.put("big",one,std::make_shared<Bytes>(7,'b'));
    cache.put("empty",one,std::make_shared<Bytes>());
    check(cache.used() <= 6 && !cache.get("empty",one), "bounded cache");
    FileCache disabled(0); disabled.put("a",one,data);
    check(!disabled.get("a",one), "disabled cache");
}
int main() {
    try { http_tests(); cache_tests(); std::cout << "HTTP/parser/cache tests passed\n"; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
