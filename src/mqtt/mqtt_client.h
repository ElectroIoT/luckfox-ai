// Minimal MQTT 3.1.1 publisher (Phase 8): CONNECT with username/password and a last-will
// message, QoS 0 PUBLISH (optionally retained), keep-alive pings, automatic reconnect with
// backoff. No TLS (LAN use only; documented). Written from the MQTT 3.1.1 spec because no
// MQTT library is available on the board; the packet encoders are unit tested on the PC and the
// client is tested against a fake broker.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aid {

struct MqttOptions {
    std::string host;
    int port = 1883;
    std::string user;
    std::string password;
    std::string clientId = "luckfox-ai";
    int keepAliveS = 30;
    // Last will: published by the broker if the connection is lost without DISCONNECT.
    std::string willTopic;
    std::string willPayload;
    bool willRetain = true;
    int reconnectMinMs = 1000;
    int reconnectMaxMs = 30000;
};

// --- packet encoders (exposed for tests)
std::vector<uint8_t> mqttEncodeRemainingLength(uint32_t n);
std::vector<uint8_t> mqttEncodeConnect(const MqttOptions& o);
std::vector<uint8_t> mqttEncodePublish(const std::string& topic, const std::string& payload, bool retain);
std::vector<uint8_t> mqttEncodePingReq();
std::vector<uint8_t> mqttEncodeDisconnect();

class MqttClient {
public:
    MqttClient();
    ~MqttClient();
    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;

    // Called on the client thread after every successful (re)connect: publish retained state here.
    void setOnConnect(std::function<void()> cb) { onConnect_ = std::move(cb); }

    bool start(const MqttOptions& opts, std::string* err);
    void stop();  // sends DISCONNECT if connected
    bool connected() const { return connected_; }
    std::string lastError() const;

    // Queues a message (thread-safe, never blocks on the network). Returns false if the queue
    // is full (64 messages) or the topic/payload is not acceptable.
    bool publish(const std::string& topic, const std::string& payload, bool retain);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::function<void()> onConnect_;
    std::atomic<bool> connected_{false};
};

}  // namespace aid
