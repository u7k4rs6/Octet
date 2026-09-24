/* bcurl: Octet client.  Usage: ./bcurl [-v] <host>:<port>/<path> [<path>...]
 *
 * Extra arguments are further paths (or URLs with the same host:port)
 * fetched in order over the same single connection. */
#define _GNU_SOURCE
#include "octet.h"

#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define EXIT_OK      0
#define EXIT_STATUS  1          /* :status 4xx or 5xx */
#define EXIT_PROTO   2          /* connect failure, early close, malformed */

static FILE *dump;              /* stderr with -v, else NULL */

struct target {
    char host[256];             /* as given, brackets stripped */
    char port[8];
    char hostport[300];         /* value of the host header */
    const char *path;
};

static int die(const char *msg)
{
    fprintf(stderr, "bcurl: %s\n", msg);
    return EXIT_PROTO;
}

/* Splits "host:port/path". Accepts "[v6]:port/path". Missing path is "/". */
static int parse_target(const char *arg, struct target *t)
{
    const char *s = arg, *slash, *hstart, *colon;
    size_t hp, hl, pl;

    if (strncmp(s, "octet://", 8) == 0)
        s += 8;
    slash = strchr(s, '/');
    t->path = slash ? slash : "/";
    hp = slash ? (size_t)(slash - s) : strlen(s);
    if (hp == 0 || hp >= sizeof t->hostport)
        return -1;
    memcpy(t->hostport, s, hp);
    t->hostport[hp] = '\0';

    if (t->hostport[0] == '[') {
        const char *rb = strchr(t->hostport, ']');
        if (!rb || rb[1] != ':')
            return -1;
        hstart = t->hostport + 1;
        hl = (size_t)(rb - hstart);
        colon = rb + 1;
    } else {
        colon = strrchr(t->hostport, ':');
        if (!colon)
            return -1;
        hstart = t->hostport;
        hl = (size_t)(colon - hstart);
    }
    pl = strlen(colon + 1);
    if (hl == 0 || hl >= sizeof t->host || pl == 0 || pl >= sizeof t->port)
        return -1;
    memcpy(t->host, hstart, hl);
    t->host[hl] = '\0';
    memcpy(t->port, colon + 1, pl + 1);
    return 0;
}

static int dial(const struct target *t)
{
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *res, *ai;
    int fd = -1, rc = getaddrinfo(t->host, t->port, &hints, &res);

    if (rc != 0) {
        fprintf(stderr, "bcurl: %s: %s\n", t->host, gai_strerror(rc));
        return -1;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    if (fd < 0)
        fprintf(stderr, "bcurl: connect %s: %s\n", t->hostport, strerror(errno));
    freeaddrinfo(res);
    return fd;
}

static int send_request(int fd, const struct target *t)
{
    struct oct_buf b = { 0 };
    int rc = -1;

    if (oct_hb_add_str(&b, OCT_H_METHOD, "GET") == 0 &&
        oct_hb_add_str(&b, OCT_H_PATH, t->path) == 0 &&
        oct_hb_add_str(&b, OCT_H_HOST, t->hostport) == 0 &&
        oct_hb_add_str(&b, OCT_H_USER_AGENT, "bcurl/1") == 0 &&
        oct_hb_add_str(&b, OCT_H_ACCEPT, "*/*") == 0)
        rc = oct_send_frame(fd, OCT_T_REQUEST, OCT_F_END, b.p, b.len, dump);
    oct_buf_free(&b);
    return rc;
}

/* Parses a decimal header value into *out. */
static int parse_num(const struct oct_hdr *h, long long *out)
{
    long long v = 0;

    if (!h || h->vlen == 0 || h->vlen > 18)
        return -1;
    for (size_t i = 0; i < h->vlen; i++) {
        if (h->value[i] < '0' || h->value[i] > '9')
            return -1;
        v = v * 10 + (h->value[i] - '0');
    }
    *out = v;
    return 0;
}

/* Reads one response (RESPONSE + DATA until END). Returns an exit code. */
static int read_response(int fd)
{
    uint8_t raw[OCT_HDR_LEN];
    struct oct_fh fh;
    uint8_t *payload = NULL;
    long long status = -1, clen = -1, got = 0;
    int rc = EXIT_PROTO;

    for (;;) {
        int r = oct_read_header(fd, raw, &fh);
        if (r == 0) {
            rc = die("connection closed before END");
            break;
        }
        if (r < 0) {
            rc = die(errno == EAGAIN || errno == EWOULDBLOCK
                     ? "timed out waiting for server" : "short frame header");
            break;
        }
        if (fh.version != OCT_VERSION) {
            if (dump)
                oct_dump_frame(dump, '<', raw, NULL, 0);
            rc = die("unsupported frame version");
            break;
        }
        int known = oct_type_name(fh.type) != NULL;
        if (known && fh.type == OCT_T_RESPONSE && fh.len > OCT_HB_CAP) {
            rc = die("RESPONSE header block too large");
            break;
        }

        free(payload);
        payload = malloc(fh.len ? fh.len : 1);
        if (!payload) {
            rc = die("out of memory");
            break;
        }
        if (oct_read_full(fd, payload, fh.len) != (ssize_t)fh.len) {
            rc = die("connection closed mid-frame");
            break;
        }
        if (dump)
            oct_dump_frame(dump, '<', raw, payload, fh.len);
        if (!known)
            continue;                        /* unknown type: skip it */

        if (fh.flags & ~OCT_F_KNOWN) {
            rc = die("reserved flag bits set");
            break;
        }
        int end = fh.flags & OCT_F_END;

        if (fh.type == OCT_T_RESPONSE) {
            struct oct_hdr h[255];
            const char *err;
            int n;

            if (status >= 0) {
                rc = die("second RESPONSE frame before END");
                break;
            }
            n = oct_hb_parse(payload, fh.len, h, &err);
            if (n < 0) {
                rc = die(err);
                break;
            }
            const struct oct_hdr *sh = oct_hb_find(h, n, OCT_H_STATUS);
            if (!sh || sh->vlen != 3 || parse_num(sh, &status) < 0 ||
                status < 100 || status > 599) {
                rc = die("missing or invalid :status");
                break;
            }
            const struct oct_hdr *cl = oct_hb_find(h, n, OCT_H_CONTENT_LENGTH);
            if (cl && parse_num(cl, &clen) < 0) {
                rc = die("invalid content-length");
                break;
            }
        } else if (fh.type == OCT_T_DATA) {
            if (status < 0) {
                rc = die("DATA before RESPONSE");
                break;
            }
            got += fh.len;
            if (clen >= 0 && got > clen) {
                rc = die("DATA exceeds content-length");
                break;
            }
            for (size_t off = 0; off < fh.len;) {
                ssize_t w = write(STDOUT_FILENO, payload + off, fh.len - off);
                if (w < 0 && errno == EINTR)
                    continue;
                if (w <= 0) {
                    free(payload);
                    return die("write to stdout failed");
                }
                off += (size_t)w;
            }
        } else {
            rc = die("server sent a REQUEST frame");
            break;
        }

        if (end) {
            if (clen >= 0 && got != clen) {
                rc = die("body shorter than content-length");
                break;
            }
            rc = status >= 200 && status < 300 ? EXIT_OK : EXIT_STATUS;
            if (rc != EXIT_OK && !dump)
                fprintf(stderr, "bcurl: server returned %lld\n", status);
            break;
        }
    }
    free(payload);
    return rc;
}

static int usage(void)
{
    fprintf(stderr, "usage: bcurl [-v] <host>:<port>/<path> [<path>...]\n");
    return EXIT_PROTO;
}

int main(int argc, char **argv)
{
    struct target first, t;
    int i = 1, fd, worst = EXIT_OK;

    if (i < argc && strcmp(argv[i], "-v") == 0) {
        dump = stderr;
        i++;
    }
    if (i >= argc)
        return usage();
    if (parse_target(argv[i], &first) < 0) {
        fprintf(stderr, "bcurl: bad target %s\n", argv[i]);
        return usage();
    }
    signal(SIGPIPE, SIG_IGN);

    fd = dial(&first);                       /* the only connection ever opened */
    if (fd < 0)
        return EXIT_PROTO;
    oct_set_timeout(fd, OCT_TIMEOUT_S);

    for (; i < argc; i++) {
        t = first;
        if (argv[i][0] == '/') {
            t.path = argv[i];
        } else if (i > 1 + (dump != NULL)) {
            if (parse_target(argv[i], &t) < 0 ||
                strcmp(t.hostport, first.hostport) != 0) {
                fprintf(stderr, "bcurl: %s: every target must share one host:port\n",
                        argv[i]);
                close(fd);
                return EXIT_PROTO;
            }
        }
        if (send_request(fd, &t) < 0) {
            close(fd);
            return die("connection closed while sending");
        }
        int rc = read_response(fd);
        if (rc > worst)
            worst = rc;
        if (rc == EXIT_PROTO)
            break;
    }
    close(fd);
    return worst;
}
