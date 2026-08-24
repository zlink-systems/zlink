// W-2 byte-equivalence proof: generated pilot codecs vs hand codecs (C++).
// Links against the real built framework library and calls the real hand
// codec functions - not a copied/hardcoded expected-bytes string.
#include "runtime/protocol/service_wire_codec.hpp"
#include <service_wire_pilot_codec.hpp>
#include <iostream>
#include <iomanip>
#include <sstream>

using namespace zlink::framework::runtime;

static std::string hex(const std::vector<std::uint8_t>& v) {
    std::ostringstream o;
    for (auto b : v) o << std::hex << std::setfill('0') << std::setw(2) << (int)b;
    return o.str();
}

static bool check(const char* name, const std::vector<std::uint8_t>& hand,
                   const std::vector<std::uint8_t>& gen) {
    bool equal = hand == gen;
    std::cout << name << ": " << (equal ? "IDENTICAL" : "DIFFERS") << "\n";
    std::cout << "  hand: " << hex(hand) << "\n";
    std::cout << "  gen : " << hex(gen) << "\n";
    return equal;
}

int main() {
    bool allEqual = true;

    // livenessProbe(5)
    allEqual &= check("livenessProbe(5)",
        protocol::encode_liveness(protocol::command::livenessProbe, 42),
        protocol::encode_liveness_probe_5({42}));

    // livenessAck(6)
    allEqual &= check("livenessAck(6)",
        protocol::encode_liveness(protocol::command::livenessAck, 42),
        protocol::encode_liveness_ack_6({42}));

    // nodeSend(16)
    allEqual &= check("nodeSend(16)",
        protocol::encode_node_send_header(),
        protocol::encode_node_send_16());

    // nodeRequest(17)
    allEqual &= check("nodeRequest(17)",
        protocol::encode_node_request_header(7),
        protocol::encode_node_request_17({7}));

    // channelSend(18)
    allEqual &= check("channelSend(18)",
        protocol::encode_channel_send_header("lobby"),
        protocol::encode_channel_send_18({"lobby"}));

    // channelRequest(19)
    allEqual &= check("channelRequest(19)",
        protocol::encode_channel_request_header(7, "lobby"),
        protocol::encode_channel_request_19({7, "lobby"}));

    std::cout << "\nlogicalMulticast(23): SKIPPED - no C++ hand codec exists\n";
    std::cout << "actorLookup(26): SKIPPED - no C++ hand codec exists\n";
    std::cout << "actorDestroy(27): SKIPPED - no C++ hand codec exists\n";

    return allEqual ? 0 : 1;
}
