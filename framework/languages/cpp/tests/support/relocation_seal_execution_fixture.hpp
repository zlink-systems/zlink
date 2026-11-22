/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/mesh/service_topology_registry.hpp"
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace framework_relocation_seal_fixture
{
inline zlink::framework::runtime::mesh::service_node_descriptor_t
test_descriptor (std::string routing_id)
{
    const std::vector<std::uint8_t> routing_bytes (routing_id.begin (), routing_id.end ());
    return {"relocation-seal-test",
            routing_bytes,
            1,
            1,
            "tcp://127.0.0.1:0",
            {},
            zlink::framework::runtime::mesh::service_node_state_t::preparing};
}

// Waits for an observation another thread makes true.
template <typename Predicate> bool eventually (Predicate predicate)
{
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (!predicate ()) {
        if (std::chrono::steady_clock::now () >= deadline)
            return false;
        std::this_thread::sleep_for (5ms);
    }
    return true;
}
} // namespace framework_relocation_seal_fixture
