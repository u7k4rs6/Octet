/* octet.h: Octet wire format (see SPEC.md). Shared by bserve and bcurl. */
#ifndef OCTET_H
#define OCTET_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#define OCT_HDR_LEN     6
#define OCT_VERSION     0x01
#define OCT_MAX_LEN     0xFFFFFFu          /* 24-bit length field */

#define OCT_T_REQUEST   0x01
#define OCT_T_RESPONSE  0x02
#define OCT_T_DATA      0x03

#define OCT_F_END       0x01
#define OCT_F_KNOWN     OCT_F_END          /* every other bit is reserved */

#define OCT_HB_INDEXED  0x80               /* 0x80 | index */
#define OCT_HB_LITERAL  0x00
#define OCT_STATIC_MAX  10

#define OCT_HB_CAP      (64 * 1024)        /* cap on a header-block payload */
#define OCT_DATA_CHUNK  (64 * 1024)        /* DATA payload size bserve sends */
#define OCT_TIMEOUT_S   30                 /* idle read timeout */

/* Static table indices (SPEC.md, "Static table"). */
enum {
    OCT_H_METHOD = 1, OCT_H_PATH, OCT_H_HOST, OCT_H_USER_AGENT, OCT_H_ACCEPT,
    OCT_H_STATUS, OCT_H_CONTENT_LENGTH, OCT_H_CONTENT_TYPE, OCT_H_SERVER,
    OCT_H_DATE
};

extern const char *const oct_static_table[OCT_STATIC_MAX + 1];

/* Decoded 6-byte frame header. */
struct oct_fh {
    uint32_t len;
    uint8_t  type;
    uint8_t  flags;
    uint8_t  version;
};

/* One decoded header; name/value point into the frame payload or the
 * static table and are NOT NUL-terminated. */
struct oct_hdr {
    uint8_t        index;          /* 1..10, or 0 for a literal name */
    const char    *name;
    size_t         nlen;
    const uint8_t *value;
    size_t         vlen;
};

/* Growable byte buffer used to build header blocks. */
struct oct_buf {
    uint8_t *p;
    size_t   len, cap;
};

/* I/O. read_full returns bytes read (< n only on EOF), or -1 on error. */
ssize_t oct_read_full(int fd, void *buf, size_t n);
int     oct_write_full(int fd, const void *buf, size_t n);
int     oct_discard(int fd, size_t n);
int     oct_set_timeout(int fd, int seconds);

/* Frame header. read_header returns 1 on success, 0 on clean EOF before
 * any byte, -1 on error or a short header. */
void oct_fh_pack(uint8_t out[OCT_HDR_LEN], uint32_t len, uint8_t type, uint8_t flags);
void oct_fh_unpack(const uint8_t in[OCT_HDR_LEN], struct oct_fh *fh);
int  oct_read_header(int fd, uint8_t raw[OCT_HDR_LEN], struct oct_fh *fh);
int  oct_send_frame(int fd, uint8_t type, uint8_t flags, const void *payload,
                    size_t len, FILE *dump);

/* Header blocks. The builder reserves the count byte on first use. */
void oct_buf_free(struct oct_buf *b);
int  oct_hb_add(struct oct_buf *b, uint8_t index, const char *name,
                const void *value, size_t vlen);
int  oct_hb_add_str(struct oct_buf *b, uint8_t index, const char *value);

/* Parses a header block that must fill exactly n bytes. Returns the header
 * count, or -1 with *err set to a short reason. out must hold 255 entries. */
int  oct_hb_parse(const uint8_t *p, size_t n, struct oct_hdr *out, const char **err);
const struct oct_hdr *oct_hb_find(const struct oct_hdr *h, int count, uint8_t index);

/* hexdump -C style frame dump, direction '>' (sent) or '<' (received). */
void oct_dump_frame(FILE *f, char dir, const uint8_t raw[OCT_HDR_LEN],
                    const uint8_t *payload, size_t len);
const char *oct_type_name(uint8_t type);

#endif
