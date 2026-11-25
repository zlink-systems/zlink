/* SPDX-License-Identifier: Elastic-2.0 */

#include "actor_maintenance_join_host_access.hpp"
#include "stateful_object_lane_test_access.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include <gtest/gtest.h>
#include <utility>

namespace zlink::framework::runtime::stateful
{
struct raw_stateful_dispatch_test_access_t
{
    static std::array<std::size_t, 3> spot_records (raw_stateful_dispatch_t &raw);
};

std::array<std::size_t, 3>
raw_stateful_dispatch_test_access_t::spot_records (raw_stateful_dispatch_t &raw)
{
    std::lock_guard lock (raw._mutex);
    std::array<std::size_t, 3> counts{};
    for (const auto &[key, pending] : raw._pending) {
        if (key.kind == object_kind_t::actor)
            continue;
        ++counts[0]; // A committed pending entry has an accepted frozen body.
        ++counts[1]; // The same entry stores its frozen summary.
    }
    for (const auto &[key, next] : raw._next_sequence)
        if (key.first != object_kind_t::actor)
            counts[2] += next - 1;
    return counts;
}
} // namespace zlink::framework::runtime::stateful

namespace zlink::framework::runtime::host
{
std::pair<std::shared_ptr<task_completion_source_t<void>>,
          std::shared_ptr<task_completion_source_t<void>>>
public_host_runtime_test_access_t::track_previous_target (public_host_runtime_t &host,
                                                          const stateful::object_ref_t &object,
                                                          bool pending_request)
{
    return host._relocation_session_terminal_lane.run_sync ([&] {
        auto &attempt = host._relocation_target_attempts[{31, 7, 1}];
        attempt.targets = {object};
        std::shared_ptr<task_completion_source_t<void>> delivery;
        if (pending_request) {
            delivery = std::make_shared<task_completion_source_t<void>> ();
            attempt.captured_requests.push_back ({object.kind, object.key, delivery});
        }
        attempt.authority_committed_at = std::chrono::steady_clock::now ();
        attempt.terminal = std::make_shared<task_completion_source_t<void>> ();
        return std::pair{attempt.terminal, delivery};
    });
}

task_t<stateful::stateful_error_t>
public_host_runtime_test_access_t::advance_authority (public_host_runtime_t &host,
                                                      stateful::object_ref_t committed)
{
    return host.advance_local_actor_authority (std::move (committed));
}

std::future<std::optional<stateful::object_ref_t>>
public_host_runtime_test_access_t::probe_actor_index (public_host_runtime_t &host,
                                                      stateful::object_ref_t replacement,
                                                      authority_interleaving_t change)
{
    return host._spot_actor_index_lane.run (
      [&host, replacement, change] () -> std::optional<stateful::object_ref_t> {
          if (change == authority_interleaving_t::erased) {
              host._actors.erase (replacement.key);
              return std::nullopt;
          }
          const auto found = host._actors.find (replacement.key);
          if (found == host._actors.end ())
              return std::nullopt;
          if (change != authority_interleaving_t::unchanged)
              found->second.second = replacement;
          return found->second.second;
      });
}

std::promise<void> public_host_runtime_test_access_t::hold_objects (public_host_runtime_t &host)
{
    return stateful::stateful_object_runtime_test_access_t::hold_lane (host._objects);
}

std::promise<void> public_host_runtime_test_access_t::hold_actor_index (public_host_runtime_t &host)
{
    return stateful::stateful_object_runtime_test_access_t::hold_lane (host._spot_actor_index_lane);
}

std::string public_host_runtime_test_access_t::entry_id (public_host_runtime_t &host)
{
    return host._entry_spot_id;
}

std::promise<void> public_host_runtime_test_access_t::hold_dispatch (public_host_runtime_t &host)
{
    return stateful::stateful_object_runtime_test_access_t::hold_lane (
      host._local_dispatch_completion_lane);
}

void public_host_runtime_test_access_t::lifecycle_turn (public_host_runtime_t &host,
                                                        std::function<void ()> work)
{
    host._lifecycle_configuration_lane.run (std::move (work)).get ();
}

task_t<zlink::submit_result_t>
public_host_runtime_test_access_t::join (actor_handle_t &actor,
                                         const zlink::routing_id_t &rid,
                                         const std::string &spot,
                                         std::uint64_t generation,
                                         pending_operation_t &operation,
                                         std::chrono::milliseconds timeout)
{
    co_return co_await actor.join_spot (rid, spot, generation, {}, operation, timeout);
}

task_t<std::size_t> public_host_runtime_test_access_t::dispatch_claim (
  public_host_runtime_t &host,
  std::shared_ptr<mesh::service_mailbox_claim_t> claim,
  const mesh::service_mailbox_t::application_dispatch_t &dispatch)
{
    return host.dispatch_application_claim (std::move (claim), dispatch);
}

task_t<void> public_host_runtime_test_access_t::ingest_cut (public_host_runtime_t &host,
                                                            stateful::object_ref_t owner,
                                                            mesh::service_mailbox_record_t original)
{
    return host.ingest_cut_original (std::move (owner), std::move (original));
}

std::array<std::size_t, 3>
public_host_runtime_test_access_t::spot_records (public_host_runtime_t &host)
{
    return stateful::raw_stateful_dispatch_test_access_t::spot_records (*host._stateful_dispatch);
}

task_t<bool> public_host_runtime_test_access_t::restore_accepted_spot (
  public_host_runtime_t &host,
  std::shared_ptr<location_repository_t> store,
  const protocol::request_source_fence_t &source,
  std::shared_ptr<std::atomic<int>> materialized)
{
    host._user_spot_store = std::move (store);
    host._objects.configure_relocation_state (
      [] (const stateful::object_ref_t &owner, const std::string &type, std::stop_token)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          co_return stateful::frozen_object_state_t{owner, type};
      },
      [materialized, source] (const auto &frozen, const auto &,
                              std::stop_token) -> zlink::framework::task_t<bool> {
          ++*materialized;
          EXPECT_EQ (frozen.pending_application.size (), 1u);
          if (!frozen.pending_application.empty ()) {
              const auto &turn = frozen.pending_application.front ();
              EXPECT_TRUE (turn.frozen_record.has_value ());
              if (turn.frozen_record)
                  EXPECT_EQ (turn.frozen_record->source, source);
          }
          co_return true;
      });
    auto pending = std::make_shared<public_host_runtime_t::pending_relocation_assembly_t> (
      public_host_runtime_t::pending_relocation_assembly_t{
        .assembly = stateful::relocation_state_assembly_t{{}, 0, {}, {}, {}}});
    auto staging = std::make_shared<public_host_runtime_t::relocation_assembly_staging_t> ();
    stateful::object_ref_t owner{
      stateful::object_kind_t::user_spot, "accepted-spot", 1, 1, "source-boundary", "source-node"};
    auto target = owner;
    target.node_id = "target-node";
    target.authority_owner_generation = 2;
    protocol::frozen_record_t record;
    record.kind = protocol::frozen_record_kind_t::spot_send;
    record.source = source;
    record.operation = {1, 1};
    record.application = protocol::application_payload_t{"Probe", "application/json", {'{', '}'}};
    stateful::turn_record_t turn;
    turn.sequence = 1;
    turn.frozen_record = std::move (record);
    staging->frozen.push_back ({owner, "room", {}, {std::move (turn)}, {}});
    staging->sources.push_back (owner);
    staging->targets.push_back (target);
    staging->wire_objects.push_back ({});
    staging->restore_identity.reference = "accepted-source-boundary";
    return host.restore_relocation_assembly (pending, staging);
}

task_t<bool> public_host_runtime_test_access_t::activate_stale_actor_attempt (
  public_host_runtime_t &host,
  bool replaced,
  bool activation_started,
  const stateful::object_ref_t &target,
  std::string stable_type)
{
    const public_host_runtime_t::relocation_attempt_key_t key{1, 2, 3};
    auto pending = std::make_shared<public_host_runtime_t::pending_relocation_assembly_t> (
      public_host_runtime_t::pending_relocation_assembly_t{
        .assembly = stateful::relocation_state_assembly_t{{}, 0, {}, {}, {}}});
    pending->prepare.relocation = {1, 2};
    pending->prepare.target_attempt_generation = 3;
    auto staging = std::make_shared<public_host_runtime_t::relocation_assembly_staging_t> ();
    staging->targets.push_back (target);
    staging->wire_objects.emplace_back ();
    staging->wire_objects.back ().stable_type = std::move (stable_type);
    host._relocation_session_terminal_lane
      .run ([&] {
          auto &attempt = host._relocation_target_attempts[key];
          attempt.prepare = pending->prepare;
          attempt.pending = pending;
          attempt.terminal = std::make_shared<task_completion_source_t<void>> ();
      })
      .get ();
    std::optional<task_t<bool>> activated;
    std::optional<public_host_runtime_t::relocation_target_attempt_t> discarded;
    // Keep registration pending behind this index turn. Attempt removal or
    // replacement happens before the index FIFO advances, without waiting
    // for an external completion from inside a state lane.
    host._spot_actor_index_lane
      .run ([&] {
          if (activation_started) {
              activated.emplace (host.activate_relocation_assembly (key, pending, staging));
              EXPECT_FALSE (activated->await_ready ());
          }
          discarded =
            host._relocation_session_terminal_lane
              .run ([&] {
                  auto &attempt = host._relocation_target_attempts.at (key);
                  auto cleanup = host.take_relocation_target_discard (attempt);
                  if (!cleanup)
                      attempt.terminal->complete (result_t<void>::success ());
                  if (replaced) {
                      attempt = public_host_runtime_t::relocation_target_attempt_t{};
                      EXPECT_TRUE (pending->request.parts ().empty ());
                      EXPECT_FALSE (pending->request.application);
                      EXPECT_FALSE (pending->request.application_permit);
                      EXPECT_FALSE (pending->request.transferred);
                      attempt.pending =
                        std::make_shared<public_host_runtime_t::pending_relocation_assembly_t> (
                          public_host_runtime_t::pending_relocation_assembly_t{
                            .prepare = pending->prepare,
                            .assembly = pending->assembly,
                            .expires_at = pending->expires_at,
                            .ready = pending->ready,
                            .cancellation = pending->cancellation});
                  } else
                      host._relocation_target_attempts.erase (key);
                  return cleanup;
              })
              .get ();
          if (!activation_started)
              activated.emplace (host.activate_relocation_assembly (key, pending, staging));
      })
      .get ();
    if (discarded) {
        std::vector<public_host_runtime_t::relocation_target_attempt_t> cleanup;
        cleanup.push_back (std::move (*discarded));
        host.discard_relocation_target_attempts (std::move (cleanup));
    }
    co_return co_await *activated;
}
} // namespace zlink::framework::runtime::host
