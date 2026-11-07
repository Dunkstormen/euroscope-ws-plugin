// Minimal, dependency-free WebSocket server with topics.
//
// - Runs its own background thread (select() loop), so it never blocks EuroScope.
// - Publish(), SendTo() and Subscribe() are thread-safe and are all the plugin thread calls.
// - Every message belongs to a topic ("aircraft", "controllers", ...). Each client is
//   subscribed to a set of topics (SetDefaultTopics() for new clients) and only receives those.
// - A published message can be retained: the latest retained message per topic is sent to
//   clients when they connect or subscribe to that topic, so they don't wait for the next tick.
// - Handles the RFC 6455 handshake, ping/pong and close. Text messages sent by clients are
//   handed to an optional callback (on the server thread), tagged with the client's id.
//
// Portable (Winsock / POSIX) so it can be tested off Windows.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class WsServer {
public:
    using ClientId = uint64_t;
    using MessageHandler = std::function<void(ClientId, const std::string&)>;
    using CloseHandler = std::function<void(ClientId)>;

    WsServer() = default;
    ~WsServer();
    WsServer(const WsServer&) = delete;
    WsServer& operator=(const WsServer&) = delete;

    // bindAddress "127.0.0.1" = this PC only, "0.0.0.0" = whole network.
    bool Start(const std::string& bindAddress, uint16_t port, std::string* error = nullptr);
    void Stop();
    bool IsRunning() const { return running_; }

    // Queue a message for every client subscribed to `topic`. Never blocks on I/O.
    void Publish(const std::string& topic, const std::string& text, bool retain);
    // Queue a message for one client (replies, filtered data).
    void SendTo(ClientId client, const std::string& text);
    // Replace a client's topics. Retained messages of newly added topics are sent to it.
    void Subscribe(ClientId client, const std::set<std::string>& topics);
    // Topics a client gets when it connects (default: none).
    void SetDefaultTopics(const std::set<std::string>& topics);
    // True if at least one connected client is subscribed to `topic`.
    bool HasSubscribers(const std::string& topic);
    // Forget all retained messages (e.g. after restarting the server).
    void ClearRetained();

    // Called on the server thread for every text message a client sends.
    void SetMessageHandler(MessageHandler handler);
    // Called on the server thread when a client that completed the handshake goes away.
    void SetCloseHandler(CloseHandler handler);

    size_t ClientCount() const { return clientCount_; }
    uint16_t Port() const { return port_; }

private:
    void Run();

    struct Outgoing {
        std::string topic;    // empty: direct message to `client`
        ClientId client = 0;
        std::string text;
    };

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<size_t> clientCount_{0};
    uint16_t port_ = 0;
    intptr_t listenSocket_ = -1;

    std::mutex mutex_;  // guards the fields below
    std::vector<Outgoing> pending_;
    std::vector<std::pair<std::string, std::string>> retained_;  // topic -> text, in first-publish order
    std::map<ClientId, std::set<std::string>> subscriptions_;     // open clients only
    std::set<std::string> defaultTopics_;
    MessageHandler handler_;
    CloseHandler closeHandler_;
};
