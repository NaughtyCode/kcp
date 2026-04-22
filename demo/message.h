#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <variant>
#include <vector>
#include <array>
#include <iostream>
#include <stdexcept>

namespace kcp_demo {

// Message types
enum class MessageType : uint8_t {
    KCP_HANDSHAKE_REQ = 1,
    KCP_HANDSHAKE_RESP = 2,
    KCP_HEARTBEAT = 3,
    KCP_DATA = 4,
    KCP_DISCONNECT = 5,
    KCP_ACK = 6,
};

constexpr uint32_t KCP_MAX_MESSAGE_SIZE = 1400;  // fit within KCP MTU
constexpr uint16_t KCP_PROTOCOL_MAGIC = 0x4B43;   // "KC"
constexpr uint8_t KCP_PROTOCOL_VERSION = 1;

// Cross-platform packed struct for wire format
// | magic(2) | version(1) | type(1) | sequence(4) | payload_len(2) | payload(N) |
#ifdef _MSC_VER
#pragma pack(push, 1)
#endif
struct MessageHeader {
    uint16_t magic;
    uint8_t version;
    uint8_t type;
    uint32_t sequence;
    uint16_t payload_len;

    static constexpr size_t SIZE = 10;

    bool validate() const {
        return magic == KCP_PROTOCOL_MAGIC &&
               version == KCP_PROTOCOL_VERSION &&
               payload_len <= KCP_MAX_MESSAGE_SIZE;
    }
};
#ifdef _MSC_VER
#pragma pack(pop)
#endif

static_assert(sizeof(MessageHeader) == 10, "MessageHeader must be 10 bytes");

// Payload types
struct HandshakeReq {
    std::string client_name;
};

struct HandshakeResp {
    int server_token;
    std::string server_message;
};

struct Heartbeat {
    uint32_t timestamp;
    std::string payload;
};

struct Disconnect {
    std::string reason;
};

// Payload helper functions - simple text-based serialization
inline std::string payload_to_string(const std::variant<HandshakeReq, HandshakeResp, Heartbeat, std::string, Disconnect, std::monostate>& payload, uint8_t type) {
    switch (type) {
        case static_cast<uint8_t>(MessageType::KCP_HANDSHAKE_REQ): {
            if (auto* p = std::get_if<HandshakeReq>(&payload)) {
                return "NAME:" + p->client_name;
            }
            return "";
        }
        case static_cast<uint8_t>(MessageType::KCP_HANDSHAKE_RESP): {
            if (auto* p = std::get_if<HandshakeResp>(&payload)) {
                return "TOKEN:" + std::to_string(p->server_token) + ";MSG:" + p->server_message;
            }
            return "";
        }
        case static_cast<uint8_t>(MessageType::KCP_HEARTBEAT): {
            if (auto* p = std::get_if<Heartbeat>(&payload)) {
                return "TS:" + std::to_string(p->timestamp) + ";PAYLOAD:" + p->payload;
            }
            return "";
        }
        case static_cast<uint8_t>(MessageType::KCP_DATA): {
            if (auto* p = std::get_if<std::string>(&payload)) {
                return *p;
            }
            return "";
        }
        case static_cast<uint8_t>(MessageType::KCP_DISCONNECT): {
            if (auto* p = std::get_if<Disconnect>(&payload)) {
                return "REASON:" + p->reason;
            }
            return "";
        }
        default:
            return "";
    }
}

inline void string_to_payload(const std::string& body, uint8_t type,
                               std::variant<HandshakeReq, HandshakeResp, Heartbeat, std::string, Disconnect, std::monostate>& payload) {
    switch (type) {
        case static_cast<uint8_t>(MessageType::KCP_HANDSHAKE_REQ): {
            size_t pos = body.find("NAME:");
            if (pos != std::string::npos) {
                payload = HandshakeReq{body.substr(pos + 5)};
            } else {
                payload = HandshakeReq{body};
            }
            break;
        }
        case static_cast<uint8_t>(MessageType::KCP_HANDSHAKE_RESP): {
            HandshakeResp resp;
            size_t pos = body.find("TOKEN:");
            if (pos != std::string::npos) {
                pos += 6;
                auto end = body.find(";", pos);
                if (end != std::string::npos) {
                    resp.server_token = std::atoi(body.substr(pos, end - pos).c_str());
                    pos = body.find("MSG:", end + 1);
                    if (pos != std::string::npos) {
                        resp.server_message = body.substr(pos + 4);
                    } else {
                        resp.server_message = body.substr(end + 1);
                    }
                } else {
                    resp.server_token = std::atoi(body.substr(pos).c_str());
                }
            } else {
                resp.server_token = 0;
                resp.server_message = body;
            }
            payload = resp;
            break;
        }
        case static_cast<uint8_t>(MessageType::KCP_HEARTBEAT): {
            Heartbeat hb;
            size_t pos = body.find("TS:");
            if (pos != std::string::npos) {
                pos += 3;
                auto end = body.find(";PAYLOAD:", pos);
                if (end != std::string::npos) {
                    hb.timestamp = static_cast<uint32_t>(std::atol(body.substr(pos, end - pos).c_str()));
                    hb.payload = body.substr(end + 9);
                } else {
                    hb.timestamp = static_cast<uint32_t>(std::atol(body.substr(pos).c_str()));
                    hb.payload = "";
                }
            } else {
                hb.timestamp = 0;
                hb.payload = body;
            }
            payload = hb;
            break;
        }
        case static_cast<uint8_t>(MessageType::KCP_DATA): {
            payload = body;
            break;
        }
        case static_cast<uint8_t>(MessageType::KCP_DISCONNECT): {
            Disconnect disc;
            size_t pos = body.find("REASON:");
            if (pos != std::string::npos) {
                disc.reason = body.substr(pos + 7);
            } else {
                disc.reason = body;
            }
            payload = disc;
            break;
        }
        default:
            payload = std::monostate{};
            break;
    }
}

class Message {
public:
    MessageHeader header;

    // Payload as variant
    std::variant<HandshakeReq, HandshakeResp, Heartbeat, std::string, Disconnect, std::monostate> payload;

    static constexpr size_t MAX_BODY_SIZE = KCP_MAX_MESSAGE_SIZE;

    Message() : header{} {
        header.magic = KCP_PROTOCOL_MAGIC;
        header.version = KCP_PROTOCOL_VERSION;
    }

    static Message create_handshake_req(const std::string& client_name, uint32_t seq) {
        Message msg;
        msg.header.type = static_cast<uint8_t>(MessageType::KCP_HANDSHAKE_REQ);
        msg.header.sequence = seq;
        msg.payload = HandshakeReq{client_name};
        return msg;
    }

    static Message create_handshake_resp(int token, const std::string& msg, uint32_t seq) {
        Message m;
        m.header.type = static_cast<uint8_t>(MessageType::KCP_HANDSHAKE_RESP);
        m.header.sequence = seq;
        m.payload = HandshakeResp{token, msg};
        return m;
    }

    static Message create_heartbeat(uint32_t timestamp, const std::string& body, uint32_t seq) {
        Message m;
        m.header.type = static_cast<uint8_t>(MessageType::KCP_HEARTBEAT);
        m.header.sequence = seq;
        m.payload = Heartbeat{timestamp, body};
        return m;
    }

    static Message create_data(const std::string& body, uint32_t seq) {
        Message m;
        m.header.type = static_cast<uint8_t>(MessageType::KCP_DATA);
        m.header.sequence = seq;
        m.payload = body;
        return m;
    }

    static Message create_disconnect(const std::string& reason, uint32_t seq) {
        Message m;
        m.header.type = static_cast<uint8_t>(MessageType::KCP_DISCONNECT);
        m.header.sequence = seq;
        m.payload = Disconnect{reason};
        return m;
    }

    static Message create_ack(uint32_t seq, uint32_t ack_seq) {
        Message m;
        m.header.type = static_cast<uint8_t>(MessageType::KCP_ACK);
        m.header.sequence = seq;
        // ACK uses a simple string payload for ack_seq
        m.payload = "ACK:" + std::to_string(ack_seq);
        return m;
    }

    // Serialization: returns bytes written
    size_t serialize(char* buf, size_t buf_size) const {
        // Serialize payload to string first
        std::string body = payload_to_string(payload, header.type);
        uint16_t body_len = static_cast<uint16_t>(body.size());

        if (body_len > KCP_MAX_MESSAGE_SIZE) {
            std::cerr << "[Message] Body too large: " << body_len << " bytes" << std::endl;
            return 0;
        }

        size_t total_size = MessageHeader::SIZE + body_len;
        if (total_size > buf_size) {
            std::cerr << "[Message] Buffer too small: need " << total_size
                      << ", have " << buf_size << std::endl;
            return 0;
        }

        // Set payload length in header
        MessageHeader writable_header = header;
        writable_header.payload_len = body_len;

        // Write header
        std::memcpy(buf, &writable_header, sizeof(writable_header));

        // Write body (as null-terminated string for simplicity, but use binary copy)
        if (body_len > 0) {
            std::memcpy(buf + MessageHeader::SIZE, body.c_str(), body_len);
        }

        return total_size;
    }

    // Deserialization: returns true if successful
    static bool deserialize(const char* data, size_t len, Message& msg) {
        if (len < MessageHeader::SIZE) {
            return false;
        }

        std::memcpy(&msg.header, data, MessageHeader::SIZE);

        if (!msg.header.validate()) {
            return false;
        }

        uint16_t body_len = msg.header.payload_len;
        if (body_len == 0) {
            // No payload
            msg.payload = std::monostate{};
            return true;
        }

        if (len < MessageHeader::SIZE + body_len) {
            return false;
        }

        // Extract body string
        std::string body(data + MessageHeader::SIZE, body_len);
        string_to_payload(body, msg.header.type, msg.payload);

        return true;
    }

    // Get payload as string (for KCP_DATA type)
    std::string get_body() const {
        switch (header.type) {
            case static_cast<uint8_t>(MessageType::KCP_DATA): {
                if (auto* p = std::get_if<std::string>(&payload)) {
                    return *p;
                }
                return "";
            }
            case static_cast<uint8_t>(MessageType::KCP_HEARTBEAT): {
                if (auto* p = std::get_if<Heartbeat>(&payload)) {
                    return p->payload;
                }
                return "";
            }
            case static_cast<uint8_t>(MessageType::KCP_HANDSHAKE_REQ): {
                if (auto* p = std::get_if<HandshakeReq>(&payload)) {
                    return p->client_name;
                }
                return "";
            }
            case static_cast<uint8_t>(MessageType::KCP_HANDSHAKE_RESP): {
                if (auto* p = std::get_if<HandshakeResp>(&payload)) {
                    return p->server_message;
                }
                return "";
            }
            case static_cast<uint8_t>(MessageType::KCP_DISCONNECT): {
                if (auto* p = std::get_if<Disconnect>(&payload)) {
                    return p->reason;
                }
                return "";
            }
            case static_cast<uint8_t>(MessageType::KCP_ACK): {
                if (auto* p = std::get_if<std::string>(&payload)) {
                    return *p;
                }
                return "";
            }
            default:
                return "";
        }
    }
};

} // namespace kcp_demo
