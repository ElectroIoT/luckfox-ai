// Small HTTP/1.1 server for aid-core: GET/POST/DELETE, bodies up to 16 KB, one short-lived thread
// per connection (at most 6 at once, small stacks), short timeouts, security headers, static files
// from a directory. It does not do authentication itself: the handler decides (see api.h).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace aid {

struct HttpRequest {
    std::string method;
    std::string path;     // without the query string
    std::string query;    // after '?', not decoded
    std::string bearer;   // from "Authorization: Bearer <token>", empty if absent
    std::string cookie;   // raw Cookie header
    std::string body;
    std::string clientIp;
    std::map<std::string, std::string> headers;  // lower-case names
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "text/plain; charset=utf-8";
    std::vector<uint8_t> body;
    std::vector<std::pair<std::string, std::string>> headers;  // extra headers (e.g. Set-Cookie)
};

// Pure helpers (unit-tested on the PC).
bool parseHttpRequest(const std::string& head, HttpRequest* out);
std::string queryParam(const std::string& query, const std::string& name);  // not URL-decoded
std::string urlDecode(const std::string& s);
std::string cookieValue(const std::string& cookieHeader, const std::string& name);
bool constantTimeEqual(const std::string& a, const std::string& b);
// Bearer header or ?token= must equal `token` (which must be non-empty).
bool tokenAuthorized(const HttpRequest& req, const std::string& token);
void setText(HttpResponse* r, int status, const std::string& text, const std::string& type = "text/plain; charset=utf-8");

// Serves a file below webDir. Only [A-Za-z0-9._/-] in the path, no "..", no hidden files,
// "/" means "/index.html". Returns false if the file is not served (the caller sends 404).
bool serveStaticFile(const std::string& webDir, const std::string& urlPath, HttpResponse* resp);

// The dashboard files held in RAM, so serving them never reads the (fragile) flash at run time.
// Loads files of the types serveStaticFile knows, from the folder and one level of sub-folders.
class StaticCache {
public:
    size_t load(const std::string& dir);   // returns the number of files loaded
    bool serve(const std::string& urlPath, HttpResponse* resp) const;
    size_t files() const { return files_.size(); }
    size_t bytes() const { return bytes_; }

private:
    struct Entry { std::string type; std::vector<uint8_t> data; };
    std::map<std::string, Entry> files_;
    size_t bytes_ = 0;
};

class HttpServer {
public:
    // Return true if the request was handled (the response starts as 200 OK with an empty body).
    using Handler = std::function<bool(const HttpRequest&, HttpResponse*)>;

    HttpServer();
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // allInterfaces=false -> 127.0.0.1 only. The caller must have authentication in place
    // before it passes true.
    bool start(int port, bool allInterfaces, Handler handler, std::string* err);
    void stop();
    bool isLocalOnly() const;
    int activeConnections() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aid
