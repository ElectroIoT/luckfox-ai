// PC-side unit tests: config parsing, HTTP request parsing, cookies, static files, and the real
// HTTP server on loopback (bodies, limits, handler dispatch). Run: bash scripts/test.sh
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "aid_config.h"
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
    CHECK(!aid::parseConfig("width=99999\n", &d, &p));
    CHECK(d.width == 1920);
    CHECK(p.size() == 1);

    aid::Config e;
    p.clear();
    CHECK(aid::parseConfig("bogus=1\nfps=abc\nnoequals\n", &e, &p));
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

static void testParsing() {
    aid::HttpRequest r;
    CHECK(aid::parseHttpRequest("GET /snapshot.jpg?token=abc&x=1 HTTP/1.1\r\nHost: x\r\n\r\n", &r));
    CHECK(r.method == "GET" && r.path == "/snapshot.jpg" && r.query == "token=abc&x=1");
    CHECK(aid::queryParam(r.query, "token") == "abc");
    CHECK(aid::queryParam(r.query, "x") == "1");
    CHECK(aid::queryParam(r.query, "nope") == "");
    CHECK(r.bearer.empty());
    CHECK(r.headers["host"] == "x");

    CHECK(aid::parseHttpRequest("GET /s HTTP/1.1\r\nauthorization:   Bearer tok123\r\nCookie: a=1; aid_session=XYZ; b=2\r\n\r\n", &r));
    CHECK(r.bearer == "tok123");
    CHECK(aid::cookieValue(r.cookie, "aid_session") == "XYZ");
    CHECK(aid::cookieValue(r.cookie, "a") == "1" && aid::cookieValue(r.cookie, "b") == "2");
    CHECK(aid::cookieValue(r.cookie, "zzz") == "");
    CHECK(aid::cookieValue("aid_session_x=1", "aid_session") == "");
    CHECK(aid::parseHttpRequest("GET /s HTTP/1.1\r\nAuthorization: Basic abc\r\n\r\n", &r));
    CHECK(r.bearer.empty());

    CHECK(!aid::parseHttpRequest("GET /s\r\n\r\n", &r));
    CHECK(!aid::parseHttpRequest("GET /s HTTP/2.0\r\n\r\n", &r));
    CHECK(!aid::parseHttpRequest("get /s HTTP/1.1\r\n\r\n", &r));
    CHECK(!aid::parseHttpRequest("GET s HTTP/1.1\r\n\r\n", &r));
    CHECK(!aid::parseHttpRequest("GET /a b HTTP/1.1\r\n\r\n", &r));
    CHECK(!aid::parseHttpRequest("", &r));
    CHECK(!aid::parseHttpRequest(std::string("GET /") + std::string(600, 'a') + " HTTP/1.1\r\n\r\n", &r));

    CHECK(aid::constantTimeEqual("abc", "abc"));
    CHECK(!aid::constantTimeEqual("abc", "abd"));
    CHECK(!aid::constantTimeEqual("abc", "abcd"));
    CHECK(aid::constantTimeEqual("", ""));

    CHECK(aid::urlDecode("a%20b+c%2Fd%zz%4") == "a b c/d%zz%4");

    aid::HttpRequest t;
    aid::parseHttpRequest("GET /x?token=sekret0123456789 HTTP/1.1\r\n\r\n", &t);
    CHECK(aid::tokenAuthorized(t, "sekret0123456789"));
    CHECK(!aid::tokenAuthorized(t, "other"));
    CHECK(!aid::tokenAuthorized(t, ""));                  // an empty token never authorizes
    aid::parseHttpRequest("GET /x HTTP/1.1\r\nAuthorization: Bearer sekret0123456789\r\n\r\n", &t);
    CHECK(aid::tokenAuthorized(t, "sekret0123456789"));
}

static void put(const std::string& path, const std::string& text) {
    std::ofstream f(path.c_str(), std::ios::binary);
    f << text;
}

static void testStaticFiles() {
    char tmpl[] = "/tmp/aidwebXXXXXX";
    if (!mkdtemp(tmpl)) { CHECK(false); return; }
    const std::string dir = tmpl;
    mkdir((dir + "/js").c_str(), 0700);
    put(dir + "/index.html", "<html>hi</html>");
    put(dir + "/style.css", "body{}");
    put(dir + "/js/app.js", "var a=1;");
    put(dir + "/.secret.txt", "nope");
    put(dir + "/notes.exe", "nope");
    put(dir + "/big.txt", std::string(600 * 1024, 'x'));
    std::string outside = dir + "_outside.txt";
    put(outside, "outside");

    aid::HttpResponse r;
    CHECK(aid::serveStaticFile(dir, "/", &r) && r.contentType.find("text/html") == 0 && r.body.size() == 15);
    CHECK(aid::serveStaticFile(dir, "/style.css", &r) && r.contentType.find("text/css") == 0);
    CHECK(aid::serveStaticFile(dir, "/js/app.js", &r) && r.contentType.find("javascript") != std::string::npos);
    CHECK(!aid::serveStaticFile(dir, "/missing.html", &r));
    CHECK(!aid::serveStaticFile(dir, "/.secret.txt", &r));               // hidden file
    CHECK(!aid::serveStaticFile(dir, "/notes.exe", &r));                 // unknown type
    CHECK(!aid::serveStaticFile(dir, "/big.txt", &r));                   // over 512 KB
    CHECK(!aid::serveStaticFile(dir, "/../" + outside.substr(5), &r));   // traversal
    CHECK(!aid::serveStaticFile(dir, "/js/../index.html", &r));
    CHECK(!aid::serveStaticFile(dir, "/js//app.js", &r));
    CHECK(!aid::serveStaticFile(dir, "/%2e%2e/x.txt", &r));
    CHECK(!aid::serveStaticFile(dir, "/js", &r));
    CHECK(!aid::serveStaticFile(dir, "index.html", &r));

    // The RAM cache serves the same files without touching the disk again.
    aid::StaticCache cache;
    CHECK(cache.load(dir) == 3);                                   // index.html, style.css, js/app.js (not hidden/unknown/big)
    CHECK(cache.bytes() == 15 + 6 + 8);
    std::string cmd = "rm -rf " + dir + " " + outside;
    CHECK(system(cmd.c_str()) == 0);                               // delete everything from disk
    CHECK(cache.serve("/", &r) && r.contentType.find("text/html") == 0 && r.body.size() == 15);
    CHECK(cache.serve("/style.css", &r) && cache.serve("/js/app.js", &r));
    CHECK(!cache.serve("/.secret.txt", &r) && !cache.serve("/big.txt", &r) && !cache.serve("/notes.exe", &r));
    CHECK(!cache.serve("/../x.html", &r) && !cache.serve("/js//app.js", &r) && !cache.serve("/missing.html", &r));
    aid::StaticCache empty;
    CHECK(empty.load("/nonexistent-dir-xyz") == 0 && !empty.serve("/", &r));
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
static bool has(const std::string& s, const char* p) { return s.find(p) != std::string::npos; }

static void testServer() {
    const std::string token = "0123456789abcdef";
    aid::HttpServer s;
    std::string err;
    CHECK(s.start(18081, true, [&](const aid::HttpRequest& req, aid::HttpResponse* resp) -> bool {
        if (req.path == "/echo") {                                  // echoes method, body, ip and a header
            aid::setText(resp, 200, req.method + "|" + req.body + "|" + req.clientIp + "|" +
                                        (req.headers.count("x-requested-with") ? req.headers.at("x-requested-with") : ""));
            return true;
        }
        if (req.path == "/secret") {                                // token-protected route
            if (!aid::tokenAuthorized(req, token)) { aid::setText(resp, 401, "unauthorized\n"); return true; }
            aid::setText(resp, 200, "the secret\n");
            return true;
        }
        if (req.path == "/cookie") {
            resp->headers.push_back({"Set-Cookie", "aid_session=abc; HttpOnly"});
            aid::setText(resp, 200, "ok");
            return true;
        }
        return false;
    }, &err));
    CHECK(!s.isLocalOnly());

    std::string r = httpExchange(18081, "GET /echo HTTP/1.1\r\nX-Requested-With: aid\r\n\r\n");
    CHECK(startsWith(r, "HTTP/1.1 200") && has(r, "\r\n\r\nGET||127.0.0.1|aid"));
    CHECK(has(r, "X-Content-Type-Options: nosniff") && has(r, "X-Frame-Options: DENY") &&
          has(r, "Content-Security-Policy: default-src 'self'") && has(r, "Cache-Control: no-store"));
    r = httpExchange(18081, "POST /echo HTTP/1.1\r\nContent-Length: 11\r\n\r\nhello world");
    CHECK(has(r, "\r\n\r\nPOST|hello world|"));
    r = httpExchange(18081, "DELETE /echo?x=1 HTTP/1.1\r\n\r\n");
    CHECK(has(r, "\r\n\r\nDELETE||"));
    r = httpExchange(18081, "POST /echo HTTP/1.1\r\nContent-Length: 5\r\n\r\nabcdefgh");   // extra bytes are ignored
    CHECK(has(r, "\r\n\r\nPOST|abcde|"));
    r = httpExchange(18081, "POST /echo HTTP/1.1\r\nContent-Length: 20000\r\n\r\n");
    CHECK(startsWith(r, "HTTP/1.1 413"));
    r = httpExchange(18081, "POST /echo HTTP/1.1\r\nContent-Length: abc\r\n\r\n");
    CHECK(startsWith(r, "HTTP/1.1 400"));
    r = httpExchange(18081, "PUT /echo HTTP/1.1\r\n\r\n");
    CHECK(startsWith(r, "HTTP/1.1 405"));
    CHECK(startsWith(httpExchange(18081, "GET /nothing HTTP/1.1\r\n\r\n"), "HTTP/1.1 404"));
    CHECK(startsWith(httpExchange(18081, "garbage\r\n\r\n"), "HTTP/1.1 400"));
    CHECK(startsWith(httpExchange(18081, "GET /echo HTTP/1.1\r\nX: " + std::string(9000, 'a') + "\r\n\r\n"), "HTTP/1.1 431"));

    CHECK(startsWith(httpExchange(18081, "GET /secret HTTP/1.1\r\n\r\n"), "HTTP/1.1 401"));
    CHECK(startsWith(httpExchange(18081, "GET /secret?token=wrong0123456789a HTTP/1.1\r\n\r\n"), "HTTP/1.1 401"));
    CHECK(startsWith(httpExchange(18081, "GET /secret HTTP/1.1\r\nAuthorization: Bearer " + token + "\r\n\r\n"), "HTTP/1.1 200"));
    CHECK(startsWith(httpExchange(18081, "GET /secret?token=" + token + " HTTP/1.1\r\n\r\n"), "HTTP/1.1 200"));
    r = httpExchange(18081, "GET /cookie HTTP/1.1\r\n\r\n");
    CHECK(has(r, "Set-Cookie: aid_session=abc; HttpOnly"));
    s.stop();

    aid::HttpServer local;
    CHECK(local.start(18082, false, [](const aid::HttpRequest&, aid::HttpResponse*) { return false; }, &err));
    CHECK(local.isLocalOnly());
    CHECK(startsWith(httpExchange(18082, "GET /x HTTP/1.1\r\n\r\n"), "HTTP/1.1 404"));
    local.stop();
}

int main() {
    testConfig();
    testParsing();
    testStaticFiles();
    testServer();
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
