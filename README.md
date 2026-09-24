# Octet: HTTP, in binary

Octet is a small binary HTTP-like protocol. It has fixed 6-byte frame headers
instead of CRLF delimiters, and HPACK-style indexed and literal headers, over
one persistent TCP connection. It is implemented by two C programs:

- **`bserve`**: serves files from a directory.
- **`bcurl`**: fetches them, and with `-v` hexdumps every frame.

The two programs share only [SPEC.md](SPEC.md). A client built from the spec
alone must work with `bserve`, and a server built from it must work with
`bcurl`.

## Build and run

Requires a C11 compiler and make on Linux or another POSIX system. The tests
also need bash and python3.

    make
    ./bserve ./www 9000 &
    ./bcurl localhost:9000/index.html
    ./bcurl -v localhost:9000/hello.txt          # hexdump every frame to stderr
    ./bcurl localhost:9000/ /hello.txt /nope     # three requests, one connection
    make test

## Deliverables

| # | Item | Where |
|---|------|-------|
| 1 | The spec, two pages | [SPEC.md](SPEC.md) |
| 2 | The programs | [`src/bserve.c`](src/bserve.c), [`src/bcurl.c`](src/bcurl.c), shared wire code in [`src/octet.c`](src/octet.c) |
| 3 | Annotated hexdump of one request and response | [docs/annotated-hexdump.md](docs/annotated-hexdump.md) |

## The wire format at a glance

    +--------+--------+--------+--------+--------+--------+-------------------+
    |        length (24 bits)  |  type  | flags  |version |  length bytes ... |
    +--------+--------+--------+--------+--------+--------+-------------------+
      type: 01 REQUEST  02 RESPONSE  03 DATA      flags: 01 END     version: 01

    header block := count:u8  header*count
    header       := 0x80|index  vlen:u16 value          (index 1..10)
                  | 0x00  nlen:u8 name  vlen:u16 value

A response is one RESPONSE frame followed by DATA frames, and the last frame
carries END. An unknown frame type is skipped, which lets a v2 peer talk to a
v1 peer. The full rules, including what counts as malformed and how paths
are confined to the root, are in [SPEC.md](SPEC.md).

## Exit codes

| Program | 0 | 1 | 2 |
|---------|---|---|---|
| bserve | SIGTERM | root missing, or port cannot be bound | |
| bcurl | `:status` 2xx | `:status` 4xx or 5xx | connect failure, early close, or malformed response |

## Tests

`make test` runs `tests/run.sh`, which covers:

- bcurl against bserve: keep-alive, `/`, 404, empty files, and a 17 MiB file
  split across DATA frames.
- bserve against [`tests/wire.py`](tests/wire.py), an independent Python
  implementation written from the spec. It checks every malformed-frame rule,
  unknown-type skipping, 1-byte TCP reads, pipelining, path traversal and
  symlink escape.
- bcurl against a fake server in the same file that sends crafted
  responses.
- Process behaviour: logging, exit codes, and shutdown on SIGTERM.

## Layout

    SPEC.md                    wire specification (the contract)
    docs/annotated-hexdump.md  deliverable 3
    src/octet.h, src/octet.c   frames, header blocks, hexdump
    src/bserve.c               server, forks one process per connection
    src/bcurl.c                client, never opens a second connection
    tests/                     run.sh (end-to-end) and wire.py (independent peer)
    www/                       sample document root
