/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/telemetry_observation.hpp"
#include "mailbox_claim_fixture.hpp"
#include "../support/m6b_cases.hpp"
#include "../support/m6b_host_access.hpp"
#include "../support/m6b_task.hpp"
#include "../support/m6b_mesh_fixture.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/allocation_counter.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"
#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace mesh = zlink::framework::runtime::mesh;
namespace protocol = zlink::framework::runtime::protocol;
namespace stateful = zlink::framework::runtime::stateful;
namespace host = zlink::framework::runtime::host;
using namespace std::chrono_literals;

namespace zlink::framework::test::m6b
{
void verify_host_send_forwarding_allocations ()
{
    using zlink::framework::tests::count_allocations;
    auto source = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("forwarding-host"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    const auto target = zlink::routing_id_t::from ("missing-forwarding-host");
    for (const bool move_parts : {false, true}) {
        std::vector<zlink::message_t> parts{zlink::message_t::from (std::string ("payload"))};
        auto baseline_parts = parts;
        std::vector<std::uint8_t> target_bytes;
        std::optional<protocol::application_payload_t> payload;
        const auto arguments = count_allocations ([&] {
            target_bytes = target.to_bytes ();
            if (move_parts)
                payload.emplace (
                  protocol::application_payload_t::from_parts (std::move (baseline_parts)));
            else
                payload.emplace (protocol::application_payload_t::from_parts (baseline_parts));
        });
        std::optional<zlink::framework::task_t<zlink::submit_result_t>> raw;
        const auto terminal = count_allocations ([&] {
            raw.emplace (
              source->transport ().send_to_node_result (target_bytes, std::move (*payload)));
        });
        std::optional<zlink::framework::task_t<zlink::submit_result_t>> submission;
        const auto observed = count_allocations ([&] {
            if (move_parts)
                submission.emplace (source->send_to_node (target, std::move (parts)));
            else
                submission.emplace (source->send_to_node (target, parts));
        });
        assert (submission->await_ready ());
        assert (submission->result ().value () == zlink::submit_result_t::not_connected);
        std::fprintf (
          stderr,
          "host send allocations move=%d observed=%zu allowed=%zu arguments=%zu terminal=%zu\n",
          move_parts, observed, arguments + terminal, arguments, terminal);
        assert (observed <= arguments + terminal);
        std::vector<zlink::message_t> empty;
        auto failed = move_parts ? source->send_to_node (target, std::move (empty))
                                 : source->send_to_node (target, empty);
        assert (failed.await_ready ());
        bool preserved = false;
        try {
            (void) failed.result ().value ();
        }
        catch (const std::invalid_argument &error) {
            preserved =
              std::string_view (error.what ()) == "framework multipart requires at least one part";
        }
        assert (preserved);
    }
}
void verify_public_host_dispatches_one_application_record_per_turn ()
{
    auto source_context = std::make_shared<zlink::context_t> ();
    auto target_context = std::make_shared<zlink::context_t> ();
    // A fixed Core HWM makes deferred admission observable before the target is pumped.
    source_context->options ().auto_hwm_enabled (false);
    target_context->options ().auto_hwm_enabled (false);
    auto source = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh =
        mesh::raw_mesh_node_options_t{
          .descriptor = descriptor ("hwm-dispatch-source"),
          .runtime_failures =
            std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()},
      .core_context = source_context});
    auto target = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh =
        mesh::raw_mesh_node_options_t{
          .descriptor = descriptor ("hwm-dispatch-target"),
          .runtime_failures =
            std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()},
      .core_context = target_context});
    source->start ();
    target->start ();
    const auto target_status = target->status ();
    const auto source_status = source->status ();
    assert (target->connect_peer (source_status.local_endpoint (), source_status.routing_id ()));

    const auto noop_dispatch = [] (const host::ready_record_t &, const host::receive_record_t &,
                                   std::vector<zlink::message_t>, auto) {};
    const auto connect_deadline = mesh::service_liveness_registry_t::clock_t::now () + 5s;
    const auto peers_ready = [&] {
        const auto source_peer =
          source->transport ().topology ().peer (target_status.routing_id ().to_bytes ());
        const auto target_peer =
          target->transport ().topology ().peer (source_status.routing_id ().to_bytes ());
        return source_peer && source_peer->ready && target_peer && target_peer->ready;
    };
    while (!peers_ready ()
           && mesh::service_liveness_registry_t::clock_t::now () < connect_deadline) {
        await_task (source->dispatch_ready (noop_dispatch));
        await_task (target->dispatch_ready (noop_dispatch));
        std::this_thread::sleep_for (1ms);
    }
    assert (source->transport ().topology ().peer (target_status.routing_id ().to_bytes ()));
    assert (target->transport ().topology ().peer (source_status.routing_id ().to_bytes ()));
    assert (peers_ready ());

    const std::vector<zlink::message_t> first{zlink::message_t::from (std::string ("first"))};
    const std::vector<zlink::message_t> second{zlink::message_t::from (std::string ("second"))};
    assert (source->send_to_node (target_status.routing_id (), first).result ().value ()
            == zlink::submit_result_t::ok);
    assert (source->send_to_node (target_status.routing_id (), second).result ().value ()
            == zlink::submit_result_t::ok);

    const auto receive_deadline = mesh::service_liveness_registry_t::clock_t::now () + 5s;
    while (
      target->transport ().mailbox ().pending_messages (mesh::service_mailbox_domain_t::application)
        < 2
      && mesh::service_liveness_registry_t::clock_t::now () < receive_deadline) {
        const auto pumped = target->transport ()
                              .pump_one (mesh::service_liveness_registry_t::clock_t::now ())
                              .result ()
                              .value ();
        assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
        if (pumped == mesh::raw_mesh_pump_result_t::no_data)
            std::this_thread::sleep_for (1ms);
    }
    assert (
      target->transport ().mailbox ().pending_messages (mesh::service_mailbox_domain_t::application)
      == 2);

    std::size_t dispatched = 0;
    const auto dispatch = [&] (const host::ready_record_t &owner,
                               const host::receive_record_t &record,
                               std::vector<zlink::message_t> parts, auto) {
        assert (owner.domain == host::ready_domain_t::application);
        assert (owner.owner_kind == host::owner_kind_t::node);
        assert (record.kind == host::record_kind_t::node_send);
        assert (parts.size () == 1);
        ++dispatched;
    };
    await_task (target->dispatch_ready (dispatch));
    assert (dispatched == 1);
    assert (
      target->transport ().mailbox ().pending_messages (mesh::service_mailbox_domain_t::application)
      == 1);

    await_task (target->dispatch_ready (dispatch));
    assert (dispatched == 2);
    assert (
      target->transport ().mailbox ().pending_messages (mesh::service_mailbox_domain_t::application)
      == 0);
    for (const bool move_parts : {false, true}) {
        std::vector<zlink::framework::task_t<zlink::submit_result_t>> sends;
        for (std::size_t index = 0;
             index < 128 && source->transport ().pending_operation_count () == 0; ++index) {
            std::vector<zlink::message_t> parts{
              zlink::message_t::from (std::string (512u * 1024u, 'Z'))};
            if (move_parts)
                sends.push_back (
                  source->send_to_node (target_status.routing_id (), std::move (parts)));
            else
                sends.push_back (source->send_to_node (target_status.routing_id (), parts));
        }
        // Caller multipart objects have all been destroyed before admission resumes.
        assert (source->transport ().pending_operation_count () != 0);
        std::size_t received = 0;
        const auto retain_dispatch = [&] (const host::ready_record_t &,
                                          const host::receive_record_t &,
                                          std::vector<zlink::message_t> parts, auto) {
            assert (parts.size () == 1);
            const auto body = protocol::message_bytes (parts.front ());
            assert (body.size () == 512u * 1024u);
            assert (
              std::all_of (body.begin (), body.end (), [] (auto value) { return value == 'Z'; }));
            ++received;
        };
        while (received < sends.size () && std::chrono::steady_clock::now () < receive_deadline) {
            await_task (source->dispatch_ready (noop_dispatch));
            await_task (target->dispatch_ready (retain_dispatch));
            std::this_thread::yield ();
        }
        assert (received == sends.size ());
        for (auto &send : sends)
            assert (send.result ().value () == zlink::submit_result_t::ok);
        assert (source->transport ().pending_operation_count () == 0);
    }
}

void verify_public_host_dispatches_one_node_record_per_claim ()
{
    auto target = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("owner-batch-target"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    target->start ();
    auto &mailbox = target->transport ().mailbox ();
    const auto enqueue = [&mailbox] (std::string value) {
        auto application = std::make_shared<host::local_application_dispatch_t> ();
        application->owner.owner_kind = host::owner_kind_t::node;
        application->owner.domain = host::ready_domain_t::application;
        application->record.kind = host::record_kind_t::node_send;
        application->record.domain = host::ready_domain_t::application;
        application->parts.push_back (zlink::message_t::from (value));
        mesh::service_mailbox_record_t record;
        record.owner = mesh::service_mailbox_t::application_owner (host::owner_kind_t::node);
        record.domain = mesh::service_mailbox_domain_t::application;
        record.application.emplace (std::move (*application));
        return mailbox.try_enqueue (std::move (record));
    };
    assert (enqueue ("first"));
    assert (enqueue ("second"));
    assert (enqueue ("third"));

    const auto owner = mesh::service_mailbox_t::application_owner (host::owner_kind_t::node);
    assert (mailbox.begin_application_drain (owner));
    std::vector<std::string> dispatched;
    std::size_t started = 0;
    std::size_t rejected = 0;
    for (std::size_t index = 0; index != 3; ++index) {
        assert (zlink::framework::test::application_owner_result (
                  target->dispatch_application_owner (
                    owner,
                    [&] (const host::ready_record_t &, const host::receive_record_t &record,
                         std::vector<zlink::message_t> parts, auto) {
                        assert (record.retain_mailbox_reservation);
                        assert (record.release_mailbox_reservation);
                        record.retain_mailbox_reservation ();
                        assert (parts.size () == 1);
                        dispatched.push_back (parts.front ().to_string ());
                        record.release_mailbox_reservation ();
                    },
                    [&] { ++started; }, [&] { ++rejected; }))
                  .value ());
        assert (dispatched.size () == index + 1);
        assert (mailbox.pending_messages (mesh::service_mailbox_domain_t::application)
                == 2 - index);
    }
    mailbox.end_application_drain (owner);

    assert ((dispatched == std::vector<std::string>{"first", "second", "third"}));
    assert (started == dispatched.size ());
    assert (rejected == 0);
    assert (mailbox.pending_messages (mesh::service_mailbox_domain_t::application) == 0);
    target->close ();
}

void verify_public_host_batches_with_finite_permits ()
{
    auto source = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("finite-permit-liveness-source"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    auto target = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("finite-permit-liveness-target"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    source->start ();
    target->start ();
    const auto target_status = target->status ();
    const auto source_status = source->status ();
    assert (target->connect_peer (source_status.local_endpoint (), source_status.routing_id ()));

    const auto noop_dispatch = [] (const host::ready_record_t &, const host::receive_record_t &,
                                   std::vector<zlink::message_t>, auto) {};
    const auto connect_deadline = mesh::service_liveness_registry_t::clock_t::now () + 5s;
    while (
      (!ready_peer (source->transport ().topology (), target_status.routing_id ().to_bytes ())
       || !ready_peer (target->transport ().topology (), source_status.routing_id ().to_bytes ()))
      && mesh::service_liveness_registry_t::clock_t::now () < connect_deadline) {
        await_task (source->dispatch_ready (noop_dispatch));
        await_task (target->dispatch_ready (noop_dispatch));
        std::this_thread::sleep_for (1ms);
    }
    assert (source->transport ().topology ().peer (target_status.routing_id ().to_bytes ()));
    assert (target->transport ().topology ().peer (source_status.routing_id ().to_bytes ()));

    const std::array values{"first", "second", "third",   "fourth",
                            "fifth", "sixth",  "seventh", "eighth"};
    for (const auto value : values) {
        assert (source
                  ->send_to_node (target_status.routing_id (),
                                  {zlink::message_t::from (std::string (value))})
                  .result ()
                  .value ()
                == zlink::submit_result_t::ok);
    }
    const auto receive_deadline = mesh::service_liveness_registry_t::clock_t::now () + 5s;
    while (
      target->transport ().mailbox ().pending_messages (mesh::service_mailbox_domain_t::application)
        < values.size ()
      && mesh::service_liveness_registry_t::clock_t::now () < receive_deadline) {
        const auto pumped = target->transport ()
                              .pump_one (mesh::service_liveness_registry_t::clock_t::now ())
                              .result ()
                              .value ();
        assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
        if (pumped == mesh::raw_mesh_pump_result_t::no_data)
            std::this_thread::sleep_for (1ms);
    }
    assert (
      target->transport ().mailbox ().pending_messages (mesh::service_mailbox_domain_t::application)
      == values.size ());

    std::vector<std::string> delivered;
    std::size_t next_permit_calls = 0;
    bool permit_denied = false;
    const auto dispatch = [&] (const host::ready_record_t &owner,
                               const host::receive_record_t &record,
                               std::vector<zlink::message_t> parts, auto) {
        if (owner.domain != host::ready_domain_t::application)
            return;
        assert (!permit_denied);
        assert (owner.owner_kind == host::owner_kind_t::node);
        assert (record.kind == host::record_kind_t::node_send);
        assert (parts.size () == 1);
        delivered.push_back (parts.front ().to_string ());
    };
    constexpr std::size_t permit_limit = 6;
    std::size_t permits_remaining = permit_limit;
    std::size_t turns = 0;
    while (permits_remaining != 0 && turns != permit_limit + 2
           && delivered.size () < values.size ()) {
        // Model the host service's initial permit for this bounded turn.
        --permits_remaining;
        permit_denied = false;
        std::size_t handoffs = 0;
        const auto next_application_receive = [&] {
            ++next_permit_calls;
            if (handoffs != 0 || permits_remaining == 0) {
                permit_denied = true;
                return false;
            }
            --permits_remaining;
            ++handoffs;
            return true;
        };
        const auto delivered_before = delivered.size ();
        await_task (target->dispatch_ready (dispatch, true, next_application_receive));
        ++turns;
        const auto delivered_this_turn = delivered.size () - delivered_before;
        assert (delivered_this_turn <= 1 + handoffs);
    }
    assert (permits_remaining == 0);
    assert (next_permit_calls <= permit_limit);
    assert (delivered.size () >= 2);
    for (std::size_t index = 0; index != delivered.size (); ++index)
        assert (delivered[index] == values[index]);

    // No initial permit means ordinary application receive is disabled for
    // the turn, and the remaining FIFO stays untouched.
    const auto delivered_before_no_permit = delivered.size ();
    await_task (target->dispatch_ready (dispatch, false));
    assert (delivered.size () == delivered_before_no_permit);
    assert (
      target->transport ().mailbox ().pending_messages (mesh::service_mailbox_domain_t::application)
      == values.size () - delivered.size ());

    source->close ();
    target->close ();
}

void verify_public_host_fifo_drains_before_liveness_probe ()
{
    using clock_t = mesh::service_liveness_registry_t::clock_t;
    const auto wait_for_input = [] (mesh::raw_mesh_node_owner_t &transport,
                                    clock_t::time_point deadline) {
        (void) transport.wait_for_activity (std::max (
          0ms, std::chrono::ceil<std::chrono::milliseconds> (deadline - clock_t::now ())));
    };
    auto source = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("fifo-liveness-source"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    auto target = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("fifo-liveness-target"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    source->start ();
    target->start ();
    const auto target_status = target->status ();
    const auto source_status = source->status ();
    assert (target->connect_peer (source_status.local_endpoint (), source_status.routing_id ()));

    const auto noop_dispatch = [] (const host::ready_record_t &, const host::receive_record_t &,
                                   std::vector<zlink::message_t>, auto) {};
    const auto connect_deadline = mesh::service_liveness_registry_t::clock_t::now () + 5s;
    const auto admit = [&] (host::public_host_runtime_t &runtime, const zlink::routing_id_t &peer) {
        while (!ready_peer (runtime.transport ().topology (), peer.to_bytes ())
               && clock_t::now () < connect_deadline) {
            await_task (runtime.dispatch_ready (noop_dispatch));
            if (!ready_peer (runtime.transport ().topology (), peer.to_bytes ()))
                wait_for_input (runtime.transport (), connect_deadline);
        }
        return ready_peer (runtime.transport ().topology (), peer.to_bytes ());
    };
    // Each endpoint must keep servicing its handshake while the other waits
    // for input. A wait on one endpoint cannot drive the other's receive turn.
    auto source_admitted =
      std::async (std::launch::async, [&] { return admit (*source, target_status.routing_id ()); });
    assert (admit (*target, source_status.routing_id ()));
    assert (source_admitted.get ());

    const std::array values{"fifo-1", "fifo-2", "fifo-3", "fifo-4"};
    for (const auto value : values) {
        assert (source
                  ->send_to_node (target_status.routing_id (),
                                  {zlink::message_t::from (std::string (value))})
                  .result ()
                  .value ()
                == zlink::submit_result_t::ok);
    }

    // Submit the probe after the application records.  The target must
    // consume the transport FIFO's application prefix before seeing it.
    // Advance the source's logical time to its exact probe deadline. From
    // here on only explicit timestamps drive its liveness state.
    const auto probe_time = source->transport ().liveness ().next_activity ().value ();
    const auto tick = await_task (source->transport ().tick_liveness (probe_time));
    assert (tick.probes.size () == 1);

    std::vector<std::string> consumed;
    mesh::raw_mesh_pump_result_t probe_pump = mesh::raw_mesh_pump_result_t::no_data;
    const auto io_deadline = mesh::service_liveness_registry_t::clock_t::now () + 5s;
    while (consumed.size () < values.size ()
           && mesh::service_liveness_registry_t::clock_t::now () < io_deadline) {
        const auto before = consumed.size ();
        await_task (target->dispatch_ready (
          [&] (const host::ready_record_t &owner, const host::receive_record_t &record,
               std::vector<zlink::message_t> parts, auto) {
              assert (owner.domain == host::ready_domain_t::application);
              assert (record.kind == host::record_kind_t::node_send);
              assert (parts.size () == 1);
              consumed.push_back (parts.front ().to_string ());
          },
          true,
          [&] {
              // One receive permit advances one FIFO record. Admission may
              // precede the prefix, but no control may overtake its remainder.
              if (before != 0)
                  assert (consumed.size () == before + 1);
              return false;
          }));
        assert (consumed.size () <= before + 1);
        if (consumed.size () == before)
            wait_for_input (target->transport (), io_deadline);
    }
    assert (consumed.size () == values.size ());
    for (std::size_t index = 0; index != values.size (); ++index)
        assert (consumed[index] == values[index]);

    // Queue a different infrastructure record ahead of the ACK even when
    // admission happened to finish early. The source must distinguish it.
    const auto target_probe_time = target->transport ().liveness ().next_activity ().value ();
    const auto target_tick = await_task (target->transport ().tick_liveness (target_probe_time));
    assert (target_tick.probes.size () == 1);

    while (probe_pump != mesh::raw_mesh_pump_result_t::infrastructure
           && mesh::service_liveness_registry_t::clock_t::now () < io_deadline) {
        probe_pump = await_task (target->transport ().pump_one (target_probe_time));
        assert (probe_pump != mesh::raw_mesh_pump_result_t::protocol_error);
        if (probe_pump == mesh::raw_mesh_pump_result_t::no_data)
            wait_for_input (target->transport (), io_deadline);
    }
    assert (probe_pump == mesh::raw_mesh_pump_result_t::infrastructure);

    // The ACK and this marker share the target-to-source FIFO. A generic
    // infrastructure result could instead be a late admission record; only
    // receiving this marker proves that the preceding ACK was processed.
    assert (target
              ->send_to_node (source_status.routing_id (),
                              {zlink::message_t::from (std::string ("after-probe-ack"))})
              .result ()
              .value ()
            == zlink::submit_result_t::ok);
    // Receive after the source probe timestamp, including when admission
    // already refreshed the deadline with a later real-clock receipt.
    const auto receipt_time = std::max (clock_t::now (), probe_time + 1ms);
    mesh::raw_mesh_pump_result_t ack_barrier = mesh::raw_mesh_pump_result_t::no_data;
    while (ack_barrier != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now () < io_deadline) {
        ack_barrier = await_task (source->transport ().pump_one (receipt_time));
        assert (ack_barrier != mesh::raw_mesh_pump_result_t::protocol_error);
        if (ack_barrier == mesh::raw_mesh_pump_result_t::no_data)
            wait_for_input (source->transport (), io_deadline);
    }
    assert (ack_barrier == mesh::raw_mesh_pump_result_t::application);
    auto marker = zlink::framework::test::claim_mailbox (
      source->transport ().mailbox (), mesh::service_mailbox_domain_t::application, 1, 1024);
    assert (marker && marker->records.size () == 1);
    const auto &record = (*marker->records.front ());
    assert (record.source_routing_id == target_status.routing_id ().to_bytes ());
    assert (record.parts ().size () == 2);
    const auto marker_parts = protocol::decode_application_parts (
      protocol::decode_application_payload (protocol::message_bytes (record.parts ().back ())));
    assert (marker_parts.size () == 1);
    assert (marker_parts.front ().to_string () == "after-probe-ack");
    assert (source->transport ().mailbox ().release (*marker));

    // Every received record refreshes the peer deadline. Admission may
    // have received records after the first probe was due, so use the
    // connection's current deadline instead of deriving it from probe_time.
    const auto connection =
      source->transport ().liveness ().connection (target_status.routing_id ().to_bytes ());
    assert (connection);
    const auto peer_deadline = connection->deadline.load ();
    assert (peer_deadline >= receipt_time + 15s);
    const auto after_ack = await_task (source->transport ().tick_liveness (peer_deadline - 1ms));
    assert (after_ack.timed_out_nodes.empty ());
    assert (after_ack.probes.size () == 1);
    assert (after_ack.probes.front ().probe_id != tick.probes.front ().probe_id);
    assert (source->transport ().topology ().peer (target_status.routing_id ().to_bytes ()));
    assert (target->transport ().topology ().peer (source_status.routing_id ().to_bytes ()));

    const auto expired = await_task (source->transport ().tick_liveness (peer_deadline));
    assert (expired.timed_out_nodes.size () == 1);
    assert (expired.timed_out_nodes.front () == target_status.routing_id ().to_bytes ());
    assert (!source->transport ().topology ().peer (target_status.routing_id ().to_bytes ()));

    source->close ();
    target->close ();
}
void verify_local_application_enqueue_wakes_dispatch_wait ()
{
    auto local_descriptor = descriptor ("local-dispatch-wake");
    local_descriptor.channels = {{"local", 0}, {"other", 100}};
    auto host = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      mesh::raw_mesh_node_options_t{
        .descriptor = std::move (local_descriptor),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()},
      "entry",
      {"framework.spot"}});
    auto entry = host->entry_spot ();
    host->start ();
    const std::string entry_spot_id = entry.spot_id ();
    assert (zlink::framework::detail::is_framework_entry_spot_id (entry_spot_id));
    entry.set_subscription ("local", "wake");
    auto closing = host->get_or_create_spot ("closing-subscriber");
    closing.set_subscription ("local", "wake");
    auto wrong_topic = host->get_or_create_spot ("other-topic");
    wrong_topic.set_subscription ("local", "different");
    auto wrong_channel = host->get_or_create_spot ("other-channel");
    wrong_channel.set_subscription ("other", "wake");

    const auto deadline = std::chrono::steady_clock::now () + 5s;
    std::promise<void> waiting_started;
    auto waiting = waiting_started.get_future ();
    auto awakened = std::async (std::launch::async, [&] {
        waiting_started.set_value ();
        return host->wait_for_dispatch_activity (5s, true, deadline);
    });
    waiting.wait ();
    await_task (
      entry.publish ("local", "wake", {zlink::message_t::from (std::string ("payload"))}));
    const auto wake_budget =
      std::min (500ms, std::max (0ms, std::chrono::duration_cast<std::chrono::milliseconds> (
                                        deadline - std::chrono::steady_clock::now ())));
    assert (awakened.wait_for (wake_budget) == std::future_status::ready);
    assert (awakened.get ());

    std::vector<std::string> delivered;
    const auto dispatch = [&] (const host::ready_record_t &owner,
                               const host::receive_record_t &record,
                               std::vector<zlink::message_t> parts, auto permit) {
        assert (owner.owner_kind == host::owner_kind_t::spot);
        assert (record.kind == host::record_kind_t::spot_multicast);
        assert (record.channel_name == "local");
        assert (record.topic == "wake");
        assert (parts.size () == 1 && parts.front ().to_string () == "payload");
        zlink::framework::runtime::handler_entry_t before_application_handler (
          record.before_application_handler, std::move (permit));
        before_application_handler ();
        delivered.push_back (owner.spot_id);
    };
    while (await_task (host->dispatch_ready (dispatch)) != 0) {
    }
    std::sort (delivered.begin (), delivered.end ());
    std::vector<std::string> expected{"closing-subscriber", entry_spot_id};
    std::sort (expected.begin (), expected.end ());
    assert (delivered == expected);

    delivered.clear ();
    entry.unset_subscription ("local", "wake");
    await_task (
      entry.publish ("local", "wake", {zlink::message_t::from (std::string ("payload"))}));
    while (await_task (host->dispatch_ready (dispatch)) != 0) {
    }
    assert ((delivered == std::vector<std::string>{"closing-subscriber"}));

    delivered.clear ();
    assert (closing.close ());
    await_task (
      entry.publish ("local", "wake", {zlink::message_t::from (std::string ("payload"))}));
    while (await_task (host->dispatch_ready (dispatch)) != 0) {
    }
    assert (delivered.empty ());

    host->close ();

    auto application_jobs = std::make_shared<zlink::framework::runtime::application_job_queue_t> (
      zlink::framework::runtime::application_job_queue_configuration_t{
        zlink::framework::application_job_queue_profile_t::balanced, std::uint32_t{1}, 1, 1});
    auto fenced_descriptor = descriptor ("local-multicast-incarnation");
    fenced_descriptor.channels = {{"local", 0}};
    auto fenced = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      mesh::raw_mesh_node_options_t{
        .descriptor = std::move (fenced_descriptor),
        .application_jobs = application_jobs,
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()},
      "entry",
      {"framework.spot"}});
    fenced->start ();
    auto publisher = fenced->entry_spot ();
    auto original = fenced->get_or_create_spot ("recreated-subscriber");
    original.set_subscription ("local", "wake");
    auto held = application_jobs->try_reserve_supply ();
    assert (held);
    auto pending =
      publisher.publish ("local", "wake", {zlink::message_t::from (std::string ("payload"))});
    while (application_jobs->snapshot ().capacity_waiters == 0
           && std::chrono::steady_clock::now () < deadline) {
        (void) fenced->wait_for_dispatch_activity (5s, true, deadline);
    }
    const auto waiting_for_capacity = application_jobs->snapshot ();
    if (waiting_for_capacity.capacity_waiters != 1)
        held.reset ();
    assert (waiting_for_capacity.capacity_waiters == 1);
    const bool pending_before_release = !pending.await_ready ();
    assert (original.close ());
    auto replacement = fenced->get_or_create_spot ("recreated-subscriber");
    replacement.set_subscription ("local", "wake");
    const bool different_incarnation =
      original.status ().lifecycle_generation () != replacement.status ().lifecycle_generation ();
    held.reset ();
    await_task (std::move (pending));
    assert (pending_before_release);
    assert (different_incarnation);
    while (await_task (fenced->dispatch_ready (dispatch)) != 0) {
    }
    assert (delivered.empty ());
    await_task (
      publisher.publish ("local", "wake", {zlink::message_t::from (std::string ("payload"))}));
    while (await_task (fenced->dispatch_ready (dispatch)) != 0) {
    }
    assert ((delivered == std::vector<std::string>{"recreated-subscriber"}));

    assert (replacement.close ());
    held = application_jobs->try_reserve_supply ();
    assert (held);
    await_task (
      publisher.publish ("local", "wake", {zlink::message_t::from (std::string ("payload"))}));
    const auto closed_target_capacity = application_jobs->snapshot ();
    assert (closed_target_capacity.reserved_supply_permits == 1);
    assert (closed_target_capacity.queued_application_jobs == 0);
    assert (closed_target_capacity.capacity_waiters == 0);
    held.reset ();
    while (await_task (fenced->dispatch_ready (dispatch)) != 0) {
    }
    assert ((delivered == std::vector<std::string>{"recreated-subscriber"}));
    fenced->close ();
}
void verify_forwarded_request_return_dispatches_without_source_operation ()
{
    using namespace zlink::framework;
    auto target = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("returned-request-target"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    target->configure_stateful_dispatch (
      [] (stateful::accepted_record_authority_query_t)
        -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          co_return std::nullopt;
      });
    auto errors =
      std::make_shared<test::telemetry_observations_t<message_dispatch_error_event_t>> ();
    dispatch_options_t options;
    detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
      options, [errors] (const message_dispatch_error_event_t &event) { errors->record (event); });
    host::public_host_runtime_test_access_t::set_dispatch (*target, std::move (options));
    target->start ();
    const auto local = target->transport ().topology ().local_descriptor ();
    const stateful::object_ref_t owner{
      stateful::object_kind_t::actor, "returned-request-actor", 7, 13, local.mesh_name,
      "returned-request-target"};
    const protocol::wire_operation_id_t operation{
      local.lifecycle_generation == 79 ? std::uint64_t{80} : std::uint64_t{79}, 90};
    assert (!target->transport ().operation_pending ({operation.high, operation.low}));
    protocol::actor_message_header_t header;
    header.target = {owner.key,
                     owner.object_generation,
                     local.node_routing_id,
                     local.lifecycle_generation,
                     owner.authority_owner_generation,
                     1};
    header.operation = operation;
    header.correlation = 99;
    header.message_follow_hop_count = 2;
    auto accepted = stateful::accepted_actor_record (
      header, {"previous-hop-owner", 1, local.node_routing_id, local.lifecycle_generation},
      protocol::application_payload_t::from_parts ({zlink::message_t::from ("{}")}), true);
    stateful::turn_record_t turn;
    turn.sequence = 1;
    turn.application_record = accepted;
    const auto frozen = protocol::summarize_frozen_application_record (accepted);
    std::size_t calls = 0;
    const auto dispatched = host::public_host_runtime_test_access_t::dispatch_saved (
      *target, {owner, std::move (turn), frozen, true, {}},
      [&] (const auto &ready, const auto &record, auto parts, auto) {
          ++calls;
          assert (ready.actor && ready.actor->actor_id ().value () == owner.key);
          assert (record.operation_id.high == operation.high);
          assert (record.operation_id.low == operation.low);
          assert (!parts.empty ());
      });
    // dispatch_saved has finished producing diagnostics; acknowledge while Host is registered.
    test::acknowledge_delivery (std::chrono::steady_clock::now () + 5s);
    assert (errors->snapshot ().empty ());
    assert (dispatched);
    assert (calls == 1);
    target->close ();
}
}
