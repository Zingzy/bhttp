#pragma once

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bhttp {

constexpr size_t kHeaderSize = 8;
constexpr uint32_t kMaxLength = 0xFFFFFF;
constexpr size_t kDataChunk = 16384;

constexpr uint8_t kRequest = 0x01;
constexpr uint8_t kResponse = 0x02;
constexpr uint8_t kData = 0x03;

constexpr uint8_t kFlagEnd = 0x01;

constexpr uint8_t kGet = 1;
constexpr uint8_t kHead = 2;
constexpr uint8_t kPost = 3;
constexpr uint8_t kPut = 4;
constexpr uint8_t kDelete = 5;

using Headers = std::vector<std::pair<std::string, std::string>>;

struct Frame {
  uint8_t type = 0;
  uint8_t flags = 0;
  uint16_t id = 0;
  std::vector<uint8_t> payload;
};

struct Request {
  uint8_t method = 0;
  std::string path;
  Headers headers;
};

struct Response {
  uint16_t status = 0;
  Headers headers;
};

struct Malformed : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct ConnectionLost : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Returns false on a clean close between frames.
bool read_frame(int fd, Frame& f);
void write_frame(int fd, const Frame& f);
std::vector<uint8_t> encode(const Frame& f);

Frame make_request(uint16_t id, uint8_t flags, const Request& r);
Frame make_response(uint16_t id, uint8_t flags, const Response& r);
Request parse_request(const Frame& f);
Response parse_response(const Frame& f);

std::string method_name(uint8_t method);
void set_nodelay(int fd);
void dump(FILE* out, char direction, const Frame& f);

}  // namespace bhttp
