# Octet v1 wire specification

Octet carries HTTP-style GET requests over one persistent TCP connection. This
document is the only contract between client and server; MUST, MUST NOT and
SHOULD are as in RFC 2119. Out of scope: TLS, compression, non-GET methods,
multiplexing, authentication.

## 1. Frames

A frame is a 6-byte header, then exactly `length` payload bytes; integers are
big-endian.

| Offset | Width   | Field     | Meaning                                           |
|-------:|--------:|-----------|---------------------------------------------------|
| 0      | 24 bits | `length`  | Payload bytes after the header, 0 to 16,777,215   |
| 3      | 8 bits  | `type`    | 0x01 REQUEST, 0x02 RESPONSE, 0x03 DATA            |
| 4      | 8 bits  | `flags`   | bit 0 (0x01) = END; all other bits MUST be 0      |
| 5      | 8 bits  | `version` | 0x01                                              |

**Why these widths.** 24 bits of length matches HTTP/2. 16 MiB exceeds any
single frame a file server needs, but it is small enough that a hostile length
cannot make a receiver allocate gigabytes. An 8-bit type leaves 253 unused
types for later versions. 8 bits of flags cost nothing and leave room for
future bits. Octet has no stream ID, because it allows only one request at a
time per connection. The version byte takes the place HTTP/2 gives its 31-bit
stream ID.

**Unknown types.** A receiver that reads a `type` it does not know MUST read
and discard exactly `length` bytes, then continue with the next frame. It MUST
NOT close the connection or reply 400. Its flags are unchecked, as a later
version may define them. This lets v2 peers talk to v1.

The client sends REQUEST (a header block, END always set). The server answers
with one RESPONSE (a header block, END set only if no body follows), then zero
or more DATA frames (file bytes), the last with END. Any body MAY be split;
over 16 MiB it MUST be.

## 2. Header block

A header block is a 1-byte header count followed by that many headers. It MUST
fill the payload exactly, and its frame MUST NOT exceed 64 KiB.

| Form    | Encoding                                                            |
|---------|---------------------------------------------------------------------|
| Indexed | `0x80` ORed with index 1 to 10 (1 byte), then value: `uint16` length + bytes |
| Literal | `0x00`, then name: `uint8` length + bytes, then value: `uint16` length + bytes |

Static table. Request names: 1 `:method`, 2 `:path`, 3 `host`, 4 `user-agent`,
5 `accept`. Response names: 6 `:status`, 7 `content-length`, 8 `content-type`,
9 `server`, 10 `date`.

Index 0 and 11 to 127 are reserved; no dynamic table, no Huffman. Literal
names MUST be 1 to 255 bytes of lowercase printable ASCII (0x21 to 0x7E, no
`A` to `Z`); values MUST be printable ASCII (0x20 to 0x7E) or empty. A literal
static name is that header; if repeated, the first wins.

**Request.** MUST have `:method` `GET` and `:path`; SHOULD have `host`,
`user-agent`, `accept`.

**Response.** MUST contain `:status`, exactly three decimal digits (200, 400,
404, 500), and `content-length`, the decimal count of DATA bytes that follow.
A 200 carries `content-type`; `server` and `date` SHOULD be sent.

## 3. Exchange

    client                                  server
      |-- TCP connect (once) ------------------>|
      |-- REQUEST [END] :method :path host ...->|
      |<- RESPONSE :status 200 content-length --|
      |<- DATA ... DATA [END] ------------------|
      |-- REQUEST [END] (same connection) ----->|
      |<- RESPONSE [END] :status 404 -----------|

The client MUST NOT open a second connection. Read 6 bytes, then `length`
bytes, looping on short reads.

## 4. Server behaviour

| Condition                                        | Response                          |
|--------------------------------------------------|-----------------------------------|
| `:path` resolves to a regular file under root    | `:status 200`, then DATA          |
| `:path` is `/`                                   | serve `/index.html`               |
| path rejected, missing, or not a regular file    | `:status 404`, END                |
| malformed frame (section 5)                      | `:status 400`, END, then close    |
| read error or other failure before the response  | `:status 500`, END                |

Except after 400, the connection stays open until 30 s of silence or a file
error mid-body.

**Path handling.** A path MUST start with `/` and MUST NOT contain `..` or
`//` (NUL is excluded by the value rules). The server canonicalises `root +
path` with `realpath()` and serves it only if it is a regular file at or under
the canonical root. Rejected, escaping and missing paths all get 404, so a
client cannot learn whether a file outside the root exists.

## 5. Malformed frames

A server replies 400 and closes when: `version` is not 0x01; a known type sets
reserved flag bits; the client sends RESPONSE, DATA, or REQUEST without END; a
REQUEST exceeds 64 KiB (checked before allocating); a count or length runs
past the frame, or bytes are left over; an index is 0 or above 10, or a form
byte is neither `0x00` nor `0x8X`; a name or value breaks the character rules;
or `:method` or `:path` is missing, or the method is not `GET`.

A client applies the same version, flag and header-block rules and exits 2 on
any violation (e.g. DATA before RESPONSE), early close, or 30 s of silence.

## 6. Command line

Exit codes of `bserve <root> <port>` and `bcurl [-v] <host>:<port>/<path>`:

| Program | 0             | 1                          | 2                                 |
|---------|---------------|----------------------------|-----------------------------------|
| bserve  | SIGTERM       | bad root or port           |                                   |
| bcurl   | `:status` 2xx | `:status` 4xx/5xx          | connect failure, early close, malformed reply |
