# Octet v1 wire specification

Octet carries HTTP-style GET requests for files over one persistent TCP
connection. It uses fixed-size binary frames instead of text delimiters, and
the first two HPACK mechanisms (a static table and literals) for headers.
This document is the only contract between a client and a server. The key
words MUST, MUST NOT and SHOULD are used as in RFC 2119.

## 1. Frames

A connection carries a sequence of frames in both directions. Every frame is
a 6-byte header followed by exactly `length` payload bytes. All multi-byte
integers are unsigned and big-endian (network byte order).

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
time per connection. The version byte takes the place that HTTP/2 gives to its
31-bit stream ID.

**Unknown types.** A receiver that reads a `type` it does not know MUST read and
discard exactly `length` bytes, then continue with the next frame. It MUST NOT
close the connection or reply 400. The flags of an unknown type are not
checked, because a later version may define them. This rule is what lets a
v2 peer talk to a v1 peer.

| Type           | Sent by | Payload                                           |
|----------------|---------|---------------------------------------------------|
| REQUEST (0x01) | client  | header block; END MUST be set                     |
| RESPONSE (0x02)| server  | header block; END set only if no body follows     |
| DATA (0x03)    | server  | raw file bytes; the last DATA frame sets END      |

A response is one RESPONSE frame followed by zero or more DATA frames. A body
of any size MAY be split across any number of DATA frames; bodies over 16 MiB
MUST be split. An empty file is a RESPONSE with END and no DATA.

## 2. Header block

A header block is a 1-byte header count followed by that many headers. It MUST
fill the payload exactly: no bytes may remain after the last header.

| Form    | Encoding                                                            |
|---------|---------------------------------------------------------------------|
| Indexed | `0x80 \| index` (1 byte, index 1 to 10), then value: `uint16` length + bytes |
| Literal | `0x00`, then name: `uint8` length + bytes, then value: `uint16` length + bytes |

| Index | Name             | Index | Name             |
|------:|------------------|------:|------------------|
| 1     | `:method`        | 6     | `:status`        |
| 2     | `:path`          | 7     | `content-length` |
| 3     | `host`           | 8     | `content-type`   |
| 4     | `user-agent`     | 9     | `server`         |
| 5     | `accept`         | 10    | `date`           |

Index 0 and 11 to 127 are reserved. There is no dynamic table and no Huffman
coding. Literal names MUST be 1 to 255 bytes of lowercase printable ASCII
(0x21 to 0x7E, no `A` to `Z`). Values MUST be printable ASCII (0x20 to 0x7E)
and MAY be empty. A literal whose name equals a static name means the same
header. If a header repeats, the first occurrence wins. Senders SHOULD use
the indexed form for the ten static names.

**Request.** It MUST contain `:method` with the value `GET`, and `:path`.
Clients SHOULD send `host` (as `host:port`), `user-agent` and `accept`.

**Response.** It MUST contain `:status`, which is three decimal digits: 200,
400, 404 or 500. It MUST contain `content-length`, in decimal, equal to the
total number of DATA bytes that follow (0 when there is no body). A 200
response carries `content-type`. `server` and `date` (IMF-fixdate, for
example `Thu, 24 Sep 2026 20:37:36 GMT`) SHOULD be sent.

## 3. Exchange

    client                                  server
      |-- TCP connect (once) ------------------>|
      |-- REQUEST [END] :method :path host ...->|
      |<- RESPONSE :status 200 content-length --|
      |<- DATA ... DATA [END] ------------------|
      |-- REQUEST [END] (same connection) ----->|
      |<- RESPONSE [END] :status 404 -----------|

The server loops `accept -> (read frame -> reply)* -> close`. The client does
`connect -> write REQUEST -> read RESPONSE and DATA until END`. It MAY repeat
this on the same connection, and it MUST NOT open a second connection. Frames
are read by reading exactly 6 bytes, decoding `length`, then reading exactly
`length` bytes, looping on short reads. TCP provides ordering and delivery.
Octet only decides where one message ends.

## 4. Server behaviour

| Condition                                        | Response                          |
|--------------------------------------------------|-----------------------------------|
| `:path` resolves to a regular file under root    | `:status 200`, then DATA          |
| `:path` is `/`                                   | serve `/index.html`               |
| path rejected, missing, or not a regular file    | `:status 404`, END                |
| malformed frame (section 5)                      | `:status 400`, END, then close    |
| read error or other failure before the response  | `:status 500`, END                |

After every response except 400, the server keeps the connection open. It
closes the connection after 30 s of silence. If the file fails
mid-body, after the RESPONSE has gone out, the server closes the connection.

**Path handling.** A path MUST start with `/` and MUST NOT contain `..` or
`//` anywhere (NUL is already excluded by the value rules). The server joins
`root + path`, canonicalises the result with `realpath()`, and checks that it
is the canonical root or lies under it. It serves only regular files.
Rejected, escaping and missing paths all get 404, so a client cannot learn
whether a file outside the root exists.

## 5. Malformed frames

A server replies 400 and closes when:
the `version` is not 0x01; a known type has reserved flag bits set; the client
sends a RESPONSE, a DATA, or a REQUEST without END; a REQUEST is longer than
64 KiB (checked before allocating); any count or length runs past the frame,
or bytes are left over; an index is 0 or greater than 10, or a form byte is
neither `0x00` nor `0x8X`; a name or value breaks the character rules; or
`:method` or `:path` is missing, or the method is not `GET`.

A client treats the following as a protocol error and exits 2: the same
version, flag and header-block violations; a RESPONSE longer than 64 KiB; a REQUEST from the server; DATA
before RESPONSE; a second RESPONSE before END; a missing or invalid
`:status`; DATA bytes that differ from `content-length`; the connection
closing before END; and no frame for 30 s. The client never trusts
`content-length` beyond the DATA actually received.

## 6. Command line

| Program | Usage                                  | Exit codes                                   |
|---------|----------------------------------------|----------------------------------------------|
| bserve  | `bserve <root> <port>`                 | 0 on SIGTERM; 1 if root is missing or port cannot be bound |
| bcurl   | `bcurl [-v] <host>:<port>/<path> [/<path> ...]` | 0 on 2xx; 1 on 4xx/5xx; 2 on connect failure, early close or malformed response |

bserve binds `0.0.0.0:<port>` and logs one line per request to stderr: client
address, path and status. bcurl writes the response bodies, unmodified, to
stdout. With `-v` it writes every frame it sends (`>`) and receives (`<`) to
stderr, as a `TYPE len=N flags=END v=1` line followed by a `hexdump -C` style
dump. Extra paths are fetched in order over the same connection.

Out of scope: TLS, compression, methods other than GET, multiplexing, and
authentication.
