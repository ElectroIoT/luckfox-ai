#include "http_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace aid {

namespace {

constexpr size_t kMaxHead = 8192;
constexpr size_t kMaxBody = 16384;
constexpr int kIoTimeoutSec = 3;
constexpr int kMaxConnections = 6;

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

std::string trimSpaces(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

const char* reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 429: return "Too Many Requests";
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
    std::string head = "HTTP/1.1 " + std::to_string(r.status) + " " + reason(r.status) + "\r\n";
    head += "Content-Type: " + r.contentType + "\r\n";
    head += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
    head += "Cache-Control: no-store\r\nConnection: close\r\n";
    head += "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\n";
    head += "Content-Security-Policy: default-src 'self'; img-src 'self' data:; style-src 'self'; "
            "script-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'\r\n";
    for (const auto& h : r.headers) head += h.first + ": " + h.second + "\r\n";
    head += "\r\n";
    if (!sendAll(fd, head.data(), head.size())) return;
    if (!r.body.empty()) sendAll(fd, r.body.data(), r.body.size());
}

}  // namespace

void setText(HttpResponse* r, int status, const std::string& text, const std::string& type) {
    r->status = status;
    r->contentType = type;
    r->body.assign(text.begin(), text.end());
}

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

    const std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    if (target.empty() || target[0] != '/' || target.size() > 512) return false;
    for (char c : target)
        if ((unsigned char)c <= ' ' || (unsigned char)c >= 0x7f) return false;
    const size_t q = target.find('?');
    out->path = q == std::string::npos ? target : target.substr(0, q);
    out->query = q == std::string::npos ? "" : target.substr(q + 1);
    out->bearer.clear();
    out->cookie.clear();
    out->headers.clear();

    size_t pos = lineEnd + 2;
    while (pos < head.size()) {
        const size_t e = head.find("\r\n", pos);
        if (e == std::string::npos || e == pos) break;
        const std::string h = head.substr(pos, e - pos);
        pos = e + 2;
        const size_t colon = h.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = lower(h.substr(0, colon));
        const std::string value = trimSpaces(h.substr(colon + 1));
        if (out->headers.size() < 40) out->headers[name] = value;
        if (name == "authorization" && lower(value.substr(0, 7)) == "bearer ") out->bearer = value.substr(7);
        if (name == "cookie") out->cookie = value;
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

std::string urlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else if (s[i] == '+') {
            o += ' ';
        } else {
            o += s[i];
        }
    }
    return o;
}

std::string cookieValue(const std::string& cookieHeader, const std::string& name) {
    size_t pos = 0;
    while (pos < cookieHeader.size()) {
        size_t semi = cookieHeader.find(';', pos);
        if (semi == std::string::npos) semi = cookieHeader.size();
        const std::string part = trimSpaces(cookieHeader.substr(pos, semi - pos));
        const size_t eq = part.find('=');
        if (eq != std::string::npos && part.substr(0, eq) == name) return part.substr(eq + 1);
        pos = semi + 1;
    }
    return "";
}

bool constantTimeEqual(const std::string& a, const std::string& b) {
    unsigned char diff = (unsigned char)(a.size() != b.size());
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

bool tokenAuthorized(const HttpRequest& req, const std::string& token) {
    if (token.empty()) return false;
    const std::string given = req.bearer.empty() ? queryParam(req.query, "token") : req.bearer;
    return constantTimeEqual(given, token);
}

bool serveStaticFile(const std::string& webDir, const std::string& urlPath, HttpResponse* resp) {
    std::string p = urlPath == "/" ? "/index.html" : urlPath;
    if (p.size() < 2 || p.size() > 128 || p[0] != '/' || p.find("..") != std::string::npos) return false;
    for (char c : p) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '_' || c == '-' || c == '/';
        if (!ok) return false;
    }
    if (p.find("//") != std::string::npos || p.find("/.") != std::string::npos) return false;  // no hidden files
    const size_t dot = p.rfind('.');
    if (dot == std::string::npos) return false;
    const std::string ext = lower(p.substr(dot + 1));
    std::string type;
    if (ext == "html") type = "text/html; charset=utf-8";
    else if (ext == "css") type = "text/css; charset=utf-8";
    else if (ext == "js") type = "application/javascript; charset=utf-8";
    else if (ext == "svg") type = "image/svg+xml";
    else if (ext == "png") type = "image/png";
    else if (ext == "ico") type = "image/x-icon";
    else if (ext == "json") type = "application/json";
    else if (ext == "txt") type = "text/plain; charset=utf-8";
    else return false;

    const std::string full = webDir + p;
    struct stat st;
    if (stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 512 * 1024) return false;
    std::ifstream f(full.c_str(), std::ios::binary);
    if (!f) return false;
    std::ostringstream all;
    all << f.rdbuf();
    const std::string s = all.str();
    resp->status = 200;
    resp->contentType = type;
    resp->body.assign(s.begin(), s.end());
    return true;
}

struct HttpServer::Impl {
    int listenFd = -1;
    std::atomic<bool> quit{false};
    pthread_t acceptThread;
    bool acceptStarted = false;
    Handler handler;
    bool localOnly = true;
    std::atomic<int> active{0};

    struct ConnArg { Impl* self; int fd; std::string ip; };

    static std::string peerIp(int fd) {
        struct sockaddr_in a;
        socklen_t l = sizeof(a);
        if (getpeername(fd, (struct sockaddr*)&a, &l) != 0) return "";
        char buf[INET_ADDRSTRLEN];
        return inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf)) ? buf : "";
    }

    void serveOne(int fd, const std::string& ip) {
        struct timeval tv;
        tv.tv_sec = kIoTimeoutSec;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        std::string buf;
        char chunk[1024];
        size_t headEnd = std::string::npos;
        while ((headEnd = buf.find("\r\n\r\n")) == std::string::npos) {
            const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0) return;
            buf.append(chunk, (size_t)n);
            if (buf.size() > kMaxHead + kMaxBody) {
                HttpResponse r; setText(&r, 431, "header too large\n"); sendResponse(fd, r);
                return;
            }
            if (buf.find("\r\n\r\n") == std::string::npos && buf.size() > kMaxHead) {
                HttpResponse r; setText(&r, 431, "header too large\n"); sendResponse(fd, r);
                return;
            }
        }
        if (headEnd > kMaxHead) { HttpResponse r; setText(&r, 431, "header too large\n"); sendResponse(fd, r); return; }

        HttpRequest req;
        if (!parseHttpRequest(buf.substr(0, headEnd + 4), &req)) {
            HttpResponse r; setText(&r, 400, "bad request\n"); sendResponse(fd, r);
            return;
        }
        req.clientIp = ip;
        if (req.method != "GET" && req.method != "POST" && req.method != "DELETE") {
            HttpResponse r; setText(&r, 405, "method not supported\n"); sendResponse(fd, r);
            return;
        }
        size_t contentLength = 0;
        auto cl = req.headers.find("content-length");
        if (cl != req.headers.end()) {
            char* end = nullptr;
            const long v = strtol(cl->second.c_str(), &end, 10);
            if (cl->second.empty() || *end != '\0' || v < 0) { HttpResponse r; setText(&r, 400, "bad content-length\n"); sendResponse(fd, r); return; }
            if ((size_t)v > kMaxBody) { HttpResponse r; setText(&r, 413, "body too large\n"); sendResponse(fd, r); return; }
            contentLength = (size_t)v;
        }
        req.body = buf.substr(headEnd + 4);
        while (req.body.size() < contentLength) {
            const ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0) return;
            req.body.append(chunk, (size_t)n);
        }
        if (req.body.size() > contentLength) req.body.resize(contentLength);

        HttpResponse resp;
        if (!handler(req, &resp)) setText(&resp, 404, "not found\n");
        sendResponse(fd, resp);
    }

    static void* connMain(void* p) {
        ConnArg* a = (ConnArg*)p;
        a->self->serveOne(a->fd, a->ip);
        close(a->fd);
        a->self->active--;
        delete a;
        return nullptr;
    }

    static void* acceptMain(void* p) {
        Impl* self = (Impl*)p;
        while (!self->quit) {
            struct pollfd pf;
            pf.fd = self->listenFd;
            pf.events = POLLIN;
            pf.revents = 0;
            if (poll(&pf, 1, 300) <= 0) continue;
            const int fd = accept(self->listenFd, nullptr, nullptr);
            if (fd < 0) continue;
            if (self->active >= kMaxConnections) {
                HttpResponse r; setText(&r, 503, "busy\n");
                sendResponse(fd, r);
                close(fd);
                continue;
            }
            ConnArg* a = new ConnArg{self, fd, peerIp(fd)};
            self->active++;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, 256 * 1024);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            pthread_t t;
            if (pthread_create(&t, &attr, connMain, a) != 0) {
                self->active--;
                close(fd);
                delete a;
            }
            pthread_attr_destroy(&attr);
        }
        return nullptr;
    }
};

HttpServer::HttpServer() : impl_(new Impl) {}
HttpServer::~HttpServer() { stop(); }
bool HttpServer::isLocalOnly() const { return impl_->localOnly; }
int HttpServer::activeConnections() const { return impl_->active; }

bool HttpServer::start(int port, bool allInterfaces, Handler handler, std::string* err) {
    std::string dummy;
    if (!err) err = &dummy;
    if (impl_->listenFd >= 0) { *err = "already started"; return false; }

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { *err = std::string("socket: ") + strerror(errno); return false; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(allInterfaces ? INADDR_ANY : INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(fd, 8) != 0) {
        *err = std::string("bind/listen on port ") + std::to_string(port) + ": " + strerror(errno);
        close(fd);
        return false;
    }

    impl_->listenFd = fd;
    impl_->handler = std::move(handler);
    impl_->localOnly = !allInterfaces;
    impl_->quit = false;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 128 * 1024);
    const int rc = pthread_create(&impl_->acceptThread, &attr, Impl::acceptMain, impl_.get());
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        close(fd);
        impl_->listenFd = -1;
        *err = "cannot start the server thread";
        return false;
    }
    impl_->acceptStarted = true;
    return true;
}

void HttpServer::stop() {
    if (impl_->listenFd < 0) return;
    impl_->quit = true;
    if (impl_->acceptStarted) pthread_join(impl_->acceptThread, nullptr);
    impl_->acceptStarted = false;
    close(impl_->listenFd);
    impl_->listenFd = -1;
    // Connection threads are short-lived (3 s timeouts); give them a moment to finish.
    for (int i = 0; i < 50 && impl_->active > 0; ++i) usleep(100 * 1000);
}

}  // namespace aid
