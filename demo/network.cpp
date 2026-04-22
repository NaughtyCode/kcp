#include "network.h"
#include <chrono>

namespace kcp_demo {

// ===== UDPSocket ===

UDPSocket::UDPSocket() : sock_(INVALID_SOCKET) {}

UDPSocket::~UDPSocket() {
    close();
}

bool UDPSocket::create() {
#ifdef _WIN32
    if (sock_ != INVALID_SOCKET) close();

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "[UDPSocket] WSAStartup failed" << std::endl;
        return false;
    }
#endif

    sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ == INVALID_SOCKET) {
        std::cerr << "[UDPSocket] Failed to create socket" << std::endl;
        return false;
    }

#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sock_, FIONBIO, &mode);
#else
    int flags = fcntl(sock_, F_GETFL, 0);
    fcntl(sock_, F_SETFL, flags | O_NONBLOCK);
#endif

    // Allow address reuse
    int opt = 1;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    return true;
}

bool UDPSocket::bind(const std::string& addr, uint16_t port) {
    if (!is_open() && !create()) return false;

    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);

    if (addr == "0.0.0.0" || addr.empty()) {
        sin.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, addr.c_str(), &sin.sin_addr);
    }

    if (::bind(sock_, reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) < 0) {
        std::cerr << "[UDPSocket] Failed to bind to " << addr << ":" << port << std::endl;
        return false;
    }

    return true;
}

int UDPSocket::send_to(const char* data, int len, const std::string& addr, uint16_t port) {
    if (!is_open()) return -1;

    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    inet_pton(AF_INET, addr.c_str(), &sin.sin_addr);

    return sendto(sock_, data, len, 0, reinterpret_cast<sockaddr*>(&sin), sizeof(sin));
}

int UDPSocket::recv_from(char* buf, int buf_size, std::string& src_addr, uint16_t& src_port) {
    if (!is_open()) return -1;

    sockaddr_in sin{};
    socklen_t sin_len = sizeof(sin);

    int n = recvfrom(sock_, buf, buf_size, 0, reinterpret_cast<sockaddr*>(&sin), &sin_len);
    if (n <= 0) return -1;

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sin.sin_addr, ip_str, sizeof(ip_str));
    src_addr = ip_str;
    src_port = ntohs(sin.sin_port);

    return n;
}

void UDPSocket::close() {
    if (is_open()) {
#ifdef _WIN32
        closesocket(sock_);
#else
        ::close(sock_);
#endif
        sock_ = INVALID_SOCKET;
    }
}

// ===== PacketQueue ===

void PacketQueue::push(const std::string& data) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(data);
    }
    cv_.notify_one();
}

bool PacketQueue::pop(std::string& data, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                      [this] { return !queue_.empty() || shutdown_; })) {
        return false;
    }
    if (shutdown_ && queue_.empty()) return false;

    data = std::move(queue_.front());
    queue_.pop();
    return true;
}

size_t PacketQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void PacketQueue::notify() {
    cv_.notify_one();
}

void PacketQueue::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
    }
    cv_.notify_all();
}

// ===== NetworkLayer ===

NetworkLayer::NetworkLayer() = default;

NetworkLayer::~NetworkLayer() {
    stop();
}

bool NetworkLayer::server_init(uint16_t port) {
    server_port_ = port;
    return udp_socket_.create() && udp_socket_.bind("0.0.0.0", port);
}

bool NetworkLayer::client_connect(const std::string& server_addr, uint16_t server_port) {
    server_addr_ = server_addr;
    client_server_port_ = server_port;
    return udp_socket_.create();
}

void NetworkLayer::start() {
    if (running_.load()) return;
    running_.store(true);
    io_thread_ = std::thread(&NetworkLayer::io_loop, this);
}

void NetworkLayer::stop() {
    if (!running_.load()) return;

    running_.store(false);
    incoming_.shutdown();
    outgoing_.shutdown();
    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        for (auto& [peer, queue] : peer_queues_) {
            queue.shutdown();
        }
    }

    if (io_thread_.joinable()) {
        io_thread_.join();
    }
}

void NetworkLayer::set_on_new_peer(std::function<bool(const PeerInfo&)> callback) {
    on_new_peer_ = std::move(callback);
}

void NetworkLayer::register_peer(const PeerInfo& peer) {
    std::lock_guard<std::mutex> lock(peer_mutex_);
    peer_queues_[peer]; // Create default-constructed PacketQueue
}

void NetworkLayer::unregister_peer(const PeerInfo& peer) {
    std::lock_guard<std::mutex> lock(peer_mutex_);
    auto it = peer_queues_.find(peer);
    if (it != peer_queues_.end()) {
        it->second.shutdown();
        peer_queues_.erase(it);
    }
}

PacketQueue& NetworkLayer::get_peer_queue(const PeerInfo& peer) {
    std::lock_guard<std::mutex> lock(peer_mutex_);
    return peer_queues_[peer];
}

int NetworkLayer::send_to_peer(const PeerInfo& peer, const char* data, int size) {
    return udp_socket_.send_to(data, size, peer.first, peer.second);
}

void NetworkLayer::send_raw(const char* data, int size) {
    // For client mode: push to outgoing queue (will be sent in io_loop)
    // For server mode: this is not used; each KCP endpoint sends via its own callback
    if (server_addr_.empty()) {
        // Server mode: ignore send_raw calls; use send_to_peer() instead
        return;
    }
    outgoing_.push(std::string(data, size));
}

uint16_t NetworkLayer::local_port() const {
    return server_port_;
}

void NetworkLayer::io_loop() {
    char recv_buf[1500];
    std::string src_addr;
    uint16_t src_port;
    std::string outgoing_data;

    while (running_.load()) {
        // Process outgoing packets (client mode)
        while (outgoing_.pop(outgoing_data, 10)) {
            if (!running_.load()) break;
            if (!server_addr_.empty()) {
                int sent = udp_socket_.send_to(
                    outgoing_data.data(), static_cast<int>(outgoing_data.size()),
                    server_addr_, client_server_port_);
                (void)sent;
            }
        }

        // Process incoming packets
        int n = udp_socket_.recv_from(recv_buf, sizeof(recv_buf),
                                       src_addr, src_port);
        if (n > 0) {
            PeerInfo peer = {src_addr, src_port};

            // Check if peer is registered
            {
                std::lock_guard<std::mutex> lock(peer_mutex_);
                auto it = peer_queues_.find(peer);
                if (it != peer_queues_.end()) {
                    // Known peer: push to its queue
                    it->second.push(std::string(recv_buf, n));
                    continue;
                }
            }

            // Unknown peer: call on_new_peer callback
            if (on_new_peer_) {
                try {
                    if (on_new_peer_(peer)) {
                        // Peer registered by callback, this packet was consumed
                        continue;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "[NetworkLayer] on_new_peer exception: " << e.what() << std::endl;
                    // Push to incoming queue as fallback
                    incoming_.push(std::string(recv_buf, n));
                    continue;
                }
            }

            // Unknown peer and no handler: push to incoming_ queue (backward compatibility)
            incoming_.push(std::string(recv_buf, n));
        } else {
            // Brief sleep to avoid busy-waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

} // namespace kcp_demo
