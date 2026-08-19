/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/client_server/raw_client_server_owner.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"

#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Sockets/message_socket_contracts.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
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

stateful::object_ref_t create_ready (
  stateful::stateful_object_runtime_t &objects)
{
    const stateful::create_request_t request{
      stateful::object_kind_t::actor,
      "reply-claim-actor",
      "actor",
      std::nullopt,
      {},
      false,
      false};
    const auto reserved = objects.begin_create (request);
    assert (reserved.status == stateful::create_status_t::reserved);
    assert (reserved.factory_owner);
    assert (objects.commit_create (reserved.attempt)
            == stateful::stateful_error_t::none);
    const auto ready = objects.find (request.kind, request.key);
    assert (ready);
    return *ready;
}

std::pair<bool, bool>
submit_client_server_reply_and_release_claim (
  client_server::raw_client_server_server_t &server,
  std::shared_ptr<mesh::service_mailbox_claim_t> claim,
  mesh::service_mailbox_record_t request)
{
    const auto delivered = server.reply (
      request,
      protocol::application_payload_t{
        "ClientServerReply", "application/json", bytes ("reply")});
    const auto released = server.mailbox ().release (*claim);
    return std::make_pair (delivered, released);
}

void verify_stateful_claim_releases_after_one_shot_reply_terminal ()
{
    auto context = std::make_shared<zlink::context_t> ();
    mesh::raw_mesh_node_owner_t target (
      mesh::raw_mesh_node_options_t{descriptor ("reply-claim-target")},
      context);
    target.start ();
    const auto target_descriptor = target.topology ().local_descriptor ();

    stateful::stateful_object_runtime_t objects;
    objects.replace_placement_candidates (
      {{"reply-claim-mesh", "reply-claim-target", {"actor"},
        100, 16, 0, 8, 0}});
    const auto actor = create_ready (objects);

    const std::string source_routing_id_text = "reply-claim-source";
    const auto source_routing_id = bytes (source_routing_id_text);
    constexpr std::uint64_t source_generation = 7;
    constexpr std::uint64_t request_sequence = 42;
    constexpr std::uint64_t correlation = 99;
    const protocol::wire_operation_id_t operation{11, 12};
    const protocol::actor_route_fence_t fence{
      actor.key,
      actor.object_generation,
      target_descriptor.node_routing_id,
      target_descriptor.lifecycle_generation,
      actor.authority_owner_generation,
      1};

    stateful::raw_stateful_dispatch_t dispatch (
      objects,
      target,
      [&] (const stateful::accepted_record_authority_query_t &query)
        -> std::optional<stateful::accepted_record_authority_t> {
          assert (query.source_node_routing_id == source_routing_id);
          assert (query.source_node_generation == source_generation);
          return stateful::accepted_record_authority_t{
            {"reply-claim-source-owner",
             1,
             source_routing_id,
             source_generation},
            1};
      });

    const std::string mailbox_owner = "actor:" + actor.key;
    assert (target.mailbox ().try_enqueue (
      mesh::service_mailbox_record_t{
        mailbox_owner,
        mesh::service_mailbox_domain_t::application,
        {protocol::encode_actor_message_header (
           protocol::command::actorRequest,
           std::nullopt,
           fence,
           operation,
           correlation),
         protocol::encode_application_payload (
           {"ActorRequest", "application/json", bytes ("request")})},
        source_routing_id,
        request_sequence,
        correlation,
        source_generation,
        std::make_pair (operation.high, operation.low)}));
    assert (dispatch.ingest (actor) == stateful::stateful_error_t::none);
    const auto [claim_error, delivery] = dispatch.try_claim (actor);
    assert (claim_error == stateful::stateful_error_t::none);
    assert (delivery && delivery->request);

    assert (target.mailbox ().try_enqueue (
      mesh::service_mailbox_record_t{
        mailbox_owner,
        mesh::service_mailbox_domain_t::application,
        {{0x01}}}));
    assert (objects.enqueue (
              actor,
              stateful::turn_domain_t::application,
              {777, {0x02}})
            == stateful::stateful_error_t::none);

    std::atomic<int> terminal_count{0};
    auto completion = std::async (
      std::launch::async,
      [pending = dispatch.complete_async (
         *delivery,
         protocol::application_payload_t{
           "ActorReply", "application/json", bytes ("reply")}),
       &terminal_count] () mutable {
          const auto result = pending.result ().value ();
          terminal_count.fetch_add (1, std::memory_order_relaxed);
          return result;
      });

    assert (completion.wait_for (250ms) == std::future_status::ready);
    assert (completion.get () == stateful::stateful_error_t::conflict);
    assert (terminal_count.load (std::memory_order_relaxed) == 1);

    auto released_mailbox = target.mailbox ().try_claim_owner (
      mesh::service_mailbox_domain_t::application,
      mailbox_owner,
      1,
      1024);
    assert (released_mailbox);
    assert (target.mailbox ().release (*released_mailbox));
    const auto [released_error, released] = objects.try_claim (
      actor, stateful::turn_domain_t::application);
    assert (released_error == stateful::stateful_error_t::none);
    assert (released && released->sequence == 777);
    assert (objects.complete_claim (
              actor, stateful::turn_domain_t::application)
            == stateful::stateful_error_t::none);

    zlink::dealer_socket_t source (*context);
    source.set_routing_id (
      zlink::routing_id_t::from (source_routing_id_text));
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
    protocol::client_server_server_admission_t descriptor{
      "reply-claim-channel",
      bytes ("reply-claim-server"),
      17,
      1,
      100,
      mesh::service_node_state_t::serving,
      "default",
      16 * 1024 * 1024,
      "tcp://127.0.0.1:0"};
    client_server::raw_client_server_server_t server (
      {{descriptor}}, context);
    server.start ();

    const std::string source_routing_id_text =
      "client-server-reply-claim-source";
    const auto source_routing_id = bytes (source_routing_id_text);
    constexpr std::uint64_t request_sequence = 51;
    constexpr std::uint64_t correlation = 61;
    const std::string mailbox_owner = descriptor.channel_name;
    /* The reply path reads the request's channel-envelope header, so the
     * synthetic record carries a real envelope header frame. */
    messaging::envelope_header_t request_header;
    request_header.kind = messaging::message_kind_t::request;
    request_header.channel_name = descriptor.channel_name;
    request_header.message_name = "ClientServerRequest";
    request_header.correlation_id = "reply-claim-corr";
    const auto request_header_bytes =
      messaging::envelope_codec_t{}.encode_header (request_header)
        .to_bytes ();
    assert (server.mailbox ().try_enqueue (
      mesh::service_mailbox_record_t{
        mailbox_owner,
        mesh::service_mailbox_domain_t::application,
        {request_header_bytes, bytes ("request")},
        source_routing_id,
        request_sequence,
        correlation}));
    assert (server.mailbox ().try_enqueue (
      mesh::service_mailbox_record_t{
        mailbox_owner,
        mesh::service_mailbox_domain_t::application,
        {{0x02}}}));
    auto claim = server.mailbox ().try_claim_owner (
      mesh::service_mailbox_domain_t::application,
      mailbox_owner,
      1,
      1024);
    assert (claim && claim->records.size () == 1);
    const auto request = claim->records.front ();
    auto retained_claim =
      std::make_shared<mesh::service_mailbox_claim_t> (std::move (*claim));

    std::atomic<int> terminal_count{0};
    auto completion = std::async (
      std::launch::async,
      [&server, retained_claim, request, &terminal_count] () mutable {
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

    auto released = server.mailbox ().try_claim_owner (
      mesh::service_mailbox_domain_t::application,
      mailbox_owner,
      1,
      1024);
    assert (released && released->records.size () == 1);
    assert (server.mailbox ().release (*released));

    zlink::dealer_socket_t source (*context);
    source.set_routing_id (
      zlink::routing_id_t::from (source_routing_id_text));
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
      mesh::raw_mesh_node_options_t{descriptor ("ingest-reply-target")},
      context);
    target.start ();

    stateful::stateful_object_runtime_t objects;
    objects.replace_placement_candidates (
      {{"reply-claim-mesh", "ingest-reply-target", {"actor"},
        100, 16, 0, 8, 0}});
    const auto actor = create_ready (objects);
    stateful::raw_stateful_dispatch_t dispatch (objects, target);

    const std::string source_routing_id_text = "ingest-reply-source";
    const auto source_routing_id = bytes (source_routing_id_text);
    const std::string mailbox_owner = "actor:" + actor.key;
    assert (target.mailbox ().try_enqueue (
      mesh::service_mailbox_record_t{
        mailbox_owner,
        mesh::service_mailbox_domain_t::application,
        {{0xff}, {0x00}},
        source_routing_id,
        71,
        81,
        1}));
    assert (target.mailbox ().try_enqueue (
      mesh::service_mailbox_record_t{
        mailbox_owner,
        mesh::service_mailbox_domain_t::application,
        {{0x01}}}));

    assert (dispatch.ingest (actor) == stateful::stateful_error_t::invalid);
    std::optional<mesh::service_mailbox_claim_t> released;
    const auto deadline = std::chrono::steady_clock::now () + 250ms;
    while (!released && std::chrono::steady_clock::now () < deadline) {
        released = target.mailbox ().try_claim_owner (
          mesh::service_mailbox_domain_t::application,
          mailbox_owner,
          1,
          1024);
        if (!released)
            std::this_thread::sleep_for (1ms);
    }
    assert (released && released->records.size () == 1);
    assert (target.mailbox ().release (*released));

    zlink::dealer_socket_t source (*context);
    source.set_routing_id (
      zlink::routing_id_t::from (source_routing_id_text));
    source.connect (target.endpoint ());
    std::this_thread::sleep_for (50ms);
    zlink::received_t ghost;
    assert (source.recv (ghost, zlink::recv_flags_t::dontwait) != 0);

    source.close ();
    target.close ();
}

} // namespace

int main ()
{
    verify_stateful_claim_releases_after_one_shot_reply_terminal ();
    verify_client_server_claim_releases_after_one_shot_reply_terminal ();
    verify_stateful_ingest_rejection_releases_on_one_shot_terminal ();
    return 0;
}
