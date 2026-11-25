/* SPDX-License-Identifier: Elastic-2.0 */

#include "actor_maintenance_join_suffix_fixture.hpp"
#include "actor_maintenance_join_host_fixture.hpp"
#include "actor_maintenance_join_packets.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace zlink::framework::test::actor_maintenance_join
{
namespace
{
std::string suffix_failure_snapshot (host_t &host)
{
    namespace st = fw::runtime::stateful;
    const auto registration =
      fw::detail::mesh_node_runtime_t::registrations (host.app.advanced ().zlink ()).front ();
    const auto node = fw::detail::spot_node_runtime_t (registration->spot_state).native_node ();
    std::string result;
    const auto inventory = node->objects ().inventory_async ().result ().value ();
    for (const auto &item : inventory) {
        if (item.owner.kind == st::object_kind_t::actor && item.owner.key == actor_id) {
            result += " kernel_state=" + std::to_string (static_cast<int> (item.state))
                      + " kernel_pending="
                      + std::to_string (
                        node->objects ().pending (item.owner, st::turn_domain_t::application));
        }
    }
    result += " native_pending="
              + std::to_string (node->transport ().mailbox ().pending_messages (
                fw::runtime::mesh::service_mailbox_domain_t::application));
    return result;
}
} // namespace

void next_join_suffix (
  bool remote, bool request, bool restore_source, bool native_initiator, bool hold_native_suffix)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    evidence.joined_payloads = {1, 2};
    evidence.transfer_suffix = true;
    evidence.suffix_request = request;
    evidence.fail_remote_factory = restore_source;
    if (hold_native_suffix)
        evidence.native_handoff_hold = std::make_shared<native_handoff_hold_t> ();
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t source, target, initiator;
    release_callbacks_t release{evidence};
    configure (source, evidence_owner, first_rid, store, relocations);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    auto &spots = services.get_required<fw::spot_manager_t> ();
    ASSERT_TRUE (spots.get_or_create (fw::spot_id_t (local_spot), room_type).async ().result ());
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    if (remote) {
        configure (target, evidence_owner, second_rid, store, relocations);
        target.start ();
        ASSERT_TRUE (ready (target));
        ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> (), second_rid));
        auto target_services = target.app.advanced ().services ().build_provider ();
        ASSERT_TRUE (set_published_placement_weight (services, 0, &target_services));
        ASSERT_TRUE (target_services.get_required<fw::spot_manager_t> ()
                       .get_or_create (fw::spot_id_t (suffix_spot), room_type)
                       .async ()
                       .result ());
    } else {
        ASSERT_TRUE (
          spots.get_or_create (fw::spot_id_t (suffix_spot), room_type).async ().result ());
    }
    auto &sender = services.get_required<fw::actor_client_t> ();
    evidence.joined_sender = &sender;
    fw::actor_client_t *initial_sender = &sender;
    std::optional<fw::service_provider_t> initiator_services;
    if (native_initiator) {
        configure (initiator, evidence_owner, third_rid, store, relocations);
        initiator.start ();
        ASSERT_TRUE (ready (initiator));
        ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> (), third_rid));
        initiator_services.emplace (initiator.app.advanced ().services ().build_provider ());
        // MeshNode §7.1: each node is Ready on the ADMIT it receives; the sender needs its own.
        ASSERT_TRUE (
          peer_ready (initiator_services->get_required<fw::route_mesh_runtime_t> (), first_rid));
        initial_sender = &initiator_services->get_required<fw::actor_client_t> ();
    }
    const auto initial_sent =
      initial_sender->send (fw::actor_id_t (actor_id), move_t{local_spot}).async ().result ();
    ASSERT_TRUE (initial_sent) << "kind=" << static_cast<int> (initial_sent.error_kind ())
                               << " error="
                               << (initial_sent.error () ? initial_sent.error ()->what ()
                                                         : "no error");
    if (const auto hold = evidence.native_handoff_hold) {
        std::unique_lock lock (evidence.mutex);
        ASSERT_TRUE (evidence.changed.wait_for (lock, wait_budget, [&] {
            return hold->entered.task ().await_ready ()
                   && hold->preceding_h_finished.task ().await_ready ()
                   && hold->next_join_started.task ().await_ready ()
                   && hold->next_join_terminal.task ().await_ready ();
        }));
        EXPECT_EQ (evidence.handled_payloads, (std::vector<int>{1}));
        EXPECT_EQ (evidence.handled_completion_counts, (std::vector<std::size_t>{1}));
        EXPECT_FALSE (hold->release.task ().await_ready ());
        const auto registration = fw::detail::mesh_node_runtime_t::registrations (
                                    (remote ? target : source).app.advanced ().zlink ())
                                    .front ();
        const auto node = fw::detail::spot_node_runtime_t (registration->spot_state).native_node ();
        const auto owner =
          node->objects ().find (fw::runtime::stateful::object_kind_t::actor, actor_id);
        EXPECT_TRUE (owner);
        if (owner)
            EXPECT_EQ (
              node->objects ().pending (*owner, fw::runtime::stateful::turn_domain_t::application),
              0u);
        const auto *failed = std::get_if<fw::actor_join_failed_t> (&evidence.completions.back ());
        std::printf ("held-join-terminal-before-release type=%zu error_kind=%d release_ready=%d "
                     "completions=%zu\n",
                     evidence.completions.back ().index (),
                     failed ? static_cast<int> (failed->error_kind) : -1,
                     hold->release.task ().await_ready (), evidence.completions.size ());
        if (!remote && owner) {
            const auto target_spot =
              node->objects ().find (fw::runtime::stateful::object_kind_t::user_spot, suffix_spot);
            EXPECT_TRUE (target_spot);
            if (target_spot) {
                auto [prepare, token] = node->objects ()
                                          .begin_membership_move_async (*owner, *target_spot)
                                          .result ()
                                          .value ();
                std::printf ("held-kernel-membership-before-release error=%d expected_ready=%d "
                             "release_ready=%d actor=%s generation=%llu authority=%llu\n",
                             static_cast<int> (prepare),
                             static_cast<int> (fw::runtime::stateful::stateful_error_t::none),
                             hold->release.task ().await_ready (), owner->key.c_str (),
                             static_cast<unsigned long long> (owner->object_generation),
                             static_cast<unsigned long long> (owner->authority_owner_generation));
                EXPECT_EQ (prepare, fw::runtime::stateful::stateful_error_t::none);
                if (prepare == fw::runtime::stateful::stateful_error_t::none)
                    EXPECT_EQ (
                      node->objects ().abort_membership_move_async (token).result ().value (),
                      fw::runtime::stateful::stateful_error_t::none);
            }
        }
        std::printf (
          "native-publication-hold entries=%d Hdone=%d next_join_started=%d handlers=%zu%s\n",
          hold->entries, hold->preceding_h_finished.task ().await_ready (),
          hold->next_join_started.task ().await_ready (), evidence.handled_payloads.size (),
          suffix_failure_snapshot (source).c_str ());
        lock.unlock ();
        hold->release.complete (fw::result_t<void>::success ());
    }
    std::unique_lock lock (evidence.mutex);
    ASSERT_TRUE (evidence.changed.wait_for (
      lock, wait_budget,
      [&] { return evidence.completions.size () >= 2 && evidence.handled_payloads.size () >= 2; }))
      << "completions=" << evidence.completions.size ()
      << " handled=" << evidence.handled_payloads.size () << suffix_failure_snapshot (source);
    EXPECT_EQ (evidence.handled_payloads, (std::vector<int>{1, 2}));
    EXPECT_EQ (evidence.handled_spots,
               (std::vector<std::string>{local_spot, restore_source ? local_spot : suffix_spot}));
    EXPECT_EQ (
      evidence.handled_nodes,
      (std::vector<std::string>{first_rid, remote && !restore_source ? second_rid : first_rid}));
    EXPECT_EQ (evidence.handled_completion_counts, (std::vector<std::size_t>{1, 2}));
    ASSERT_EQ (evidence.completions.size (), 2u);
    EXPECT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.front ()));
    if (restore_source) {
        EXPECT_FALSE (
          std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.back ()));
        EXPECT_EQ (evidence.remote_factory_failure_count, 1);
    } else
        EXPECT_TRUE (
          std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.back ()));
    const auto reply = evidence.suffix_reply;
    lock.unlock ();
    if (request) {
        ASSERT_TRUE (reply);
        const auto result = reply ();
        ASSERT_TRUE (result) << (result.error () ? result.error ()->what () : "no error");
        EXPECT_EQ (result.value (), 2);
    }
}
} // namespace zlink::framework::test::actor_maintenance_join
