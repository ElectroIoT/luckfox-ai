// PC-side unit tests (host compiler) for code that does not need the board:
// config parsing and HTTP request parsing. Run: bash scripts/test.sh
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "config.h"
#include "http_server.h"

static int g_fail = 0, g_run = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_run;                                                             \
        if (!(cond)) {                                                       \
            ++g_fail;                                                        \
            printf("FAIL line %d: %s\n", __LINE__, #cond);                   \
        }                                                                    \
    } while (0)

static void testConfig() {
    aid::Config c;
    std::vector<std::string> p;
    CHECK(aid::parseConfig("width=1280\nheight=720 # comment\n  fps = 25 \n", &c, &p));
    CHECK(c.width == 1280 && c.height == 720 && c.fps == 25);
    CHECK(p.empty());

    aid::Config d;
    p.clear();
    CHECK(!aid::parseConfig("width=99999\n", &d, &p));  // out of range is a hard error
    CHECK(d.width == 1920);                              // default kept
    CHECK(p.size() == 1);

    aid::Config e;
    p.clear();
    CHECK(aid::parseConfig("bogus=1\nfps=abc\nnoequals\n", &e, &p));  // soft problems
    CHECK(p.size() == 3);
    CHECK(e.fps == 30);

    aid::Config f;
    p.clear();
    CHECK(aid::parseConfig("http_token=abcdef0123456789\nrtsp_path=/live/1\n", &f, &p));
    CHECK(f.httpToken == "abcdef0123456789" && f.rtspPath == "/live/1");

    aid::Config g;
    p.clear();
    CHECK(!aid::parseConfig("rtsp_path=live\n", &g, &p));
    p.clear();
    CHECK(!aid::parseConfig("rtsp_path=/a/../b\n", &g, &p));
    p.clear();
    CHECK(!aid::parseConfig("http_token=has space\n", &g, &p));
}

static void testHttp() {
    aid::HttpRequest r;
    CHECK(aid::parseHttpRequest("GET /snapshot.jpg?token=abc&x=1 HTTP/1.1\r\nHost: x\r\n\r\n", &r));
    CHECK(r.method == "GET" && r.path == "/snapshot.jpg" && r.query == "token=abc&x=1");
    CHECK(aid::queryParam(r.query, "token") == "abc");
    CHECK(aid::queryParam(r.query, "x") == "1");
    CHECK(aid::queryParam(r.query, "nope") == "");
    CHECK(r.bearer.empty());

    CHECK(aid::parseHttpRequest("GET /s HTTP/1.1\r\nauthorization:   Bearer tok123\r\n\r\n", &r));
    CHECK(r.bearer == "tok123");
    CHECK(aid::parseHttpRequest("GET /s HTTP/1.1\r\nAuthorization: Basic abc\r\n\r\n", &r));
    CHECK(r.bearer.empty());

    CHECK(!aid::parseHttpRequest("GET /s\r\n\r\n", &r));                // no version
    CHECK(!aid::parseHttpRequest("GET /s HTTP/2.0\r\n\r\n", &r));       // wrong version
    CHECK(!aid::parseHttpRequest("get /s HTTP/1.1\r\n\r\n", &r));       // lower-case method
    CHECK(!aid::parseHttpRequest("GET s HTTP/1.1\r\n\r\n", &r));        // target must start with /
    CHECK(!aid::parseHttpRequest("GET /a b HTTP/1.1\r\n\r\n", &r));     // space in target
    CHECK(!aid::parseHttpRequest("", &r));
    CHECK(!aid::parseHttpRequest(std::string("GET /") + std::string(600, 'a') + " HTTP/1.1\r\n\r\n", &r));

    CHECK(aid::constantTimeEqual("abc", "abc"));
    CHECK(!aid::constantTimeEqual("abc", "abd"));
    CHECK(!aid::constantTimeEqual("abc", "abcd"));
    CHECK(aid::constantTimeEqual("", ""));
}

// Talks to a real HttpServer over a local socket.
static std::string httpExchange(int port, const std::string& request) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr*)&a, sizeof(a)) != 0) { close(fd); return "CONNECT-FAILED"; }
    send(fd, request.data(), request.size(), MSG_NOSIGNAL);
    std::string out;
    char buf[2048];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) out.append(buf, (size_t)n);
    close(fd);
    return out;
}

static bool startsWith(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }

static void testServer() {
    const std::string token = "0123456789abcdef";
    aid::HttpServer s;
    std::string err;
    CHECK(!s.start(18081, "short", [](const aid::HttpRequest&, aid::HttpResponse*) { return false; }, &err));  // token too short

    CHECK(s.start(18081, token, [](const aid::HttpRequest& req, aid::HttpResponse* resp) -> bool {
        if (req.path != "/hello") return false;
        const std::string t = "hi\n";
        resp->body.assign(t.begin(), t.end());
        return true;
    }, &err));
    CHECK(!s.isLocalOnly());

    CHECK(startsWith(httpExchange(18081, "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n"), "HTTP/1.1 401"));
    CHECK(startsWith(httpExchange(18081, "GET /hello?token=wrong0123456789a HTTP/1.1\r\n\r\n"), "HTTP/1.1 401"));
    CHECK(startsWith(httpExchange(18081, "GET /hello HTTP/1.1\r\nAuthorization: Bearer " + token + "\r\n\r\n"), "HTTP/1.1 200"));
    CHECK(startsWith(httpExchange(18081, "GET /hello?token=" + token + " HTTP/1.1\r\n\r\n"), "HTTP/1.1 200"));
    const std::string ok = httpExchange(18081, "GET /hello?token=" + token + " HTTP/1.1\r\n\r\n");
    CHECK(ok.find("\r\n\r\nhi\n") != std::string::npos);
    CHECK(startsWith(httpExchange(18081, "GET /nothing?token=" + token + " HTTP/1.1\r\n\r\n"), "HTTP/1.1 404"));
    CHECK(startsWith(httpExchange(18081, "POST /hello?token=" + token + " HTTP/1.1\r\n\r\n"), "HTTP/1.1 405"));
    CHECK(startsWith(httpExchange(18081, "garbage\r\n\r\n"), "HTTP/1.1 400"));
    CHECK(startsWith(httpExchange(18081, "GET /hello HTTP/1.1\r\nX: " + std::string(9000, 'a') + "\r\n\r\n"), "HTTP/1.1 431"));
    s.stop();

    aid::HttpServer local;
    CHECK(local.start(18082, "", [](const aid::HttpRequest&, aid::HttpResponse*) { return false; }, &err));
    CHECK(local.isLocalOnly());
    CHECK(startsWith(httpExchange(18082, "GET /x HTTP/1.1\r\n\r\n"), "HTTP/1.1 404"));  // no token needed on loopback
    local.stop();
}

int main() {
    testConfig();
    testHttp();
    testServer();
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
