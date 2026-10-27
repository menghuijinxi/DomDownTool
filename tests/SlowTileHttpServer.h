#pragma once

#include <winsock2.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace tile_queue_test
{
class WinsockScope
{
  public:
    WinsockScope()
    {
        WSADATA information{};
        if (WSAStartup(MAKEWORD(2, 2), &information) != 0)
        {
            throw std::runtime_error("Cannot initialize loopback tile server");
        }
    }
    ~WinsockScope() { WSACleanup(); }
};

class Socket
{
  public:
    explicit Socket(SOCKET handle) : handle_(handle)
    {
        if (handle_ == INVALID_SOCKET)
        {
            throw std::runtime_error("Cannot open loopback tile socket");
        }
    }
    ~Socket()
    {
        if (handle_ != INVALID_SOCKET)
        {
            closesocket(handle_);
        }
    }
    Socket(Socket&& other) noexcept : handle_(std::exchange(other.handle_, INVALID_SOCKET)) {}
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    SOCKET GetHandle() const { return handle_; }

  protected:
    SOCKET handle_;
};

class SlowTileHttpServer
{
  public:
    explicit SlowTileHttpServer(const std::vector<unsigned char>& image)
        : listener_(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)),
          image_(reinterpret_cast<const char*>(image.data()), image.size())
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener_.GetHandle(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
                0 ||
            listen(listener_.GetHandle(), SOMAXCONN) != 0)
        {
            throw std::runtime_error("Cannot bind loopback tile server");
        }
        int addressLength = sizeof(address);
        if (getsockname(listener_.GetHandle(), reinterpret_cast<sockaddr*>(&address),
                        &addressLength) != 0)
        {
            throw std::runtime_error("Cannot query loopback tile port");
        }
        url_ = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
        listenerThread_ = std::jthread([this](std::stop_token stop) { AcceptRequests(stop); });
    }

    ~SlowTileHttpServer()
    {
        listenerThread_.request_stop();
        listenerThread_.join();
        for (auto& client : clients_)
        {
            client.request_stop();
        }
        condition_.notify_all();
        clients_.clear();
    }

    const std::string& GetUrl() const { return url_; }
    size_t GetSlowRequestCount() const { return slowRequests_.load(); }
    size_t GetPartialRequestCount() const
    {
        return partialRequests_.load();
    }
    size_t GetHttpErrorRequestCount() const
    {
        return httpErrorRequests_.load();
    }
    void ReleaseSlowResponses()
    {
        {
            const std::lock_guard lock(mutex_);
            releaseSlowResponses_ = true;
        }
        condition_.notify_all();
    }

  protected:
    WinsockScope winsock_;
    Socket listener_;
    std::string image_;
    std::string url_;
    std::atomic_size_t slowRequests_ = 0;
    std::atomic_size_t partialRequests_ = 0;
    std::atomic_size_t httpErrorRequests_ = 0;
    bool releaseSlowResponses_ = false;
    std::mutex mutex_;
    std::condition_variable_any condition_;
    std::vector<std::jthread> clients_;
    std::jthread listenerThread_;

  private:
    static bool Send(SOCKET socket, std::string_view bytes)
    {
        while (!bytes.empty())
        {
            const int sent = send(socket, bytes.data(), static_cast<int>(bytes.size()), 0);
            if (sent <= 0)
            {
                return false;
            }
            bytes.remove_prefix(static_cast<size_t>(sent));
        }
        return true;
    }

    void AcceptRequests(std::stop_token stop)
    {
        while (!stop.stop_requested())
        {
            fd_set sockets;
            FD_ZERO(&sockets);
            FD_SET(listener_.GetHandle(), &sockets);
            timeval timeout{0, 100000};
            if (select(0, &sockets, nullptr, nullptr, &timeout) <= 0)
            {
                continue;
            }
            const SOCKET accepted = accept(listener_.GetHandle(), nullptr, nullptr);
            if (accepted == INVALID_SOCKET)
            {
                continue;
            }
            clients_.emplace_back([this, client = Socket(accepted)](std::stop_token clientStop) {
                const DWORD receiveTimeout = 1000;
                setsockopt(client.GetHandle(), SOL_SOCKET, SO_RCVTIMEO,
                           reinterpret_cast<const char*>(&receiveTimeout), sizeof(receiveTimeout));
                char request[4096]{};
                const int received = recv(client.GetHandle(), request, sizeof(request), 0);
                if (received <= 0)
                {
                    return;
                }
                const bool slow = std::string_view(request, received).find("GET /slow") == 0;
                const bool partial =
                    std::string_view(request, received).find("GET /retry-partial") == 0;
                const bool httpError =
                    std::string_view(request, received).find("GET /retry-http-error") == 0;
                const bool truncate = partial && partialRequests_.fetch_add(1) == 0;
                const bool reject = httpError && httpErrorRequests_.fetch_add(1) == 0;
                const std::string status = reject ? "503 Service Unavailable" : "200 OK";
                const std::string header =
                    "HTTP/1.1 " + status +
                    "\r\nContent-Type: image/png\r\nConnection: close\r\nContent-Length: " +
                    std::to_string(image_.size()) + "\r\n\r\n";
                if (!Send(client.GetHandle(), header))
                {
                    return;
                }
                if (truncate)
                {
                    (void)Send(client.GetHandle(), std::string_view(image_).substr(0, 16));
                }
                else if (slow)
                {
                    if (!Send(client.GetHandle(), std::string_view(image_).substr(0, 1)))
                    {
                        return;
                    }
                    ++slowRequests_;
                    std::unique_lock lock(mutex_);
                    condition_.wait_for(lock, clientStop, std::chrono::seconds(30),
                                        [this] { return releaseSlowResponses_; });
                    (void)Send(client.GetHandle(), std::string_view(image_).substr(1));
                }
                else
                {
                    (void)Send(client.GetHandle(), image_);
                }
            });
        }
    }
};
} // namespace tile_queue_test
