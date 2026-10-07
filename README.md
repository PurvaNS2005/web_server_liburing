# ZeroHTTPd (`web_server_liburing`)

A small, single-threaded HTTP static file server built on Linux's [`io_uring`](https://kernel.dk/io_uring.pdf) via [liburing](https://github.com/axboe/liburing).

Instead of the usual `epoll` + `read()`/`write()` pattern, every socket operation — `accept`, `read`, `write` — is submitted to an `io_uring` ring and completed asynchronously. Response headers and file bodies are sent with a single scatter-gather `writev`, so the kernel moves the data in one shot instead of round-tripping through user space per buffer.

The server identifies itself as `zerohttpd/0.1`.

> **This is a learning/hobby project, not production software.** See [Known limitations](#known-limitations) before using it for anything real.

---

## Requirements

| | |
|---|---|
| **OS** | Linux only (io_uring does not exist on Windows or macOS) |
| **Kernel** | 5.5 or newer |
| **liburing** | 2.0 or newer |
| **Build** | CMake ≥ 3.15, GCC or Clang (C99) |

If you're on Windows, build and test inside [WSL2](https://learn.microsoft.com/windows/wsl/install) or a Linux VM.

### Install dependencies

**Debian / Ubuntu**
```bash
sudo apt update
sudo apt install build-essential cmake liburing-dev
```

**Fedora**
```bash
sudo dnf install gcc cmake liburing-devel
```

**Arch**
```bash
sudo pacman -S base-devel cmake liburing
```

---

## Build

```bash
mkdir -p build
cd build
cmake ..
make
```

This produces `build/webserver_liburing` alongside the liburing example binaries.

To build just the server:

```bash
cmake --build build --target webserver_liburing
```

---

## Run

The server resolves every request against the `public/` directory **relative to your current working directory**, so start it from the repository root:

```bash
./build/webserver_liburing
```

```
ZeroHTTPd listening on port: 8000
```

Then open <http://localhost:8000> in a browser, or from a terminal:

```bash
curl -i http://localhost:8000/
curl -i http://localhost:8000/tux.png
curl -i http://localhost:8000/does-not-exist     # 404
curl -i -X POST http://localhost:8000/         # 400
```

Press `Ctrl-C` to shut down cleanly.

---

## Behaviour

**Routing** — the request path is appended to `public/`. A path ending in `/` has `index.html` appended, so `/` serves `public/index.html`. Anything that isn't a regular file returns `404`.

**Methods** — only `GET` is implemented. Any other method gets a `400 Bad Request` with a short explanation.

**Supported content types**

| Extension | `Content-Type` |
|---|---|
| `.html`, `.htm` | `text/html` |
| `.css` | `text/css` |
| `.js` | `application/javascript` |
| `.txt` | `text/plain` |
| `.png` | `image/png` |
| `.jpg`, `.jpeg` | `image/jpeg` |
| `.gif` | `image/gif` |

Extensions are matched case-insensitively (`TUX.PNG` works). Any other extension falls through — see [Known limitations](#known-limitations).

**Connection handling** — responses are `HTTP/1.0`, so the server closes the socket after every response. There's no keep-alive.

---

## Configuration

Tunable constants live at the top of `webserver_liburing.c`:

```c
#define DEFAULT_SERVER_PORT     8000
#define QUEUE_DEPTH             256
#define READ_SZ                 8192
```

Change the port there and rebuild. Nothing is currently configurable at runtime — there's no flag parsing or config file.

---

## How it works

```
io_uring_wait_cqe()
        │
        ├── EVENT_TYPE_ACCEPT ──▶ re-arm accept
        │                       └─▶ queue readv on the new socket
        │
        ├── EVENT_TYPE_READ ────▶ parse the request line
        │                       └─▶ stat() + build 6 iovecs (5 headers + body)
        │                           └─▶ queue writev
        │
        └── EVENT_TYPE_WRITE ───▶ free iovec buffers, close() the client socket
```

1. A single `accept` is in flight at all times, so the ring never runs dry.
2. When it completes, a `readv` is queued on the returned client fd.
3. The request line is tokenised with `strtok_r` into a method and a path.
4. For `GET`, the file is `stat`ed, the five HTTP headers are formatted into
   separate heap buffers, and the file body is read into a sixth buffer.
5. One `writev` writes all six buffers in a single kernel round-trip.
6. On completion the buffers are freed and the client socket is closed.

The ring is created with `io_uring_queue_init(QUEUE_DEPTH, ...)` and torn down in the
`SIGINT` handler.

---

## Project layout

| File | Purpose |
|---|---|
| `webserver_liburing.c` | The server. This is the only file the project really needs. |
| `public/` | Document root — `index.html`, `tux.png` |
| `CMakeLists.txt` | Build definition (note: the project is still named `liburing_examples`) |
| `probe.c`, `link.c`, `fixed_buffers.c`, `sq_poll.c`, `provide_buffers.c`, `eventfd.c`, `cat_io_uring.c`, `cat_liburing.c` | Unmodified test programs copied from liburing's own `test/` suite. Useful as io_uring examples; not part of the server. |

---

## Known limitations

Roughly in order of how much they'd hurt you:

- **Fixed-size stack buffers are used to build request paths.** `final_path` and
  `small_case_path` are `char[1024]` and are filled with `strcpy`/`strcat`, so a
  long URL overruns the stack. Needs bounds checking.
- **Uninitialised header buffer for unknown extensions.** `send_headers` declares
  `char send_buffer[1024]` and only writes to it inside a chain of extension
  comparisons. A file with an unrecognised extension sends whatever happened to be
  on the stack as its `Content-Type`.
- **The kernel version check is wrong.** It tests `major >= 5 && minor >= 5`, so
  Linux 6.x is rejected because `6.1` has `minor < 5`. The comparison needs to be
  lexicographic.
- **No `Content-Type` for unknown types.** Nothing sensible is sent — there's no
  `application/octet-stream` fallback.
- **Fixed 1024-byte request buffer.** `get_line` copies until `\r\n`; an oversized
  first line overruns `http_request`.
- **Short reads are not retried.** `copy_file_contents` reads once and warns on
  truncation rather than looping.
- **No keep-alive, no `HEAD`, no request body parsing, no directory listing, no
  TLS, no access logging, no graceful shutdown.**
- **A crash kills every connection.** Several paths call `exit(1)` on malformed
  input rather than rejecting the one bad request.

---

## Credits

- The server is adapted from [ZeroHTTPd](https://github.com/rxi/zerohttpd) by
  rxi, with `accept`/`read`/`write` rewritten onto io_uring.
- The supporting `.c` files are from [liburing](https://github.com/axboe/liburing)'s
  test suite, by Jens Axboe and contributors.
- Licensed under the [MIT License](LICENSE).

---

## Contributing

Issues and pull requests are welcome.

```bash
git checkout -b my-change
# ...work...
git commit -m "fix: bounds-check the request path buffer"
git push -u origin my-change
```

Then open a PR against `main`. Please keep pull requests focused on one change,
and note that **the project cannot be built or tested on Windows** — use WSL2 or a
Linux machine and include the output of `make` in your PR.

The [Known limitations](#known-limitations) list is a good source of first issues,
especially the stack overflow on long paths and the uninitialised `send_buffer`.
