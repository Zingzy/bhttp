# BHTTP/1: HTTP in binary frames

## 1. Overview

A client opens one TCP connection to a server and sends requests on it one at a time. Each request is a REQUEST frame, optionally followed by DATA frames. The server answers each request, in order, with a RESPONSE frame followed by DATA frames. The connection stays open until either side closes it.

Two rules apply everywhere in this document:

- Every integer is unsigned and big-endian (network byte order).
- A **str16** is a 2-byte length followed by that many bytes of UTF-8. There is no terminator. The maximum is 65,535 bytes.

The key words MUST, MUST NOT and MAY mean what they mean in RFC 2119.

## 2. Frame header

Every frame starts with the same 8 bytes:

```
byte     0      1      2      3      4      5      6      7
      +------+------+------+------+------+------+------+------+
      |  Length (24 bits)  | Type |Flags | Rsvd | Request ID  |
      +------+------+------+------+------+------+------+------+
```

| Field | Bytes | Meaning |
|---|---|---|
| Length | 0-2 | Number of payload bytes after the header, 0 to 16,777,215. Does not include the 8 header bytes. |
| Type | 3 | What the payload contains (section 3). |
| Flags | 4 | Bit `0x01` is **END**: this is the last frame of the message. Senders MUST set all other bits to 0; receivers MUST ignore them. |
| Reserved | 5 | Senders MUST send 0; receivers MUST ignore it. |
| Request ID | 6-7 | Which request this frame belongs to. The client picks a nonzero ID for each request, starting at 1 and adding 1 per request (65,535 wraps to 1). The server MUST copy that ID into every frame of its response. ID 0 means the connection as a whole; no v1 frame uses it. |

## 3. Frame types and messages

| Type | Name | Payload |
|---|---|---|
| `0x01` | REQUEST | method (1 byte), path (str16), header block |
| `0x02` | RESPONSE | status code (2 bytes), header block |
| `0x03` | DATA | body bytes, any length |

A **message** is one REQUEST or RESPONSE frame followed by zero or more DATA frames, all with the same request ID. The last frame of the message has END set. If the REQUEST or RESPONSE frame itself has END set, there is no body. A sender MAY split a body into as many DATA frames as it likes; bserve uses 16,384 bytes per frame.

Methods: `1` GET, `2` HEAD, `3` POST, `4` PUT, `5` DELETE.

### Unknown frame types (MUST)

A receiver that reads a frame with a type it does not know MUST read and discard exactly Length bytes of payload, then carry on with the next frame. It MUST NOT reply, close the connection, or treat the frame as an error. This holds at any point on the connection, including between the frames of one message.

This rule is what makes a version 2 possible. Length sits at the same position in every frame and always counts the whole payload, so a v1 receiver can step over any frame type added later.

## 4. Header block

A header block fills the rest of the payload after the fixed fields. It has no count; the receiver reads entries until the payload ends. Each entry is:

```
index (1 byte) | [name (str16), only if index = 0] | value (str16)
```

If the index is between 1 and 10, the name comes from this table. A sender MUST use the index whenever the name is in the table.

| Index | Name | Index | Name |
|---|---|---|---|
| 1 | host | 6 | content-length |
| 2 | user-agent | 7 | last-modified |
| 3 | accept | 8 | date |
| 4 | connection | 9 | server |
| 5 | content-type | 10 | allow |

Index 0 means the name follows as a str16. Literal names MUST be lowercase and MUST NOT be empty. Indexes 11 to 255 are reserved for future versions; a receiver MUST read the value and drop the entry.

## 5. Server behaviour

The path MUST start with `/`. The server ignores everything from the first `?`. It maps the path onto its root directory; a path naming a directory serves `index.html` inside it. A path that resolves outside the root gets 404.

| Status | When |
|---|---|
| 200 | GET or HEAD for a file that exists. |
| 400 | The frame is malformed: a str16 or fixed field runs past the end of the payload, a literal header name is empty, the path is empty or does not start with `/`, the request ID is 0, or the client sent a RESPONSE frame. |
| 404 | No such file, or the path escapes the root. |
| 405 | POST, PUT or DELETE. The response carries `allow: GET, HEAD`. |
| 501 | Any other method code. |

After a 400 the connection stays open. The bad frame's header still said how long it was, so the server knows where the next frame starts. The one case the server cannot recover from is the connection closing partway through a frame; it then closes without replying.

Every response carries `date` and `server`. A 200 also carries `content-type`, `content-length` and `last-modified`. Error responses carry a short `text/plain` body such as `404 Not Found`. A response to HEAD has the same headers as GET, sets END on the RESPONSE frame and sends no DATA.

v1 servers accept no request bodies and discard DATA frames from the client. If a request carries `connection: close`, the server closes the connection after answering it.

## 6. Client behaviour

The client opens exactly one connection and never a second. It sends one request, reads frames until the response's END, then sends the next. It MUST check that the response's request ID matches. A RESPONSE or DATA frame where it does not belong is a protocol error. bcurl sends `connection: close` on its last request, writes bodies to stdout, and exits 0 on success, 4 if any response was 4xx, 5 if any was 5xx, and 1 on a network or protocol error.

## 7. Why these widths

HTTP/2's header is Length 24, Type 8, Flags 8, then 1 reserved bit and a 31-bit stream ID, for 9 bytes. BHTTP/1 keeps the first three and changes the rest.

- **8 bytes, every field on a byte boundary.** You can read a frame in a hexdump by eye, and parsing is byte shifts with no bit masks.
- **Length, 24 bits (16 MiB max).** 16 bits would cap frames at 64 KiB, which is fine for DATA but tight for a header block full of long values. 32 bits would let a single 4 GiB frame tie up the connection, since frames cannot be interleaved. 24 bits is the same ceiling HTTP/2 settled on.
- **Type, 8 bits.** v1 uses 3 of 256 values. The rest are for v2, and the skip rule means old peers survive them.
- **Flags, 8 bits.** v1 uses one bit. Per-type yes/no markers cost nothing to add later.
- **Reserved, 8 bits.** Keeps the header at 8 bytes and gives v2 a field to use without changing the header size.
- **Request ID, 16 bits instead of 31.** HTTP/2 never reuses a stream ID on a connection, so it needs a large space. BHTTP IDs only have to differ among requests still waiting for an answer, and no client has 65,535 of those. v1 strictly does one request at a time and does not need IDs at all, but checking them catches a client and server that have fallen out of step, and v2 can run requests in parallel without changing the header.
- **str16 for every string.** One length rule for paths, names and values means one parsing routine and one less thing to get wrong.
- **A static table of 10 names.** These are exactly the names bserve and bcurl send. The request in HEXDUMP.md is 63 bytes including the frame header. The same request as HTTP/1.1 text is 103 bytes.
