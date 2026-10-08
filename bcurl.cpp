#include "frame.hpp"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace bhttp;

namespace {

bool g_verbose = false;

struct Target {
  std::string host;
  std::string port;
  std::string path;
};

Target parse_target(std::string s) {
  size_t scheme = s.find("://");
  if (scheme != std::string::npos) s.erase(0, scheme + 3);
  size_t slash = s.find('/');
  std::string authority = s.substr(0, slash);
  size_t colon = authority.rfind(':');
  Target t;
  t.host = authority.substr(0, colon);
  t.port = colon == std::string::npos ? "9000" : authority.substr(colon + 1);
  t.path = slash == std::string::npos ? "/" : s.substr(slash);
  return t;
}

int connect_to(const Target& t) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  int rc = getaddrinfo(t.host.c_str(), t.port.c_str(), &hints, &res);
  if (rc != 0) throw ConnectionLost(t.host + ": " + gai_strerror(rc));
  int fd = -1;
  for (addrinfo* a = res; a && fd < 0; a = a->ai_next) {
    fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
    if (fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen) != 0) {
      close(fd);
      fd = -1;
    }
  }
  freeaddrinfo(res);
  if (fd < 0) throw ConnectionLost("cannot connect to " + t.host + ":" + t.port);
  set_nodelay(fd);
  return fd;
}

void send_frame(int fd, const Frame& f) {
  if (g_verbose) dump(stderr, '>', f);
  write_frame(fd, f);
}

Frame next_frame(int fd) {
  Frame f;
  if (!read_frame(fd, f)) throw ConnectionLost("server closed the connection");
  if (g_verbose) dump(stderr, '<', f);
  return f;
}

uint16_t fetch(int fd, uint16_t id, const Target& t, bool last) {
  Request req{kGet, t.path, {{"host", t.host + ":" + t.port}, {"user-agent", "bcurl/1"}, {"accept", "*/*"}}};
  if (last) req.headers.emplace_back("connection", "close");
  send_frame(fd, make_request(id, kFlagEnd, req));

  Frame f = next_frame(fd);
  while (f.type != kResponse) {
    if (f.type == kRequest || f.type == kData) throw Malformed("expected a RESPONSE frame");
    f = next_frame(fd);
  }
  if (f.id != id)
    throw Malformed("response id " + std::to_string(f.id) + " does not match request id " + std::to_string(id));
  Response res = parse_response(f);

  for (bool end = f.flags & kFlagEnd; !end;) {
    f = next_frame(fd);
    if (f.type == kRequest || f.type == kResponse) throw Malformed("expected a DATA frame");
    if (f.type != kData) continue;
    if (f.id != id) throw Malformed("DATA frame for the wrong request id");
    std::fwrite(f.payload.data(), 1, f.payload.size(), stdout);
    end = f.flags & kFlagEnd;
  }
  std::fflush(stdout);
  return res.status;
}

int usage() {
  std::fputs("usage: bcurl [-v] [--unknown-frame] host[:port]/path ...\n", stderr);
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
  bool unknown_frame = false;
  std::vector<Target> targets;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-v")
      g_verbose = true;
    else if (a == "--unknown-frame")
      unknown_frame = true;
    else if (a.empty() || a[0] == '-')
      return usage();
    else
      targets.push_back(parse_target(a));
  }
  if (targets.empty()) return usage();
  for (const Target& t : targets) {
    if (t.host != targets[0].host || t.port != targets[0].port) {
      std::fputs("bcurl: every URL must share one host:port, because bcurl opens one connection\n", stderr);
      return 2;
    }
  }

  int worst = 0;
  int fd = -1;
  try {
    fd = connect_to(targets[0]);
    if (unknown_frame) {
      std::string note = "frame type 0x7f is unknown to v1 and must be skipped";
      send_frame(fd, Frame{0x7f, 0, 0, {note.begin(), note.end()}});
    }
    for (size_t i = 0; i < targets.size(); ++i) {
      uint16_t id = uint16_t(i % 0xFFFF + 1);
      uint16_t status = fetch(fd, id, targets[i], i + 1 == targets.size());
      if (status >= 400) std::fprintf(stderr, "bcurl: %s returned %u\n", targets[i].path.c_str(), status);
      if (status >= 500)
        worst = 5;
      else if (status >= 400)
        worst = std::max(worst, 4);
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "bcurl: %s\n", e.what());
    worst = 1;
  }
  if (fd >= 0) close(fd);
  return worst;
}
