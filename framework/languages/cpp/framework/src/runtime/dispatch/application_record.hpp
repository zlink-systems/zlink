/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/operations/call_id.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/framework/contracts/actors/actor.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zlink::framework::runtime::mesh
{
struct service_mailbox_record_t;
}

namespace zlink::framework::runtime::host
{
using call_id_t = runtime::call_id_t;

enum class record_kind_t
{
    node_send,
    node_request,
    channel_send,
    channel_request,
    spot_send,
    spot_request,
    actor_send,
    actor_request,
    completion,
    send_ready,
    spot_control,
    spot_multicast
};

enum class ready_domain_t
{
    application,
    infrastructure
};

enum class owner_kind_t
{
    node,
    channel,
    spot,
    actor
};

enum class operation_kind_t
{
    none,
    actor_join
};

enum class lifecycle_kind_t
{
    joined,
    left
};

enum class actor_join_result_t
{
    accepted,
    rejected
};

enum class join_admission_t
{
    accepted,
    rejected
};

struct actor_join_completion_t
{
    join_admission_t join_result = join_admission_t::rejected;
    actor_ref_t current_actor;
};

struct actor_control_t
{
    lifecycle_kind_t kind = lifecycle_kind_t::joined;
    actor_ref_t current_actor;
};

struct send_ready_data_t
{
    enum class destination_kind_t
    {
        node,
        channel,
        spot,
        actor,
        bound_session
    };

    destination_kind_t destination_kind = destination_kind_t::node;
    zlink::routing_id_t target_node_rid = zlink::routing_id_t::from (std::uint32_t{0});
    std::string target_spot_id;
    std::string channel_name;
    actor_ref_t target_actor;
};

class public_host_runtime_t;

struct reply_token_t
{
    std::weak_ptr<public_host_runtime_t> host;
    std::shared_ptr<mesh::service_mailbox_record_t> request;
    std::function<bool (const std::vector<zlink::message_t> &)> local_reply;
    std::function<bool (actor_join_result_t, const std::vector<zlink::message_t> &)>
      local_actor_join;
};

struct receive_record_t
{
    record_kind_t kind = record_kind_t::node_send;
    ready_domain_t domain = ready_domain_t::application;
    call_id_t operation_id;
    operation_kind_t operation_kind = operation_kind_t::none;
    zlink::routing_id_t source_node_rid = zlink::routing_id_t::from (std::uint32_t{0});
    std::optional<zlink::routing_id_t> source_session_rid;
    std::uint64_t source_binding_generation = 0;
    std::uint64_t source_session_sequence = 0;
    /* Preserve the target fence until the Spot owner admits the message.
     * The owner must be able to reject stale work before body deserialization. */
    std::optional<protocol::spot_route_fence_t> spot_route;
    std::optional<protocol::actor_route_fence_t> actor_route;
    std::uint8_t message_follow_hop_count = 0;
    std::uint64_t reply_route_id = 0;
    std::string channel_name;
    std::string topic;
    int terminal_result = 0;
    int failure_errno = 0;
    reply_token_t reply_token;
    std::optional<actor_join_completion_t> join_completion;
    std::optional<actor_control_t> actor_control;
    std::optional<send_ready_data_t> send_ready;
    /* The owner queue keeps the original byte reservation while this record
     * is claimed. Its handler terminal releases that reservation once. Stateful
     * gate handoffs carry this exact byte charge without a second admission;
     * their terminal retains release only as a failure fallback. */
    std::size_t transferred_owner_byte_cost = 0;
    std::function<void ()> retain_mailbox_reservation;
    std::function<void ()> release_mailbox_reservation;
    std::function<void ()> complete_stateful_dispatch;
    std::function<void ()> before_application_handler;
};

struct ready_record_t
{
    owner_kind_t owner_kind = owner_kind_t::node;
    ready_domain_t domain = ready_domain_t::application;
    std::string spot_id;
    std::optional<actor_ref_t> actor;
    std::string channel_name;
};

struct local_application_dispatch_t
{
    ready_record_t owner;
    receive_record_t record;
    std::vector<zlink::message_t> parts;
};

} // namespace zlink::framework::runtime::host
