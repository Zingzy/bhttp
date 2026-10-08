#include "frame.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace bhttp;

namespace {

std::string g_root;
bool g_unknown_frame = false;
std::atomic<int> g_connections{0};

std::string http_date(time_t t) {
  struct tm tm;
  gmtime_r(&t, &tm);
  char buf[64];
  std::strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S GMT", &tm);
  return buf;
}

std::string content_type(const std::string& path) {
  static const std::pair<const char*, const char*> kTypes[] = {
      {".html", "text/html; charset=utf-8"}, {".htm", "text/html; charset=utf-8"},
      {".txt", "text/plain; charset=utf-8"}, {".css", "text/css"},
      {".js", "text/javascript"},            {".json", "application/json"},
      {".png", "image/png"},                 {".jpg", "image/jpeg"},
      {".jpeg", "image/jpeg"},               {".gif", "image/gif"},
      {".svg", "image/svg+xml"},             {".ico", "image/x-icon"},
  };
  size_t dot = path.rfind('.');
  if (dot != std::string::npos) {
    std::string ext = path.substr(dot);
    for (const auto& [suffix, type] : kTypes)
      if (ext == suffix) return type;
  }
  return "application/octet-stream";
}

// Returns an empty string for anything that should be a 404, including paths that escape the root.
std::string resolve(std::string path) {
  path = path.substr(0, path.find('?'));
  std::string candidate = g_root + path;
  struct stat st;
  if (stat(candidate.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) candidate += "/index.html";
  char real[PATH_MAX];
  if (!realpath(candidate.c_str(), real)) return "";
  std::string r = real;
  if (r.compare(0, g_root.size() + 1, g_root + "/") != 0) return "";
  if (stat(r.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return "";
  return r;
}

Headers base_headers() {
  return {{"date", http_date(time(nullptr))}, {"server", "bserve/1"}};
}

void send_response(int fd, uint16_t id, bool end, uint16_t status, const Headers& h) {
  if (g_unknown_frame) {
    std::string note = "frame type 0x7f is unknown to v1 and must be skipped";
    write_frame(fd, Frame{0x7f, 0, id, {note.begin(), note.end()}});
  }
  write_frame(fd, make_response(id, end ? kFlagEnd : 0, Response{status, h}));
}

uint16_t send_error(int fd, uint16_t id, uint16_t status, const char* reason, bool head, Headers extra = {}) {
  std::string body = std::to_string(status) + " " + reason + "\n";
  Headers h = base_headers();
  h.emplace_back("content-type", "text/plain; charset=utf-8");
  h.emplace_back("content-length", std::to_string(body.size()));
  h.insert(h.end(), extra.begin(), extra.end());
  send_response(fd, id, head, status, h);
  if (!head) write_frame(fd, Frame{kData, kFlagEnd, id, {body.begin(), body.end()}});
  return status;
}

uint16_t send_file(int fd, uint16_t id, const std::string& path, bool head) {
  struct stat st;
  std::ifstream in(path, std::ios::binary);
  if (!in || stat(path.c_str(), &st) != 0) return send_error(fd, id, 404, "Not Found", head);

  Headers h = base_headers();
  h.emplace_back("content-type", content_type(path));
  h.emplace_back("content-length", std::to_string(st.st_size));
  h.emplace_back("last-modified", http_date(st.st_mtime));
  bool no_body = head || st.st_size == 0;
  send_response(fd, id, no_body, 200, h);
  if (no_body) return 200;

  off_t left = st.st_size;
  std::vector<char> buf(kDataChunk);
  while (left > 0) {
    size_t n = size_t(std::min<off_t>(left, off_t(buf.size())));
    // The RESPONSE already promised content-length, so closing is the only honest signal left.
    if (!in.read(buf.data(), std::streamsize(n))) throw ConnectionLost(path + " shrank while being sent");
    left -= off_t(n);
    write_frame(fd, Frame{kData, uint8_t(left == 0 ? kFlagEnd : 0), id, {buf.begin(), buf.begin() + n}});
  }
  return 200;
}

// Returns false when the client asked to close after this response.
bool handle_request(int fd, int conn, const Frame& f) {
  Request req;
  try {
    req = parse_request(f);
  } catch (const Malformed& e) {
    send_error(fd, f.id, 400, "Bad Request", false);
    std::fprintf(stderr, "[conn %d] malformed REQUEST (%s) -> 400\n", conn, e.what());
    return true;
  }

  bool head = req.method == kHead;
  uint16_t status;
  if (f.id == 0 || req.path.empty() || req.path[0] != '/') {
    status = send_error(fd, f.id, 400, "Bad Request", head);
  } else if (req.method == kGet || head) {
    std::string file = resolve(req.path);
    status = file.empty() ? send_error(fd, f.id, 404, "Not Found", head) : send_file(fd, f.id, file, head);
  } else if (req.method >= kPost && req.method <= kDelete) {
    status = send_error(fd, f.id, 405, "Method Not Allowed", false, {{"allow", "GET, HEAD"}});
  } else {
    status = send_error(fd, f.id, 501, "Not Implemented", false);
  }
  std::fprintf(stderr, "[conn %d] %s %s -> %u\n", conn, method_name(req.method).c_str(), req.path.c_str(), status);

  for (const auto& [name, value] : req.headers)
    if (name == "connection" && value == "close") return false;
  return true;
}

void handle_connection(int fd) {
  int conn = ++g_connections;
  try {
    Frame f;
    while (read_frame(fd, f)) {
      if (f.type == kRequest) {
        if (!handle_request(fd, conn, f)) break;
      } else if (f.type == kResponse) {
        send_error(fd, f.id, 400, "Bad Request", false);
        std::fprintf(stderr, "[conn %d] RESPONSE frame from a client -> 400\n", conn);
      } else if (f.type != kData) {
        std::fprintf(stderr, "[conn %d] skipped unknown frame type 0x%02x (%zu bytes)\n", conn, f.type,
                     f.payload.size());
      }
      // DATA frames are dropped: v1 servers take no request bodies.
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[conn %d] closed: %s\n", conn, e.what());
  }
  close(fd);
}

int usage() {
  std::fputs("usage: bserve [--unknown-frame] ROOT PORT\n", stderr);
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--unknown-frame") == 0)
      g_unknown_frame = true;
    else
      args.push_back(argv[i]);
  }
  if (args.size() != 2) return usage();

  char real[PATH_MAX];
  struct stat st;
  if (!realpath(args[0].c_str(), real) || stat(real, &st) != 0 || !S_ISDIR(st.st_mode)) {
    std::fprintf(stderr, "bserve: %s is not a directory\n", args[0].c_str());
    return 1;
  }
  g_root = real;

  char* end = nullptr;
  long port = std::strtol(args[1].c_str(), &end, 10);
  if (*end != '\0' || port < 1 || port > 65535) return usage();

  int srv = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(uint16_t(port));
  if (srv < 0 || bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(srv, 64) != 0) {
    std::perror("bserve");
    return 1;
  }
  std::fprintf(stderr, "bserve: serving %s on port %ld\n", g_root.c_str(), port);

  for (;;) {
    int fd = accept(srv, nullptr, nullptr);
    if (fd < 0) {
      if (errno != EINTR) std::perror("bserve: accept");
      continue;
    }
    set_nodelay(fd);
    std::thread(handle_connection, fd).detach();
  }
}
