// Minimal HTTP/1.1 server for aid-core (Phase 4): GET only, one connection at a time,
// short timeouts, optional bearer token. It is NOT the dashboard server: no login pages,
// no file serving. The dashboard (Phase 7) will build on or replace it.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aid {

struct HttpRequest {
    std::string method;
    std::string path;    // without the query string
    std::string query;   // after '?', not decoded
    std::string bearer;  // from "Authorization: Bearer <token>", empty if absent
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "text/plain";
    std::vector<uint8_t> body;
};

// Pure function, unit-testable on the PC. `head` is the request text up to and including the
// blank line. Returns false for anything that is not a well-formed request line.
bool parseHttpRequest(const std::string& head, HttpRequest* out);

// Looks for `token=<value>` in a query string; returns "" if absent.
std::string queryParam(const std::string& query, const std::string& name);

// Compares in time that does not depend on where the strings differ.
bool constantTimeEqual(const std::string& a, const std::string& b);

class HttpServer {
public:
    // Return true if the request was handled (the response then starts as 200 OK with an
    // empty body; set status/contentType/body as needed). Return false for "not found".
    using Handler = std::function<bool(const HttpRequest&, HttpResponse*)>;

    HttpServer();
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // token empty  -> listens on 127.0.0.1 only, no token check.
    // token set    -> listens on all interfaces; every request needs the token
    //                 (Authorization: Bearer <token> or ?token=<token>).
    // A token shorter than 16 characters is refused.
    bool start(int port, const std::string& token, Handler handler, std::string* err);
    void stop();
    bool isLocalOnly() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aid
