#include "http_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <thread>

namespace aid {

namespace {

constexpr size_t kMaxHead = 8192;
constexpr int kIoTimeoutSec = 3;

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

const char* reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "Error";
    }
}

bool sendAll(int fd, const void* data, size_t len) {
    const char* p = (const char*)data;
    while (len > 0) {
        const ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

void sendResponse(int fd, const HttpResponse& r) {
    char head[256];
    const int n = snprintf(head, sizeof(head),
                           "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                           "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                           r.status, reason(r.status), r.contentType.c_str(), r.body.size());
    if (n <= 0 || !sendAll(fd, head, (size_t)n)) return;
    if (!r.body.empty()) sendAll(fd, r.body.data(), r.body.size());
}

HttpResponse textResponse(int status, const std::string& text) {
    HttpResponse r;
    r.status = status;
    r.body.assign(text.begin(), text.end());
    return r;
}

}  // namespace

bool parseHttpRequest(const std::string& head, HttpRequest* out) {
    const size_t lineEnd = head.find("\r\n");
    if (lineEnd == std::string::npos) return false;
    const std::string line = head.substr(0, lineEnd);

    const size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos || sp1 == 0) return false;
    const size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;
    const std::string version = line.substr(sp2 + 1);
    if (version != "HTTP/1.1" && version != "HTTP/1.0") return false;

    out->method = line.substr(0, sp1);
    for (char c : out->method)
        if (c < 'A' || c > 'Z') return false;

    std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    if (target.empty() || target[0] != '/' || target.size() > 512) return false;
    for (char c : target)
        if ((unsigned char)c <= ' ' || (unsigned char)c >= 0x7f) return false;
    const size_t q = target.find('?');
    out->path = q == std::string::npos ? target : target.substr(0, q);
    out->query = q == std::string::npos ? "" : target.substr(q + 1);
    out->bearer.clear();

    size_t pos = lineEnd + 2;
    while (pos < head.size()) {
        const size_t e = head.find("\r\n", pos);
        if (e == std::string::npos || e == pos) break;
        const std::string h = head.substr(pos, e - pos);
        pos = e + 2;
        const size_t colon = h.find(':');
        if (colon == std::string::npos) continue;
        if (lower(h.substr(0, colon)) == "authorization") {
            size_t v = colon + 1;
            while (v < h.size() && h[v] == ' ') ++v;
            const std::string val = h.substr(v);
            if (lower(val.substr(0, 7)) == "bearer ") out->bearer = val.substr(7);
        }
    }
    return true;
}

std::string queryParam(const std::string& query, const std::string& name) {
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        const std::string pair = query.substr(pos, amp - pos);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == name) return pair.substr(eq + 1);
        pos = amp + 1;
    }
    return "";
}

bool constantTimeEqual(const std::string& a, const std::string& b) {
    unsigned char diff = (unsigned char)(a.size() != b.size());
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

struct HttpServer::Impl {
    int listenFd = -1;
    std::atomic<bool> quit{false};
    std::thread thread;
    std::string token;
    Handler handler;
    bool localOnly = true;

    void serveOne(int fd) {
        struct timeval tv;
        tv.tv_sec = kIoTimeoutSec;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        std::string head;
        char buf[1024];
        while (head.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) return;  // timeout, closed, or error: just drop the connection
            head.append(buf, (size_t)n);
            if (head.size() > kMaxHead) {
                sendResponse(fd, textResponse(431, "header too large\n"));
                return;
            }
        }

        HttpRequest req;
        if (!parseHttpRequest(head, &req)) {
            sendResponse(fd, textResponse(400, "bad request\n"));
            return;
        }
        if (req.method != "GET") {
            sendResponse(fd, textResponse(405, "only GET is supported\n"));
            return;
        }
        if (!token.empty()) {
            std::string given = req.bearer.empty() ? queryParam(req.query, "token") : req.bearer;
            if (!constantTimeEqual(given, token)) {
                sendResponse(fd, textResponse(401, "unauthorized\n"));
                return;
            }
        }
        HttpResponse resp;
        if (!handler(req, &resp)) resp = textResponse(404, "not found\n");
        sendResponse(fd, resp);
    }

    void loop() {
        while (!quit) {
            struct pollfd p;
            p.fd = listenFd;
            p.events = POLLIN;
            p.revents = 0;
            const int r = poll(&p, 1, 500);
            if (r <= 0) continue;
            const int fd = accept(listenFd, nullptr, nullptr);
            if (fd < 0) continue;
            serveOne(fd);
            close(fd);
        }
    }
};

HttpServer::HttpServer() : impl_(new Impl) {}
HttpServer::~HttpServer() { stop(); }
bool HttpServer::isLocalOnly() const { return impl_->localOnly; }

bool HttpServer::start(int port, const std::string& token, Handler handler, std::string* err) {
    std::string dummy;
    if (!err) err = &dummy;
    if (impl_->listenFd >= 0) { *err = "already started"; return false; }
    if (!token.empty() && token.size() < 16) {
        *err = "http_token must be at least 16 characters";
        return false;
    }

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { *err = std::string("socket: ") + strerror(errno); return false; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(token.empty() ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(fd, 4) != 0) {
        *err = std::string("bind/listen on port ") + std::to_string(port) + ": " + strerror(errno);
        close(fd);
        return false;
    }

    impl_->listenFd = fd;
    impl_->token = token;
    impl_->handler = std::move(handler);
    impl_->localOnly = token.empty();
    impl_->quit = false;
    impl_->thread = std::thread([this] { impl_->loop(); });
    return true;
}

void HttpServer::stop() {
    if (impl_->listenFd < 0) return;
    impl_->quit = true;
    if (impl_->thread.joinable()) impl_->thread.join();
    close(impl_->listenFd);
    impl_->listenFd = -1;
}

}  // namespace aid
