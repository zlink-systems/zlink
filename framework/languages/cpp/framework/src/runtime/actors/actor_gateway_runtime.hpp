/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/actors/actor.hpp>

#include "runtime/actors/actor_ref_access.hpp"
#include <zlink/framework/contracts/channels/channel.hpp>
#include <zlink/framework/contracts/dispatch/execution.hpp>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zlink::framework::detail
{

inline constexpr std::string_view local_actor_node_placeholder = "local";

inline bool is_local_actor_ref (const actor_ref_t &actor_ref)
{
    return actor_ref.node_rid ().value () == local_actor_node_placeholder;
}

class stream_relay_dispatch_scope_t final
{
  public:
    explicit stream_relay_dispatch_scope_t (stream_header_t header);
    ~stream_relay_dispatch_scope_t () noexcept;

    stream_relay_dispatch_scope_t (const stream_relay_dispatch_scope_t &) = delete;
    stream_relay_dispatch_scope_t &operator= (const stream_relay_dispatch_scope_t &) = delete;
};

std::optional<stream_header_t> current_stream_relay_dispatch ();

struct actor_bound_session_route_t
{
    zlink::routing_id_t node_rid;
    std::optional<zlink::routing_id_t> session_rid;
    std::uint64_t object_generation = 0;
    std::uint64_t node_generation = 0;
    std::uint64_t authority_owner_generation = 0;
    std::uint64_t owner_lease_generation = 0;
    std::uint64_t binding_generation = 0;
    std::uint64_t binding_token = 0;
    std::uint64_t session_sequence = 0;
};

using bound_session_sink_t =
  std::function<task_t<void> (std::string,
                              stream_codec_t,
                              const zlink::message_t &)>;

struct actor_record_t
{
    actor_ref_t ref;
    bool bound = false;
    bool disconnected = false;
    stream_codec_t bound_session_codec = stream_codec_t::message_pack;
    bool bound_session_stream_sink = false;
    std::optional<zlink::message_t> create_payload;
    std::optional<actor_bound_session_route_t> bound_session_route;
    std::string binding_session_id;
    std::uint64_t binding_token = 0;
    std::uint64_t source_binding_generation = 0;
};

class session_actor_binding_context_t
{
  public:
    std::mutex mutex;
    std::optional<stream_t> stream;
    std::string session_id;
    stream_codec_t codec = stream_codec_t::message_pack;
    std::map<std::string, std::uint64_t> actor_tokens;
    std::function<result_t<void> (const actor_ref_t &)> native_binder;
};

class session_actor_manager_access_t
{
  public:
    static void attach (session_actor_manager_t &manager, stream_t stream);
    static void set_codec (session_actor_manager_t &manager, stream_codec_t codec);
    static void bind_native (
      session_actor_manager_t &manager,
      std::function<result_t<void> (const actor_ref_t &)> binder);
    static void disconnect (session_actor_manager_t &manager) noexcept;
};

struct relayed_frame_t
{
    actor_ref_t actor;
    stream_header_t header;
    zlink::message_t payload;
};

class actor_gateway_state_t
{
  public:
    mutable std::recursive_mutex mutex;
    using create_dispatcher_t = std::function<result_t<actor_ref_t> (
      std::string, std::string, const std::optional<zlink::message_t> &)>;
    using join_spot_dispatcher_t = std::function<result_t<actor_join_reply_t> (
      const actor_ref_t &, spot_id_t, const zlink::message_t &,
      std::chrono::milliseconds)>;
    using join_entry_spot_dispatcher_t = std::function<result_t<actor_join_reply_t> (
      const actor_ref_t &, const zlink::message_t &, std::chrono::milliseconds)>;
    using relay_dispatcher_t = std::function<result_t<std::optional<zlink::message_t>> (
      const actor_ref_t &, actor_context_t, const stream_header_t &, const zlink::message_t &)>;
    using disconnect_dispatcher_t = std::function<result_t<void> (const actor_ref_t &)>;
    using bound_session_registrar_t = std::function<result_t<void> (const actor_ref_t &)>;
    using bound_session_sender_t = std::function<result_t<void> (
      const actor_ref_t &, std::uint64_t, const stream_header_t &, const zlink::message_t &)>;
    using membership_query_t = std::function<std::optional<spot_id_t> (const actor_ref_t &)>;
    using join_barrier_reserver_t =
      std::function<result_t<std::shared_ptr<deferred_barrier_t>> (
        const actor_ref_t &)>;

    std::map<std::string, actor_record_t> actors_by_id;
    std::map<std::string, std::shared_ptr<bound_session_sink_t>>
      bound_session_sinks;
    std::vector<relayed_frame_t> relayed_frames;
    std::vector<relayed_frame_t> bound_session_pushes;
    create_dispatcher_t create_dispatcher;
    join_spot_dispatcher_t join_spot_dispatcher;
    join_entry_spot_dispatcher_t join_entry_spot_dispatcher;
    relay_dispatcher_t relay_dispatcher;
    disconnect_dispatcher_t disconnect_dispatcher;
    bound_session_registrar_t bound_session_registrar;
    bound_session_sender_t bound_session_sender;
    membership_query_t membership_query;
    join_barrier_reserver_t join_barrier_reserver;
    serializer_registry_t *serializers = nullptr;
    dispatch_options_t dispatch;
    std::uint64_t next_binding_token = 1;
};

class actor_gateway_runtime_t
{
  public:
    actor_gateway_runtime_t ();
    explicit actor_gateway_runtime_t (std::shared_ptr<actor_gateway_state_t> state);

    session_actor_manager_t manager () const;
    std::vector<relayed_frame_t> relayed_frames () const;
    std::vector<relayed_frame_t> bound_session_pushes () const;
    std::optional<actor_bound_session_route_t>
    bound_session_route (const actor_ref_t &actor_ref) const;
    bool actor_bound (std::string actor_id) const;
    bool actor_disconnected (std::string actor_id) const;
    actor_context_t actor_context (const actor_ref_t &actor_ref,
                                   std::uint64_t source_binding_generation = 0) const;
    bool same_context_source_fence (const actor_context_t &left,
                                    const actor_context_t &right) const noexcept;
    result_t<void> update_actor_ref (const actor_ref_t &actor_ref);
    result_t<void> destroy_actor (const actor_ref_t &actor_ref);
    void bind_session_stream (std::string actor_id,
                              stream_t stream,
                              stream_codec_t codec = stream_codec_t::message_pack,
                              std::string session_id = {},
                              std::uint64_t binding_token = 0);
    result_t<void> bind_session_route (actor_ref_t actor_ref,
                                       route_client_t route_client,
                                       std::string route_channel_name,
                                       zlink::routing_id_t target_node_rid,
                                       stream_codec_t codec = stream_codec_t::message_pack,
                                       bool replace_existing = true,
                                       std::optional<zlink::routing_id_t> session_rid = std::nullopt);
    result_t<void>
    bind_session_sink (actor_ref_t actor_ref,
                       std::function<task_t<void> (std::string,
                                                   stream_codec_t,
                                                   const zlink::message_t &)> sink,
                       stream_codec_t codec = stream_codec_t::message_pack,
                       bool replace_existing = true);
    result_t<void> record_bound_session_route (
      const actor_ref_t &actor_ref,
      zlink::routing_id_t node_rid,
      std::optional<zlink::routing_id_t> session_rid = std::nullopt,
      std::uint64_t node_generation = 0,
      std::uint64_t authority_owner_generation = 0,
      std::uint64_t owner_lease_generation = 0,
      std::uint64_t binding_generation = 0,
      std::uint64_t binding_token = 0,
      std::uint64_t session_sequence = 0);
    void unbind_session_stream (std::string actor_id,
                                std::string session_id = {},
                                std::uint64_t binding_token = 0);
    result_t<void> dispatch_bound_session_send (const actor_ref_t &actor_ref,
                                                std::string packet_name,
                                                stream_codec_t codec,
                                                const zlink::message_t &payload) const;
    void on_join_spot (actor_gateway_state_t::join_spot_dispatcher_t dispatcher);
    void on_create (actor_gateway_state_t::create_dispatcher_t dispatcher);
    void on_join_entry_spot (actor_gateway_state_t::join_entry_spot_dispatcher_t dispatcher);
    void on_relay (actor_gateway_state_t::relay_dispatcher_t dispatcher);
    void on_membership (actor_gateway_state_t::membership_query_t query);
    void on_join_barrier (
      actor_gateway_state_t::join_barrier_reserver_t reserver);
    void on_disconnect (actor_gateway_state_t::disconnect_dispatcher_t dispatcher);
    void on_bound_session (actor_gateway_state_t::bound_session_registrar_t registrar);
    void on_bound_session_send (actor_gateway_state_t::bound_session_sender_t sender);
    void bind_serializers (serializer_registry_t &serializers);
    void set_dispatch (dispatch_options_t options);

  private:
    std::shared_ptr<actor_gateway_state_t> _state;
};

} // namespace zlink::framework::detail
