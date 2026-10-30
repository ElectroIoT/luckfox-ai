// Dashboard API (Phases 7 and 9): routes, login/session handling, CSRF guard, and the glue
// between HTTP and the rest of aid-core through ApiBackend callbacks (so it can be unit tested
// on the PC with fake backends).
//
// Access rules
//   public:        GET /health, static dashboard files, GET /api/auth/state, POST /api/login
//   token OR login: GET /snapshot.jpg, GET /status.json   (token = Authorization: Bearer / ?token=)
//   login only:    everything else below /api/
//   state-changing requests (POST/DELETE) must also send "X-Requested-With: aid-dashboard"
//   (a header a foreign web page cannot add without a CORS preflight, which we never allow).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "auth.h"
#include "aid_config.h"
#include "event_store.h"
#include "http_server.h"

namespace aid {

struct ApiBackend {
    std::function<std::string()> statusJson;       // /status.json and /api/status
    std::function<std::string()> detectionsJson;   // current detections + AI status
    std::function<bool(std::vector<uint8_t>*, std::string*)> snapshot;
    std::function<std::string()> mqttJson;
    std::function<std::string()> gpioJson;
    std::function<bool(const std::string&, bool, std::string*)> gpioSet;
    std::function<Config()> getConfig;
    // Validates nothing itself (the API already did); saves and applies. *restartRequired is set
    // when some changed setting only takes effect after a restart.
    std::function<bool(const Config&, std::string*, bool*)> applyConfig;
    std::function<bool(const std::string&)> systemAction;   // "restart_service", "reboot", "shutdown"
    std::function<std::string()> systemInfoJson;            // firmware, kernel, network, storage
};

class Api {
public:
    Api(AuthManager* auth, EventStore* events, const std::string& webDir,
        const std::string& machineToken, int sessionMaxHours, ApiBackend backend);

    // For HttpServer::start(). nowMs is injected so tests can control time (0 = real clock).
    bool handle(const HttpRequest& req, HttpResponse* resp, int64_t nowMs = 0);

private:
    bool handleApi(const HttpRequest& req, HttpResponse* resp, int64_t nowMs);
    void json(HttpResponse* r, int status, const std::string& body) const;
    void error(HttpResponse* r, int status, const std::string& msg) const;
    std::string configJson(const Config& c) const;
    bool applyConfigPatch(const std::string& body, std::string* err, bool* restartRequired);

    AuthManager* auth_;
    EventStore* events_;
    std::string webDir_, token_;
    int maxHours_;
    ApiBackend be_;
};

}  // namespace aid
