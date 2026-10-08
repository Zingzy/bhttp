#include "frame.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace bhttp {
namespace {

const char* const kStaticTable[] = {
    nullptr,        "host",           "user-agent",    "accept", "connection",
    "content-type", "content-length", "last-modified", "date",   "server",
    "allow",
};
constexpr size_t kTableSize = 10;

std::string to_lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

size_t table_index(const std::string& name) {
  for (size_t i = 1; i <= kTableSize; ++i)
    if (name == kStaticTable[i]) return i;
  return 0;
}

// Returns fewer than n bytes only if the peer closed.
size_t read_full(int fd, uint8_t* p, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t r = ::read(fd, p + got, n - got);
    if (r == 0) break;
    if (r < 0) {
      if (errno == EINTR) continue;
      throw ConnectionLost(std::strerror(errno));
    }
    got += size_t(r);
  }
  return got;
}

void write_full(int fd, const uint8_t* p, size_t n) {
  while (n > 0) {
    ssize_t w = ::write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      throw ConnectionLost(std::strerror(errno));
    }
    p += w;
    n -= size_t(w);
  }
}

class Writer {
 public:
  void u8(uint8_t v) { buf_.push_back(v); }

  void u16(uint16_t v) {
    buf_.push_back(uint8_t(v >> 8));
    buf_.push_back(uint8_t(v));
  }

  void str16(const std::string& s) {
    if (s.size() > 0xFFFF) throw std::length_error("string longer than 65535 bytes");
    u16(uint16_t(s.size()));
    buf_.insert(buf_.end(), s.begin(), s.end());
  }

  void headers(const Headers& hs) {
    for (const auto& [name, value] : hs) {
      std::string lower = to_lower(name);
      size_t idx = table_index(lower);
      u8(uint8_t(idx));
      if (idx == 0) str16(lower);
      str16(value);
    }
  }

  std::vector<uint8_t> take() { return std::move(buf_); }

 private:
  std::vector<uint8_t> buf_;
};

class Reader {
 public:
  explicit Reader(const std::vector<uint8_t>& buf) : buf_(buf) {}

  uint8_t u8() {
    need(1);
    return buf_[pos_++];
  }

  uint16_t u16() {
    need(2);
    uint16_t v = uint16_t(buf_[pos_] << 8 | buf_[pos_ + 1]);
    pos_ += 2;
    return v;
  }

  std::string str16() {
    size_t n = u16();
    need(n);
    std::string s(buf_.begin() + pos_, buf_.begin() + pos_ + n);
    pos_ += n;
    return s;
  }

  Headers headers() {
    Headers hs;
    while (pos_ < buf_.size()) {
      uint8_t idx = u8();
      std::string name;
      if (idx == 0) {
        name = to_lower(str16());
        if (name.empty()) throw Malformed("empty literal header name");
      } else if (idx <= kTableSize) {
        name = kStaticTable[idx];
      }
      // Indexes past the table are reserved for v2: the value is still read so parsing stays in step.
      std::string value = str16();
      if (!name.empty()) hs.emplace_back(std::move(name), std::move(value));
    }
    return hs;
  }

 private:
  void need(size_t n) {
    if (buf_.size() - pos_ < n) throw Malformed("field runs past the end of the payload");
  }

  const std::vector<uint8_t>& buf_;
  size_t pos_ = 0;
};

const char* type_name(uint8_t type) {
  switch (type) {
    case kRequest: return "REQUEST";
    case kResponse: return "RESPONSE";
    case kData: return "DATA";
    default: return nullptr;
  }
}

void print_headers(FILE* out, const Headers& hs) {
  for (const auto& [name, value] : hs) std::fprintf(out, "    %s: %s\n", name.c_str(), value.c_str());
}

}  // namespace

bool read_frame(int fd, Frame& f) {
  uint8_t h[kHeaderSize];
  size_t got = read_full(fd, h, kHeaderSize);
  if (got == 0) return false;
  if (got < kHeaderSize) throw ConnectionLost("connection closed inside a frame header");
  uint32_t len = uint32_t(h[0]) << 16 | uint32_t(h[1]) << 8 | h[2];
  f.type = h[3];
  f.flags = h[4];
  f.id = uint16_t(h[6] << 8 | h[7]);
  f.payload.resize(len);
  if (read_full(fd, f.payload.data(), len) < len) throw ConnectionLost("connection closed inside a frame payload");
  return true;
}

std::vector<uint8_t> encode(const Frame& f) {
  size_t len = f.payload.size();
  if (len > kMaxLength) throw std::length_error("frame payload longer than 16777215 bytes");
  std::vector<uint8_t> out = {
      uint8_t(len >> 16), uint8_t(len >> 8), uint8_t(len), f.type,
      f.flags,            0,                 uint8_t(f.id >> 8), uint8_t(f.id),
  };
  out.insert(out.end(), f.payload.begin(), f.payload.end());
  return out;
}

void write_frame(int fd, const Frame& f) {
  std::vector<uint8_t> out = encode(f);
  write_full(fd, out.data(), out.size());
}

Frame make_request(uint16_t id, uint8_t flags, const Request& r) {
  Writer w;
  w.u8(r.method);
  w.str16(r.path);
  w.headers(r.headers);
  return Frame{kRequest, flags, id, w.take()};
}

Frame make_response(uint16_t id, uint8_t flags, const Response& r) {
  Writer w;
  w.u16(r.status);
  w.headers(r.headers);
  return Frame{kResponse, flags, id, w.take()};
}

Request parse_request(const Frame& f) {
  Reader r(f.payload);
  Request req;
  req.method = r.u8();
  req.path = r.str16();
  req.headers = r.headers();
  return req;
}

Response parse_response(const Frame& f) {
  Reader r(f.payload);
  Response res;
  res.status = r.u16();
  res.headers = r.headers();
  return res;
}

std::string method_name(uint8_t method) {
  switch (method) {
    case kGet: return "GET";
    case kHead: return "HEAD";
    case kPost: return "POST";
    case kPut: return "PUT";
    case kDelete: return "DELETE";
  }
  char buf[16];
  std::snprintf(buf, sizeof buf, "METHOD(0x%02x)", method);
  return buf;
}

void set_nodelay(int fd) {
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

void dump(FILE* out, char direction, const Frame& f) {
  std::vector<uint8_t> b = encode(f);
  const char* name = type_name(f.type);
  char unknown[24];
  if (!name) {
    std::snprintf(unknown, sizeof unknown, "UNKNOWN(0x%02x)", f.type);
    name = unknown;
  }
  std::fprintf(out, "%c %s id=%u flags=0x%02x%s length=%zu\n", direction, name, f.id, f.flags,
               f.flags & kFlagEnd ? " END" : "", f.payload.size());
  std::fprintf(out, "  header  %02x %02x %02x  %02x  %02x  %02x  %02x %02x\n", b[0], b[1], b[2], b[3], b[4], b[5],
               b[6], b[7]);
  for (size_t off = 0; off < f.payload.size(); off += 16) {
    size_t n = std::min<size_t>(16, f.payload.size() - off);
    std::fprintf(out, "  %06zx ", off);
    for (size_t i = 0; i < 16; ++i) {
      if (i == 8) std::fputc(' ', out);
      if (i < n)
        std::fprintf(out, " %02x", f.payload[off + i]);
      else
        std::fputs("   ", out);
    }
    std::fputs("  |", out);
    for (size_t i = 0; i < n; ++i) {
      uint8_t c = f.payload[off + i];
      std::fputc(c >= 0x20 && c < 0x7f ? c : '.', out);
    }
    std::fputs("|\n", out);
  }
  try {
    if (f.type == kRequest) {
      Request r = parse_request(f);
      std::fprintf(out, "  = %s %s\n", method_name(r.method).c_str(), r.path.c_str());
      print_headers(out, r.headers);
    } else if (f.type == kResponse) {
      Response r = parse_response(f);
      std::fprintf(out, "  = status %u\n", r.status);
      print_headers(out, r.headers);
    } else if (f.type != kData) {
      std::fputs("  = unknown type, skipped\n", out);
    }
  } catch (const Malformed& e) {
    std::fprintf(out, "  = malformed: %s\n", e.what());
  }
}

}  // namespace bhttp
