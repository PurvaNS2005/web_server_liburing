#include "config.hpp"
#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace zerohttpd {
void print_usage() {
    std::cout << "ZeroHTTPd: C++17 / io_uring HTTP/1.1 static server\n"
      "Usage: webserver_liburing [options]\n"
      "  --bind ADDRESS              IPv4 bind address (127.0.0.1)\n"
      "  --port N                    TCP port, 0 selects an available port (8000)\n"
      "  --root PATH                 document root (public)\n"
      "  --queue-depth N             ring entries, 8..4096 (256)\n"
      "  --batch-size N              completion batch size, 1..queue-depth (64)\n"
      "  --max-connections N         active connection limit, 1..16384 (1024)\n"
      "  --timeout-ms N              idle/I/O timeout (10000)\n"
      "  --header-timeout-ms N       absolute header assembly deadline (10000)\n"
      "  --grace-ms N                shutdown drain deadline (3000)\n"
      "  --max-requests N            requests per connection (1000)\n"
      "  --header-limit N            maximum header bytes, 256..1048576 (16384)\n"
      "  --buffer-size N             file chunk bytes, 1024..1048576 (65536)\n"
      "  --cache-bytes N             LRU payload budget, 0 disables (16777216)\n"
      "  --cache-file-bytes N        maximum cached file size (262144)\n"
      "  --metrics-file PATH         write JSON-lines interval/final metrics\n"
      "  --metrics-interval-ms N     snapshot interval (1000)\n"
      "  --access-log                log completed requests to stderr\n"
      "  --help                      show this help\n";
}
Config parse_config(int argc, char** argv) {
    Config c;
    for (int i = 1; i < argc; ++i) {
        std::string_view key = argv[i];
        if (key == "--access-log") { c.access_log = true; continue; }
        if (key == "--help") continue; // main handles this before constructing the server.
        if (i + 1 == argc) throw std::runtime_error("missing value for " + std::string(key));
        std::string value = argv[++i];
        if (key == "--root") { c.root = value; continue; }
        if (key == "--bind") { c.bind = value; continue; }
        if (key == "--metrics-file") { c.metrics_file = value; continue; }
        std::size_t n = 0;
        auto parsed = std::from_chars(value.data(), value.data()+value.size(), n);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data()+value.size())
            throw std::runtime_error("invalid number for " + std::string(key));
        auto bounded = [&](std::size_t lo, std::size_t hi) {
            if (n < lo || n > hi) throw std::runtime_error("out of range: " + std::string(key));
            return n;
        };
        if (key == "--port") c.port = bounded(0, 65535);
        else if (key == "--queue-depth") c.queue_depth = bounded(8, 4096);
        else if (key == "--batch-size") c.batch_size = bounded(1, 4096);
        else if (key == "--max-connections") c.max_connections = bounded(1, 16384);
        else if (key == "--timeout-ms") c.timeout_ms = bounded(10, 3600000);
        else if (key == "--header-timeout-ms") c.header_timeout_ms = bounded(10, 3600000);
        else if (key == "--grace-ms") c.grace_ms = bounded(0, 60000);
        else if (key == "--max-requests") c.max_requests = bounded(1, 1000000);
        else if (key == "--header-limit") c.header_limit = bounded(256, 1048576);
        else if (key == "--buffer-size") c.buffer_size = bounded(1024, 1048576);
        else if (key == "--cache-bytes") c.cache_bytes = bounded(0, 1024ULL*1024*1024);
        else if (key == "--cache-file-bytes") c.cache_file_bytes = bounded(0, 16*1024*1024);
        else if (key == "--metrics-interval-ms") c.metrics_interval_ms = bounded(10, 3600000);
        else throw std::runtime_error("unknown option: " + std::string(key));
    }
    if (c.batch_size > c.queue_depth) throw std::runtime_error("batch-size exceeds queue-depth");
    return c;
}
}
