/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "allocation_counter.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime_failure_fixture.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace
{
using namespace zlink::framework;
using dispatch_t = runtime::mesh::service_mailbox_t::application_dispatch_t;
using pending_t = task_t<std::size_t>;

void require (bool value, const char *message)
{
    if (!value)
        throw std::runtime_error (message);
}

// The former forwarding algorithm. The native owner remains the only pump.
pending_t forwarding_reference (detail::mesh_node_runtime_t &node, dispatch_t dispatch)
{
    co_return co_await node.native_node ().dispatch_ready (std::move (dispatch), false);
}

pending_t invoke (detail::mesh_node_runtime_t &node, const dispatch_t &dispatch, int path)
{
    if (path == 0)
        return node.native_node ().dispatch_ready (dispatch, false);
    if (path == 1)
        return forwarding_reference (node, dispatch);
    return node.dispatch_ready (dispatch, false);
}

tests::allocation_measurement_t
measure (detail::mesh_node_runtime_t &node, const dispatch_t &dispatch, int path)
{
    std::optional<pending_t> pending;
    const auto cost =
      tests::measure_allocations ([&] { pending.emplace (invoke (node, dispatch, path)); });
    require (pending->await_ready (), "empty receive rotation unexpectedly suspended");
    require (pending->result ().value () == 0, "empty receive rotation dispatched a record");
    return cost;
}
}

int main ()
{
    completion_owner_t completion_owner;
    try {
        auto state =
          test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("receive-cost");
        state->core_context = std::make_shared<zlink::context_t> ();
        state->listen_endpoint = "tcp://127.0.0.1:5685";
        state->routing_id = zlink::routing_id_t::from ("receive-cost");
        detail::mesh_node_runtime_t node (state);
        node.start ();
        const dispatch_t dispatch = [] (const auto &, const auto &, auto) {
            throw std::runtime_error ("empty receive invoked an application handler");
        };
        for (int path = 0; path != 3; ++path)
            (void) measure (node, dispatch, path);
        std::array<tests::allocation_measurement_t, 3> totals{};
        constexpr int rounds = 16;
        for (int round = 0; round != rounds; ++round) {
            for (int index = 0; index != 3; ++index) {
                const int path = round % 2 ? 2 - index : index;
                const auto cost = measure (node, dispatch, path);
                totals[path].count += cost.count;
                totals[path].bytes += cost.bytes;
            }
        }
        std::cout << "receive-dispatch rounds=" << rounds;
        for (int path = 0; path != 3; ++path)
            std::cout << " path=" << path << " count=" << totals[path].count
                      << " bytes=" << totals[path].bytes;
        std::cout << '\n';
        require (totals[1].count > totals[0].count && totals[1].bytes > totals[0].bytes,
                 "forwarding reference failed to expose its task allocation");
        require (totals[2].count <= totals[0].count && totals[2].bytes <= totals[0].bytes,
                 "receive forwarding allocated beyond the native owner baseline");

        if (std::getenv ("ZLINK_CPP_RECEIVE_DISPATCH_TIMING")) {
            constexpr int iterations = 1000;
            for (int round = 0; round != 12; ++round) {
                for (int index = 0; index != 2; ++index) {
                    const int path = round % 2 ? 2 - index : 1 + index;
                    const auto start = std::chrono::steady_clock::now ();
                    for (int i = 0; i != iterations; ++i)
                        require (invoke (node, dispatch, path).result ().value () == 0,
                                 "timed empty rotation dispatched a record");
                    const auto elapsed = std::chrono::duration<double, std::nano> (
                                           std::chrono::steady_clock::now () - start)
                                           .count ();
                    std::cout << "timing round=" << round << " path=" << path
                              << " ns=" << elapsed / iterations << '\n';
                }
            }
        }
        auto rejected = node.dispatch_ready ({});
        require (!rejected.result (), "missing callback unexpectedly succeeded");
        node.stop ();
    }
    catch (const std::exception &error) {
        std::cerr << error.what () << '\n';
        return 1;
    }
}
