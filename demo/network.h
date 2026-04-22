#pragma once
#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
#endif

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <iostream>
#include <cstring>
#include <map>

namespace kcp_demo {

// UDP Socket abstraction
class UDPSocket {
public:
    UDPSocket();
    ~UDPSocket();

    bool create();
    bool bind(const std::string& addr, uint16_t port);
    int send_to(const char* data, int len, const std::string& addr, uint16_t port);
    int recv_from(char* buf, int buf_size, std::string& src_addr, uint16_t& src_port);
    void close();

    bool is_open() const { return sock_ != INVALID_SOCKET; }

private:
#ifdef _WIN32
    SOCKET sock_;
#else
    int sock_;
    static constexpr int INVALID_SOCKET = -1;
#endif
};

// Thread-safe packet queue for the I/O loop
class PacketQueue {
public:
    void push(const std::string& data);
    bool pop(std::string& data, int timeout_ms = 100);
    size_t size() const;
    void notify();  // Wake up waiting threads
    void shutdown();

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::string> queue_;
    bool shutdown_ = false;
};

// Peer info: (address, port) pair
using PeerInfo = std::pair<std::string, uint16_t>;

// Per-peer send callback: sends KCP data to a specific peer
using PeerSendFunc = std::function<void(const char* data, int size)>;

// The network layer: handles UDP I/O, packet queueing, and thread management
// For multi-client server mode: each peer has its own send callback
class NetworkLayer {
public:
    NetworkLayer();
    ~NetworkLayer();

    // Server mode
    bool server_init(uint16_t port);

    // Client mode
    bool client_connect(const std::string& server_addr, uint16_t server_port);

    // Start network I/O thread
    void start();

    // Stop network I/O thread
    void stop();

    // Server mode: register a send callback for a peer
    void register_peer(const PeerInfo& peer, PeerSendFunc send_func);
    
    // Server mode: register a peer with default callback (uses send_to_peer)
    void register_peer(const PeerInfo& peer);

    // Server mode: remove a peer and its callback
    void unregister_peer(const PeerInfo& peer);

    // Server mode: notify that a peer's address changed
    void update_peer(const PeerInfo& old_peer, const PeerInfo& new_peer);

    // Server mode: get the queue for a specific peer
    PacketQueue& get_peer_queue(const PeerInfo& peer);

    // Server mode: send data to a specific peer
    int send_to_peer(const PeerInfo& peer, const char* data, int size);

    // Client mode: send raw bytes (from KCP) to configured server
    void send_raw(const char* data, int size);

    // Get the local UDP socket's port
    uint16_t local_port() const;

    // Check if running
    bool is_running() const { return running_.load(); }

    // Check if in server mode
    bool is_server() const { return server_port_ > 0; }

    // Get incoming queue (used by server/client main loop)
    PacketQueue& incoming_queue() { return incoming_; }

    // Server mode: set callback for new peers
    void set_on_new_peer(std::function<bool(const PeerInfo&)> callback);

private:
    void io_loop();

    UDPSocket udp_socket_;
    std::thread io_thread_;
    std::atomic<bool> running_{false};

    // Server mode state
    uint16_t server_port_ = 0;

    // Client mode state
    std::string server_addr_ = "";
    uint16_t client_server_port_ = 0;

    // Packet queues (both directions)
    PacketQueue incoming_;   // Network -> KCP

    // Client mode: outgoing packet queue (KCP -> Network)
    PacketQueue outgoing_;

    // Server mode: per-peer send callbacks
    std::map<PeerInfo, PeerSendFunc> peer_send_funcs_;
    
    // Server mode: per-peer packet queues
    std::map<PeerInfo, PacketQueue> peer_queues_;
    mutable std::mutex peer_mutex_;
    
    // Callback for new peers
    std::function<bool(const PeerInfo&)> on_new_peer_;
};

} // namespace kcp_demo
