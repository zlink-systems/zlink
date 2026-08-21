/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/mesh/mesh_node_runtime.hpp"

#include <iostream>

int main ()
{
    const auto handoff_id = zlink::framework::detail::canonical_actor_join_handoff_id (
      {0x01, 0x02, 0x03}, "actor-1", 7, 8, 42);
    if (handoff_id != "dc51920c6ab3c318ca8acaaed4bcb1ec") {
        std::cerr << "canonical actor Join handoff ID mismatch: " << handoff_id << '\n';
        return 1;
    }
    const auto zero_byte_payload =
      zlink::framework::detail::canonical_actor_join_application_payload (
        "ZeroByteJoin", "application/x-test", zlink::message_t{});
    if (!zero_byte_payload || !zero_byte_payload->payload.empty ()
        || zero_byte_payload->packet_name != "ZeroByteJoin"
        || zero_byte_payload->content_type != "application/x-test") {
        std::cerr << "typed zero-byte actor Join payload was omitted\n";
        return 2;
    }
    return 0;
}
