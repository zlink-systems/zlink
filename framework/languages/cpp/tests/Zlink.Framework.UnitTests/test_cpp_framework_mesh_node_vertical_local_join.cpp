/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/mesh_node_vertical_cases.hpp"
#include <zlink.hpp>
#include <atomic>
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/dispatch/completion_turn.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/execution/infrastructure_wait_guard.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/mesh/route_mesh_runtime_options_service.hpp"
#include "runtime/mesh/route_mesh_runtime_service.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "../support/loopback_tcp_endpoint.hpp"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <latch>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include "../support/mesh_node_vertical_transport_fixture.hpp"

using namespace framework_mesh_node_vertical_fixture;

using namespace std::chrono_literals;

namespace framework_mesh_node_vertical_tests
{
void verify_local_join_timeout_releases_membership ()
{
    namespace host = zlink::framework::runtime::host;
    using namespace std::chrono_literals;
    auto state = make_node ("tcp://127.0.0.1:*", "join-timeout-owner");
    zlink::framework::detail::mesh_node_runtime_t node (state);
    node.start ().result ().value ();
    auto &runtime = node.native_node ();
    auto actor = runtime.create_actor ("vertical.actor", "join-timeout-actor");
    auto target = runtime.get_or_create_spot_async ("join-timeout-spot").result ().value ();
    const auto rid = runtime.status ().routing_id ();
    const auto generation = target.status ().lifecycle_generation ();
    host::pending_operation_t expired;
    assert (
      actor.join_spot (rid, "join-timeout-spot", generation, {}, expired, 25ms).result ().value ()
      == zlink::submit_result_t::ok);
    assert (runtime.transport ()
              .expire_requests (std::chrono::steady_clock::now () + 1s)
              .result ()
              .value ()
            == 1);
    auto first = expired.completion->task ();
    assert (!first.result ());
    assert (first.result ().error_kind ()
            == zlink::framework::framework_error_kind_t::deadline_exceeded);
    assert (zlink::framework::detail::boundary_state (*first.result ().error ())
            == zlink::framework::detail::boundary_error_t::timed_out);

    // The expired queued control is skipped, and the same Actor can start
    // another membership move because the terminal owner released its token.
    auto &retried = expired;
    assert (
      actor.join_spot (rid, "join-timeout-spot", generation, {}, retried, 1s).result ().value ()
      == zlink::submit_result_t::ok);
    bool replied = false;
    (void) runtime
      .dispatch_ready ([&] (const host::ready_record_t &, const host::receive_record_t &record,
                            std::vector<zlink::message_t>, auto) {
          if (record.kind == host::record_kind_t::spot_control)
              replied = host::actor_join_reply (
                record.reply_token,
                zlink::framework::result_t<host::actor_join_result_t>::success (
                  host::actor_join_result_t::accepted),
                {});
      })
      .result ()
      .value ();
    assert (replied);
    auto second = retried.completion->task ();
    assert (second.result ());
    assert (second.result ().value ().record.join_completion);
    assert (second.result ().value ().record.join_completion->value ().join_result
            == host::join_admission_t::accepted);
    assert (!first.result ());
    node.stop ();
}
} // namespace framework_mesh_node_vertical_tests
