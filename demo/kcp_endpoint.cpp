#include "kcp_endpoint.h"
#include "../ikcp.h"
#include <iostream>
#include <chrono>
#include <cstring>

namespace kcp_demo {

// Helper: get current timestamp in ms
static uint32_t current_ms() {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count() & 0xFFFFFFFF
    );
}

KCP_ENDPOINT::KCP_ENDPOINT() = default;

KCP_ENDPOINT::~KCP_ENDPOINT() {
    shutdown();
    if (kcp_) {
        ikcp_release(kcp_);
        kcp_ = nullptr;
    }
}

bool KCP_ENDPOINT::init(const KCPConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    config_ = config;

    // Create KCP object, pass 'this' as user pointer
    kcp_ = ikcp_create(config_.conv, this);
    if (!kcp_) {
        std::cerr << "[KCP_ENDPOINT] Failed to create KCP object" << std::endl;
        return false;
    }

    // Set output callback
    ikcp_setoutput(kcp_, kcp_output);

    // Apply nodelay mode
    ikcp_nodelay(kcp_, config_.nodelay, config_.interval, config_.resend, config_.nc);

    // Set window sizes
    ikcp_wndsize(kcp_, config_.sndwnd, config_.rcvwnd);

    // Set MTU
    ikcp_setmtu(kcp_, config_.mtu);

    // Set RTO min
    kcp_->rx_minrto = config_.rx_minrto;

    // Stream mode
    kcp_->stream = config_.stream_mode ? 1 : 0;

    // Log mask
    kcp_->logmask = config_.logmask;

    // Set custom log function if logging is enabled
    if (config_.logmask != 0) {
        kcp_->writelog = [](const char* log, ikcpcb* kcp, void* user) {
            (void)kcp;
            if (user) {
                auto* endpoint = static_cast<KCP_ENDPOINT*>(user);
                if (endpoint->on_log_) {
                    endpoint->on_log_(log);
                }
            }
        };
    }

    return true;
}

void KCP_ENDPOINT::set_network_send(NetworkSendFunc send_func) {
    std::lock_guard<std::mutex> lock(mutex_);
    network_send_ = std::move(send_func);
}

void KCP_ENDPOINT::set_on_data(OnDataFunc callback) {
    on_data_ = std::move(callback);
}

void KCP_ENDPOINT::set_on_connect(OnConnectFunc callback) {
    on_connect_ = std::move(callback);
}

void KCP_ENDPOINT::set_on_disconnect(OnDisconnectFunc callback) {
    on_disconnect_ = std::move(callback);
}

void KCP_ENDPOINT::set_on_log(OnLogFunc callback) {
    on_log_ = std::move(callback);
}

int KCP_ENDPOINT::kcp_output(const char* buf, int len, ikcpcb* kcp, void* user) {
    auto* endpoint = static_cast<KCP_ENDPOINT*>(user);
    if (!endpoint) {
        return -1;
    }

    // Invoke the application-provided network send callback
    NetworkSendFunc send_func;
    {
        std::lock_guard<std::mutex> lock(endpoint->mutex_);
        send_func = endpoint->network_send_;
    }

    if (!send_func) {
        return -1;
    }

    int ret = send_func(buf, len, kcp->user);
    if (ret < 0) {
        return -1;
    }

    // Update stats
    endpoint->stats_.sent_bytes.fetch_add(static_cast<uint64_t>(len));
    endpoint->stats_.sent_packets.fetch_add(1);

    return ret;
}

int KCP_ENDPOINT::on_packet_received(const char* data, int size) {
    if (shutdown_.load() || !kcp_) {
        return -1;
    }

    int ret = ikcp_input(kcp_, data, size);
    if (ret < 0) {
        return -1;
    }

    // Update stats
    stats_.recv_bytes.fetch_add(static_cast<uint64_t>(size));
    stats_.recv_packets.fetch_add(1);

    // Deliver received data to application
    deliver_received_data();

    return 0;
}

int KCP_ENDPOINT::send_data(const std::string& data) {
    if (shutdown_.load() || !kcp_) {
        return -1;
    }

    Message msg = Message::create_data(data, 0);
    return send_message(msg);
}

int KCP_ENDPOINT::send_message(const Message& msg) {
    if (shutdown_.load() || !kcp_) {
        return -1;
    }

    // Serialize the message into a buffer
    std::array<char, KCP_MAX_MESSAGE_SIZE + MessageHeader::SIZE> buf;
    size_t serialized_size = msg.serialize(buf.data(), buf.size());
    if (serialized_size == 0) {
        return -1;
    }

    // Send via KCP
    int ret = ikcp_send(kcp_, buf.data(), static_cast<int>(serialized_size));
    if (ret < 0) {
        return -1;
    }

    return ret;
}

void KCP_ENDPOINT::update() {
    if (!kcp_) return;

    uint32_t now = current_ms();
    ikcp_update(kcp_, now);
}

uint32_t KCP_ENDPOINT::get_next_update_time() const {
    if (!kcp_) return 0;
    return ikcp_check(kcp_, current_ms());
}

void KCP_ENDPOINT::flush() {
    if (!kcp_) return;

    ikcp_flush(kcp_);
}

int KCP_ENDPOINT::peek_data_size() const {
    if (!kcp_) return -1;
    return ikcp_peeksize(kcp_);
}

int KCP_ENDPOINT::pending_send_count() const {
    if (!kcp_) return 0;
    return ikcp_waitsnd(kcp_);
}

void KCP_ENDPOINT::shutdown() {
    shutdown_.store(true);
}

void KCP_ENDPOINT::flush_kcp_output() {
    // KCP output is handled directly via the kcp_output callback
    // which invokes the network_send_ function immediately
}

void KCP_ENDPOINT::deliver_received_data() {
    if (!kcp_) return;

    int peek_size = ikcp_peeksize(kcp_);
    while (peek_size > 0) {
        if (peek_size > static_cast<int>(recv_buf_.size())) {
            // Message too large, skip it to avoid infinite loop
            std::cerr << "[KCP_ENDPOINT] Message too large: " << peek_size << " bytes" << std::endl;
            break;
        }

        int received = ikcp_recv(kcp_, recv_buf_.data(), peek_size);
        if (received <= 0) {
            break;
        }

        // Deserialize and deliver to application callback
        Message msg;
        if (Message::deserialize(recv_buf_.data(), static_cast<size_t>(received), msg)) {
            if (on_data_) {
                on_data_(msg);
            }
        }

        // Check for more data
        peek_size = ikcp_peeksize(kcp_);
    }
}

} // namespace kcp_demo
