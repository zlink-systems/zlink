/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "../support/actor_maintenance_join_native_fixture.hpp"
#include "../support/actor_maintenance_join_host_access.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "runtime/stateful/accepted_actor_record.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace zlink::framework::test::actor_maintenance_join
{
TEST (NativeIntake, OrdinarySpotDoesNotMaterializeRelocationRecord)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    evidence.ordinary_spot = std::make_shared<ordinary_spot_observation_t> ();
    const auto observation = evidence.ordinary_spot;
    struct release_t
    {
        std::shared_ptr<ordinary_spot_observation_t> observation;
        ~release_t () { observation->release.complete (fw::result_t<void>::success ()); }
    };
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t host;
    release_t release{observation};
    configure (host, evidence_owner, first_rid, store, relocations);
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (local_spot), room_type)
                   .async ()
                   .result ());
    auto route = host.app.advanced ().zlink ().route_client (
      services.get_required<fw::serializer_registry_t> ());
    auto first = route.request_to_spot (fw::spot_id_t (local_spot), probe_t{1}).async<probe_t> ();
    observation->entered.task ().result ().value ();
    const auto registration =
      fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ()).front ();
    auto node = fw::detail::spot_node_runtime_t (registration->spot_state).native_node ();
    const auto counts = fw::runtime::host::public_host_runtime_test_access_t::spot_records (*node);
    EXPECT_EQ (counts[0], 0u) << "frozen record before seal";
    EXPECT_EQ (counts[1], 0u) << "summary before seal";
    EXPECT_EQ (counts[2], 0u) << "raw pending registrations before seal";
    observation->release.complete (fw::result_t<void>::success ());
    const auto reply = first.result ();
    ASSERT_TRUE (reply);
    EXPECT_EQ (reply.value ().value, 1);
    ASSERT_TRUE (route.send_to_spot (fw::spot_id_t (local_spot), probe_t{2}).async ().result ());
    const auto barrier =
      route.request_to_spot (fw::spot_id_t (local_spot), probe_t{3}).async<probe_t> ().result ();
    ASSERT_TRUE (barrier);
    EXPECT_EQ (barrier.value ().value, 3);
    EXPECT_EQ (observation->handled, (std::vector<int>{1, 2, 3}));
    const auto completed =
      fw::runtime::host::public_host_runtime_test_access_t::spot_records (*node);
    EXPECT_EQ (completed[2], 0u) << "raw pending registrations on ordinary terminal";
}

TEST (NativeIntake, LocalAcceptedRecordReleasesAfterNativeTerminal)
{
    native_intake_fixture_t fixture;
    auto actor = fixture.host->create_actor (actor_type, actor_id);
    auto wire = fixture.actor_record (1);
    auto parts =
      fw::runtime::protocol::decode_application_parts (std::move (wire.body_parts ().back ()));
    const auto sent = actor.send_to (actor.ref (), parts).result ();
    ASSERT_TRUE (sent);
    ASSERT_EQ (sent.value (), zlink::submit_result_t::ok);
    std::weak_ptr<fw::runtime::serial_application_record_t> accepted;
    std::size_t received = 0;
    auto dispatch = [&] (const fw::runtime::host::ready_record_t &,
                         const fw::runtime::host::receive_record_t &record,
                         std::vector<zlink::message_t>, auto) {
        ++received;
        accepted = record.application_record;
        ASSERT_TRUE (record.application_record);
        ASSERT_TRUE (record.complete_stateful_dispatch);
        record.complete_stateful_dispatch ();
    };
    ASSERT_TRUE (fixture.host->dispatch_ready (dispatch).result ());
    ASSERT_EQ (received, 1u);
    fixture.host->close ();
    EXPECT_TRUE (accepted.expired ());
}

TEST (NativeIntake, LocalSourceAwaitKeepsCanonicalOriginalAndIgnoresStaleCompletion)
{
    namespace rt = fw::runtime;
    namespace st = rt::stateful;
    fw::task_completion_source_t<std::optional<st::accepted_record_authority_t>> authority;
    int queries = 0;
    std::vector<std::uint8_t> observed_source;
    native_intake_fixture_t fixture ({}, [&] (st::accepted_record_authority_query_t query) {
        ++queries;
        observed_source = query.source_node_routing_id;
        return authority.task ();
    });
    struct release_t
    {
        fw::task_completion_source_t<std::optional<st::accepted_record_authority_t>> &source;
        ~release_t ()
        {
            source.complete (
              fw::result_t<std::optional<st::accepted_record_authority_t>>::success (std::nullopt));
        }
    } release{authority};
    auto host = fixture.host;
    auto actor = host->create_actor (actor_type, actor_id);
    auto wire = fixture.actor_record (1);
    auto parts = rt::protocol::decode_application_parts (std::move (wire.body_parts ().back ()));
    ASSERT_TRUE (actor.send_to (actor.ref (), parts).result ());
    auto &mailbox = host->transport ().mailbox ();
    const auto key =
      rt::mesh::service_mailbox_t::application_owner (rt::host::owner_kind_t::actor, actor_id);
    auto claim =
      mailbox.try_claim_owner (rt::mesh::service_mailbox_domain_t::application, key, 1, 4096);
    ASSERT_TRUE (claim);
    auto old = claim;
    int dispatched = 0;
    auto pending = rt::host::public_host_runtime_test_access_t::dispatch_claim (
      *host, claim, [&] (const auto &, const auto &record, auto, auto) {
          ++dispatched;
          ASSERT_TRUE (record.complete_stateful_dispatch);
          record.complete_stateful_dispatch ();
      });
    ASSERT_EQ (queries, 1);
    EXPECT_EQ (observed_source, fixture.rid);
    ASSERT_FALSE (pending.await_ready ());
    auto sealed = mailbox.take ({old, 0});
    ASSERT_TRUE (sealed);
    ASSERT_TRUE (sealed->application);
    EXPECT_EQ (sealed->application->parts.size (), parts.size ());
    ASSERT_TRUE (mailbox.release (*old));
    ASSERT_TRUE (mailbox.try_enqueue (std::move (*sealed)));
    authority.complete (fw::result_t<std::optional<st::accepted_record_authority_t>>::success (
      st::accepted_record_authority_t{{fixture.source_id, 1, fixture.rid, 1}, 1}));
    ASSERT_EQ (pending.result ().value (), 0u);
    EXPECT_EQ (dispatched, 0);
    EXPECT_EQ (queries, 1);
    EXPECT_FALSE (mailbox.take ({old, 0}));
    EXPECT_FALSE (mailbox.release (*old));
    auto restored =
      mailbox.try_claim_owner (rt::mesh::service_mailbox_domain_t::application, key, 1, 4096);
    ASSERT_TRUE (restored);
    EXPECT_EQ (rt::host::public_host_runtime_test_access_t::dispatch_claim (
                 *host, std::move (restored),
                 [&] (const auto &, const auto &record, auto, auto) {
                     ++dispatched;
                     ASSERT_TRUE (record.complete_stateful_dispatch);
                     record.complete_stateful_dispatch ();
                 })
                 .result ()
                 .value (),
               1u);
    EXPECT_EQ (dispatched, 1);
    EXPECT_EQ (queries, 2);
    host->close ();
}
} // namespace zlink::framework::test::actor_maintenance_join
