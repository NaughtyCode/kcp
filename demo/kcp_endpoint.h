#pragma once
#include "../ikcp.h"
#include "message.h"
#include <functional>
#include <mutex>
#include <memory>
#include <queue>
#include <atomic>
#include <string>
#include <array>

namespace kcp_demo {

// Network output callback type
// Returns actual bytes sent (or -1 for error)
using NetworkSendFunc = std::function<int(const char*, int, void*)>;

// Application callbacks
using OnDataFunc = std::function<void(const Message&)>;
using OnConnectFunc = std::function<void()>;
using OnDisconnectFunc = std::function<void(const std::string& reason)>;
using OnLogFunc = std::function<void(const std::string&)>;

// KCP configuration
struct KCPConfig {
    uint32_t conv = 0;          // Session ID (must match peer)
    int nodelay = 1;            // Enable nodelay (0=default, 1=normal, 2=fast, 3=extreme)
    int interval = 10;          // Internal update interval (ms)
    int resend = 2;             // Fast resend threshold
    int nc = 1;                 // No congestion control
    int sndwnd = 1024;
    int rcvwnd = 1024;
    int mtu = 1400;
    int rx_minrto = 10;
    bool stream_mode = false;
    int logmask = IKCP_LOG_OUTPUT;  // Log mask
};

// Statistics
struct EndpointStats {
    std::atomic<uint64_t> sent_bytes{0};
    std::atomic<uint64_t> recv_bytes{0};
    std::atomic<uint32_t> sent_packets{0};
    std::atomic<uint32_t> recv_packets{0};
    std::atomic<uint32_t> resend_count{0};
};

class KCP_ENDPOINT {
public:
    KCP_ENDPOINT();
    ~KCP_ENDPOINT();

    // Non-copyable
    KCP_ENDPOINT(const KCP_ENDPOINT&) = delete;
    KCP_ENDPOINT& operator=(const KCP_ENDPOINT&) = delete;

    // Initialize with config
    bool init(const KCPConfig& config);

    // Set network output function (called by KCP when it has data to send)
    void set_network_send(NetworkSendFunc send_func);

    // Set application callbacks
    void set_on_data(OnDataFunc callback);
    void set_on_connect(OnConnectFunc callback);
    void set_on_disconnect(OnDisconnectFunc callback);
    void set_on_log(OnLogFunc callback);

    // Call this when a KCP packet is received (from network)
    int on_packet_received(const char* data, int size);

    // Send application data (automatic message framing)
    int send_data(const std::string& data);

    // Send any message type
    int send_message(const Message& msg);

    // Update KCP state (call regularly, ~10ms interval recommended)
    void update();

    // Get next update time
    uint32_t get_next_update_time() const;

    // Flush pending data
    void flush();

    // Check received data size
    int peek_data_size() const;

    // Get pending send count
    int pending_send_count() const;

    // Get stats
    const EndpointStats& stats() const { return stats_; }

    // Shutdown
    void shutdown();

    // Check if shutdown
    bool is_shutdown() const { return shutdown_.load(); }

private:
    // KCP output callback (static, bridges to instance method)
    static int kcp_output(const char* buf, int len, ikcpcb* kcp, void* user);

    // Internal flush KCP data and deliver via network
    void flush_kcp_output();

    // Deliver received data to application
    void deliver_received_data();

    // KCP internals
    ikcpcb* kcp_ = nullptr;
    KCPConfig config_;
    NetworkSendFunc network_send_;

    // Application callbacks
    OnDataFunc on_data_;
    OnConnectFunc on_connect_;
    OnDisconnectFunc on_disconnect_;
    OnLogFunc on_log_;

    // Internal buffers
    std::array<char, KCP_MAX_MESSAGE_SIZE> recv_buf_;

    // Stats
    EndpointStats stats_;

    // State
    std::atomic<bool> shutdown_{false};
    std::mutex mutex_;
};

} // namespace kcp_demo
