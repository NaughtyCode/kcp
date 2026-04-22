// KCP Server Application
// Usage: kcp_server [port]
// Default port: 7788

#include "kcp_endpoint.h"
#include "network.h"
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>

namespace {

constexpr uint16_t DEFAULT_PORT = 7788;
constexpr uint32_t DEFAULT_CONV = 0xDEADBEEF;  // Server-side KCP conversation ID

}  // namespace

int main(int argc, char* argv[]) {
    uint16_t port = DEFAULT_PORT;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::atoi(argv[1]));
    }

    std::cout << "========================================" << std::endl;
    std::cout << "  KCP Server" << std::endl;
    std::cout << "  Listening on UDP port " << port << std::endl;
    std::cout << "  Press Ctrl+C to quit." << std::endl;
    std::cout << "========================================" << std::endl;

    // Create network layer (server mode - bind to port)
    kcp_demo::NetworkLayer network;
    if (!network.server_init(port)) {
        std::cerr << "Failed to initialize network server" << std::endl;
        return 1;
    }

    // Session tracking
    std::atomic<uint32_t> client_count{0};
    std::mutex session_mutex;

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

    // Set up message handler
    kcp.set_on_data([&](const kcp_demo::Message& msg) {
        switch (msg.header.type) {
            case kcp_demo::MessageType::KCP_HANDSHAKE_REQ: {
                auto* req = std::get_if<kcp_demo::HandshakeReq>(&msg.payload);
                if (req) {
                    std::string client_name = req->client_name;
                    {
                        std::lock_guard<std::mutex> lock(session_mutex);
                        client_count++;
                    }
                    std::cout << "[+] Client connected: " << client_name
                              << " (total: " << client_count.load() << ")" << std::endl;

                    // Send handshake response
                    auto resp = kcp_demo::Message::create_handshake_resp(
                        42, "Welcome to KCP Server!", 0);
                    kcp.send_message(resp);
                }
                break;
            }

            case kcp_demo::MessageType::KCP_HEARTBEAT: {
                auto* hb = std::get_if<kcp_demo::Heartbeat>(&msg.payload);
                if (hb) {
                    std::cout << "[HEARTBEAT] from client: " << hb->payload << std::endl;
                }
                break;
            }

            case kcp_demo::MessageType::KCP_DATA: {
                std::string body = msg.get_body();
                std::cout << "[DATA] from client: " << body << std::endl;

                // Echo back
                auto echo = kcp_demo::Message::create_data(
                    "SERVER_ECHO: " + body, 0);
                kcp.send_message(echo);
                break;
            }

            case kcp_demo::MessageType::KCP_DISCONNECT: {
                auto* disc = std::get_if<kcp_demo::Disconnect>(&msg.payload);
                std::string reason = disc ? disc->reason : "unknown";
                std::cout << "[-] Client disconnected: " << reason << std::endl;
                {
                    std::lock_guard<std::mutex> lock(session_mutex);
                    client_count--;
                }
                break;
            }

            default:
                std::cout << "[?] Unknown message type: " << (int)msg.header.type << std::endl;
                break;
        }
    });

    // Log callback (disabled for clean output)
    kcp.set_on_log([](const std::string& log) {
        // Uncomment for debug: std::cout << "[KCP] " << log << std::endl;
    });

    // Connect/disconnect callbacks
    kcp.set_on_connect([]() {
        std::cout << "[KCP] Connection established" << std::endl;
    });

    kcp.set_on_disconnect([](const std::string& reason) {
        std::cout << "[KCP] Connection lost: " << reason << std::endl;
    });

    // Start network I/O thread
    network.start();

    // Input thread for server control
    std::thread input_thread([&]() {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty()) {
                // Empty line: send test message
                auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                auto msg = kcp_demo::Message::create_data(
                    "SERVER_TEST: " + std::to_string(static_cast<uint32_t>(now_ms)), 0);
                kcp.send_message(msg);
            } else if (line == "quit" || line == "exit") {
                auto disc = kcp_demo::Message::create_disconnect("Server shutting down", 0);
                kcp.send_message(disc);
                kcp.shutdown();
                break;
            } else {
                // Send user input to client
                auto msg = kcp_demo::Message::create_data(line, 0);
                kcp.send_message(msg);
            }
        }
    });

    std::cout << "\nReady! Type messages to send to client." << std::endl;
    std::cout << "Press Enter for a test message, 'quit' to exit.\n" << std::endl;

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
            std::string hb_body = "ping from server at " + std::to_string(now_ms);
            auto hb_msg = kcp_demo::Message::create_heartbeat(now_ms, hb_body, 0);
            kcp.send_message(hb_msg);
        }

        // Print stats every 5 seconds
        static uint32_t stats_timer = 0;
        if (now_ms - stats_timer >= 5000) {
            stats_timer = now_ms;
            const auto& stats = kcp.stats();
            std::cout << "[STATS] sent=" << stats.sent_bytes.load()
                      << "B recv=" << stats.recv_bytes.load() << "B "
                      << "pkt_tx=" << stats.sent_packets.load()
                      << " pkt_rx=" << stats.recv_packets.load()
                      << " pending=" << kcp.pending_send_count() << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Cleanup
    network.stop();
    if (input_thread.joinable()) {
        input_thread.join();
    }

    std::cout << "\nServer stopped. Final stats:" << std::endl;
    const auto& stats = kcp.stats();
    std::cout << "  Sent: " << stats.sent_bytes.load() << " bytes ("
              << stats.sent_packets.load() << " packets)" << std::endl;
    std::cout << "  Recv: " << stats.recv_bytes.load() << " bytes ("
              << stats.recv_packets.load() << " packets)" << std::endl;

    return 0;
}
