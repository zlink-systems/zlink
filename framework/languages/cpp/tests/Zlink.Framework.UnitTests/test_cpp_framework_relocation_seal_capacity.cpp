/* SPDX-License-Identifier: Elastic-2.0 */
#include "mailbox_claim_fixture.hpp"
#include "../support/telemetry_observation.hpp"
#include "../support/stateful_object_sync.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/exception_payload.hpp"
#include "../support/manual_internal_scheduler.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "../support/stateful_ingress_fixture.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/accepted_actor_record.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/timers/timer_runtime.hpp"
#include <gtest/gtest.h>
#include <service_wire_pilot_codec.hpp>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <string>
#include <utility>
#include <type_traits>
#include <variant>
#include <vector>


using namespace zlink::framework;

using namespace zlink::framework::detail;

namespace host = zlink::framework::runtime::host;

namespace mesh = zlink::framework::runtime::mesh;

namespace protocol = zlink::framework::runtime::protocol;

namespace stateful = zlink::framework::runtime::stateful;

TEST (FrameworkRelocationSealContract, restored_actor_owns_active_capacity_until_destroy)
{
    stateful::stateful_object_runtime_t objects;
    objects.replace_placement_candidates ({{.mesh_name = "mesh",
                                            .node_id = "target",
                                            .stable_types = {"actor"},
                                            .active_capacity = 1,
                                            .pending_capacity = 1}});
    objects.configure_relocation_state (
      [] (const auto &, const auto &, std::stop_token)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          co_return stateful::frozen_object_state_t{};
      },
      [] (const auto &, const auto &, std::stop_token) -> zlink::framework::task_t<bool> {
          co_return true;
      });
    const stateful::object_ref_t source{
      stateful::object_kind_t::actor, "restored", 1, 1, "mesh", "source"};
    auto target = source;
    target.node_id = "target";
    ++target.authority_owner_generation;
    const stateful::relocation_restore_identity_t identity{"restore-capacity", 1, {}};
    ASSERT_EQ (objects.restore_relocation ({source, "actor"}, target, identity).result ().value (),
               stateful::stateful_error_t::none);
    ASSERT_EQ (objects.commit_relocation_restore (target, identity),
               stateful::stateful_error_t::none);
    EXPECT_EQ (objects.commit_relocation_restore (target, identity),
               stateful::stateful_error_t::already_exists);
    EXPECT_EQ (objects.destroy_actor (target), stateful::stateful_error_t::none);
    const auto next = objects.begin_create ({.kind = stateful::object_kind_t::actor,
                                             .key = "next",
                                             .stable_type = "actor",
                                             .mesh_name = std::string ("mesh"),
                                             .exclusive = true});
    ASSERT_EQ (next.status, stateful::create_status_t::reserved);
    EXPECT_EQ (objects.commit_create (next.attempt), stateful::stateful_error_t::none);
    EXPECT_EQ (objects.destroy_actor (next.object), stateful::stateful_error_t::none);
}

TEST (FrameworkRelocationSealContract, restored_aggregate_releases_each_active_object_once)
{
    stateful::stateful_object_runtime_t objects;
    objects.replace_placement_candidates ({{.mesh_name = "mesh",
                                            .node_id = "target",
                                            .stable_types = {"actor", "spot"},
                                            .active_capacity = 2,
                                            .pending_capacity = 2}});
    objects.configure_relocation_state (
      [] (const auto &, const auto &, std::stop_token)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          co_return stateful::frozen_object_state_t{};
      },
      [] (const auto &, const auto &, std::stop_token) -> zlink::framework::task_t<bool> {
          co_return true;
      });
    const std::vector<stateful::frozen_object_state_t> frozen{
      {{stateful::object_kind_t::user_spot, "room", 1, 1, "mesh", "source"}, "spot"},
      {{stateful::object_kind_t::actor, "member", 1, 1, "mesh", "source"}, "actor"}};
    std::vector<stateful::object_ref_t> targets;
    for (const auto &entry : frozen) {
        auto target = entry.owner;
        target.node_id = "target";
        ++target.authority_owner_generation;
        targets.push_back (target);
    }
    const stateful::relocation_restore_identity_t identity{"aggregate-capacity", 1, {}};
    ASSERT_EQ (objects.restore_relocation_aggregate (frozen, targets, identity).result ().value (),
               stateful::stateful_error_t::none);
    ASSERT_EQ (objects.commit_relocation_restore_aggregate (targets, identity),
               stateful::stateful_error_t::none);
    EXPECT_EQ (objects.destroy_actor (targets[1]), stateful::stateful_error_t::none);
    EXPECT_EQ (objects.close_spot (targets[0]).first, stateful::stateful_error_t::none);
    for (int index = 0; index != 2; ++index) {
        const auto next = objects.begin_create ({.kind = stateful::object_kind_t::actor,
                                                 .key = "next-" + std::to_string (index),
                                                 .stable_type = "actor",
                                                 .mesh_name = std::string ("mesh"),
                                                 .exclusive = true});
        ASSERT_EQ (next.status, stateful::create_status_t::reserved);
        EXPECT_EQ (objects.commit_create (next.attempt), stateful::stateful_error_t::none);
    }
}

TEST (FrameworkRelocationSealContract, finalized_source_releases_capacity_and_retains_follow)
{
    stateful::stateful_object_runtime_t objects;
    objects.replace_placement_candidates ({{.mesh_name = "mesh",
                                            .node_id = "source",
                                            .stable_types = {"actor"},
                                            .active_capacity = 1,
                                            .pending_capacity = 1}});
    const auto created = objects.begin_create ({.kind = stateful::object_kind_t::actor,
                                                .key = "moving",
                                                .stable_type = "actor",
                                                .mesh_name = std::string ("mesh"),
                                                .exclusive = true});
    ASSERT_EQ (created.status, stateful::create_status_t::reserved);
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    const auto sealed = objects.try_seal_relocation_aggregate ({created.object}).result ().value ();
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
    int followed = 0;
    ASSERT_EQ (objects
                 .begin_relocation_boundary (sealed.seal.token,
                                             [&] (const auto &, const auto &) { ++followed; })
                 .first,
               stateful::stateful_error_t::none);
    ASSERT_EQ (objects.finalize_relocation_cutover (sealed.seal.token).result ().value (),
               stateful::stateful_error_t::none);
    const auto next = objects.begin_create ({.kind = stateful::object_kind_t::actor,
                                             .key = "next",
                                             .stable_type = "actor",
                                             .mesh_name = std::string ("mesh"),
                                             .exclusive = true});
    EXPECT_EQ (next.status, stateful::create_status_t::reserved);
    EXPECT_EQ (objects.enqueue (created.object, stateful::turn_domain_t::application, {1, {1}}),
               stateful::stateful_error_t::none);
    EXPECT_EQ (followed, 1);
    EXPECT_TRUE (objects.finalized_relocation_source (created.object.kind, created.object.key,
                                                      created.object.object_generation));
}

// The invariant is asserted by the runtime in Debug builds.
#ifdef ZLINK_FRAMEWORK_DEBUG_WAIT_GUARD
TEST (FrameworkRelocationSealContract, source_capacity_underflow_is_an_invariant_failure)
{
    EXPECT_DEATH (
      ([] {
          stateful::stateful_object_runtime_t objects;
          stateful::placement_candidate_t candidate{.mesh_name = "mesh",
                                                    .node_id = "source",
                                                    .stable_types = {"actor"},
                                                    .active_capacity = 1,
                                                    .pending_capacity = 1};
          objects.replace_placement_candidates ({candidate});
          const auto created = objects.begin_create ({.kind = stateful::object_kind_t::actor,
                                                      .key = "underflow",
                                                      .stable_type = "actor",
                                                      .mesh_name = std::string ("mesh"),
                                                      .exclusive = true});
          (void) objects.commit_create (created.attempt);
          // Fault injection: discard the live object's capacity accounting.
          objects.replace_placement_candidates ({candidate});
          const auto sealed =
            objects.try_seal_relocation_aggregate ({created.object}).result ().value ();
          (void) objects.begin_relocation_boundary (sealed.seal.token,
                                                    [] (const auto &, const auto &) {});
          (void) objects.finalize_relocation_cutover (sealed.seal.token).result ().value ();
      }()),
      "active_count");
}

#endif
