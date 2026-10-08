# bserve and bcurl

A static file server and client that speak BHTTP/1, a binary framing of HTTP. The protocol is in [SPEC.md](SPEC.md) and an annotated request and response is in [HEXDUMP.md](HEXDUMP.md).

```
make
./bserve ./www 9000
./bcurl -v localhost:9000/index.html
./bcurl localhost:9000/index.html localhost:9000/index.html   # two requests, one connection
make test
```

`--unknown-frame` on either program sends an extra frame of type `0x7f` so you can watch the other side skip it.
