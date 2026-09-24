# Annotated hexdump: one request and response

Captured with:

    ./bserve ./www 9124 &
    ./bcurl -v localhost:9124/hello.txt

Every byte below is explained by [SPEC.md](../SPEC.md). Section numbers
(for example §1) refer to that document. The `date` value changes on every
run. Nothing else does.

## Raw capture

```
> REQUEST len=53 flags=END v=1
00000000  00 00 35 01 01 01 05 81  00 03 47 45 54 82 00 0a  |..5.......GET...|
00000010  2f 68 65 6c 6c 6f 2e 74  78 74 83 00 0e 6c 6f 63  |/hello.txt...loc|
00000020  61 6c 68 6f 73 74 3a 39  31 32 34 84 00 07 62 63  |alhost:9124...bc|
00000030  75 72 6c 2f 31 85 00 03  2a 2f 2a                 |url/1...*/*|
0000003b
< RESPONSE len=83 flags= v=1
00000000  00 00 53 02 00 01 05 86  00 03 32 30 30 87 00 02  |..S.......200...|
00000010  31 33 88 00 19 74 65 78  74 2f 70 6c 61 69 6e 3b  |13...text/plain;|
00000020  20 63 68 61 72 73 65 74  3d 75 74 66 2d 38 89 00  | charset=utf-8..|
00000030  08 62 73 65 72 76 65 2f  31 8a 00 1d 54 68 75 2c  |.bserve/1...Thu,|
00000040  20 32 34 20 53 65 70 20  32 30 32 36 20 32 30 3a  | 24 Sep 2026 20:|
00000050  33 37 3a 33 36 20 47 4d  54                       |37:36 GMT|
00000059
< DATA len=13 flags=END v=1
00000000  00 00 0d 03 01 01 68 65  6c 6c 6f 2c 20 6f 63 74  |......hello, oct|
00000010  65 74 0a                                          |et.|
00000013
```

## Frame 1: REQUEST, client to server (59 bytes)

| Offset | Bytes | Field | Meaning |
|---|---|---|---|
| 0x00 | `00 00 35` | length (§1) | 53 payload bytes follow the header |
| 0x03 | `01` | type (§1) | REQUEST |
| 0x04 | `01` | flags (§1) | END: the request is this one frame, as REQUEST requires |
| 0x05 | `01` | version (§1) | Octet v1 |
| 0x06 | `05` | count (§2) | 5 headers follow |
| 0x07 | `81` | form (§2) | indexed, `0x80 \| 1`: `:method` |
| 0x08 | `00 03` | value length | 3 bytes |
| 0x0a | `47 45 54` | value | `GET`, the only method allowed (§2 Request) |
| 0x0d | `82` | form | indexed, `0x80 \| 2`: `:path` |
| 0x0e | `00 0a` | value length | 10 bytes |
| 0x10 | `2f 68 65 6c 6c 6f 2e 74 78 74` | value | `/hello.txt`, starts with `/` (§4 Path handling) |
| 0x1a | `83` | form | indexed, `0x80 \| 3`: `host` |
| 0x1b | `00 0e` | value length | 14 bytes |
| 0x1d | `6c 6f 63 61 6c 68 6f 73 74 3a 39 31 32 34` | value | `localhost:9124` |
| 0x2b | `84` | form | indexed, `0x80 \| 4`: `user-agent` |
| 0x2c | `00 07` | value length | 7 bytes |
| 0x2e | `62 63 75 72 6c 2f 31` | value | `bcurl/1` |
| 0x35 | `85` | form | indexed, `0x80 \| 5`: `accept` |
| 0x36 | `00 03` | value length | 3 bytes |
| 0x38 | `2a 2f 2a` | value | `*/*` |
| 0x3b | | end | 6 + 53 = 59 bytes. The block fills the payload exactly (§2) |

Payload check: 1 (count) + 6 (`:method`) + 13 (`:path`) + 17 (`host`) +
10 (`user-agent`) + 6 (`accept`) = 53 = `0x35`.

## Frame 2: RESPONSE, server to client (89 bytes)

| Offset | Bytes | Field | Meaning |
|---|---|---|---|
| 0x00 | `00 00 53` | length | 83 payload bytes |
| 0x03 | `02` | type | RESPONSE |
| 0x04 | `00` | flags | END clear: DATA frames follow (§1). bcurl prints this as `flags=` |
| 0x05 | `01` | version | v1 |
| 0x06 | `05` | count | 5 headers |
| 0x07 | `86` | form | indexed 6: `:status` |
| 0x08 | `00 03` | value length | 3 |
| 0x0a | `32 30 30` | value | `200`: a regular file under the root (§4) |
| 0x0d | `87` | form | indexed 7: `content-length` |
| 0x0e | `00 02` | value length | 2 |
| 0x10 | `31 33` | value | `13`: exactly the DATA bytes that follow (§2 Response) |
| 0x12 | `88` | form | indexed 8: `content-type` |
| 0x13 | `00 19` | value length | 25 |
| 0x15 | `74 65 ... 2d 38` | value | `text/plain; charset=utf-8`, taken from the `.txt` extension |
| 0x2e | `89` | form | indexed 9: `server` |
| 0x2f | `00 08` | value length | 8 |
| 0x31 | `62 73 65 72 76 65 2f 31` | value | `bserve/1` |
| 0x39 | `8a` | form | indexed 10: `date` |
| 0x3a | `00 1d` | value length | 29 |
| 0x3c | `54 68 ... 4d 54` | value | `Thu, 24 Sep 2026 20:37:36 GMT` (IMF-fixdate, UTC) |
| 0x59 | | end | 6 + 83 = 89 bytes |

Payload check: 1 + 6 + 5 + 28 + 11 + 32 = 83 = `0x53`.

## Frame 3: DATA, server to client (19 bytes)

| Offset | Bytes | Field | Meaning |
|---|---|---|---|
| 0x00 | `00 00 0d` | length | 13 payload bytes |
| 0x03 | `03` | type | DATA |
| 0x04 | `01` | flags | END: the last DATA frame, so the response is complete (§1) |
| 0x05 | `01` | version | v1 |
| 0x06 | `68 65 6c 6c 6f 2c 20 6f 63 74 65 74 0a` | payload | `hello, octet\n`, raw file bytes with no header block |
| 0x13 | | end | 6 + 13 = 19 bytes. Total DATA (13) equals `content-length` (13) |

After END the client has a complete response. It writes the 13 body bytes to
stdout and exits 0 because the status is 2xx. The connection stays open, so
the server is already waiting for the next REQUEST.
