# Annotated exchange: GET /index.html

Captured from `./bcurl -v localhost:9000/index.html` against `./bserve ./www 9000`. The client sends 63 bytes in one frame. The server answers with 178 bytes in two frames.

## Frame 1: client to server, REQUEST

```
00 00 37  01  01  00  00 01
01 00 0b 2f 69 6e 64 65 78 2e 68 74 6d 6c 01 00
0e 6c 6f 63 61 6c 68 6f 73 74 3a 39 30 30 30 02
00 07 62 63 75 72 6c 2f 31 03 00 03 2a 2f 2a 04
00 05 63 6c 6f 73 65
```

| Bytes | Field | Value |
|---|---|---|
| `00 00 37` | Length | 55 payload bytes |
| `01` | Type | REQUEST |
| `01` | Flags | END, so no body follows |
| `00` | Reserved | 0 |
| `00 01` | Request ID | 1 |
| `01` | Method | GET |
| `00 0b` | Path length | 11 |
| `2f 69 6e 64 65 78 2e 68 74 6d 6c` | Path | `/index.html` |
| `01` | Header index | 1 = host |
| `00 0e` | Value length | 14 |
| `6c 6f 63 61 6c 68 6f 73 74 3a 39 30 30 30` | Value | `localhost:9000` |
| `02` | Header index | 2 = user-agent |
| `00 07` + `62 63 75 72 6c 2f 31` | Value | `bcurl/1` |
| `03` | Header index | 3 = accept |
| `00 03` + `2a 2f 2a` | Value | `*/*` |
| `04` | Header index | 4 = connection |
| `00 05` + `63 6c 6f 73 65` | Value | `close` |

Payload check: 1 (method) + 13 (path) + 17 + 10 + 6 + 8 (headers) = 55.

## Frame 2: server to client, RESPONSE

```
00 00 6d  02  00  00  00 01
00 c8 08 00 1d 54 68 75 2c 20 30 38 20 4f 63 74
20 32 30 32 36 20 31 32 3a 32 32 3a 34 30 20 47
4d 54 09 00 08 62 73 65 72 76 65 2f 31 05 00 18
74 65 78 74 2f 68 74 6d 6c 3b 20 63 68 61 72 73
65 74 3d 75 74 66 2d 38 06 00 02 35 33 07 00 1d
54 68 75 2c 20 30 38 20 4f 63 74 20 32 30 32 36
20 31 32 3a 32 31 3a 30 38 20 47 4d 54
```

| Bytes | Field | Value |
|---|---|---|
| `00 00 6d` | Length | 109 payload bytes |
| `02` | Type | RESPONSE |
| `00` | Flags | END not set, so DATA follows |
| `00` | Reserved | 0 |
| `00 01` | Request ID | 1, copied from the request |
| `00 c8` | Status | 200 |
| `08` + `00 1d` + 29 bytes | date | `Thu, 08 Oct 2026 12:22:40 GMT` |
| `09` + `00 08` + 8 bytes | server | `bserve/1` |
| `05` + `00 18` + 24 bytes | content-type | `text/html; charset=utf-8` |
| `06` + `00 02` + `35 33` | content-length | `53` |
| `07` + `00 1d` + 29 bytes | last-modified | `Thu, 08 Oct 2026 12:21:08 GMT` |

Payload check: 2 (status) + 32 + 11 + 27 + 5 + 32 = 109.

## Frame 3: server to client, DATA

```
00 00 35  03  01  00  00 01
3c 21 64 6f 63 74 79 70 65 20 68 74 6d 6c 3e 0a
3c 74 69 74 6c 65 3e 62 73 65 72 76 65 3c 2f 74
69 74 6c 65 3e 0a 3c 68 31 3e 68 65 6c 6c 6f 3c
2f 68 31 3e 0a
```

| Bytes | Field | Value |
|---|---|---|
| `00 00 35` | Length | 53, matching content-length |
| `03` | Type | DATA |
| `01` | Flags | END, the response is complete |
| `00` | Reserved | 0 |
| `00 01` | Request ID | 1 |
| 53 bytes | Body | `<!doctype html>\n<title>bserve</title>\n<h1>hello</h1>\n` |

Because the request carried `connection: close`, the server closes the connection after this frame. Without it, the client could send request 2 on the same connection.
