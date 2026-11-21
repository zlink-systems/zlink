/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6a_runtime_tests.hpp"
#include "../support/m6a_mesh_fixture.hpp"

#include "mailbox_claim_fixture.hpp"

#include "../support/runtime_failure_fixture.hpp"


#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include <limits>
#include <zlink/framework/contracts/dispatch/request_task.hpp>
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/mesh/user_spot_terminal_mapping.hpp"
#include "runtime/locations/service_descriptor_registry.hpp"
#include "runtime/fanout/raw_fanout_owner.hpp"
#include "runtime/client_server/raw_client_server_owner.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Sockets/message_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>
#include <zlink/socket/api.h>
#include "runtime/client_server/weighted_selector.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

#include <service_wire_pilot_codec.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <future>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace zlink::framework::test::m6a
{
namespace mesh = zlink::framework::runtime::mesh;
namespace locations = zlink::framework::runtime::locations;
namespace fanout = zlink::framework::runtime::fanout;
namespace client_server = zlink::framework::runtime::client_server;
namespace foundation = zlink::framework::runtime::foundation;
namespace protocol = zlink::framework::runtime::protocol;
namespace runtime = zlink::framework::runtime;
using namespace std::chrono_literals;

void verify_envelope_body_retains_native_storage ()
{
    namespace messaging = zlink::framework::runtime::messaging;
    std::optional<zlink::message_t> retained;
    {
        std::vector<zlink::message_t> values;
        values.emplace_back ();
        values.push_back (zlink::message_t::from (std::vector<std::uint8_t> (4096, 7)));
        messaging::message_parts_t parts (std::move (values));
        const auto address = parts[1].data ();
        auto body = messaging::envelope_codec_t{}.decode_body (parts);
        assert (body && body.value ().data () == address);
        retained.emplace (std::move (body.value ()));
    }
    assert (retained->size () == 4096);
    assert (retained->to_bytes () == std::vector<std::uint8_t> (4096, 7));
}

void verify_application_owner_drain_transitions ()
{
    mesh::service_mailbox_t manual;
    manual.begin_application_receive_turn ();
    assert (manual.try_enqueue (mesh::service_mailbox_record_t::from_parts (
      "manual", mesh::service_mailbox_domain_t::application,
      zlink::framework::detail::backend::materialize_binding_parts ({{1}}))));
    manual.end_application_receive_turn ();
    auto manual_claim = manual.try_claim (mesh::service_mailbox_domain_t::application, 1, 4096);
    assert (manual_claim && manual_claim->records.size () == 1);
    assert (manual.release (*manual_claim));

    mesh::service_mailbox_t mailbox;
    std::vector<std::string> ready;
    int prepared = 0;
    int ready_copies = 0;
    struct ready_observer_t
    {
        std::vector<std::string> &ready;
        int &copies;
        ready_observer_t (std::vector<std::string> &ready, int &copies) :
            ready (ready), copies (copies)
        {
        }
        ready_observer_t (const ready_observer_t &other) :
            ready (other.ready), copies (other.copies)
        {
            ++copies;
        }
        ready_observer_t (ready_observer_t &&) = default;
        void operator() (const std::string &owner) const { ready.push_back (owner); }
    };
    mailbox.bind_application_dispatch ([&] (mesh::service_mailbox_record_t &) { ++prepared; },
                                       ready_observer_t{ready, ready_copies});
    const auto registered_copies = ready_copies;
    mailbox.begin_application_receive_turn ();
    mailbox.end_application_receive_turn ();
    assert (ready_copies == registered_copies); // An idle ingress turn has no callback work.
    const auto enqueue = [&] (std::string owner, std::uint8_t value) {
        return mailbox.try_enqueue (mesh::service_mailbox_record_t::from_parts (
          std::move (owner), mesh::service_mailbox_domain_t::application,
          zlink::framework::detail::backend::materialize_binding_parts ({{value}})));
    };
    mailbox.begin_application_receive_turn ();
    assert (enqueue ("a", 1));
    assert (ready.empty ());
    assert (enqueue ("a", 2));
    assert (enqueue ("b", 3));
    assert (ready.empty ());
    mailbox.end_application_receive_turn ();
    assert (prepared == 3);
    assert ((ready == std::vector<std::string>{"a", "b"}));
    const auto delivered_copies = ready_copies;
    assert (mailbox.begin_application_drain ("a"));
    assert (!mailbox.begin_application_drain ("a"));
    assert (mailbox.begin_application_drain ("b"));
    auto first =
      mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application, "a", 1, 4096);
    auto other =
      mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application, "b", 1, 4096);
    assert (first && other && first->records[0]->parts ()[0].to_bytes ()[0] == 1);
    assert (mailbox.release (*first));
    assert (ready_copies == delivered_copies); // Terminal release keeps the existing drain.
    auto second =
      mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application, "a", 1, 4096);
    assert (second && second->records[0]->parts ()[0].to_bytes ()[0] == 2);
    assert (ready.size () == 2); // The same drain keeps its execution right.
    assert (enqueue ("a", 4));
    mailbox.end_application_drain ("a"); // A deferred handler retains its reservation.
    assert (ready.size () == 2);
    assert (!mailbox.begin_application_drain ("a"));
    assert (ready_copies == delivered_copies); // Retained work has no ready owner to notify.
    mailbox.begin_application_receive_turn ();
    assert (mailbox.release (*second));
    assert (ready.size () == 2);
    mailbox.end_application_receive_turn ();
    assert (ready.size () == 3 && ready.back () == "a");
    assert (!mailbox.release (*second));
    assert (mailbox.begin_application_drain ("a"));
    auto third =
      mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application, "a", 1, 4096);
    assert (third && third->records[0]->parts ()[0].to_bytes ()[0] == 4);
    assert (enqueue ("a", 5));
    assert (mailbox.release (*third));
    mailbox.end_application_drain ("a"); // A time-budget yield wakes this owner once.
    assert (ready.size () == 4 && ready.back () == "a");
    const auto resumed_copies = ready_copies;
    assert (mailbox.release (*other));
    mailbox.end_application_drain ("b");
    assert (mailbox.begin_application_drain ("a"));
    auto final =
      mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application, "a", 1, 4096);
    assert (final && final->records[0]->parts ()[0].to_bytes ()[0] == 5);
    assert (mailbox.release (*final));
    mailbox.end_application_drain ("a");
    assert (mailbox.pending_messages (mesh::service_mailbox_domain_t::application) == 0);
    assert (ready.size () == 4);
    assert (ready_copies == resumed_copies); // Empty owner completion does not copy callbacks.
}

void verify_independent_mailbox_domains_and_claim_fence ()
{
    constexpr auto fixed_work_cost = runtime::dispatch_limits::fixed_work_byte_cost;
    mesh::service_mailbox_t owner_scoped;
    assert (owner_scoped.try_enqueue (mesh::service_mailbox_record_t::from_parts (
      "owner-a", mesh::service_mailbox_domain_t::application,
      zlink::framework::detail::backend::materialize_binding_parts ({{1}}))));
    auto owner_a_claim = owner_scoped.try_claim_owner (mesh::service_mailbox_domain_t::application,
                                                       "owner-a", 1, 4096);
    assert (owner_a_claim && owner_a_claim->records.size () == 1);
    assert (owner_scoped.try_enqueue (mesh::service_mailbox_record_t::from_parts (
      "owner-a", mesh::service_mailbox_domain_t::application,
      zlink::framework::detail::backend::materialize_binding_parts ({{2}}))));
    assert (owner_scoped.try_enqueue (mesh::service_mailbox_record_t::from_parts (
      "owner-b", mesh::service_mailbox_domain_t::application,
      zlink::framework::detail::backend::materialize_binding_parts ({{3}}))));
    assert (owner_scoped.release (*owner_a_claim));
    assert (owner_scoped.try_enqueue (mesh::service_mailbox_record_t::from_parts (
      "owner-a", mesh::service_mailbox_domain_t::application,
      zlink::framework::detail::backend::materialize_binding_parts ({{6}}))));
    auto owner_a_remaining = owner_scoped.try_claim_owner (
      mesh::service_mailbox_domain_t::application, "owner-a", 2, 4096);
    assert (owner_a_remaining && owner_a_remaining->records.size () == 2);
    assert (owner_scoped.release (*owner_a_remaining));

    mesh::service_mailbox_t mailbox;
    assert (mailbox.try_enqueue (mesh::service_mailbox_record_t::from_parts (
      "owner-a", mesh::service_mailbox_domain_t::application,
      zlink::framework::detail::backend::materialize_binding_parts ({{1, 2, 3}}))));
    assert (mailbox.try_enqueue (mesh::service_mailbox_record_t::from_parts (
      "owner-a", mesh::service_mailbox_domain_t::application,
      zlink::framework::detail::backend::materialize_binding_parts ({{4, 5}}))));
    assert (mailbox.try_enqueue (mesh::service_mailbox_record_t::from_parts (
      "peer-a", mesh::service_mailbox_domain_t::infrastructure,
      zlink::framework::detail::backend::materialize_binding_parts ({{9}}))));
    assert (mailbox.try_enqueue (mesh::service_mailbox_record_t::from_parts (
      "owner-b", mesh::service_mailbox_domain_t::application,
      zlink::framework::detail::backend::materialize_binding_parts ({{7}}))));

    auto owner_b =
      mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application, "owner-b", 1, 64);
    assert (owner_b && owner_b->records.size () == 1);
    assert (mailbox.release (*owner_b));

    auto application = mailbox.try_claim (mesh::service_mailbox_domain_t::application, 1, 64);
    assert (application && application->records.size () == 1);
    assert (!mailbox.try_claim (mesh::service_mailbox_domain_t::application, 1, 64));

    auto infrastructure = mailbox.try_claim (mesh::service_mailbox_domain_t::infrastructure, 1, 32);
    assert (infrastructure && infrastructure->records.size () == 1);
    assert (mailbox.release (*infrastructure));
    assert (!mailbox.release (*infrastructure));

    assert (mailbox.release (*application));
    auto remaining = mailbox.try_claim (mesh::service_mailbox_domain_t::application, 2, 64);
    assert (remaining && remaining->records.size () == 1);
    assert (mailbox.pending_messages (mesh::service_mailbox_domain_t::application) == 0);
    assert (mailbox.pending_bytes (mesh::service_mailbox_domain_t::application) == 0);
    assert (mailbox.release (*remaining));

    mesh::service_mailbox_t accounting;
    assert (accounting.try_enqueue (mesh::service_mailbox_record_t::from_parts (
      "accounted", mesh::service_mailbox_domain_t::application,
      zlink::framework::detail::backend::materialize_binding_parts ({{1, 2, 3}}))));
    assert (accounting.pending_bytes (mesh::service_mailbox_domain_t::application)
            == fixed_work_cost + 3);
    assert (accounting.try_enqueue (mesh::service_mailbox_record_t::from_parts (
      "accounted", mesh::service_mailbox_domain_t::application,
      zlink::framework::detail::backend::materialize_binding_parts ({{4}}))));
    // The claim byte budget bounds one receive turn; it is not a queue limit.
    // A batch always takes its first record and then stops at the budget.
    auto accounted = accounting.try_claim (mesh::service_mailbox_domain_t::application, 2, 1);
    assert (accounted && accounted->records.size () == 1);
    assert (accounting.release (*accounted));
    auto accounted_rest =
      accounting.try_claim (mesh::service_mailbox_domain_t::application, 2, 4096);
    assert (accounted_rest && accounted_rest->records.size () == 1);
    assert (accounting.release (*accounted_rest));

    assert (mailbox.try_enqueue (mesh::service_mailbox_record_t::from_parts (
      "owner-large", mesh::service_mailbox_domain_t::application,
      zlink::framework::detail::backend::materialize_binding_parts (
        {std::vector<std::uint8_t> (20, 7)}))));
    auto oversized = mailbox.try_claim (mesh::service_mailbox_domain_t::application, 1, 1);
    assert (oversized && oversized->records.size () == 1);
    assert (mailbox.release (*oversized));

    mesh::service_mailbox_t saturated;
    assert (saturated.try_enqueue (mesh::service_mailbox_record_t::from_parts (
      "first", mesh::service_mailbox_domain_t::application,
      zlink::framework::detail::backend::materialize_binding_parts ({{1}}))));
    auto retained = mesh::service_mailbox_record_t::from_parts (
      "first", mesh::service_mailbox_domain_t::application,
      zlink::framework::detail::backend::materialize_binding_parts ({{2, 3, 4}}));
    assert (saturated.try_enqueue (std::move (retained)));
    auto first = saturated.try_claim (mesh::service_mailbox_domain_t::application, 2, 64);
    assert (first && first->records.size () == 1);
    assert (saturated.release (*first));
    auto second = saturated.try_claim (mesh::service_mailbox_domain_t::application, 2, 4096);
    assert (second && second->records.size () == 1);
    assert (second->records.front ()->owner == "first");
    assert (
      (zlink::framework::detail::backend::copy_binding_parts (second->records.front ()->parts ())
       == std::vector<std::vector<std::uint8_t>>{{2, 3, 4}}));
    assert (saturated.release (*second));
}

void verify_completion_receipt_without_application_receive ()
{
    const auto context = std::make_shared<zlink::context_t> ();
    mesh::raw_mesh_node_owner_t requester (
      zlink::framework::test::runtime_failure_options (
        zlink::framework::runtime::mesh::raw_mesh_node_options_t{descriptor ("receipt-requester")}),
      context);
    mesh::raw_mesh_node_owner_t replier (
      zlink::framework::test::runtime_failure_options (
        zlink::framework::runtime::mesh::raw_mesh_node_options_t{descriptor ("receipt-replier")}),
      context);
    requester.start ();
    replier.start ();
    const auto source = requester.topology ().local_descriptor ();
    const auto target = replier.topology ().local_descriptor ();
    replier.expect_peer (source);
    assert (requester.connect_peer (replier.endpoint (), target));
    const auto admission_deadline = std::chrono::steady_clock::now () + 5s;
    while ((!peer_ready (requester, target.node_routing_id)
            || !peer_ready (replier, source.node_routing_id))
           && std::chrono::steady_clock::now () < admission_deadline) {
        (void) requester.observe_routes ();
        (void) replier.observe_routes ();
        (void) await_task (requester.pump_one (std::chrono::steady_clock::now ()));
        (void) await_task (replier.pump_one (std::chrono::steady_clock::now ()));
        std::this_thread::yield ();
    }
    assert (peer_ready (requester, target.node_routing_id));
    assert (peer_ready (replier, source.node_routing_id));
    std::promise<foundation::operation_terminal_t> completion;
    auto terminal = completion.get_future ();
    const auto submitted_at = std::chrono::steady_clock::now ();
    assert (await_task (requester.request_to_node (
      target.node_routing_id, {"Question", "application/json", bytes ("request")}, 2s,
      [&completion] (auto result, auto) { completion.set_value (result); })));
    const auto reply_deadline = submitted_at + 2s;
    while (terminal.wait_for (0ms) != std::future_status::ready
           && std::chrono::steady_clock::now () < reply_deadline) {
        (void) await_task (replier.pump_one (std::chrono::steady_clock::now ()));
        auto claim = zlink::framework::test::claim_mailbox (
          replier.mailbox (), mesh::service_mailbox_domain_t::application, 1, 4096);
        if (claim) {
            assert (replier.reply ((*claim->records.front ()),
                                   {"Answer", "application/json", bytes ("reply")}));
            assert (replier.mailbox ().release (*claim));
        }
        // No Application Job Queue permit: receive on this lane stays blocked.
        assert (await_task (requester.pump_one (std::chrono::steady_clock::now (), false))
                == mesh::raw_mesh_pump_result_t::no_data);
        std::this_thread::yield ();
    }
    assert (terminal.wait_for (0ms) == std::future_status::ready);
    assert (terminal.get () == foundation::operation_terminal_t::completed);
    const auto received_by = std::chrono::steady_clock::now ();
    assert (requester.liveness ().tick (submitted_at + 15s).timed_out_nodes.empty ());
    assert (requester.liveness ().tick (received_by + 15s).timed_out_nodes
            == std::vector<std::vector<std::uint8_t>>{target.node_routing_id});
    requester.close ();
    replier.close ();
}
}
