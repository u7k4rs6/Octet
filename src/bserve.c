/* bserve: Octet file server.  Usage: ./bserve <root> <port> */
#define _GNU_SOURCE
#include "octet.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SERVER_NAME "bserve/1"

static char root[PATH_MAX];
static size_t root_len;

static void on_term(int sig)
{
    (void)sig;
    _exit(0);
}

static const char *content_type(const char *path)
{
    static const struct { const char *ext, *type; } map[] = {
        { ".html", "text/html; charset=utf-8" },
        { ".htm",  "text/html; charset=utf-8" },
        { ".txt",  "text/plain; charset=utf-8" },
        { ".css",  "text/css" },
        { ".js",   "text/javascript" },
        { ".json", "application/json" },
        { ".png",  "image/png" },
        { ".jpg",  "image/jpeg" },
        { ".jpeg", "image/jpeg" },
        { ".gif",  "image/gif" },
        { ".svg",  "image/svg+xml" },
        { ".ico",  "image/x-icon" },
        { ".pdf",  "application/pdf" },
    };
    const char *dot = strrchr(path, '.');

    if (dot && !strchr(dot, '/'))
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
            if (strcasecmp(dot, map[i].ext) == 0)
                return map[i].type;
    return "application/octet-stream";
}

/* Sends a RESPONSE frame. body_follows clears END so DATA frames can follow. */
static int send_response(int fd, int status, long long clen, const char *ctype,
                         int body_follows)
{
    struct oct_buf b = { 0 };
    char st[8], len[32], date[64];
    time_t now = time(NULL);
    struct tm tm;
    int rc = -1;

    snprintf(st, sizeof st, "%d", status);
    snprintf(len, sizeof len, "%lld", clen);
    gmtime_r(&now, &tm);
    strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", &tm);

    if (oct_hb_add_str(&b, OCT_H_STATUS, st) == 0 &&
        oct_hb_add_str(&b, OCT_H_CONTENT_LENGTH, len) == 0 &&
        (!ctype || oct_hb_add_str(&b, OCT_H_CONTENT_TYPE, ctype) == 0) &&
        oct_hb_add_str(&b, OCT_H_SERVER, SERVER_NAME) == 0 &&
        oct_hb_add_str(&b, OCT_H_DATE, date) == 0)
        rc = oct_send_frame(fd, OCT_T_RESPONSE, body_follows ? 0 : OCT_F_END,
                            b.p, b.len, NULL);
    oct_buf_free(&b);
    return rc;
}

/* Applies SPEC.md "Path handling". Returns an open fd of a regular file
 * under the root, or -1 with *status set to 404 or 500. */
static int open_under_root(const char *path, int *status, struct stat *st)
{
    char joined[PATH_MAX], real[PATH_MAX];
    int fd;

    *status = 404;
    if (path[0] != '/' || strstr(path, "..") || strstr(path, "//"))
        return -1;
    if (strcmp(path, "/") == 0)
        path = "/index.html";
    if (snprintf(joined, sizeof joined, "%s%s", root, path) >= (int)sizeof joined)
        return -1;
    if (!realpath(joined, real))
        return -1;
    if (strncmp(real, root, root_len) != 0 ||
        (real[root_len] != '/' && real[root_len] != '\0'))
        return -1;                           /* escaped the root: same as missing */

    fd = open(real, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOENT && errno != ENOTDIR && errno != ELOOP)
            *status = 500;
        return -1;
    }
    if (fstat(fd, st) < 0 || !S_ISREG(st->st_mode)) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Streams the file as DATA frames, the last one carrying END. */
static int send_file(int sock, int fd, off_t size)
{
    static uint8_t chunk[OCT_DATA_CHUNK];
    off_t left = size;

    while (left > 0) {
        size_t want = left < (off_t)sizeof chunk ? (size_t)left : sizeof chunk;
        ssize_t r = oct_read_full(fd, chunk, want);
        if (r != (ssize_t)want)
            return -1;                       /* file changed under us */
        left -= r;
        if (oct_send_frame(sock, OCT_T_DATA, left == 0 ? OCT_F_END : 0,
                           chunk, want, NULL) < 0)
            return -1;
    }
    return 0;
}

/* Handles one REQUEST payload. Returns the status sent, or -1 if the
 * connection must close. 400 also closes the connection. */
static int handle_request(int sock, const uint8_t *p, size_t n, char *logpath,
                          size_t logsz)
{
    struct oct_hdr h[255];
    const char *err = NULL;
    const struct oct_hdr *m, *pa;
    char path[UINT16_MAX + 1];
    struct stat st;
    int count, status, fd;

    count = oct_hb_parse(p, n, h, &err);
    if (count < 0)
        return 400;
    m = oct_hb_find(h, count, OCT_H_METHOD);
    pa = oct_hb_find(h, count, OCT_H_PATH);
    if (!m || !pa || m->vlen != 3 || memcmp(m->value, "GET", 3) != 0)
        return 400;

    memcpy(path, pa->value, pa->vlen);
    path[pa->vlen] = '\0';
    snprintf(logpath, logsz, "%.*s", (int)logsz - 1, path);

    fd = open_under_root(path, &status, &st);
    if (fd < 0)
        return send_response(sock, status, 0, NULL, 0) < 0 ? -1 : status;

    if (send_response(sock, 200, (long long)st.st_size,
                      content_type(strcmp(path, "/") == 0 ? "/index.html" : path),
                      st.st_size > 0) < 0 ||
        send_file(sock, fd, st.st_size) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 200;
}

static void serve_connection(int sock, const char *peer)
{
    static uint8_t payload[OCT_HB_CAP];
    uint8_t raw[OCT_HDR_LEN];
    struct oct_fh fh;

    oct_set_timeout(sock, OCT_TIMEOUT_S);
    for (;;) {
        char logpath[256] = "-";
        int status;

        if (oct_read_header(sock, raw, &fh) != 1)
            return;                          /* EOF, timeout or short header */

        if (fh.version != OCT_VERSION) {
            status = 400;
        } else if (fh.type != OCT_T_REQUEST && fh.type != OCT_T_RESPONSE &&
                   fh.type != OCT_T_DATA) {
            if (oct_discard(sock, fh.len) < 0)   /* unknown type: skip it */
                return;
            continue;
        } else if (fh.type != OCT_T_REQUEST || (fh.flags & ~OCT_F_KNOWN) ||
                   !(fh.flags & OCT_F_END) || fh.len > OCT_HB_CAP) {
            status = 400;
        } else {
            if (oct_read_full(sock, payload, fh.len) != (ssize_t)fh.len)
                return;
            status = handle_request(sock, payload, fh.len, logpath, sizeof logpath);
        }

        if (status == 400)
            send_response(sock, 400, 0, NULL, 0);
        fprintf(stderr, "%s %s %d\n", peer, logpath, status < 0 ? 500 : status);
        if (status == 400 || status < 0)
            return;
    }
}

int main(int argc, char **argv)
{
    struct sockaddr_in addr = { 0 };
    struct sigaction sa = { 0 };
    struct stat st;
    char *end;
    long port;
    int lfd, one = 1;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <root> <port>\n", argv[0]);
        return 1;
    }
    if (!realpath(argv[1], root) || stat(root, &st) < 0) {
        fprintf(stderr, "bserve: root %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "bserve: root %s: not a directory\n", argv[1]);
        return 1;
    }
    root_len = strlen(root);
    if (root_len == 1)                       /* root is "/": compare against "" */
        root_len = 0, root[0] = '\0';

    errno = 0;
    port = strtol(argv[2], &end, 10);
    if (errno || *end || port < 1 || port > 65535) {
        fprintf(stderr, "bserve: bad port %s\n", argv[2]);
        return 1;
    }

    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);                /* auto-reap forked children */

    lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd < 0) {
        perror("bserve: socket");
        return 1;
    }
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(lfd, 16) < 0) {
        fprintf(stderr, "bserve: bind 0.0.0.0:%ld: %s\n", port, strerror(errno));
        return 1;
    }
    fprintf(stderr, "bserve: serving %s on 0.0.0.0:%ld\n",
            root_len ? root : "/", port);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        char name[INET_ADDRSTRLEN + 8];
        int c = accept(lfd, (struct sockaddr *)&peer, &plen);

        if (c < 0) {
            if (errno != EINTR)
                perror("bserve: accept");
            continue;
        }
        inet_ntop(AF_INET, &peer.sin_addr, name, sizeof name);
        snprintf(name + strlen(name), 8, ":%u", ntohs(peer.sin_port));

        pid_t pid = fork();                  /* one process per connection */
        if (pid == 0) {
            close(lfd);
            serve_connection(c, name);
            close(c);
            _exit(0);
        }
        if (pid < 0) {                       /* no fork: serve inline */
            serve_connection(c, name);
        }
        close(c);
    }
}
