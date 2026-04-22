// KCP Client Application
// Usage: kcp_client [server_ip] [port]
// Defaults: 127.0.0.1:7788

#include "kcp_endpoint.h"
#include "network.h"
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>
#include <random>

namespace {

constexpr const char* DEFAULT_SERVER_ADDR = "127.0.0.1";
constexpr uint16_t DEFAULT_SERVER_PORT = 7788;
constexpr uint32_t DEFAULT_CONV = 0xDEADBEEF;  // Must match server CONV

}  // namespace

int main(int argc, char* argv[]) {
    std::string server_addr = DEFAULT_SERVER_ADDR;
    uint16_t server_port = DEFAULT_SERVER_PORT;

    if (argc > 1) server_addr = argv[1];
    if (argc > 2) server_port = static_cast<uint16_t>(std::atoi(argv[2]));

    std::cout << "========================================" << std::endl;
    std::cout << "  KCP Client" << std::endl;
    std::cout << "  Connecting to " << server_addr << ":" << server_port << std::endl;
    std::cout << "  Press Ctrl+C to quit." << std::endl;
    std::cout << "========================================" << std::endl;

    // Create network layer (client mode - connect to server)
    kcp_demo::NetworkLayer network;
    if (!network.client_connect(server_addr, server_port)) {
        std::cerr << "Failed to initialize network client" << std::endl;
        return 1;
    }

    // Use the same CONV as server so KCP accepts the packets
    // Configure KCP - fast mode for low latency
    kcp_demo::KCPConfig config;
    config.conv = DEFAULT_CONV;
    config.nodelay = 2;       // Fast mode
    config.interval = 10;     // 10ms internal timer
    config.resend = 2;        // Fast resend
    config.nc = 1;            // No congestion control
    config.sndwnd = 1024;
    config.rcvwnd = 1024;
    config.mtu = 1400;
    config.rx_minrto = 10;    // Minimum RTO 10ms

    // Create KCP endpoint
    kcp_demo::KCP_ENDPOINT kcp;
    if (!kcp.init(config)) {
        std::cerr << "Failed to initialize KCP" << std::endl;
        return 1;
    }

    // Set network output: KCP sends raw bytes to network layer
    kcp.set_network_send([&network](const char* data, int size, void* user) -> int {
        (void)user;
        network.send_raw(data, size);
        return size;
    });

    // Sequence counter for application messages
    std::atomic<uint32_t> seq{0};
    std::atomic<uint32_t> received_count{0};
    std::atomic<uint32_t> echo_count{0};
    bool handshake_complete = false;
    std::mutex handshake_mutex;

    // Set up message handler
    kcp.set_on_data([&](const kcp_demo::Message& msg) {
        switch (msg.header.type) {
            case kcp_demo::MessageType::KCP_HANDSHAKE_REQ: {
                std::cout << "[HANDSHAKE REQ] Unexpected on client" << std::endl;
                break;
            }

            case kcp_demo::MessageType::KCP_HANDSHAKE_RESP: {
                auto* resp = std::get_if<kcp_demo::HandshakeResp>(&msg.payload);
                if (resp) {
                    std::cout << "[HANDSHAKE OK] token=" << resp->server_token
                              << " msg=" << resp->server_message << std::endl;
                    {
                        std::lock_guard<std::mutex> lock(handshake_mutex);
                        handshake_complete = true;
                    }
                }
                break;
            }

            case kcp_demo::MessageType::KCP_HEARTBEAT: {
                auto* hb = std::get_if<kcp_demo::Heartbeat>(&msg.payload);
                if (hb) {
                    std::cout << "[HEARTBEAT] from server: " << hb->payload << std::endl;
                }
                break;
            }

            case kcp_demo::MessageType::KCP_DATA: {
                std::string body = msg.get_body();
                std::cout << "[ECHO] " << body << std::endl;
                echo_count++;
                received_count++;
                break;
            }

            case kcp_demo::MessageType::KCP_DISCONNECT: {
                auto* disc = std::get_if<kcp_demo::Disconnect>(&msg.payload);
                std::string reason = disc ? disc->reason : "unknown";
                std::cout << "[DISCONNECT] " << reason << std::endl;
                kcp.shutdown();
                break;
            }

            default:
                std::cout << "[?] Unknown type: " << (int)msg.header.type << std::endl;
                break;
        }
    });

    // Log callback
    kcp.set_on_log([](const std::string& log) {
        // Uncomment for debug: std::cout << "[KCP] " << log << std::endl;
    });

    // Connect/disconnect callbacks
    kcp.set_on_connect([]() {
        std::cout << "[KCP] Connected to server" << std::endl;
    });

    kcp.set_on_disconnect([](const std::string& reason) {
        std::cout << "[KCP] Disconnected: " << reason << std::endl;
    });

    // Start network I/O thread
    network.start();

    // Wait for network to initialize, then send handshake
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    uint32_t current_seq = seq++;
    auto hs_msg = kcp_demo::Message::create_handshake_req("KCP_Demo_Client", current_seq);
    kcp.send_message(hs_msg);
    std::cout << "[SENT] Handshake request (seq=" << current_seq << ")" << std::endl;

    std::cout << "\nClient ready. Type messages to send to server." << std::endl;
    std::cout << "Type 'quit' or 'exit' to disconnect.\n" << std::endl;

    // Input thread for user input
    std::thread input_thread([&]() {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (kcp.is_shutdown()) break;

            if (line.empty()) continue;

            if (line == "quit" || line == "exit") {
                current_seq = seq++;
                auto disc = kcp_demo::Message::create_disconnect("Client exiting", current_seq);
                kcp.send_message(disc);
                kcp.shutdown();
                break;
            }

            current_seq = seq++;
            auto msg = kcp_demo::Message::create_data(line, current_seq);
            kcp.send_message(msg);
            std::cout << "[SENT] " << line << " (seq=" << current_seq << ")" << std::endl;
        }
    });

    // Main event loop
    uint32_t last_heartbeat = 0;
    uint32_t heartbeat_interval = 3000;  // 3 seconds

    while (!kcp.is_shutdown()) {
        // Update KCP timers
        kcp.update();
        kcp.flush();

        // Process incoming network packets
        std::string packet;
        while (network.incoming_queue().pop(packet, 10)) {
            kcp.on_packet_received(packet.data(), static_cast<int>(packet.size()));
        }

        // Periodic heartbeat
        auto now = std::chrono::steady_clock::now();
        uint32_t now_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count() & 0xFFFFFFFF);

        if (now_ms - last_heartbeat >= heartbeat_interval) {
            last_heartbeat = now_ms;
            current_seq = seq++;
            std::string hb_body = "ping from client at " + std::to_string(now_ms);
            auto hb_msg = kcp_demo::Message::create_heartbeat(now_ms, hb_body, current_seq);
            kcp.send_message(hb_msg);
        }

        // Print stats every 5 seconds
        static uint32_t stats_timer = 0;
        if (now_ms - stats_timer >= 5000) {
            stats_timer = now_ms;
            const auto& stats = kcp.stats();
            std::cout << "[STATS] sent=" << stats.sent_bytes.load()
                      << "B recv=" << stats.recv_bytes.load() << "B "
                      << "pending=" << kcp.pending_send_count()
                      << " echoes=" << echo_count.load() << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Cleanup
    network.stop();
    if (input_thread.joinable()) {
        input_thread.join();
    }

    std::cout << "\nClient stopped. Final stats:" << std::endl;
    const auto& stats = kcp.stats();
    std::cout << "  Sent: " << stats.sent_bytes.load() << " bytes ("
              << stats.sent_packets.load() << " packets)" << std::endl;
    std::cout << "  Recv: " << stats.recv_bytes.load() << " bytes ("
              << stats.recv_packets.load() << " packets)" << std::endl;
    std::cout << "  Echoes received: " << echo_count.load() << std::endl;

    return 0;
}
