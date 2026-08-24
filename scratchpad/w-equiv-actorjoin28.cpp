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

int main() {
    protocol::actor_route_fence_t actor{"actor", 2, {0x01,0x02}, 3, 4, 5};
    protocol::spot_route_fence_t spot{"spot", 6, {0x07,0x08}, 9, 10, 11};
    protocol::actor_join_request_t req{1, actor, true, spot};
    auto handBytes = protocol::encode_actor_join_request(req);

    protocol::service_wire_pilot_fence gActor{"actor", 2, {0x01,0x02}, 3, 4, 5};
    protocol::service_wire_pilot_fence gSpot{"spot", 6, {0x07,0x08}, 9, 10, 11};
    protocol::service_wire_pilot_actor_join_28 greq{1, gActor, true, gSpot};
    auto genBytes = protocol::encode_actor_join_28(greq);

    std::cout << "hand:      " << hex(handBytes) << "\n";
    std::cout << "generated: " << hex(genBytes) << "\n";
    std::cout << "equal: " << (handBytes == genBytes ? "true" : "false") << "\n";
    return handBytes == genBytes ? 0 : 1;
}
