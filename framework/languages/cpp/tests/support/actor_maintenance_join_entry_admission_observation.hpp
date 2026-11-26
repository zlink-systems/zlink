/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "actor_maintenance_join_host_access.hpp"
#include "actor_maintenance_join_host_fixture.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace zlink::framework::runtime::host
{
inline void public_host_runtime_test_access_t::observe_actor_join (
  public_host_runtime_t &host,
  std::function<void (const protocol::actor_join_request_t &)> received,
  std::function<void (std::uint64_t, const actor_join_operation_result_t &)> replied)
{
    lifecycle_turn (host, [&] {
        host._actor_join_target =
          [admit = std::move (host._actor_join_target), received = std::move (received),
           replied = std::move (replied)] (const auto &request, auto payload, auto complete) {
              received (request);
              admit (request, std::move (payload),
                     [correlation = request.correlation, replied,
                      complete = std::move (complete)] (auto result) {
                         replied (correlation, result);
                         complete (std::move (result));
                     });
          };
    });
}
} // namespace zlink::framework::runtime::host

namespace zlink::framework::test::actor_maintenance_join
{
struct entry_admission_observation_t
{
    struct request_t
    {
        std::uint64_t correlation;
        std::uint64_t target_generation;
    };
    struct reply_t
    {
        std::uint64_t correlation;
        std::uint32_t terminal;
        std::uint32_t failure;
        fw::runtime::protocol::actor_join_result_t result;
    };

    std::mutex mutex;
    std::vector<request_t> requests;
    std::vector<reply_t> replies;
};

inline std::shared_ptr<entry_admission_observation_t> observe_entry_admission (host_t &host)
{
    const auto registration =
      fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ()).front ();
    const auto node = fw::detail::spot_node_runtime_t (registration->spot_state).native_node ();
    if (!node)
        return {};
    auto observation = std::make_shared<entry_admission_observation_t> ();
    fw::runtime::host::public_host_runtime_test_access_t::observe_actor_join (
      *node,
      [observation] (const auto &request) {
          if (!request.entry || request.actor.actor_id != actor_id)
              return;
          std::lock_guard lock (observation->mutex);
          observation->requests.push_back (
            {request.correlation, request.target_spot.target_node_generation});
      },
      [observation] (auto correlation, const auto &reply) {
          std::lock_guard lock (observation->mutex);
          observation->replies.push_back (
            {correlation, reply.terminal_result, reply.failure_code, reply.join_result});
      });
    return observation;
}
} // namespace zlink::framework::test::actor_maintenance_join
