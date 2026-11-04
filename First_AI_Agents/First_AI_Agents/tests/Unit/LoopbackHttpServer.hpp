#pragma once

// Test double for the REAL transport: a tiny HTTP/1.1 server on 127.0.0.1 (no external network, ephemeral port) that
// answers every request with one scripted status and body and counts how many requests it actually received. It lets a
// test drive the production HttpClient + libcurl + retry policy end to end and read the true number of sends.
// Windows only (Winsock); on other platforms the tests that use it compile to a no-op.

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>

#pragma comment(lib, "ws2_32.lib")

namespace ai::testing {

class LoopbackHttpServer {
public:
    LoopbackHttpServer(int status, std::string body)
        : status_(status), body_(std::move(body))
    {
        WSADATA data {};
        started_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        if (!started_) return;
        const SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET) return;
        listener_.store(listener);   // published before the worker thread exists
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = 0;
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0
                || ::listen(listener, 8) != 0) {
            closeListener();
            return;
        }
        int length = sizeof(address);
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        worker_ = std::thread([this] { serve(); });
    }

    ~LoopbackHttpServer()
    {
        closeListener();
        if (worker_.joinable()) worker_.join();
        if (started_) WSACleanup();
    }

    LoopbackHttpServer(const LoopbackHttpServer&) = delete;
    LoopbackHttpServer& operator=(const LoopbackHttpServer&) = delete;

    [[nodiscard]] bool ready() const noexcept { return port_ != 0; }
    [[nodiscard]] std::string url(const std::string& path = "/v1/responses") const
    {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }
    [[nodiscard]] int requestsServed() const noexcept { return served_.load(); }

private:
    void closeListener()
    {
        // The worker thread reads listener_ while the destructor retires it: ownership is handed over atomically, so
        // exactly one side closes the socket, and shutdown() unblocks a pending accept() before join().
        const SOCKET listener = listener_.exchange(INVALID_SOCKET);
        if (listener != INVALID_SOCKET) {
            ::shutdown(listener, SD_BOTH);
            ::closesocket(listener);
        }
    }

    void serve()
    {
        for (;;) {
            const SOCKET listener = listener_.load();
            if (listener == INVALID_SOCKET) return;
            const SOCKET client = ::accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET) return;
            handle(client);
            ::closesocket(client);
        }
    }

    void handle(SOCKET client)
    {
        DWORD timeoutMs = 3000;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
        std::string received;
        char buffer[4096];
        std::size_t headerEnd = std::string::npos;
        std::size_t needed = 0;
        for (;;) {
            const int count = ::recv(client, buffer, sizeof(buffer), 0);
            if (count <= 0) break;
            received.append(buffer, static_cast<std::size_t>(count));
            if (headerEnd == std::string::npos) {
                headerEnd = received.find("\r\n\r\n");
                if (headerEnd != std::string::npos) {
                    needed = headerEnd + 4;
                    std::string lower = received.substr(0, headerEnd);
                    for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    const auto at = lower.find("content-length:");
                    if (at != std::string::npos) needed += static_cast<std::size_t>(std::atoll(lower.c_str() + at + 15));
                }
            }
            if (headerEnd != std::string::npos && received.size() >= needed) break;
        }
        if (headerEnd == std::string::npos) return;   // not a request: not counted
        ++served_;
        const std::string reply = "HTTP/1.1 " + std::to_string(status_) + " Scripted\r\n"
            "Content-Type: application/json\r\nContent-Length: " + std::to_string(body_.size())
            + "\r\nConnection: close\r\n\r\n" + body_;
        ::send(client, reply.data(), static_cast<int>(reply.size()), 0);
    }

    int status_;
    std::string body_;
    bool started_ {false};
    std::atomic<SOCKET> listener_ {INVALID_SOCKET};
    unsigned short port_ {0};
    std::atomic<int> served_ {0};
    std::thread worker_;
};

} // namespace ai::testing

#endif // _WIN32
