/* octet.c: framing, header blocks and hexdump shared by bserve and bcurl. */
#include "octet.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

const char *const oct_static_table[OCT_STATIC_MAX + 1] = {
    NULL,             /* 0 reserved */
    ":method",        /* 1 */
    ":path",          /* 2 */
    "host",           /* 3 */
    "user-agent",     /* 4 */
    "accept",         /* 5 */
    ":status",        /* 6 */
    "content-length", /* 7 */
    "content-type",   /* 8 */
    "server",         /* 9 */
    "date",           /* 10 */
};

/* ---- I/O --------------------------------------------------------------- */

ssize_t oct_read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    size_t got = 0;

    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r == 0)
            break;
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        got += (size_t)r;
    }
    return (ssize_t)got;
}

int oct_write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;

    while (n > 0) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

int oct_discard(int fd, size_t n)
{
    uint8_t sink[4096];

    while (n > 0) {
        size_t want = n < sizeof sink ? n : sizeof sink;
        ssize_t r = oct_read_full(fd, sink, want);
        if (r != (ssize_t)want)
            return -1;
        n -= want;
    }
    return 0;
}

int oct_set_timeout(int fd, int seconds)
{
    struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) < 0)
        return -1;
    return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

/* ---- Frame header ------------------------------------------------------ */

/* The 24-bit length is the low three bytes of a big-endian uint32. */
void oct_fh_pack(uint8_t out[OCT_HDR_LEN], uint32_t len, uint8_t type, uint8_t flags)
{
    uint32_t be = htonl(len & OCT_MAX_LEN);

    memcpy(out, (uint8_t *)&be + 1, 3);
    out[3] = type;
    out[4] = flags;
    out[5] = OCT_VERSION;
}

void oct_fh_unpack(const uint8_t in[OCT_HDR_LEN], struct oct_fh *fh)
{
    uint32_t be = 0;

    memcpy((uint8_t *)&be + 1, in, 3);
    fh->len = ntohl(be);
    fh->type = in[3];
    fh->flags = in[4];
    fh->version = in[5];
}

int oct_read_header(int fd, uint8_t raw[OCT_HDR_LEN], struct oct_fh *fh)
{
    ssize_t r = oct_read_full(fd, raw, OCT_HDR_LEN);

    if (r == 0)
        return 0;
    if (r != OCT_HDR_LEN)
        return -1;
    oct_fh_unpack(raw, fh);
    return 1;
}

int oct_send_frame(int fd, uint8_t type, uint8_t flags, const void *payload,
                   size_t len, FILE *dump)
{
    uint8_t raw[OCT_HDR_LEN];

    if (len > OCT_MAX_LEN)
        return -1;
    oct_fh_pack(raw, (uint32_t)len, type, flags);
    if (dump)
        oct_dump_frame(dump, '>', raw, payload, len);
    if (oct_write_full(fd, raw, sizeof raw) < 0)
        return -1;
    return len ? oct_write_full(fd, payload, len) : 0;
}

/* ---- Header blocks ----------------------------------------------------- */

static int buf_reserve(struct oct_buf *b, size_t extra)
{
    if (b->len + extra <= b->cap)
        return 0;
    size_t cap = b->cap ? b->cap : 128;
    while (cap < b->len + extra)
        cap *= 2;
    uint8_t *p = realloc(b->p, cap);
    if (!p)
        return -1;
    b->p = p;
    b->cap = cap;
    return 0;
}

void oct_buf_free(struct oct_buf *b)
{
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

int oct_hb_add(struct oct_buf *b, uint8_t index, const char *name,
               const void *value, size_t vlen)
{
    size_t nlen = index ? 0 : strlen(name);
    uint16_t be;

    if (vlen > UINT16_MAX || nlen > UINT8_MAX || index > OCT_STATIC_MAX ||
        (!index && nlen == 0))
        return -1;
    if (b->len == 0) {                       /* reserve the count byte */
        if (buf_reserve(b, 1) < 0)
            return -1;
        b->p[b->len++] = 0;
    }
    if (b->p[0] == UINT8_MAX)
        return -1;
    if (buf_reserve(b, 1 + 1 + nlen + 2 + vlen) < 0)
        return -1;

    if (index) {
        b->p[b->len++] = OCT_HB_INDEXED | index;
    } else {
        b->p[b->len++] = OCT_HB_LITERAL;
        b->p[b->len++] = (uint8_t)nlen;
        memcpy(b->p + b->len, name, nlen);
        b->len += nlen;
    }
    be = htons((uint16_t)vlen);
    memcpy(b->p + b->len, &be, 2);
    b->len += 2;
    memcpy(b->p + b->len, value, vlen);
    b->len += vlen;
    b->p[0]++;
    return 0;
}

int oct_hb_add_str(struct oct_buf *b, uint8_t index, const char *value)
{
    return oct_hb_add(b, index, NULL, value, strlen(value));
}

static int printable(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] < 0x20 || p[i] > 0x7e)
            return 0;
    return 1;
}

static int valid_name(const uint8_t *p, size_t n)
{
    if (n == 0)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (p[i] <= 0x20 || p[i] > 0x7e || (p[i] >= 'A' && p[i] <= 'Z'))
            return 0;
    return 1;
}

int oct_hb_parse(const uint8_t *p, size_t n, struct oct_hdr *out, const char **err)
{
    size_t off = 0;
    int count;

#define NEED(k, why) do { if (n - off < (size_t)(k)) { *err = (why); return -1; } } while (0)

    NEED(1, "header block is empty");
    count = p[off++];
    for (int i = 0; i < count; i++) {
        struct oct_hdr *h = &out[i];
        uint16_t be;

        NEED(1, "header runs past frame");
        uint8_t form = p[off++];
        if (form & OCT_HB_INDEXED) {
            h->index = form & 0x7f;
            if (h->index == 0 || h->index > OCT_STATIC_MAX) {
                *err = "reserved static table index";
                return -1;
            }
            h->name = oct_static_table[h->index];
            h->nlen = strlen(h->name);
        } else {
            if (form != OCT_HB_LITERAL) {
                *err = "unknown header form";
                return -1;
            }
            NEED(1, "name length runs past frame");
            h->index = 0;
            h->nlen = p[off++];
            NEED(h->nlen, "name runs past frame");
            if (!valid_name(p + off, h->nlen)) {
                *err = "literal name is not lowercase printable ASCII";
                return -1;
            }
            h->name = (const char *)(p + off);
            off += h->nlen;
        }
        NEED(2, "value length runs past frame");
        memcpy(&be, p + off, 2);
        off += 2;
        h->vlen = ntohs(be);
        NEED(h->vlen, "value runs past frame");
        h->value = p + off;
        if (!printable(h->value, h->vlen)) {
            *err = "header value is not printable ASCII";
            return -1;
        }
        off += h->vlen;
    }
    if (off != n) {
        *err = "trailing bytes after header block";
        return -1;
    }
    return count;
#undef NEED
}

const struct oct_hdr *oct_hb_find(const struct oct_hdr *h, int count, uint8_t index)
{
    const char *name = oct_static_table[index];
    size_t nlen = strlen(name);

    for (int i = 0; i < count; i++)
        if (h[i].index == index ||
            (h[i].index == 0 && h[i].nlen == nlen && memcmp(h[i].name, name, nlen) == 0))
            return &h[i];
    return NULL;
}

/* ---- Hexdump ----------------------------------------------------------- */

const char *oct_type_name(uint8_t type)
{
    switch (type) {
    case OCT_T_REQUEST:  return "REQUEST";
    case OCT_T_RESPONSE: return "RESPONSE";
    case OCT_T_DATA:     return "DATA";
    default:             return NULL;
    }
}

static uint8_t frame_byte(const uint8_t *raw, const uint8_t *payload, size_t i)
{
    return i < OCT_HDR_LEN ? raw[i] : payload[i - OCT_HDR_LEN];
}

void oct_dump_frame(FILE *f, char dir, const uint8_t raw[OCT_HDR_LEN],
                    const uint8_t *payload, size_t len)
{
    struct oct_fh fh;
    const char *name;
    size_t total = OCT_HDR_LEN + len;

    oct_fh_unpack(raw, &fh);
    name = oct_type_name(fh.type);
    if (name)
        fprintf(f, "%c %s len=%u flags=", dir, name, fh.len);
    else
        fprintf(f, "%c UNKNOWN(0x%02x) len=%u flags=", dir, fh.type, fh.len);
    if (fh.flags == OCT_F_END)
        fputs("END", f);
    else if (fh.flags)
        fprintf(f, "0x%02x", fh.flags);
    fprintf(f, " v=%u\n", fh.version);

    for (size_t row = 0; row < total; row += 16) {
        fprintf(f, "%08zx ", row);
        for (size_t i = row; i < row + 16; i++) {
            if (i % 8 == 0)
                fputc(' ', f);
            if (i < total)
                fprintf(f, "%02x ", frame_byte(raw, payload, i));
            else
                fputs("   ", f);
        }
        fputs(" |", f);
        for (size_t i = row; i < row + 16 && i < total; i++) {
            uint8_t c = frame_byte(raw, payload, i);
            fputc(c >= 0x20 && c <= 0x7e ? c : '.', f);
        }
        fputs("|\n", f);
    }
    fprintf(f, "%08zx\n", total);
}
