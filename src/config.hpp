#pragma once
#include <cstddef>
#include <string>

namespace zerohttpd {
struct Config {
    std::string bind = "127.0.0.1", root = "public", metrics_file;
    unsigned port = 8000, queue_depth = 256, batch_size = 64, max_connections = 1024;
    unsigned timeout_ms = 10000, header_timeout_ms = 10000, grace_ms = 3000, max_requests = 1000;
    unsigned metrics_interval_ms = 1000;
    std::size_t header_limit = 16384, buffer_size = 65536, cache_bytes = 16 * 1024 * 1024;
    std::size_t cache_file_bytes = 256 * 1024;
    bool access_log = false;
};
Config parse_config(int argc, char** argv);
void print_usage();
}
