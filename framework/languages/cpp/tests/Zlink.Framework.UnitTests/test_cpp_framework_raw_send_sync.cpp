/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include <zlink.hpp>

#include <cassert>
#include <cstdio>
#include <mutex>
#include <optional>
#include <pthread.h>

namespace fw = zlink::framework;
namespace runtime = fw::runtime;
namespace mesh = runtime::mesh;
namespace protocol = runtime::protocol;

namespace
{
thread_local bool tracking = false;
thread_local unsigned mutex_locks = 0;

template <typename Action> unsigned count_locks (Action &&action)
{
    mutex_locks = 0;
    tracking = true;
    std::forward<Action> (action) ();
    tracking = false;
    return mutex_locks;
}
}

extern "C" int __real_pthread_mutex_lock (pthread_mutex_t *);
extern "C" int __wrap_pthread_mutex_lock (pthread_mutex_t *mutex)
{
    if (tracking)
        ++mutex_locks;
    return __real_pthread_mutex_lock (mutex);
}

int main ()
{
    fw::completion_owner_t completions;
    std::mutex calibration;
    assert (count_locks ([] {}) == 0);
    assert (count_locks ([&] { std::lock_guard lock (calibration); }) == 1);
    mesh::service_node_descriptor_t local{
      "sync-send-mesh", {'l', 'o', 'c', 'a', 'l'},          1, 1, "tcp://127.0.0.1:0",
      {{"alpha", 1}},   mesh::service_node_state_t::serving};
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = local,
      .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()});
    auto peer_descriptor = local;
    peer_descriptor.node_routing_id = {'p', 'e', 'e', 'r'};
    assert (source.topology ().admit (peer_descriptor, peer_descriptor.node_routing_id)
            == mesh::peer_admission_result_t::admitted);
    assert (source.topology ().peer (peer_descriptor.node_routing_id)->ready);
    runtime::offload_executor_t executor;
    runtime::state_lane_t lane (executor);
    unsigned peer_locks = 0, lane_locks = 0, result_locks = 0, observed = 0;
    for (int pass = 0; pass < 2; ++pass) {
        peer_locks = count_locks (
          [&] { assert (source.topology ().peer (peer_descriptor.node_routing_id)->ready); });
        lane_locks = count_locks ([&] { lane.run_sync ([] {}); });
        std::optional<fw::task_t<zlink::submit_result_t>> baseline;
        result_locks = count_locks ([&] {
            baseline.emplace (
              fw::result_t<zlink::submit_result_t>::success (zlink::submit_result_t::terminated));
        });
        protocol::application_payload_t payload{"Probe", "application/octet-stream", {1, 2, 3}};
        std::optional<fw::task_t<zlink::submit_result_t>> submitted;
        observed = count_locks ([&] {
            submitted.emplace (
              source.send_to_node_result (peer_descriptor.node_routing_id, std::move (payload)));
        });
        // Ready topology reaches the owner lane; the absent native port ends
        // submission there, without another resource or pending operation.
        assert (submitted->await_ready ());
        assert (submitted->result ().value () == zlink::submit_result_t::terminated);
        assert (source.pending_operation_count () == 0);
    }
    std::fprintf (stderr, "raw owner locks observed=%u peer=%u lane=%u result=%u\n", observed,
                  peer_locks, lane_locks, result_locks);
    assert (observed == peer_locks + lane_locks + result_locks);

    source.close ();
    const auto close_turn_locks = count_locks ([&] {
        lane.run ([] {}).get ();
        lane.run ([] {}).get ();
    });
    std::shared_ptr<fw::detail::backend::raw_route_port_t> closed_port;
    std::unique_ptr<zlink::poller_t> closed_poller;
    std::unique_ptr<zlink::router_socket_t> closed_socket;
    const auto disposal_locks = count_locks ([&] {
        runtime::runtime_failure_collector_t::close_resources (closed_port);
        runtime::runtime_failure_collector_t::close_resources (closed_poller, closed_socket);
    });
    const auto publication_locks = count_locks ([&] {
        {
            const std::lock_guard lock (calibration);
        }
        {
            const std::lock_guard lock (calibration);
        }
    });
    const auto repeated_close_locks = count_locks ([&] { source.close (); });
    std::fprintf (stderr,
                  "close publication locks observed=%u turns=%u disposal=%u publication=%u\n",
                  repeated_close_locks, close_turn_locks, disposal_locks, publication_locks);
    assert (repeated_close_locks == close_turn_locks + disposal_locks + publication_locks);
}
