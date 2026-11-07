// Minimal, dependency-free WebSocket broadcast server.
//
// - Runs its own background thread (select() loop), so it never blocks EuroScope.
// - Broadcast() is thread-safe and is the only thing the plugin thread calls.
// - New clients immediately receive the most recent message, so they don't
//   have to wait for the next timer tick.
// - Handles the RFC 6455 handshake, ping/pong and close. Messages sent by
//   clients are handed to an optional callback (on the server thread).
//
// Portable (Winsock / POSIX) so it can be tested off Windows.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class WsServer {
public:
    using MessageHandler = std::function<void(const std::string&)>;

    WsServer() = default;
    ~WsServer();
    WsServer(const WsServer&) = delete;
    WsServer& operator=(const WsServer&) = delete;

    // bindAddress "127.0.0.1" = this PC only, "0.0.0.0" = whole network.
    bool Start(const std::string& bindAddress, uint16_t port, std::string* error = nullptr);
    void Stop();
    bool IsRunning() const { return running_; }

    // Queue a text message for every connected client. Thread-safe, never blocks on I/O.
    void Broadcast(const std::string& text);

    // Called on the server thread for every text message a client sends.
    void SetMessageHandler(MessageHandler handler);

    size_t ClientCount() const { return clientCount_; }
    uint16_t Port() const { return port_; }

private:
    void Run();

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<size_t> clientCount_{0};
    uint16_t port_ = 0;
    intptr_t listenSocket_ = -1;

    std::mutex mutex_;                  // guards the fields below
    std::vector<std::string> pending_;  // messages waiting to be framed
    std::string lastMessage_;           // sent to newly connected clients
    MessageHandler handler_;
};
