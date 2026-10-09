/*
 * examples/http_client.c
 *
 * An HTTP/1.0 client built on io_uring, talking to webserver_liburing.
 *
 * The server demonstrates the three opcodes that matter for *serving* files:
 * IORING_OP_ACCEPT, IORING_OP_READV and IORING_OP_WRITEV. This program
 * demonstrates the other half of the connection lifecycle, using three opcodes
 * that appear nowhere else in this repository:
 *
 *     IORING_OP_CONNECT   establish the TCP connection
 *     IORING_OP_SEND      write the request
 *     IORING_OP_RECV      read the response
 *
 * It also chains connect -> send with IOSQE_IO_LINK, so that a failed
 * connection cancels the request write instead of writing into a dead socket.
 *
 * Requires Linux 5.6 or newer: IORING_OP_SEND and IORING_OP_RECV were added in
 * 5.6. The server itself only needs 5.5.
 *
 * Build:
 *     cmake -S . -B build
 *     cmake --build build --target http_client
 *
 * Start the server first:
 *     ./build/webserver_liburing
 *
 * Usage:
 *     ./build/http_client [host] [port] [path] [-b]
 *
 *     -b, --body   print the response body as well as the headers
 *
 * Examples:
 *     ./build/http_client                          # 127.0.0.1:8000/
 *     ./build/http_client localhost 8000 /tux.png -b
 *
 * Exit status: 0 on a 2xx response, 1 on an HTTP error status or a transport
 * failure, 2 on bad usage.
 */

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <liburing.h>

#define DEFAULT_HOST       "127.0.0.1"
#define DEFAULT_PORT       "8000"
#define DEFAULT_PATH       "/"

/* Deliberately larger than the two submissions we make up front, so that both
 * SQEs of the linked connect/send pair fit in a single submission. If the
 * submission queue filled up between the two io_uring_get_sqe() calls,
 * liburing would flush it and the IOSQE_IO_LINK dependency would be lost. */
#define QUEUE_DEPTH        8

#define REQ_BUF_SZ         1024
#define RESP_CHUNK         16384

/* Refuse to buffer more than this, so a hostile or broken peer cannot make us
 * allocate without limit. */
#define RESP_MAX           (32UL * 1024UL * 1024UL)

/* Passed through user_data so we can tell the completions apart. */
enum tag {
    TAG_CONNECT = 1,
    TAG_SEND,
    TAG_RECV
};

struct client {
    int             fd;
    char            req[REQ_BUF_SZ];
    size_t          req_len;
    size_t          req_sent;
    unsigned char  *resp;
    size_t          resp_len;
    size_t          resp_cap;
};

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [host] [port] [path] [-b|--body]\n"
            "\n"
            "  host   default %s\n"
            "  port   default %s\n"
            "  path   default %s\n"
            "  -b     also print the response body\n",
            argv0, DEFAULT_HOST, DEFAULT_PORT, DEFAULT_PATH);
}

/* Make sure at least `extra` bytes of room remain past what we have read. */
static int reserve(struct client *c, size_t extra) {
    size_t cap;

    if (c->resp_cap - c->resp_len >= extra)
        return 0;

    cap = c->resp_cap ? c->resp_cap : RESP_CHUNK;
    while (cap - c->resp_len < extra) {
        if (cap > RESP_MAX)
            return -1;
        cap *= 2;
    }
    if (cap > RESP_MAX)
        cap = RESP_MAX;

    unsigned char *tmp = realloc(c->resp, cap);
    if (!tmp)
        return -1;

    c->resp = tmp;
    c->resp_cap = cap;
    return 0;
}

/*
 * Queue connect and send as one linked pair, then wait for both.
 * Returns 0 on success, or -errno on failure.
 */
static int connect_and_send(struct io_uring *ring, struct client *c,
                            const struct addrinfo *ai) {
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;

    int sent_done = 0;

    sqe = io_uring_get_sqe(ring);
    if (!sqe)
        return -EAGAIN;
    io_uring_prep_connect(sqe, c->fd, ai->ai_addr, ai->ai_addrlen);
    io_uring_sqe_set_data64(sqe, TAG_CONNECT);

    sqe = io_uring_get_sqe(ring);
    if (!sqe)
        return -EAGAIN;
    io_uring_prep_send(sqe, c->fd, c->req, c->req_len, 0);
    io_uring_sqe_set_data64(sqe, TAG_SEND);
    io_uring_sqe_set_flags(sqe, IOSQE_IO_LINK);

    if (io_uring_submit(ring) < 0)
        return -errno;

    while (!sent_done) {
        uint64_t tag;
        int res;

        if (io_uring_wait_cqe(ring, &cqe) < 0)
            return -EIO;

        tag = io_uring_cqe_get_data64(cqe);
        res = cqe->res;
        io_uring_cqe_seen(ring, cqe);

        if (tag == TAG_CONNECT) {
            if (res < 0)
                return res;
            continue;
        }

        if (res < 0)
            return res;

        c->req_sent += (size_t)res;
        if (c->req_sent >= c->req_len) {
            sent_done = 1;
            continue;
        }

        /* Short write. send() is allowed to transfer fewer bytes than asked;
         * submit just the remainder. */
        sqe = io_uring_get_sqe(ring);
        if (!sqe)
            return -EAGAIN;
        io_uring_prep_send(sqe, c->fd, c->req + c->req_sent,
                           c->req_len - c->req_sent, 0);
        io_uring_sqe_set_data64(sqe, TAG_SEND);
        if (io_uring_submit(ring) < 0)
            return -errno;
    }

    return 0;
}

/*
 * Read until the peer closes.
 *
 * The server speaks HTTP/1.0, sends no Content-Length on its error pages, and
 * closes the socket straight after the write completes -- so end-of-file is the
 * only reliable end-of-response signal. Stopping at the header block, or
 * assuming one recv() returns the whole body, both truncate large files.
 */
static int recv_all(struct io_uring *ring, struct client *c) {
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;

    for (;;) {
        size_t room;
        int res;

        if (reserve(c, RESP_CHUNK) < 0)
            return -ENOMEM;

        room = c->resp_cap - c->resp_len;

        sqe = io_uring_get_sqe(ring);
        if (!sqe)
            return -EAGAIN;
        io_uring_prep_recv(sqe, c->fd, c->resp + c->resp_len, room, 0);
        io_uring_sqe_set_data64(sqe, TAG_RECV);
        if (io_uring_submit(ring) < 0)
            return -errno;

        if (io_uring_wait_cqe(ring, &cqe) < 0)
            return -EIO;

        res = cqe->res;
        io_uring_cqe_seen(ring, cqe);

        if (res < 0)
            return res;
        if (res == 0)
            return 0;               /* peer closed: response complete */

        c->resp_len += (size_t)res;
    }
}

/* Print headers, and optionally the body. Returns the HTTP status code, or 0
 * if the response could not be parsed. */
static int print_response(const struct client *c, int dump_body) {
    const unsigned char *body = NULL;
    size_t body_len = 0;
    size_t i;
    int status = 0;

    for (i = 0; i + 3 < c->resp_len; i++) {
        if (c->resp[i] == '\r' && c->resp[i + 1] == '\n' &&
            c->resp[i + 2] == '\r' && c->resp[i + 3] == '\n') {
            body = c->resp + i + 4;
            body_len = c->resp_len - (i + 4);
            fwrite(c->resp, 1, i + 4, stdout);
            break;
        }
    }

    if (!body) {
        /* No blank line. Show what arrived rather than silently dropping it. */
        fwrite(c->resp, 1, c->resp_len, stdout);
        fprintf(stderr, "\nwarning: no header/body separator in response\n");
        return 0;
    }

    if (c->resp_len >= 12)
        sscanf((const char *)c->resp, "HTTP/%*d.%*d %d", &status);

    printf("\n%zu bytes of body", body_len);
    if (dump_body) {
        printf(", contents:\n---\n");
        fwrite(body, 1, body_len, stdout);
        printf("\n---\n");
    } else {
        printf(" (pass -b to print it)");
    }
    printf("\n");

    return status;
}

int main(int argc, char **argv) {
    const char *host = DEFAULT_HOST;
    const char *port = DEFAULT_PORT;
    const char *path = DEFAULT_PATH;
    struct addrinfo hints, *ai = NULL;
    struct io_uring ring;
    struct client c;
    int dump_body = 0;
    int status;
    int ret;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--body") == 0) {
            dump_body = 1;
        } else if (i == 1) {
            host = argv[i];
        } else if (i == 2) {
            port = argv[i];
        } else if (i == 3) {
            path = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;          /* the server binds AF_INET only */
    hints.ai_socktype = SOCK_STREAM;

    ret = getaddrinfo(host, port, &hints, &ai);
    if (ret != 0) {
        fprintf(stderr, "error: cannot resolve %s:%s: %s\n",
                host, port, gai_strerror(ret));
        return 1;
    }

    ret = io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
    if (ret < 0) {
        fprintf(stderr, "error: io_uring_queue_init: %s\n", strerror(-ret));
        freeaddrinfo(ai);
        return 1;
    }

    memset(&c, 0, sizeof(c));
    c.fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (c.fd < 0) {
        perror("socket");
        io_uring_queue_exit(&ring);
        freeaddrinfo(ai);
        return 1;
    }

    /* HTTP/1.0 plus "Connection: close" tells the server we want the
     * connection closed after the response, which is what recv_all() relies on
     * to know it has read everything. */
    ret = snprintf(c.req, sizeof(c.req),
                   "GET %s HTTP/1.0\r\n"
                   "Host: %s\r\n"
                   "Connection: close\r\n"
                   "\r\n",
                   path, host);
    if (ret < 0 || (size_t)ret >= sizeof(c.req)) {
        fprintf(stderr, "error: request does not fit in %d bytes "
                        "(is the path too long?)\n", REQ_BUF_SZ);
        close(c.fd);
        io_uring_queue_exit(&ring);
        freeaddrinfo(ai);
        return 1;
    }
    c.req_len = (size_t)ret;

    ret = connect_and_send(&ring, &c, ai);
    if (ret < 0) {
        fprintf(stderr, "error: %s %s: %s\n", "connecting to", port,
                strerror(-ret));
        close(c.fd);
        io_uring_queue_exit(&ring);
        freeaddrinfo(ai);
        return 1;
    }

    ret = recv_all(&ring, &c);
    if (ret < 0) {
        fprintf(stderr, "error: reading response: %s\n", strerror(-ret));
        close(c.fd);
        io_uring_queue_exit(&ring);
        freeaddrinfo(ai);
        return 1;
    }

    status = print_response(&c, dump_body);

    free(c.resp);
    close(c.fd);
    io_uring_queue_exit(&ring);
    freeaddrinfo(ai);

    /* Mirror curl -f: success means the server said the request was fine. */
    if (status >= 200 && status < 300)
        return 0;
    if (status == 0)
        return 1;
    return 1;
}
