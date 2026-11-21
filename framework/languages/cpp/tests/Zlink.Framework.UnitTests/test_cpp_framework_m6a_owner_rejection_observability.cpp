/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6a_runtime_tests.hpp"
#include "metric_test_reader.hpp"
#include "../support/m6a_mesh_fixture.hpp"

#include "mailbox_claim_fixture.hpp"
#include "../support/telemetry_observation.hpp"

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

void verify_raw_owner_node_send_and_liveness (zlink::framework::message_flow_log_mode_t mode,
                                              bool framework_multipart)
{
    namespace fw = zlink::framework;
    auto observations =
      std::make_shared<fw::test::telemetry_observations_t<std::map<std::string, std::string>>> ();
    fw::logging_builder_t logging;
    logging.use_provider ("owner-rejection-test", [observations] (const fw::log_record_t &record) {
        std::map<std::string, std::string> fields;
        for (const auto &field : record.fields)
            fields.emplace (field.key, field.value);
        observations->record (fields);
    });
    auto monitoring = zlink::framework::test::runtime_failure_monitoring ();
    monitoring->diagnostics_logger = logging.create_logger ("owner-rejection-test");
    fw::dispatch_options_t dispatch;
    dispatch.message_flow (mode).trace_sample_rate (0);
    fw::detail::dispatch_options_access_t::set_logger (
      dispatch, logging.create_logger ("owner-rejection-test"));
    auto first_context = std::make_shared<zlink::context_t> ();
    auto second_context = std::make_shared<zlink::context_t> ();
    first_context->options ().auto_hwm_enabled (false);
    second_context->options ().auto_hwm_enabled (false);
    mesh::raw_mesh_node_owner_t first (
      mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("raw-a"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()},
      first_context);
    metric_test::provider_t metric_provider;
    mesh::raw_mesh_node_owner_t second (
      mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("raw-b"),
        .dispatch = dispatch,
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (),
        .metric_provider = metric_provider.api ()},
      second_context);
    assert (first.topology ().local_descriptor ().state == mesh::service_node_state_t::preparing);
    assert (second.topology ().local_descriptor ().state == mesh::service_node_state_t::preparing);
    first.start ();
    second.start ();
    assert (first.started () && second.started ());
    assert (first.topology ().local_descriptor ().state == mesh::service_node_state_t::serving);
    assert (second.topology ().local_descriptor ().state == mesh::service_node_state_t::serving);

    auto first_descriptor = first.topology ().local_descriptor ();
    auto second_descriptor = second.topology ().local_descriptor ();
    // The higher RID does not dial, but it retains discovery data to
    // validate the inbound endpoint and security identity.
    second.expect_peer (first_descriptor);
    const auto now = mesh::service_liveness_registry_t::clock_t::now ();
    const auto deadline = now + 5s;
    assert (first.connect_peer (second.endpoint (), second_descriptor));
    while ((!peer_ready (first, second_descriptor.node_routing_id)
            || !peer_ready (second, first_descriptor.node_routing_id))
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto progress_now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) first.observe_routes ();
        (void) second.observe_routes ();
        (void) await_task (first.pump_one (progress_now));
        (void) await_task (second.pump_one (progress_now));
        std::this_thread::sleep_for (1ms);
    }
    assert (peer_ready (first, second_descriptor.node_routing_id));
    assert (peer_ready (second, first_descriptor.node_routing_id));

    // Each peer probes right after admission (05-transport-liveness.ko.md:95).
    // Finish that probe and ACK exchange here: until then the probe is due and
    // wait_for_activity returns at once, so the waits below would not observe
    // the second ROUTER's DATA admission they expect.
    const auto admission_probe_now = mesh::service_liveness_registry_t::clock_t::now ();
    const auto first_admission_tick = await_task (first.tick_liveness (admission_probe_now));
    const auto second_admission_tick = await_task (second.tick_liveness (admission_probe_now));
    assert (first_admission_tick.probes.size () == 1);
    assert (second_admission_tick.probes.size () == 1);
    const auto probe_outstanding = [] (mesh::raw_mesh_node_owner_t &node,
                                       const std::vector<std::uint8_t> &peer_routing_id) {
        const auto peer = node.liveness ().connection (peer_routing_id);
        return !peer || peer->outstanding_probe.has_value ();
    };
    while ((probe_outstanding (first, second_descriptor.node_routing_id)
            || probe_outstanding (second, first_descriptor.node_routing_id))
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto progress_now = mesh::service_liveness_registry_t::clock_t::now ();
        const auto first_pumped = await_task (first.pump_one (progress_now));
        const auto second_pumped = await_task (second.pump_one (progress_now));
        assert (first_pumped != mesh::raw_mesh_pump_result_t::protocol_error);
        assert (second_pumped != mesh::raw_mesh_pump_result_t::protocol_error);
        if (first_pumped == mesh::raw_mesh_pump_result_t::no_data
            && second_pumped == mesh::raw_mesh_pump_result_t::no_data)
            std::this_thread::sleep_for (1ms);
    }
    assert (!probe_outstanding (first, second_descriptor.node_routing_id));
    assert (!probe_outstanding (second, first_descriptor.node_routing_id));

    std::atomic_int local_operation_callbacks{0};
    const auto local_operation =
      first
        .register_local_operation_async (
          foundation::operation_registry_t::clock_t::time_point::max (),
          [&] (auto, auto) { local_operation_callbacks.fetch_add (1, std::memory_order_release); })
        .result ()
        .value ();
    assert (local_operation);

    bool submitted = false;
    while (!submitted && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        try {
            submitted = await_task (first.send_to_node (
              second_descriptor.node_routing_id, {"Probe", "application/json", bytes ("payload")}));
        }
        catch (...) {
        }
        if (!submitted) {
            std::this_thread::sleep_for (5ms);
        }
    }
    assert (submitted);

    // A missing host-wide Application Job Queue permit fences the ordinary
    // ROUTER before Core receive.  Poll readiness alone must not consume or
    // retain the application record in the Framework owner.
    // The fence is meaningful only once the record is at the ROUTER: wait on
    // the ingress poller until it reports POLLIN without receiving. A
    // management deadline can end a wait first, so the wait repeats within
    // the test deadline.
    const auto wait_for_router_input = [&deadline] (mesh::raw_mesh_node_owner_t &node) {
        const auto router_input_ready = [&node] {
            const auto readiness = node.begin_receive_turn ();
            return readiness
                   && (static_cast<short> (*readiness)
                       & static_cast<short> (zlink::poll_event_flag_t::pollin))
                        != 0;
        };
        while (!router_input_ready ()
               && mesh::service_liveness_registry_t::clock_t::now () < deadline)
            (void) node.wait_for_activity (
              std::max (0ms, std::chrono::ceil<std::chrono::milliseconds> (
                               deadline - mesh::service_liveness_registry_t::clock_t::now ())),
              true);
        assert (router_input_ready ());
    };
    wait_for_router_input (second);
    const auto without_shared_permit =
      await_task (second.pump_one (mesh::service_liveness_registry_t::clock_t::now (), false));
    assert (without_shared_permit == mesh::raw_mesh_pump_result_t::no_data);
    assert (second.last_pump_bytes () == 0);
    assert (second.mailbox ().pending_messages (mesh::service_mailbox_domain_t::application) == 0);

    mesh::raw_mesh_pump_result_t pumped = mesh::raw_mesh_pump_result_t::no_data;
    while (pumped != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        pumped = await_task (second.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
        if (pumped != mesh::raw_mesh_pump_result_t::application) {
            std::this_thread::sleep_for (2ms);
        }
    }
    assert (pumped == mesh::raw_mesh_pump_result_t::application);
    assert (second.mailbox ().pending_messages (mesh::service_mailbox_domain_t::application) == 1);

    bool retained_submitted = false;
    protocol::application_payload_t retained_payload{
      "Probe", "application/json", bytes ("retained"), "019fc5b9-9df3-786b-bb69-d55358f6d48b",
      fw::flow_origin_t::inbound};
    if (framework_multipart) {
        runtime::messaging::envelope_header_t header;
        header.kind = runtime::messaging::message_kind_t::command;
        header.message_name = "Probe";
        header.correlation_id = "owner-correlation";
        header.flow_id = retained_payload.flow_id;
        header.flow_origin = retained_payload.flow_origin;
        const auto parts = runtime::messaging::envelope_codec_t ().encode_raw_body_parts (
          header, zlink::message_t::from (bytes ("retained")));
        retained_payload = protocol::application_payload_t::from_parts (parts.items ());
    }
    while (!retained_submitted && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        retained_submitted =
          await_task (first.send_to_node (second_descriptor.node_routing_id, retained_payload));
        if (!retained_submitted)
            std::this_thread::sleep_for (1ms);
    }
    assert (retained_submitted);
    mesh::raw_mesh_pump_result_t retained_pump = mesh::raw_mesh_pump_result_t::no_data;
    while (retained_pump != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        retained_pump =
          await_task (second.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        assert (retained_pump != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    // The owner mailbox has no capacity rule, so the second one-way payload is
    // received like the first one. Nothing is dropped.
    assert (retained_pump == mesh::raw_mesh_pump_result_t::application);

    // No drop occurred, so a provider need not emit a drop series. Any samples
    // it does emit must stay zero across both collections.
    for (int scrape = 0; scrape < 2; ++scrape) {
        auto collected = metric_provider.collect_fields ();
        for (const auto &fields : collected)
            observations->record (fields);
    }
    fw::test::acknowledge_delivery (deadline);
    {
        const auto snapshot = observations->snapshot ();
        std::size_t flows = 0, errors = 0;
        for (const auto &fields : snapshot) {
            const auto get = [&] (const char *key) -> std::string {
                const auto found = fields.find (key);
                return found == fields.end () ? "" : found->second;
            };
            if (get ("name") == "zlink.mesh_node.messages.dropped") {
                assert (get ("mesh_name") == second_descriptor.mesh_name);
                assert (get ("message_kind") == "send");
                assert (get ("reason") == "backpressure");
                assert (get ("unit") == "{message}");
                assert (get ("instrument_kind") == "counter");
                assert (get ("temporality") == "current");
                assert (std::stod (get ("value")) == 0);
            } else if (get ("event_id") == "zlink.message_flow" && get ("phase") == "dropped") {
                ++flows;
            } else if (get ("event_id") == "zlink.dispatch_error" && get ("action") == "drop") {
                ++errors;
            }
        }
        assert (flows == 0);
        assert (errors == 0);
    }

    // Liveness is a finite ordinary control record: it shares the pre-receive
    // permit, then returns that permit as soon as its internal processing
    // completes.
    const auto paused_liveness_base = mesh::service_liveness_registry_t::clock_t::now ();
    const auto paused_probe = await_task (first.tick_liveness (paused_liveness_base + 5s));
    assert (paused_probe.probes.size () == 1);
    wait_for_router_input (second);
    const auto paused_without_shared_permit =
      await_task (second.pump_one (mesh::service_liveness_registry_t::clock_t::now (), false));
    assert (paused_without_shared_permit == mesh::raw_mesh_pump_result_t::no_data);
    assert (second.last_pump_bytes () == 0);
    mesh::raw_mesh_pump_result_t paused_probe_pump = mesh::raw_mesh_pump_result_t::no_data;
    while (paused_probe_pump != mesh::raw_mesh_pump_result_t::infrastructure
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        paused_probe_pump =
          await_task (second.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        assert (paused_probe_pump != mesh::raw_mesh_pump_result_t::protocol_error);
        if (paused_probe_pump != mesh::raw_mesh_pump_result_t::infrastructure)
            std::this_thread::sleep_for (1ms);
    }
    assert (paused_probe_pump == mesh::raw_mesh_pump_result_t::infrastructure);
    mesh::raw_mesh_pump_result_t paused_ack_pump = mesh::raw_mesh_pump_result_t::no_data;
    while (paused_ack_pump != mesh::raw_mesh_pump_result_t::infrastructure
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        paused_ack_pump =
          await_task (first.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        assert (paused_ack_pump != mesh::raw_mesh_pump_result_t::protocol_error);
        if (paused_ack_pump != mesh::raw_mesh_pump_result_t::infrastructure)
            std::this_thread::sleep_for (1ms);
    }
    assert (paused_ack_pump == mesh::raw_mesh_pump_result_t::infrastructure);

    auto claim = zlink::framework::test::claim_mailbox (
      second.mailbox (), mesh::service_mailbox_domain_t::application, 1, 1024);
    assert (claim && claim->records.size () == 1);
    assert (
      protocol::decode_header (protocol::message_bytes (claim->records.front ()->parts ().front ()))
        .kind
      == protocol::command::nodeSend);
    const protocol::application_payload_t expected_payload{"Probe", "application/json",
                                                           bytes ("payload")};
    assert (protocol::decode_application_payload (
              protocol::message_bytes (claim->records.front ()->parts ().at (1)))
            == expected_payload);
    assert (second.mailbox ().release (*claim));

    for (std::size_t attempt = 0; attempt < 10; ++attempt) {
        const auto after_release =
          await_task (second.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        assert (after_release != mesh::raw_mesh_pump_result_t::protocol_error);
        assert (after_release != mesh::raw_mesh_pump_result_t::application);
        if (after_release == mesh::raw_mesh_pump_result_t::no_data)
            break;
    }
    // The second payload was retained, not dropped, so it is still claimable.
    auto retained_claim = zlink::framework::test::claim_mailbox (
      second.mailbox (), mesh::service_mailbox_domain_t::application, 1, 1024);
    assert (retained_claim && retained_claim->records.size () == 1);
    assert (second.mailbox ().release (*retained_claim));
    assert (!zlink::framework::test::claim_mailbox (
      second.mailbox (), mesh::service_mailbox_domain_t::application, 1, 1024));

    bool channel_submitted = false;
    while (!channel_submitted && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        try {
            channel_submitted = await_task (first.send_to_channel (
              "alpha", {"ChannelProbe", "application/json", bytes ("channel")}));
        }
        catch (...) {
        }
    }
    assert (channel_submitted);
    mesh::raw_mesh_pump_result_t channel_pump = mesh::raw_mesh_pump_result_t::no_data;
    while (channel_pump != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        channel_pump =
          await_task (second.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        assert (channel_pump != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (channel_pump == mesh::raw_mesh_pump_result_t::application);
    auto channel_claim = zlink::framework::test::claim_mailbox (
      second.mailbox (), mesh::service_mailbox_domain_t::application, 1, 1024);
    assert (channel_claim && channel_claim->owner == "channel:alpha");
    assert (protocol::decode_channel_send_header (
              protocol::message_bytes (channel_claim->records.front ()->parts ().front ()))
            == "alpha");
    assert (second.mailbox ().release (*channel_claim));

    using request_result_t = std::pair<foundation::operation_terminal_t, std::vector<std::uint8_t>>;
    std::promise<request_result_t> request_promise;
    auto request_future = request_promise.get_future ();
    assert (await_task (first.request_to_node (
      second_descriptor.node_routing_id, {"RequestProbe", "application/json", bytes ("request")},
      2s,
      [&request_promise] (
        foundation::operation_terminal_t terminal,
        ::zlink::framework::result_t<zlink::message_t> completion_payload) mutable {
          auto native_payload = std::move (completion_payload.value ());
          auto payload = native_payload.to_bytes ();
          request_promise.set_value ({terminal, std::move (payload)});
      })));
    mesh::raw_mesh_pump_result_t request_pump = mesh::raw_mesh_pump_result_t::no_data;
    while (request_pump != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        request_pump =
          await_task (second.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        assert (request_pump != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (request_pump == mesh::raw_mesh_pump_result_t::application);
    auto request_claim = zlink::framework::test::claim_mailbox (
      second.mailbox (), mesh::service_mailbox_domain_t::application, 1, 1024);
    assert (request_claim && request_claim->records.size () == 1);
    const auto &request_record = (*request_claim->records.front ());
    assert (request_record.reply_token ());
    assert (request_record.correlation);
    assert (protocol::decode_node_request_header (
              protocol::message_bytes (request_record.parts ().front ()))
            == *request_record.correlation);
    assert (second.reply (request_record, {"RequestReply", "application/json", bytes ("reply")}));
    assert (second.mailbox ().release (*request_claim));
    const auto request_deadline = mesh::service_liveness_registry_t::clock_t::now () + 2s;
    // Reply/error completion is pre-classified on the binding completion
    // path, so it must settle even while ordinary receive has no shared
    // Application Job Queue permit.
    while (request_future.wait_for (0ms) != std::future_status::ready
           && mesh::service_liveness_registry_t::clock_t::now () < request_deadline) {
        const auto client_pump =
          await_task (first.pump_one (mesh::service_liveness_registry_t::clock_t::now (), false));
        assert (client_pump == mesh::raw_mesh_pump_result_t::no_data);
        std::this_thread::sleep_for (1ms);
    }
    assert (request_future.wait_for (0ms) == std::future_status::ready);
    auto request_result = request_future.get ();
    assert (request_result.first == foundation::operation_terminal_t::completed);
    const protocol::application_payload_t expected_reply{"RequestReply", "application/json",
                                                         bytes ("reply")};
    assert (protocol::decode_application_payload (request_result.second) == expected_reply);
    assert (first.unregister_local_operation (*local_operation));
    assert (local_operation_callbacks.load (std::memory_order_acquire) == 0);

    std::promise<request_result_t> actor_create_promise;
    auto actor_create_future = actor_create_promise.get_future ();
    protocol::actor_create_header_t actor_create{
      0,
      {first_descriptor.lifecycle_generation, 91},
      first_descriptor.node_routing_id,
      first_descriptor.lifecycle_generation,
      "actor-remote",
      "player",
      {"reservation-remote", "store-remote", 1, 1, second_descriptor.node_routing_id,
       second_descriptor.lifecycle_generation, "owner-remote", 1, 1},
      static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                    std::chrono::system_clock::now ().time_since_epoch () + 2s)
                                    .count ())};
    assert (await_task (first.request_actor_create (
      second_descriptor.node_routing_id, actor_create, 2s,
      [&actor_create_promise] (
        foundation::operation_terminal_t terminal,
        ::zlink::framework::result_t<zlink::message_t> completion_payload) mutable {
          auto native_payload = std::move (completion_payload.value ());
          auto payload = native_payload.to_bytes ();
          actor_create_promise.set_value ({terminal, std::move (payload)});
      })));
    mesh::raw_mesh_pump_result_t actor_create_pump = mesh::raw_mesh_pump_result_t::no_data;
    while (actor_create_pump != mesh::raw_mesh_pump_result_t::infrastructure
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        actor_create_pump =
          await_task (second.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        assert (actor_create_pump != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (actor_create_pump == mesh::raw_mesh_pump_result_t::infrastructure);
    auto actor_create_claim =
      second.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure, 1, 4096);
    assert (actor_create_claim && actor_create_claim->records.size () == 1);
    const auto &actor_create_record = (*actor_create_claim->records.front ());
    const auto decoded_actor_create = protocol::decode_actor_create_header (
      protocol::message_bytes (actor_create_record.parts ().front ()));
    assert (decoded_actor_create.operation == actor_create.operation);
    assert (decoded_actor_create.actor_id == "actor-remote");
    protocol::actor_create_reply_t actor_create_reply{{*actor_create_record.correlation, 0, 0},
                                                      protocol::actor_create_result_t::created,
                                                      second_descriptor.node_routing_id,
                                                      "actor-remote",
                                                      1};
    assert (second.reply_actor_create (actor_create_record, actor_create_reply));
    assert (second.mailbox ().release (*actor_create_claim));
    while (actor_create_future.wait_for (0ms) != std::future_status::ready
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto pump =
          await_task (first.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        assert (pump != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (actor_create_future.wait_for (0ms) == std::future_status::ready);
    const auto actor_create_result = actor_create_future.get ();
    assert (actor_create_result.first == foundation::operation_terminal_t::completed);
    assert (!actor_create_result.second.empty ());

    constexpr std::size_t actor_create_burst_size = 8;
    std::vector<std::future<request_result_t>> actor_create_burst_futures;
    actor_create_burst_futures.reserve (actor_create_burst_size);
    for (std::size_t index = 0; index < actor_create_burst_size; ++index) {
        auto promise = std::make_shared<std::promise<request_result_t>> ();
        actor_create_burst_futures.push_back (promise->get_future ());
        auto burst_request = actor_create;
        burst_request.operation.low = 100 + index;
        burst_request.actor_id = "actor-burst-" + std::to_string (index);
        assert (await_task (first.request_actor_create (
          second_descriptor.node_routing_id, burst_request, 2s,
          [promise] (foundation::operation_terminal_t terminal,
                     ::zlink::framework::result_t<zlink::message_t> completion_payload) mutable {
              auto native_payload = std::move (completion_payload.value ());
              auto payload = native_payload.to_bytes ();
              promise->set_value ({terminal, std::move (payload)});
          })));
    }

    std::size_t burst_received = 0;
    while (burst_received < actor_create_burst_size
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto pump =
          await_task (second.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        assert (pump != mesh::raw_mesh_pump_result_t::protocol_error);
        if (pump == mesh::raw_mesh_pump_result_t::infrastructure)
            ++burst_received;
    }
    assert (burst_received == actor_create_burst_size);

    std::size_t burst_replied = 0;
    while (burst_replied < actor_create_burst_size) {
        auto claim =
          second.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure,
                                       actor_create_burst_size - burst_replied, 256 * 1024);
        assert (claim && !claim->records.empty ());
        for (const auto &slot : claim->records) {
            const auto &record = *slot;
            const auto decoded = protocol::decode_actor_create_header (
              protocol::message_bytes (record.parts ().front ()));
            protocol::actor_create_reply_t burst_reply{{*record.correlation, 0, 0},
                                                       protocol::actor_create_result_t::created,
                                                       second_descriptor.node_routing_id,
                                                       decoded.actor_id,
                                                       1};
            assert (second.reply_actor_create (record, burst_reply));
            ++burst_replied;
        }
        assert (second.mailbox ().release (*claim));
    }

    std::size_t burst_completed = 0;
    while (burst_completed < actor_create_burst_size
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        burst_completed = static_cast<std::size_t> (
          std::count_if (actor_create_burst_futures.begin (), actor_create_burst_futures.end (),
                         [] (std::future<request_result_t> &future) {
                             return future.wait_for (0ms) == std::future_status::ready;
                         }));
        if (burst_completed < actor_create_burst_size) {
            const auto pump =
              await_task (first.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
            assert (pump != mesh::raw_mesh_pump_result_t::protocol_error);
        }
    }
    assert (burst_completed == actor_create_burst_size);
    for (auto &future : actor_create_burst_futures) {
        const auto result = future.get ();
        assert (result.first == foundation::operation_terminal_t::completed);
        assert (!result.second.empty ());
    }

    // The paused probe advanced the registry's logical next-probe time by
    // one interval. Continue from that same logical clock instead of mixing
    // the synthetic probe time with the wall clock used by the pump loop.
    const auto liveness_base = paused_liveness_base + 5s;
    const auto first_probe = await_task (first.tick_liveness (liveness_base + 5s));
    assert (first_probe.probes.size () == 1);
    mesh::raw_mesh_pump_result_t probe_pump = mesh::raw_mesh_pump_result_t::no_data;
    while (probe_pump == mesh::raw_mesh_pump_result_t::no_data
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        probe_pump =
          await_task (second.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
    }
    assert (probe_pump == mesh::raw_mesh_pump_result_t::infrastructure);

    mesh::raw_mesh_pump_result_t ack_pump = mesh::raw_mesh_pump_result_t::no_data;
    while (ack_pump == mesh::raw_mesh_pump_result_t::no_data
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        ack_pump = await_task (first.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
    }
    assert (ack_pump == mesh::raw_mesh_pump_result_t::infrastructure);
    const auto next_probe = await_task (first.tick_liveness (liveness_base + 10s));
    assert (next_probe.probes.size () == 1);
    assert (next_probe.probes.front ().probe_id != first_probe.probes.front ().probe_id);

    // Fill the source-to-target Core HWM without pumping the target.  A
    // registry entry remains present only while binding admission is still
    // pending; dispatcher scheduling after an immediate admission cannot
    // produce this observation because terminal claim removes the entry.
    assert (first.pending_operation_count () == 0);
    std::vector<std::shared_ptr<zlink::framework::task_t<zlink::submit_result_t>>> saturated_sends;
    std::size_t saturated_received = 0;
    for (std::size_t index = 0; index < 128 && first.pending_operation_count () == 0; ++index) {
        protocol::application_payload_t saturated_payload{
          "SaturatedControl", "application/octet-stream",
          std::vector<std::uint8_t> (512u * 1024u, 0x5a)};
        saturated_sends.push_back (
          std::make_shared<zlink::framework::task_t<zlink::submit_result_t>> (
            first.send_to_node_result (second_descriptor.node_routing_id,
                                       std::move (saturated_payload))));
    }
    assert (first.pending_operation_count () != 0);
    const auto verify_retained_payloads =
      [] (const zlink::framework::test::mailbox_claim_t &claim) {
          for (const auto &record : claim.records) {
              const auto payload = protocol::decode_application_payload (
                protocol::message_bytes (record->parts ().back ()), false);
              assert (payload.packet_name () == "SaturatedControl");
              const auto &body = payload.payload_bytes ();
              assert (body.size () == 512u * 1024u);
              assert (
                std::all_of (body.begin (), body.end (), [] (auto byte) { return byte == 0x5a; }));
          }
      };

    // The reverse-direction probe itself is not behind the saturated route.
    // Claiming it starts one ACK binding operation, then I3 must return so the
    // same source poller remains able to drain that operation's completion.
    const auto reverse_probe_time = mesh::service_liveness_registry_t::clock_t::now () + 5s;
    const auto reverse_probe = await_task (second.tick_liveness (reverse_probe_time));
    assert (reverse_probe.probes.size () == 1);
    bool reverse_probe_claimed = false;
    const auto reverse_probe_deadline = std::chrono::steady_clock::now () + 2s;
    while (!reverse_probe_claimed && std::chrono::steady_clock::now () < reverse_probe_deadline) {
        auto pumping = first.pump_one (mesh::service_liveness_registry_t::clock_t::now ());
        assert (pumping.await_ready ());
        const auto result = pumping.result ().value ();
        assert (result != mesh::raw_mesh_pump_result_t::protocol_error);
        reverse_probe_claimed = result == mesh::raw_mesh_pump_result_t::infrastructure;
        if (!reverse_probe_claimed)
            std::this_thread::sleep_for (1ms);
    }
    assert (reverse_probe_claimed);

    // Drain the original application prefix and drive both existing pollers.
    // All application sends and the registered ACK must reach terminal without
    // a second poller or a detached control-send table.
    const auto saturation_deadline = std::chrono::steady_clock::now () + 5s;
    auto all_sends_ready = [&] {
        return std::all_of (saturated_sends.begin (), saturated_sends.end (),
                            [] (const auto &send) { return send->await_ready (); });
    };
    while ((!all_sends_ready () || first.pending_operation_count () != 0
            || second.pending_operation_count () != 0)
           && std::chrono::steady_clock::now () < saturation_deadline) {
        auto target_pump = second.pump_one (mesh::service_liveness_registry_t::clock_t::now ());
        assert (target_pump.await_ready ());
        const auto target_result = target_pump.result ().value ();
        assert (target_result != mesh::raw_mesh_pump_result_t::protocol_error);
        auto claim = zlink::framework::test::claim_mailbox (
          second.mailbox (), mesh::service_mailbox_domain_t::application, 1, 1024u * 1024u);
        if (claim) {
            verify_retained_payloads (claim);
            saturated_received += claim->records.size ();
            assert (saturated_received <= saturated_sends.size ());
            assert (second.mailbox ().release (*claim));
        }

        auto source_pump = first.pump_one (mesh::service_liveness_registry_t::clock_t::now ());
        assert (source_pump.await_ready ());
        assert (source_pump.result ().value () != mesh::raw_mesh_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (all_sends_ready ());
    assert (first.pending_operation_count () == 0);
    assert (second.pending_operation_count () == 0);
    for (const auto &send : saturated_sends)
        assert (send->result ().value () == zlink::submit_result_t::ok);

    // Every saturating one-way send is retained. Cross the phase boundary only
    // after the target has received exactly that many application records; an
    // idle pump does not prove that the transport has finished delivering them.
    const auto quiesce_deadline = std::chrono::steady_clock::now () + 10s;
    while (saturated_received < saturated_sends.size ()
           && std::chrono::steady_clock::now () < quiesce_deadline) {
        const auto pumped_quiesce =
          await_task (second.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        assert (pumped_quiesce != mesh::raw_mesh_pump_result_t::protocol_error);
        while (auto quiesce_claim = zlink::framework::test::claim_mailbox (
                 second.mailbox (), mesh::service_mailbox_domain_t::application, 256,
                 16u * 1024u * 1024u)) {
            verify_retained_payloads (quiesce_claim);
            saturated_received += quiesce_claim->records.size ();
            assert (saturated_received <= saturated_sends.size ());
            assert (second.mailbox ().release (*quiesce_claim));
        }
        const auto source_quiesce =
          await_task (first.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        assert (source_quiesce != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (saturated_received == saturated_sends.size ());
    assert (second.mailbox ().pending_messages (mesh::service_mailbox_domain_t::application) == 0);

    constexpr std::size_t request_count = 4'200;
    const protocol::application_payload_t request_payload{"ConcurrentRequest", "application/json",
                                                          bytes ("payload")};
    const protocol::application_payload_t expected_concurrent_reply{
      "ConcurrentReply", "application/json", bytes ("reply")};
    std::atomic_size_t completed_requests{0};
    std::vector<std::shared_ptr<zlink::framework::task_t<bool>>> requests;
    requests.reserve (request_count);
    for (std::size_t index = 0; index < request_count; ++index) {
        requests.push_back (
          std::make_shared<zlink::framework::task_t<bool>> (first.request_to_node (
            second_descriptor.node_routing_id, request_payload, 30s,
            [&] (foundation::operation_terminal_t terminal,
                 ::zlink::framework::result_t<zlink::message_t> completion_payload) {
                auto native_payload = std::move (completion_payload.value ());
                auto payload = native_payload.to_bytes ();
                assert (terminal == foundation::operation_terminal_t::completed);
                assert (protocol::decode_application_payload (payload)
                        == expected_concurrent_reply);
                completed_requests.fetch_add (1, std::memory_order_release);
            })));
    }
    const auto requests_deadline = std::chrono::steady_clock::now () + 30s;
    const auto all_submitted = [&] {
        return std::all_of (requests.begin (), requests.end (),
                            [] (const auto &request) { return request->await_ready (); });
    };
    while (
      (!all_submitted () || completed_requests.load (std::memory_order_acquire) != request_count)
      && std::chrono::steady_clock::now () < requests_deadline) {
        const auto target_pump =
          await_task (second.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        assert (target_pump != mesh::raw_mesh_pump_result_t::protocol_error);
        auto claim = zlink::framework::test::claim_mailbox (
          second.mailbox (), mesh::service_mailbox_domain_t::application, 256, 16u * 1024u * 1024u);
        if (claim) {
            for (const auto &slot : claim->records) {
                const auto &record = *slot;
                assert (record.reply_token ());
                assert (
                  second.reply (record, {"ConcurrentReply", "application/json", bytes ("reply")}));
            }
            assert (second.mailbox ().release (*claim));
        }
        const auto source_pump =
          await_task (first.pump_one (mesh::service_liveness_registry_t::clock_t::now (), false));
        assert (source_pump != mesh::raw_mesh_pump_result_t::protocol_error);
        std::this_thread::yield ();
    }
    assert (all_submitted ());
    for (const auto &request : requests)
        assert (request->result ().value ());
    assert (completed_requests.load (std::memory_order_acquire) == request_count);
    assert (first.pending_operation_count () == 0);

    first.close ();
    second.close ();
}
}
