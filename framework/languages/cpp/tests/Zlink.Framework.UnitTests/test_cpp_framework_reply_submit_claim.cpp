/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/backend/raw_binding_adapter.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/stateful_ingress_fixture.hpp"

#include "runtime/client_server/raw_client_server_owner.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"

#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Sockets/message_socket_contracts.hpp>

#include <atomic>
#include <barrier>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace mesh = zlink::framework::runtime::mesh;
namespace messaging = zlink::framework::runtime::messaging;
namespace client_server = zlink::framework::runtime::client_server;
namespace protocol = zlink::framework::runtime::protocol;
namespace stateful = zlink::framework::runtime::stateful;
using namespace std::chrono_literals;

namespace
{

std::vector<std::uint8_t> bytes (std::string value)
{
    return {value.begin (), value.end ()};
}

mesh::service_node_descriptor_t descriptor (std::string routing_id)
{
    return {"reply-claim-mesh",
            bytes (std::move (routing_id)),
            1,
            1,
            "tcp://127.0.0.1:0",
            {},
            mesh::service_node_state_t::preparing};
}

stateful::object_ref_t create_ready (stateful::stateful_object_runtime_t &objects)
{
    const stateful::create_request_t request{
      stateful::object_kind_t::actor, "reply-claim-actor", "actor", std::nullopt, {}, false, false};
    const auto reserved = objects.begin_create (request);
    assert (reserved.status == stateful::create_status_t::reserved);
    assert (reserved.factory_owner);
    assert (objects.commit_create (reserved.attempt) == stateful::stateful_error_t::none);
    const auto ready = objects.find (request.kind, request.key);
    assert (ready);
    return *ready;
}

std::pair<bool, bool>
submit_client_server_reply_and_release_claim (client_server::raw_client_server_server_t &server,
                                              std::shared_ptr<mesh::service_mailbox_claim_t> claim,
                                              mesh::service_mailbox_record_t request)
{
    const auto delivered =
      server
        .reply (request, protocol::application_payload_t{"ClientServerReply", "application/json",
                                                         bytes ("reply")})
        .result ()
        .value ();
    const auto released = server.mailbox ().release (*claim);
    return std::make_pair (delivered, released);
}

void verify_stateful_claim_releases_after_one_shot_reply_terminal ()
{
    auto context = std::make_shared<zlink::context_t> ();
    mesh::raw_mesh_node_owner_t target (
      mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("reply-claim-target"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()},
      context);
    target.start ();
    const auto target_descriptor = target.topology ().local_descriptor ();

    stateful::stateful_object_runtime_t objects;
    objects.replace_placement_candidates (
      {{"reply-claim-mesh", "reply-claim-target", {"actor"}, 100, 16, 0, 8, 0}});
    const auto actor = create_ready (objects);

    const std::string source_routing_id_text = "reply-claim-source";
    const auto source_routing_id = bytes (source_routing_id_text);
    constexpr std::uint64_t source_generation = 7;
    constexpr std::uint64_t request_sequence = 42;
    constexpr std::uint64_t correlation = 99;
    const protocol::wire_operation_id_t operation{11, 12};
    const protocol::actor_route_fence_t fence{actor.key,
                                              actor.object_generation,
                                              target_descriptor.node_routing_id,
                                              target_descriptor.lifecycle_generation,
                                              actor.authority_owner_generation,
                                              1};

    stateful::raw_stateful_dispatch_t dispatch (
      objects, target,
      [&] (stateful::accepted_record_authority_query_t query)
        -> zlink::framework::task_t<std::optional<stateful::accepted_record_authority_t>> {
          assert (query.source_node_routing_id == source_routing_id);
          assert (query.source_node_generation == source_generation);
          co_return stateful::accepted_record_authority_t{
            {"reply-claim-source-owner", 1, source_routing_id, source_generation}, 1};
      });

    const std::string mailbox_owner = "actor:" + actor.key;
    assert (target.mailbox ().try_enqueue (mesh::service_mailbox_record_t{
      mailbox_owner, mesh::service_mailbox_domain_t::application,
      zlink::framework::detail::backend::materialize_binding_parts (
        {protocol::encode_actor_message_header (protocol::command::actorRequest, std::nullopt,
                                                fence, operation, correlation),
         protocol::encode_application_payload (
           {"ActorRequest", "application/json", bytes ("request")})}),
      source_routing_id, std::nullopt, correlation, source_generation,
      std::make_pair (operation.high, operation.low)}));
    assert (
      zlink::framework::test::ingest_stateful_mailbox_record (objects, target, dispatch, actor)
        .result ()
        .value ()
      == stateful::stateful_error_t::none);
    const auto [claim_error, delivery] = dispatch.try_claim (actor);
    assert (claim_error == stateful::stateful_error_t::none);
    assert (delivery && delivery->request);

    assert (target.mailbox ().try_enqueue (mesh::service_mailbox_record_t{
      mailbox_owner, mesh::service_mailbox_domain_t::application,
      zlink::framework::detail::backend::materialize_binding_parts ({{0x01}})}));
    assert (objects.enqueue (actor, stateful::turn_domain_t::application, {777, {0x02}})
            == stateful::stateful_error_t::none);

    std::atomic<int> terminal_count{0};
    auto completion =
      std::async (std::launch::async,
                  [pending = dispatch.complete_async (
                     *delivery, protocol::application_payload_t{"ActorReply", "application/json",
                                                                bytes ("reply")}),
                   &terminal_count] () mutable {
                      const auto result = pending.result ().value ();
                      terminal_count.fetch_add (1, std::memory_order_relaxed);
                      return result;
                  });

    assert (completion.wait_for (250ms) == std::future_status::ready);
    assert (completion.get () == stateful::stateful_error_t::conflict);
    assert (terminal_count.load (std::memory_order_relaxed) == 1);

    auto released_mailbox = target.mailbox ().try_claim_owner (
      mesh::service_mailbox_domain_t::application, mailbox_owner, 1, 1024);
    assert (released_mailbox);
    assert (target.mailbox ().release (*released_mailbox));
    const auto [released_error, released] =
      objects.try_claim (actor, stateful::turn_domain_t::application);
    assert (released_error == stateful::stateful_error_t::none);
    assert (released && released->sequence == 777);
    assert (objects.complete_claim (actor, stateful::turn_domain_t::application)
            == stateful::stateful_error_t::none);

    zlink::dealer_socket_t source (*context);
    source.set_routing_id (zlink::routing_id_t::from (source_routing_id_text));
    source.connect (target.endpoint ());
    std::this_thread::sleep_for (50ms);
    assert (terminal_count.load (std::memory_order_relaxed) == 1);
    zlink::received_t ghost;
    assert (source.recv (ghost, zlink::recv_flags_t::dontwait) != 0);

    source.close ();
    target.close ();
}

void verify_client_server_claim_releases_after_one_shot_reply_terminal ()
{
    auto context = std::make_shared<zlink::context_t> ();
    protocol::client_server_server_admission_t descriptor{"reply-claim-channel",
                                                          bytes ("reply-claim-server"),
                                                          17,
                                                          1,
                                                          100,
                                                          mesh::service_node_state_t::serving,
                                                          "default",
                                                          16 * 1024 * 1024,
                                                          "tcp://127.0.0.1:0"};
    client_server::raw_client_server_server_t server (
      zlink::framework::test::runtime_failure_options (
        client_server::raw_client_server_server_options_t{descriptor}),
      context);
    server.start ();

    const std::string source_routing_id_text = "client-server-reply-claim-source";
    const auto source_routing_id = bytes (source_routing_id_text);
    constexpr std::uint64_t request_sequence = 51;
    constexpr std::uint64_t correlation = 61;
    const std::string mailbox_owner = descriptor.channel_name;
    /* The synthetic record carries the ingress header and its wire frame. */
    messaging::envelope_header_t request_header;
    request_header.kind = messaging::message_kind_t::request;
    request_header.channel_name = descriptor.channel_name;
    request_header.message_name = "ClientServerRequest";
    request_header.correlation_id = "reply-claim-corr";
    const auto request_header_bytes =
      messaging::envelope_codec_t{}.encode_header (request_header).to_bytes ();
    auto request_record =
      mesh::service_mailbox_record_t{mailbox_owner,
                                     mesh::service_mailbox_domain_t::application,
                                     zlink::framework::detail::backend::materialize_binding_parts (
                                       {request_header_bytes, bytes ("request")}),
                                     source_routing_id,
                                     std::nullopt,
                                     correlation};
    request_record.envelope_header = request_header;
    assert (server.mailbox ().try_enqueue (std::move (request_record)));
    assert (server.mailbox ().try_enqueue (mesh::service_mailbox_record_t{
      mailbox_owner, mesh::service_mailbox_domain_t::application,
      zlink::framework::detail::backend::materialize_binding_parts ({{0x02}})}));
    auto claim = server.mailbox ().try_claim_owner (mesh::service_mailbox_domain_t::application,
                                                    mailbox_owner, 1, 1024);
    assert (claim && claim->records.size () == 1);
    const auto request = (*claim->records.front ());
    auto retained_claim = claim;

    std::atomic<int> terminal_count{0};
    auto completion = std::async (
      std::launch::async, [&server, retained_claim, request, &terminal_count] () mutable {
          const auto result = submit_client_server_reply_and_release_claim (
            server, std::move (retained_claim), std::move (request));
          terminal_count.fetch_add (1, std::memory_order_relaxed);
          return result;
      });

    assert (completion.wait_for (250ms) == std::future_status::ready);
    const auto [delivered, claim_released] = completion.get ();
    assert (!delivered);
    assert (claim_released);
    assert (terminal_count.load (std::memory_order_relaxed) == 1);

    auto released = server.mailbox ().try_claim_owner (mesh::service_mailbox_domain_t::application,
                                                       mailbox_owner, 1, 1024);
    assert (released && released->records.size () == 1);
    assert (server.mailbox ().release (*released));

    zlink::dealer_socket_t source (*context);
    source.set_routing_id (zlink::routing_id_t::from (source_routing_id_text));
    source.connect (server.endpoint ());
    std::this_thread::sleep_for (50ms);
    assert (terminal_count.load (std::memory_order_relaxed) == 1);
    zlink::received_t ghost;
    assert (source.recv (ghost, zlink::recv_flags_t::dontwait) != 0);

    source.close ();
    server.close ();
}

void verify_stateful_ingest_rejection_releases_on_one_shot_terminal ()
{
    auto context = std::make_shared<zlink::context_t> ();
    mesh::raw_mesh_node_owner_t target (
      mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("ingest-reply-target"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()},
      context);
    target.start ();

    stateful::stateful_object_runtime_t objects;
    objects.replace_placement_candidates (
      {{"reply-claim-mesh", "ingest-reply-target", {"actor"}, 100, 16, 0, 8, 0}});
    const auto actor = create_ready (objects);
    stateful::raw_stateful_dispatch_t dispatch (objects, target);

    const std::string source_routing_id_text = "ingest-reply-source";
    const auto source_routing_id = bytes (source_routing_id_text);
    const std::string mailbox_owner = "actor:" + actor.key;
    assert (target.mailbox ().try_enqueue (mesh::service_mailbox_record_t{
      mailbox_owner, mesh::service_mailbox_domain_t::application,
      zlink::framework::detail::backend::materialize_binding_parts ({{0xff}, {0x00}}),
      source_routing_id, std::nullopt, 81, 1}));
    assert (target.mailbox ().try_enqueue (mesh::service_mailbox_record_t{
      mailbox_owner, mesh::service_mailbox_domain_t::application,
      zlink::framework::detail::backend::materialize_binding_parts ({{0x01}})}));

    assert (
      zlink::framework::test::ingest_stateful_mailbox_record (objects, target, dispatch, actor)
        .result ()
        .value ()
      == stateful::stateful_error_t::invalid);
    std::shared_ptr<mesh::service_mailbox_claim_t> released;
    const auto deadline = std::chrono::steady_clock::now () + 250ms;
    while (!released && std::chrono::steady_clock::now () < deadline) {
        released = target.mailbox ().try_claim_owner (mesh::service_mailbox_domain_t::application,
                                                      mailbox_owner, 1, 1024);
        if (!released)
            std::this_thread::sleep_for (1ms);
    }
    assert (released && released->records.size () == 1);
    assert (target.mailbox ().release (*released));

    zlink::dealer_socket_t source (*context);
    source.set_routing_id (zlink::routing_id_t::from (source_routing_id_text));
    source.connect (target.endpoint ());
    std::this_thread::sleep_for (50ms);
    zlink::received_t ghost;
    assert (source.recv (ghost, zlink::recv_flags_t::dontwait) != 0);

    source.close ();
    target.close ();
}

void verify_claim_original_remains_takeable ()
{
    mesh::service_mailbox_t mailbox;
    assert (mailbox.try_enqueue (
      {"spot:canonical", mesh::service_mailbox_domain_t::application,
       zlink::framework::detail::backend::materialize_binding_parts ({{1}, {2}})}));
    auto claim = mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application,
                                          "spot:canonical", 1, 4096);
    assert (claim);
    // This is the explicit barrier immediately after claim, before a source query.
    auto sealed = mailbox.take ({claim, 0});
    assert (sealed && "claimed original must remain takeable by the mailbox owner");
    assert (!mailbox.take ({claim, 0}));
    assert (mailbox.release (*claim));
}

void verify_source_wait_original (bool seal_first, bool reject, bool restore)
{
    using namespace zlink::framework;
    auto context = std::make_shared<zlink::context_t> ();
    mesh::raw_mesh_node_owner_t target (
      mesh::raw_mesh_node_options_t{.descriptor = descriptor ("reply-claim-target"),
                                    .runtime_failures =
                                      std::make_shared<runtime::runtime_failure_collector_t> ()},
      context);
    target.start ();
    stateful::stateful_object_runtime_t objects;
    objects.replace_placement_candidates (
      {{"reply-claim-mesh", "reply-claim-target", {"actor"}, 100, 16, 0, 8, 0}});
    const auto owner = create_ready (objects);
    const auto local = target.topology ().local_descriptor ();
    const auto source = bytes ("canonical-source");
    task_completion_source_t<std::optional<stateful::accepted_record_authority_t>> authority;
    int queries = 0;
    stateful::raw_stateful_dispatch_t dispatch (
      objects, target, [&] (stateful::accepted_record_authority_query_t query) {
          ++queries;
          assert (query.source_node_routing_id == source);
          assert (query.source_node_generation == 7);
          return authority.task ();
      });
    const protocol::actor_route_fence_t fence{owner.key,
                                              owner.object_generation,
                                              local.node_routing_id,
                                              local.lifecycle_generation,
                                              owner.authority_owner_generation,
                                              1};
    const auto key =
      mesh::service_mailbox_t::application_owner (runtime::host::owner_kind_t::actor, owner.key);
    int permits = 0;
    mesh::service_mailbox_record_t record{
      key,
      mesh::service_mailbox_domain_t::application,
      detail::backend::materialize_binding_parts (
        {protocol::encode_actor_message_header (protocol::command::actorSend, std::nullopt, fence,
                                                {11, 12}),
         protocol::encode_application_payload ({"Probe", "application/json", bytes ("body")})}),
      source,
      std::nullopt,
      std::nullopt,
      7};
    record.before_application_handler = [&] { ++permits; };
    assert (target.mailbox ().try_enqueue (std::move (record)));
    auto value =
      target.mailbox ().try_claim_owner (mesh::service_mailbox_domain_t::application, key, 1, 4096);
    assert (value);
    auto claim = value;
    auto pending = dispatch.ingest_claimed (owner, {claim, 0}, std::nullopt);
    assert (queries == 1 && !pending.await_ready ());
    // The source resolver is now suspended at an explicit completion-source barrier.
    std::optional<mesh::service_mailbox_record_t> sealed;
    if (seal_first) {
        sealed = target.mailbox ().take ({claim, 0});
        assert (sealed && "source await must leave the original in its canonical slot");
        assert (permits == 0);
        if (restore) {
            assert (target.mailbox ().release (*claim));
            assert (target.mailbox ().try_enqueue (std::move (*sealed)));
            sealed.reset ();
        }
    }
    authority.complete (result_t<std::optional<stateful::accepted_record_authority_t>>::success (
      reject ? std::nullopt
             : std::make_optional (
                 stateful::accepted_record_authority_t{{"canonical-owner", 3, source, 7}, 1})));
    const auto result = pending.result ().value ();
    if (seal_first) {
        assert (result == stateful::stateful_error_t::not_found);
        assert (!objects.try_claim (owner, stateful::turn_domain_t::application).second);
        assert (permits == 0 && queries == 1);
        assert (!target.mailbox ().take ({claim, 0}));
        if (restore) {
            auto restored = target.mailbox ().try_claim_owner (
              mesh::service_mailbox_domain_t::application, key, 1, 4096);
            assert (restored);
            sealed = target.mailbox ().take ({restored, 0});
            assert (sealed);
            assert (!target.mailbox ().release (*claim));
            assert (target.mailbox ().release (*restored));
        } else
            assert (target.mailbox ().release (*claim));
        sealed->before_application_handler ();
        assert (permits == 1);
    } else {
        assert (result == stateful::stateful_error_t::none);
        assert (!target.mailbox ().take ({claim, 0}));
        std::function<void ()> before;
        auto [error, delivery] = dispatch.try_claim (owner, &before);
        assert (error == stateful::stateful_error_t::none && delivery);
        assert (before && permits == 0);
        before ();
        assert (permits == 1);
        assert (dispatch.complete (*delivery, std::nullopt) == stateful::stateful_error_t::none);
        assert (dispatch.complete (*delivery, std::nullopt) != stateful::stateful_error_t::none);
    }
    target.close ();
}

void verify_partial_prefix_and_stale_claim (bool local)
{
    using namespace zlink::framework;
    mesh::service_mailbox_t mailbox;
    const std::string owner = "spot:canonical-prefix";
    int permits = 0;
    for (std::uint8_t value : {1, 2, 3}) {
        mesh::service_mailbox_record_t record;
        record.owner = owner;
        record.domain = mesh::service_mailbox_domain_t::application;
        record.operation = std::pair<std::uint64_t, std::uint64_t>{9, value};
        record.before_application_handler = [&] { ++permits; };
        if (local) {
            record.application = std::make_shared<runtime::host::local_application_dispatch_t> ();
            record.application->parts.push_back (zlink::message_t::from ("body"));
        } else
            record.parts.push_back (zlink::message_t::from ("body"));
        assert (mailbox.try_enqueue (std::move (record)));
    }
    auto claim =
      mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application, owner, 3, 4096);
    assert (claim && claim->records.size () == 3);
    assert (!mailbox.try_claim_owner (claim->domain, owner, 3, 4096));
    assert (!mailbox.take ({claim, 1})); // A handoff cannot skip the prefix.
    auto gate = mailbox.take ({claim, 0});
    auto source =
      mailbox.prepare ({claim, 1}, [] (const auto &record) { return *record.operation; });
    assert (gate && source && source->second == 2 && permits == 0);
    auto seal = mailbox.take ({claim, 1});
    auto suffix = mailbox.take ({claim, 2});
    assert (seal && suffix && seal->operation == source);
    assert (!mailbox.prepare ({claim, 1}, [] (const auto &record) { return record.operation; }));
    assert (!mailbox.take ({claim, 0}) && !mailbox.take ({claim, 1}));
    gate->transferred (); // The first handoff settles only its own claim credit.
    assert (!mailbox.try_claim_owner (claim->domain, owner, 3, 4096));
    seal->transferred ();
    suffix->transferred ();
    assert (!mailbox.release (*claim));
    assert (permits == 0);
    // Abort publishes new source slots. The old handles remain empty.
    assert (mailbox.try_enqueue (std::move (*seal)));
    assert (mailbox.try_enqueue (std::move (*suffix)));
    auto restored = mailbox.try_claim_owner (claim->domain, owner, 2, 4096);
    assert (restored && restored->records.size () == 2);
    assert (!mailbox.take ({claim, 1}) && !mailbox.release (*claim));
    auto second = mailbox.take ({restored, 0});
    auto third = mailbox.take ({restored, 1});
    assert (second && third && second->operation->second == 2 && third->operation->second == 3);
    gate->before_application_handler ();
    second->before_application_handler ();
    third->before_application_handler ();
    assert (permits == 3);
    assert (mailbox.release (*restored));
    assert (!mailbox.release (*restored));
}

void verify_record_payload_is_exclusive ()
{
    using namespace zlink::framework;
    runtime::serial_application_record_t unjournaled;
    unjournaled.set_invocation (zlink::message_t::from ("native without a journal"), {});
    assert (!unjournaled.payload () && unjournaled.invocation ());
    assert (unjournaled.invocation ()->first.to_string () == "native without a journal");
    runtime::messaging::envelope_header_t header;
    header.message_name = "Canonical";
    header.correlation_id = "original-correlation";
    header.metadata.emplace ("original", "metadata");
    auto body = zlink::message_t::from ("encoded body");
    auto parts =
      runtime::messaging::envelope_codec_t{}.encode_raw_body_parts (header, body.copy ());
    const protocol::actor_route_fence_t target{"actor", 2, bytes ("target"), 3, 4, 5};
    protocol::actor_message_header_t wire;
    wire.target = target;
    wire.operation = {0xffffffffffffffffULL, 0x8000000000000001ULL};
    wire.correlation = 91;
    auto accepted = stateful::accepted_actor_record (
      wire, {"original-owner", 7, bytes ("source"), 8},
      protocol::application_payload_t::from_parts (parts.items ()), true);
    std::visit (
      [] (auto &value) {
          value.application.flow_id = "01930000-0000-7000-8000-000000002119";
          value.application.flow_origin = flow_origin_t::application;
      },
      accepted.body);
    const auto encoded = protocol::encode_application_payload (
      protocol::application_payload_t::from_parts (parts.items ()));
    auto flat = protocol::decode_application_payload (encoded);
    assert (protocol::decode_application_header (flat).to_bytes () == parts[0].to_bytes ());
    auto truncated = flat.payload_bytes ();
    truncated.pop_back ();
    bool rejected = false;
    try {
        (void) protocol::decode_application_header (
          {flat.packet_name, flat.content_type, std::move (truncated)});
    }
    catch (const protocol::service_wire_error_t &) {
        rejected = true;
    }
    assert (rejected); // Retaining only the header still validates the trailing body frame.
    const auto original = protocol::encode_frozen_application_record (accepted);
    runtime::serial_application_record_t record;
    record.payload ().emplace ();
    record.payload ()->sequence = 19;
    record.payload ()->application_record = std::move (accepted);
    int terminals = 0;
    record.native_reply = [&] (auto) {
        ++terminals;
        return true;
    };
    record.set_invocation (std::move (body), {});
    assert (record.invocation () && record.saved_work ());
    const auto *borrowed = record.invocation ();
    const auto observed = runtime::messaging::envelope_codec_t{}.decode_header (
      std::as_const (record).application_header ());
    assert (observed && observed.value ().correlation_id == header.correlation_id);
    assert (record.invocation () == borrowed);
    assert (record.invocation ()->first.to_bytes () == bytes ("encoded body"));
    auto &frozen = record.payload ();
    assert (!record.invocation () && frozen && frozen->sequence == 19);
    const auto roundtrip = protocol::encode_frozen_application_record (*frozen->application_record);
    assert (roundtrip == original && terminals == 0);
    assert (record.complete_native (std::nullopt));
    assert (!record.complete_native (std::nullopt) && terminals == 1);
}

void verify_concurrent_gate_seal_take ()
{
    for (int attempt = 0; attempt < 32; ++attempt) {
        mesh::service_mailbox_t mailbox;
        assert (mailbox.try_enqueue (
          {"spot:race", mesh::service_mailbox_domain_t::application,
           zlink::framework::detail::backend::materialize_binding_parts ({{1}, {2}})}));
        auto claim = mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application,
                                              "spot:race", 1, 4096);
        assert (claim);
        std::barrier ready (3);
        std::optional<mesh::service_mailbox_record_t> gate, seal;
        std::thread gate_owner ([&] {
            ready.arrive_and_wait ();
            gate = mailbox.take ({claim, 0});
        });
        std::thread seal_owner ([&] {
            ready.arrive_and_wait ();
            seal = mailbox.take ({claim, 0});
        });
        ready.arrive_and_wait ();
        gate_owner.join ();
        seal_owner.join ();
        assert (gate.has_value () != seal.has_value ());
        auto &original = gate ? *gate : *seal;
        original.transferred ();
        assert (!mailbox.release (*claim));
    }
}

void verify_claim_owner_can_expire ()
{
    std::optional<mesh::service_mailbox_record_t> owned;
    {
        mesh::service_mailbox_t mailbox;
        assert (mailbox.try_enqueue (
          {"spot:expired", mesh::service_mailbox_domain_t::application,
           zlink::framework::detail::backend::materialize_binding_parts ({{1}})}));
        auto claim = mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application,
                                              "spot:expired", 1, 4096);
        assert (claim);
        owned = mailbox.take ({claim, 0});
        assert (owned);
    }
    owned->transferred (); // The canonical source owner has already been destroyed.
}

} // namespace

int main ()
{
    verify_source_wait_original (true, false, false);
    verify_source_wait_original (true, true, false);
    verify_source_wait_original (true, false, true);
    verify_source_wait_original (false, false, false);
    verify_partial_prefix_and_stale_claim (false);
    verify_partial_prefix_and_stale_claim (true);
    verify_record_payload_is_exclusive ();
    verify_claim_owner_can_expire ();
    verify_concurrent_gate_seal_take ();
    verify_claim_original_remains_takeable ();
    verify_stateful_claim_releases_after_one_shot_reply_terminal ();
    verify_client_server_claim_releases_after_one_shot_reply_terminal ();
    verify_stateful_ingest_rejection_releases_on_one_shot_terminal ();
    std::puts ("canonical claim: source barriers, prefix/abort, exclusive payload, concurrent "
               "takes and native terminals passed");
    return 0;
}
