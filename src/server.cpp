#include "cache.hpp"
#include "config.hpp"
#include "http.hpp"
#include <liburing.h>
#include <linux/openat2.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace zerohttpd {
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
static volatile std::sig_atomic_t interrupted = 0;
static void on_signal(int) { interrupted = 1; }

class Fd {
    int value_ = -1;
public:
    explicit Fd(int fd = -1) : value_(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return value_; }
    void reset(int fd = -1) { if (value_ >= 0) ::close(value_); value_ = fd; }
};
struct Ring {
    io_uring value{};
    bool ready = false;
    void close() { if (ready) { io_uring_queue_exit(&value); ready = false; } }
    ~Ring() { close(); }
};
struct Metrics {
    std::uint64_t accepted = 0, requests = 0, completed = 0, errors = 0, io_errors = 0;
    std::uint64_t bytes = 0, hits = 0, misses = 0, timeouts = 0, submits = 0, submitted = 0;
    std::uint64_t file_reads = 0, buffers_allocated = 0, buffers_reused = 0;
    std::uint64_t partial_sends = 0;
    std::array<std::uint64_t, 256> latency{};
    void observe(double microseconds) {
        auto slot = static_cast<std::size_t>(std::max(0.0, std::ceil(std::log(std::max(1.0, microseconds)) / std::log(1.1))));
        ++latency[std::min(slot, latency.size()-1)];
    }
    double percentile(double p) const {
        if (!completed) return 0;
        auto wanted = static_cast<std::uint64_t>(std::ceil(completed * p));
        std::uint64_t count = 0;
        for (std::size_t i = 0; i < latency.size(); ++i) {
            count += latency[i];
            if (count >= wanted) return std::pow(1.1, static_cast<double>(i)) / 1000;
        }
        return 0;
    }
};
enum class Kind { accept, receive, open, stat, read, send, verify, cancel };
struct Connection {
    Fd socket, file;
    bool closing = false, responding = false, keep_alive = false, head = false;
    std::uint64_t pending = 0;
    unsigned requests = 0;
    Time activity = Clock::now(), header_start = activity, started = activity;
    std::array<char, 8192> receive{};
    std::string input, path, target, method, output;
    std::size_t output_sent = 0, chunk_size = 0, chunk_sent = 0;
    std::uint64_t file_size = 0, file_offset = 0;
    int status = 200;
    bool sending_header = true;
    Bytes buffer;
    FileVersion version;
    std::shared_ptr<const Bytes> cached;
    std::shared_ptr<Bytes> candidate;
    explicit Connection(int fd) : socket(fd) {}
};
// The operation owns the connection and all kernel-referenced argument structures.
// Cancellation never releases these objects before the original completion arrives.
struct Operation {
    Kind kind;
    std::shared_ptr<Connection> connection;
    open_how how{};
    struct statx stat{};
    std::size_t requested = 0;
    explicit Operation(Kind k, std::shared_ptr<Connection> c = {}) : kind(k), connection(std::move(c)) {}
};

class Server {
    Config config_;
    Fd root_, listener_;
    Ring ring_;
    FileCache cache_;
    Metrics metrics_;
    std::unordered_map<int, std::shared_ptr<Connection>> connections_;
    std::unordered_map<std::uint64_t, std::unique_ptr<Operation>> operations_;
    std::vector<Bytes> buffers_;
    std::uint64_t sequence_ = 0, accept_id_ = 0;
    bool stopping_ = false;
    Time began_ = Clock::now(), shutdown_at_{}, next_metrics_ = began_, accept_retry_ = began_;
    std::ofstream metrics_file_;

    void flush() {
        while (io_uring_sq_ready(&ring_.value)) {
            int n = io_uring_submit(&ring_.value);
            ++metrics_.submits;
            if (n == -EINTR) continue;
            if (n <= 0) throw std::runtime_error("io_uring_submit: " + std::string(n < 0 ? std::strerror(-n) : "no progress"));
            metrics_.submitted += static_cast<unsigned>(n);
        }
    }
    template<class Prepare>
    std::uint64_t queue(Kind kind, const std::shared_ptr<Connection>& c, Prepare prepare) {
        auto op = std::make_unique<Operation>(kind, c);
        auto* sqe = io_uring_get_sqe(&ring_.value);
        if (!sqe) { flush(); sqe = io_uring_get_sqe(&ring_.value); }
        if (!sqe) throw std::runtime_error("submission queue full after flush");
        auto id = ++sequence_;
        prepare(sqe, *op);
        io_uring_sqe_set_data64(sqe, id);
        operations_.emplace(id, std::move(op));
        if (c && kind != Kind::cancel) c->pending = id;
        if (config_.batch_size == 1 || io_uring_sq_ready(&ring_.value) >= config_.batch_size) flush();
        return id;
    }
    void accept() {
        if (stopping_ || accept_id_ || connections_.size() >= config_.max_connections || Clock::now() < accept_retry_) return;
        accept_id_ = queue(Kind::accept, {}, [&](auto* sqe, auto&) {
            io_uring_prep_accept(sqe, listener_.get(), nullptr, nullptr, SOCK_CLOEXEC);
        });
    }
    void receive(const std::shared_ptr<Connection>& c) {
        queue(Kind::receive, c, [&](auto* sqe, auto&) {
            io_uring_prep_recv(sqe, c->socket.get(), c->receive.data(), c->receive.size(), 0);
        });
    }
    void release_response(const std::shared_ptr<Connection>& c) {
        c->file.reset();
        c->cached.reset(); c->candidate.reset();
        c->output.clear();
        if (!c->buffer.empty()) { buffers_.push_back(std::move(c->buffer)); c->buffer = {}; }
    }
    void cancel(std::uint64_t id) {
        if (!id) return;
        queue(Kind::cancel, {}, [&](auto* sqe, auto&) { io_uring_prep_cancel64(sqe, id, 0); });
    }
    void close(const std::shared_ptr<Connection>& c) {
        if (c->closing) return;
        c->closing = true;
        ::shutdown(c->socket.get(), SHUT_RDWR);
        if (c->pending) cancel(c->pending);
        else release_response(c);
        // Keep closing connections in the limit until their original CQE arrives.
        if (!c->pending) connections_.erase(c->socket.get());
    }
    void send(const std::shared_ptr<Connection>& c) {
        const char* data;
        std::size_t length;
        if (c->sending_header) {
            data = c->output.data() + c->output_sent;
            length = c->output.size() - c->output_sent;
        } else {
            data = (c->cached ? c->cached->data() : c->buffer.data()) + c->chunk_sent;
            length = c->chunk_size - c->chunk_sent;
        }
        // A cached body can be larger than a streaming chunk; send it in bounded pieces.
        length = std::min(length, config_.buffer_size);
        queue(Kind::send, c, [&](auto* sqe, auto& op) {
            op.requested = length;
            io_uring_prep_send(sqe, c->socket.get(), data, length, MSG_NOSIGNAL);
        });
    }
    void headers(const std::shared_ptr<Connection>& c, int status, std::size_t size, std::string_view type) {
        c->status = status;
        c->sending_header = true;
        c->output_sent = 0;
        c->output = response_header(status, size, type, c->keep_alive && !stopping_);
        send(c);
    }
    void error(const std::shared_ptr<Connection>& c, int status, bool force_close = false) {
        if (force_close) c->keep_alive = false;
        c->file.reset(); c->candidate.reset(); c->cached.reset();
        auto body = std::to_string(status) + " " + std::string(reason(status)) + "\n";
        c->status = status; c->sending_header = true; c->output_sent = 0;
        c->file_size = 0;
        c->output = response_header(status, body.size(), "text/plain; charset=utf-8", c->keep_alive && !stopping_);
        if (!c->head) c->output += body;
        send(c);
    }
    void request(const std::shared_ptr<Connection>& c) {
        auto parsed = parse_request(c->input, config_.header_limit);
        if (parsed.state == ParseState::incomplete) { receive(c); return; }
        c->responding = true; c->started = c->header_start;
        ++c->requests; ++metrics_.requests;
        c->head = parsed.request.head;
        c->target = parsed.request.target; c->method = parsed.request.method;
        c->file_size = c->file_offset = 0; c->chunk_size = c->chunk_sent = 0;
        c->keep_alive = parsed.request.keep_alive && c->requests < config_.max_requests && !stopping_;
        if (parsed.state == ParseState::error) { error(c, parsed.status, true); return; }
        c->input.erase(0, parsed.consumed);
        c->path = std::move(parsed.request.path);
        queue(Kind::open, c, [&](auto* sqe, auto& op) {
            op.how.flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
            op.how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS;
            io_uring_prep_openat2(sqe, root_.get(), c->path.c_str(), &op.how);
        });
    }
    void stat(const std::shared_ptr<Connection>& c, Kind kind) {
        queue(kind, c, [&](auto* sqe, auto& op) {
            io_uring_prep_statx(sqe, c->file.get(), "", AT_EMPTY_PATH, STATX_BASIC_STATS, &op.stat);
        });
    }
    static FileVersion version(const struct statx& st) {
        return {st.stx_ino, st.stx_size, st.stx_mtime.tv_sec, st.stx_ctime.tv_sec,
                st.stx_mtime.tv_nsec, st.stx_ctime.tv_nsec, st.stx_dev_major, st.stx_dev_minor};
    }
    void read(const std::shared_ptr<Connection>& c) {
        if (c->buffer.empty()) {
            if (buffers_.empty()) { c->buffer.resize(config_.buffer_size); ++metrics_.buffers_allocated; }
            else { c->buffer = std::move(buffers_.back()); buffers_.pop_back(); ++metrics_.buffers_reused; }
        }
        auto n = static_cast<unsigned>(std::min<std::uint64_t>(c->buffer.size(), c->file_size - c->file_offset));
        queue(Kind::read, c, [&](auto* sqe, auto&) {
            io_uring_prep_read(sqe, c->file.get(), c->buffer.data(), n, c->file_offset);
        });
        ++metrics_.file_reads;
    }
    void finished(const std::shared_ptr<Connection>& c) {
        ++metrics_.completed;
        if (c->status >= 400) ++metrics_.errors;
        auto us = std::chrono::duration<double, std::micro>(Clock::now() - c->started).count();
        metrics_.observe(us);
        if (config_.access_log) {
            std::cerr << c->method << ' ' << std::quoted(c->target) << ' ' << c->status
                      << ' ' << c->file_size << ' ' << us / 1000 << "ms\n";
        }
        release_response(c);
        c->responding = false;
        if (stopping_ || !c->keep_alive) { close(c); return; }
        c->header_start = c->activity = Clock::now();
        request(c); // Any pipelined bytes are parsed only after completing this response.
    }
    void body_finished(const std::shared_ptr<Connection>& c) {
        if (c->candidate) stat(c, Kind::verify);
        else finished(c);
    }
    void on_stat(const std::shared_ptr<Connection>& c, const struct statx& st) {
        if ((st.stx_mode & S_IFMT) != S_IFREG) { error(c, 404); return; }
        constexpr unsigned needed = STATX_TYPE | STATX_SIZE | STATX_INO | STATX_MTIME | STATX_CTIME;
        if ((st.stx_mask & needed) != needed) { error(c, 500); return; }
        c->version = version(st); c->file_size = st.stx_size;
        if (!c->head && c->file_size) {
            c->cached = cache_.get(c->path, c->version);
            if (c->cached) { ++metrics_.hits; c->file.reset(); }
            else {
                ++metrics_.misses;
                if (c->file_size <= std::min(config_.cache_file_bytes, config_.cache_bytes)) {
                    c->candidate = std::make_shared<Bytes>();
                    c->candidate->reserve(c->file_size);
                }
            }
        }
        headers(c, 200, c->file_size, content_type(c->path));
    }
    void complete(std::uint64_t id, int result) {
        auto it = operations_.find(id);
        if (it == operations_.end()) throw std::runtime_error("unknown completion identifier");
        auto op = std::move(it->second);
        operations_.erase(it);
        auto c = op->connection;
        if (c) c->pending = 0;
        if (op->kind == Kind::cancel) return;
        if (op->kind == Kind::accept) {
            accept_id_ = 0;
            if (result >= 0) {
                if (stopping_) ::close(result);
                else {
                    int one = 1;
                    ::setsockopt(result, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    c = std::make_shared<Connection>(result);
                    connections_.emplace(result, c); ++metrics_.accepted;
                    receive(c);
                }
            } else if (!stopping_) {
                ++metrics_.io_errors;
                accept_retry_ = Clock::now() + std::chrono::milliseconds(100);
            }
            accept(); return;
        }
        if (c->closing) {
            if (op->kind == Kind::open && result >= 0) ::close(result);
            release_response(c);
            connections_.erase(c->socket.get());
            return;
        }
        c->activity = Clock::now();
        if (result < 0) {
            ++metrics_.io_errors;
            if (op->kind == Kind::open || op->kind == Kind::stat) {
                int status = (result == -ENOENT || result == -ENOTDIR) ? 404 :
                    (result == -EACCES || result == -EXDEV || result == -ELOOP) ? 403 : 500;
                error(c, status);
            } else if (op->kind == Kind::verify) finished(c);
            else close(c);
            return;
        }
        switch (op->kind) {
        case Kind::receive:
            if (!result) { close(c); break; }
            if (c->input.empty()) c->header_start = Clock::now();
            c->input.append(c->receive.data(), static_cast<std::size_t>(result));
            request(c); break;
        case Kind::open:
            c->file.reset(result); stat(c, Kind::stat); break;
        case Kind::stat:
            on_stat(c, op->stat); break;
        case Kind::read:
            if (!result) { ++metrics_.io_errors; close(c); break; } // File shrank; never send uninitialised bytes.
            c->chunk_size = static_cast<std::size_t>(result); c->chunk_sent = 0;
            c->file_offset += c->chunk_size;
            if (c->candidate) c->candidate->insert(c->candidate->end(), c->buffer.begin(), c->buffer.begin()+result);
            send(c); break;
        case Kind::send:
            if (!result) { close(c); break; }
            if (static_cast<std::size_t>(result) < op->requested) ++metrics_.partial_sends;
            metrics_.bytes += static_cast<unsigned>(result);
            if (c->sending_header) {
                c->output_sent += static_cast<unsigned>(result);
                if (c->output_sent < c->output.size()) { send(c); break; }
                c->sending_header = false;
                if (c->head || !c->file_size) { finished(c); break; }
                if (c->cached) {
                    c->chunk_size = c->cached->size(); c->chunk_sent = 0; send(c);
                } else read(c);
            } else {
                c->chunk_sent += static_cast<unsigned>(result);
                if (c->chunk_sent < c->chunk_size) send(c);
                else if (c->cached || c->file_offset == c->file_size) body_finished(c);
                else read(c);
            }
            break;
        case Kind::verify:
            if (version(op->stat) == c->version) cache_.put(c->path, c->version, c->candidate);
            finished(c); break;
        default: break;
        }
    }
    void snapshot(bool final) {
        auto elapsed = std::chrono::duration<double>(Clock::now() - began_).count();
        struct rusage usage{};
        getrusage(RUSAGE_SELF, &usage);
        double cpu = usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
                     usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
        // VmHWM measures this executable; ru_maxrss can retain the spawning
        // process's pre-exec high-water mark when launched by a large driver.
        long peak_rss = usage.ru_maxrss;
        std::ifstream status("/proc/self/status");
        std::string line;
        while (std::getline(status, line)) {
            if (line.compare(0, 6, "VmHWM:") == 0) {
                std::istringstream field(line.substr(6)); field >> peak_rss; break;
            }
        }
        std::ostringstream out;
        out << std::fixed << std::setprecision(3)
            << "{\"final\":" << (final ? "true" : "false") << ",\"uptime_seconds\":" << elapsed
            << ",\"accepted\":" << metrics_.accepted << ",\"active_connections\":" << connections_.size()
            << ",\"requests\":" << metrics_.requests << ",\"completed\":" << metrics_.completed
            << ",\"http_errors\":" << metrics_.errors << ",\"io_errors\":" << metrics_.io_errors
            << ",\"bytes_sent\":" << metrics_.bytes << ",\"timeouts\":" << metrics_.timeouts
            << ",\"requests_per_second_avg\":" << metrics_.completed / std::max(elapsed, 0.001)
            << ",\"p50_ms\":" << metrics_.percentile(.50) << ",\"p95_ms\":" << metrics_.percentile(.95)
            << ",\"p99_ms\":" << metrics_.percentile(.99) << ",\"cpu_seconds\":" << cpu
            << ",\"peak_rss_kib\":" << peak_rss << ",\"cache_hits\":" << metrics_.hits
            << ",\"cache_misses\":" << metrics_.misses << ",\"cache_bytes\":" << cache_.used()
            << ",\"cache_entries\":" << cache_.entries() << ",\"submit_calls\":" << metrics_.submits
            << ",\"operations_submitted\":" << metrics_.submitted << ",\"file_reads\":" << metrics_.file_reads
            << ",\"buffers_allocated\":" << metrics_.buffers_allocated
            << ",\"buffers_reused\":" << metrics_.buffers_reused
            << ",\"partial_sends\":" << metrics_.partial_sends << "}";
        if (metrics_file_.is_open()) {
            metrics_file_ << out.str() << '\n'; metrics_file_.flush();
            if (!metrics_file_) throw std::runtime_error("metrics file write failed");
        }
        if (final) std::cerr << out.str() << '\n';
    }
    void tick() {
        auto now = Clock::now();
        if (interrupted && !stopping_) {
            stopping_ = true; shutdown_at_ = now + std::chrono::milliseconds(config_.grace_ms);
            cancel(accept_id_);
            std::vector<std::shared_ptr<Connection>> idle;
            for (auto& [fd,c] : connections_) { (void)fd; if (!c->responding) idle.push_back(c); }
            for (auto& c : idle) close(c);
        }
        std::vector<std::shared_ptr<Connection>> expired;
        for (auto& [fd,c] : connections_) {
            (void)fd;
            if (c->closing) continue;
            bool timeout = now - c->activity >= std::chrono::milliseconds(config_.timeout_ms) ||
                (!c->responding && !c->input.empty() && now - c->header_start >= std::chrono::milliseconds(config_.header_timeout_ms));
            if (timeout || (stopping_ && now >= shutdown_at_)) {
                if (timeout) ++metrics_.timeouts;
                expired.push_back(c);
            }
        }
        for (auto& c : expired) close(c);
        if (now >= next_metrics_) {
            if (metrics_file_.is_open()) snapshot(false);
            next_metrics_ = now + std::chrono::milliseconds(config_.metrics_interval_ms);
        }
        accept();
    }
public:
    explicit Server(Config config) : config_(std::move(config)), cache_(config_.cache_bytes) {
        root_.reset(::open(config_.root.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
        if (root_.get() < 0) throw std::runtime_error("document root: " + std::string(std::strerror(errno)));
        int result = io_uring_queue_init(config_.queue_depth, &ring_.value, 0);
        if (result < 0) throw std::runtime_error("io_uring_queue_init: " + std::string(std::strerror(-result)) +
            "; run on a Linux host permitting io_uring (containers may restrict it)");
        ring_.ready = true;
        auto* probe = io_uring_get_probe_ring(&ring_.value);
        if (!probe) throw std::runtime_error("cannot probe io_uring operations");
        bool supported = true;
        for (int opcode : {IORING_OP_ACCEPT, IORING_OP_RECV, IORING_OP_SEND, IORING_OP_OPENAT2,
                           IORING_OP_STATX, IORING_OP_READ, IORING_OP_ASYNC_CANCEL})
            supported &= io_uring_opcode_supported(probe, opcode);
        io_uring_free_probe(probe);
        if (!supported) throw std::runtime_error("kernel lacks required io_uring operations (Linux 5.6+ required)");
        if (!config_.metrics_file.empty()) {
            metrics_file_.open(config_.metrics_file, std::ios::trunc);
            if (!metrics_file_) throw std::runtime_error("cannot open metrics file");
        }
        listener_.reset(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        if (listener_.get() < 0) throw std::runtime_error("socket: " + std::string(std::strerror(errno)));
        int one = 1;
        if (::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
            throw std::runtime_error("setsockopt failed");
        sockaddr_in address{};
        address.sin_family = AF_INET; address.sin_port = htons(config_.port);
        if (inet_pton(AF_INET, config_.bind.c_str(), &address.sin_addr) != 1) throw std::runtime_error("invalid IPv4 bind address");
        if (::bind(listener_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
            ::listen(listener_.get(), 1024) < 0) throw std::runtime_error("bind/listen: " + std::string(std::strerror(errno)));
        socklen_t size = sizeof(address);
        if (::getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&address), &size) < 0) throw std::runtime_error("getsockname failed");
        std::cout << "ZeroHTTPd listening on " << config_.bind << ':' << ntohs(address.sin_port) << std::endl;
    }
    ~Server() {
        // Even on an exception the kernel must release buffers before their C++ owners.
        for (auto& [fd,c] : connections_) { (void)c; ::shutdown(fd, SHUT_RDWR); }
        ring_.close();
        operations_.clear(); connections_.clear();
    }
    void run() {
        accept();
        for (;;) {
            tick(); flush();
            if (stopping_ && operations_.empty()) break;
            io_uring_cqe* cqe = nullptr;
            __kernel_timespec timeout{0, 20 * 1000 * 1000};
            int result = io_uring_wait_cqe_timeout(&ring_.value, &cqe, &timeout);
            if (result == -ETIME || result == -EINTR) continue;
            if (result < 0) throw std::runtime_error("io_uring_wait_cqe: " + std::string(std::strerror(-result)));
            unsigned count = 0;
            do {
                auto id = io_uring_cqe_get_data64(cqe);
                int res = cqe->res;
                io_uring_cqe_seen(&ring_.value, cqe);
                complete(id, res);
                ++count;
            } while (count < config_.batch_size && io_uring_peek_cqe(&ring_.value, &cqe) == 0);
        }
        snapshot(true);
    }
};
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) if (std::string_view(argv[i]) == "--help") { zerohttpd::print_usage(); return 0; }
    try {
        auto config = zerohttpd::parse_config(argc, argv);
        struct sigaction action{};
        action.sa_handler = zerohttpd::on_signal;
        sigemptyset(&action.sa_mask);
        sigaction(SIGINT, &action, nullptr); sigaction(SIGTERM, &action, nullptr);
        std::signal(SIGPIPE, SIG_IGN);
        zerohttpd::Server server(std::move(config));
        server.run(); return 0;
    } catch (const std::exception& e) {
        std::cerr << "zerohttpd: " << e.what() << '\n'; return 1;
    }
}
