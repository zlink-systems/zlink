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
#include "../support/relocation_seal_execution_fixture.hpp"

using namespace framework_relocation_seal_fixture;

using zlink::framework::test::begin_reserved_object;

namespace zlink::framework::runtime::stateful
{
struct stateful_object_runtime_test_access_t
{
    // Holds the object state lane in one turn until the returned promise is set.
    static std::promise<void> hold_lane (stateful_object_runtime_t &objects)
    {
        std::promise<void> release;
        std::promise<void> entered;
        auto held = release.get_future ().share ();
        auto running = entered.get_future ();
        if (!objects._lane.try_post ([held, &entered] {
                entered.set_value ();
                held.wait ();
            }))
            throw std::runtime_error ("object lane is closed");
        running.wait ();
        return release;
    }
};
} // namespace zlink::framework::runtime::stateful

using namespace zlink::framework;

using namespace zlink::framework::detail;

namespace host = zlink::framework::runtime::host;

namespace mesh = zlink::framework::runtime::mesh;

namespace protocol = zlink::framework::runtime::protocol;

namespace stateful = zlink::framework::runtime::stateful;

TEST (FrameworkRelocationSealContract, lookup_owner_ports_suspend_without_blocking)
{
    stateful::stateful_object_runtime_t objects;
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "spot", 1, 1, "mesh", "source"};
    const auto created = begin_reserved_object (objects, spot, "room", {});
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    auto release = stateful::stateful_object_runtime_test_access_t::hold_lane (objects);
    auto find = objects.find_async (spot.kind, spot.key);
    auto resolve = objects.resolve_spot_async (spot.key);
    EXPECT_FALSE (find.await_ready ());
    EXPECT_FALSE (resolve.await_ready ());
    release.set_value ();
    EXPECT_EQ (find.result ().value (), spot);
    EXPECT_EQ (resolve.result ().value (), spot);
}

TEST (FrameworkRelocationSealContract, relocated_stage_returns_before_owner_lane_completes)
{
    using namespace std::chrono_literals;
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("async-stage"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    auto &objects = host->objects ();
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "spot", 1, 1, "mesh", "source"};
    const auto created = begin_reserved_object (objects, spot, "room", {});
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    stateful::raw_stateful_dispatch_t dispatch (objects, host->transport ());
    protocol::spot_message_header_t header;
    header.target = {spot.key, 1, {1}, 1, 1, 3};
    header.operation = {1, 1};
    const auto accepted = stateful::accepted_spot_record (
      header, {"sender", 3, {2}, 1}, {"Probe", "application/json", {'{', '}'}}, false);
    const auto as_task = []<typename T> (T value) -> task_t<stateful::stateful_error_t> {
        if constexpr (std::is_same_v<T, task_t<stateful::stateful_error_t>>)
            return std::move (value);
        else
            return task_t<stateful::stateful_error_t> (
              result_t<stateful::stateful_error_t>::success (value));
    };
    auto release = stateful::stateful_object_runtime_test_access_t::hold_lane (objects);
    auto returned = std::async (std::launch::async, [&] {
        return as_task (dispatch.stage_relocated (
          spot, {0, protocol::encode_frozen_application_record (accepted).canonical_bytes},
          [] (const auto &) { return true; }));
    });
    EXPECT_EQ (returned.wait_for (100ms), std::future_status::ready);
    release.set_value ();
    auto completed = returned.get ();
    EXPECT_EQ (completed.result ().value (), stateful::stateful_error_t::none);
    host->close ();
}

TEST (FrameworkRelocationSealContract, raw_dispatch_waits_for_state_lane_outside_its_mutex)
{
    using namespace std::chrono_literals;
    using access_t = stateful::stateful_object_runtime_test_access_t;
    for (const bool ingest : {false, true}) {
        SCOPED_TRACE (ingest ? "ingress commit" : "claim");
        auto host = std::make_shared<host::public_host_runtime_t> (
          host::host_options_t{mesh::raw_mesh_node_options_t{
            .descriptor = test_descriptor ("lane-wait"),
            .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
        auto &objects = host->objects ();
        const stateful::object_ref_t object{
          stateful::object_kind_t::actor, "lane-owner", 1, 1, "mesh", "source"};
        const auto created = begin_reserved_object (objects, object, "actor", {});
        ASSERT_EQ (created.error, stateful::stateful_error_t::none);
        ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
        const protocol::request_source_fence_t fence{"source-owner", 3, {2}, 1};
        stateful::raw_stateful_dispatch_t dispatch (
          objects, host->transport (),
          [&] (const auto &) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
              co_return stateful::accepted_record_authority_t{fence, 3};
          });
        std::optional<mesh::service_mailbox_t::claim_handle_t> slot;
        std::optional<stateful::retained_relocation_source_t> retained;
        if (ingest) {
            const protocol::actor_route_fence_t route{object.key, 1, {1}, 1, 1, 3};
            auto record = mesh::service_mailbox_record_t::from_parts (
              {}, mesh::service_mailbox_domain_t::application,
              detail::backend::materialize_binding_parts (
                {protocol::encode_actor_message_header (protocol::command::actorSend, std::nullopt,
                                                        route, {1, 1}),
                 protocol::encode_application_payload ({"Probe", "application/json", {'{', '}'}})}),
              {2}, std::nullopt, 1);
            slot = zlink::framework::test::publish_stateful_claim (host->transport (), object,
                                                                   std::move (record));
            retained = zlink::framework::test::retained_ingress_snapshot (objects, object);
        }
        auto release = access_t::hold_lane (objects);
        auto waiting = std::async (std::launch::async, [&] {
            if (ingest)
                return dispatch.ingest_claimed (object, *slot, retained).result ().value ();
            return dispatch.try_claim (object).first;
        });
        // The waiting call has reached the held lane.
        std::this_thread::sleep_for (100ms);
        const stateful::object_ref_t other{
          stateful::object_kind_t::actor, "other-owner", 1, 1, "mesh", "source"};
        auto unrelated =
          std::async (std::launch::async, [&] { dispatch.release_relocated_payloads (other, {}); });
        EXPECT_EQ (unrelated.wait_for (2s), std::future_status::ready);
        release.set_value ();
        unrelated.get ();
        EXPECT_EQ (waiting.get (), stateful::stateful_error_t::none);
        host->close ();
    }
}

// A seal that waits for an active turn returns to its caller; the turn's terminal resumes it.
TEST (FrameworkRelocationSealContract, seal_waiting_for_active_turn_does_not_hold_its_caller)
{
    using namespace std::chrono_literals;
    stateful::stateful_object_runtime_t objects;
    objects.replace_placement_candidates ({{.mesh_name = "mesh",
                                            .node_id = "source",
                                            .stable_types = {"actor"},
                                            .active_capacity = 1,
                                            .pending_capacity = 1}});
    const auto created = objects.begin_create ({.kind = stateful::object_kind_t::actor,
                                                .key = "busy",
                                                .stable_type = "actor",
                                                .mesh_name = std::string ("mesh"),
                                                .exclusive = true});
    ASSERT_EQ (created.status, stateful::create_status_t::reserved);
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    ASSERT_EQ (objects.enqueue (created.object, stateful::turn_domain_t::application, {1, {1}}),
               stateful::stateful_error_t::none);
    ASSERT_TRUE (objects.try_claim (created.object, stateful::turn_domain_t::application).second);
    auto started = std::async (
      std::launch::async, [&] { return objects.try_seal_relocation_aggregate ({created.object}); });
    EXPECT_EQ (started.wait_for (2s), std::future_status::ready);
    EXPECT_EQ (objects.complete_claim (created.object, stateful::turn_domain_t::application),
               stateful::stateful_error_t::none);
    auto seal = started.get ();
    const auto sealed = seal.result ().value ();
    EXPECT_EQ (sealed.error, stateful::stateful_error_t::none);
    ASSERT_EQ (sealed.seal.participants.size (), 1u);
    EXPECT_EQ (sealed.seal.participants.front ().pending_application.size (), 0u);
}
