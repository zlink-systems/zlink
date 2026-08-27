/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/actors/actor.hpp>

#include "runtime/actors/actor_ref_access.hpp"
#include "runtime/execution/state_lane.hpp"
#include <zlink/framework/contracts/channels/channel.hpp>
#include <zlink/framework/contracts/dispatch/execution.hpp>

#include <functional>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace zlink::framework::runtime::protocol
{
struct bound_session_replaced_t;
struct bound_session_send_t;
struct actor_route_fence_t;
struct session_relocation_route_t;
}

namespace zlink::framework::runtime::stateful
{
struct stream_binding_t;
}

namespace zlink::framework::detail
{

class bound_session_delivery_fence_t;

inline constexpr std::string_view local_actor_node_placeholder = "local";
inline constexpr std::string_view bound_session_relay_binding_key =
  "__zlink.boundSessionRelayBinding";
inline constexpr std::string_view bound_session_relay_sequence_key =
  "__zlink.boundSessionRelaySequence";

struct bound_session_relay_source_t
{
    zlink::routing_id_t session_rid;
    std::uint64_t binding_generation = 0;
    std::uint64_t session_sequence = 0;
};

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
    //  A relocation-target route does not know the source's relay
    //  high-water (command 43/44 carry no numeric high-water by spec 20).
    //  The first positive sequence admitted on the exact route becomes the
    //  baseline; ordinary initial binds keep the known baseline 0.
    bool session_sequence_baseline_unknown = false;

    friend bool operator== (const actor_bound_session_route_t &,
                            const actor_bound_session_route_t &) = default;
};

struct actor_bound_session_transition_t
{
    std::optional<actor_bound_session_route_t> current;
    std::optional<actor_bound_session_route_t> previous;
    bool changed = false;
};

struct session_relay_completion_fence_t
{
    std::string actor_id;
    std::uint64_t object_generation = 0;
    std::string source_node_rid;
    std::string session_rid;
    std::uint64_t binding_generation = 0;
    std::uint64_t session_sequence = 0;

    friend bool operator< (const session_relay_completion_fence_t &left,
                           const session_relay_completion_fence_t &right) noexcept
    {
        return std::tie (left.actor_id, left.object_generation, left.source_node_rid,
                         left.session_rid, left.binding_generation, left.session_sequence)
               < std::tie (right.actor_id, right.object_generation, right.source_node_rid,
                           right.session_rid, right.binding_generation, right.session_sequence);
    }
};

using bound_session_sink_t =
  std::function<task_t<void> (std::string, stream_codec_t, const zlink::message_t &)>;
using bound_session_replacement_handler_t =
  std::function<bool (const runtime::protocol::bound_session_replaced_t &)>;

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
    std::optional<zlink::routing_id_t> source_session_rid;
    std::uint64_t next_session_relay_sequence = 1;
};

struct actor_session_binding_snapshot_t
{
    std::optional<actor_record_t> record;
    std::shared_ptr<bound_session_sink_t> sink;
};

class session_actor_binding_context_t
{
  public:
    std::mutex mutex;
    std::optional<stream_t> stream;
    std::shared_ptr<stream_state_t> stream_state;
    std::string session_id;
    stream_codec_t codec = stream_codec_t::message_pack;
    std::map<std::string, std::uint64_t> actor_tokens;
    std::set<std::string> ready_actors;
    std::map<std::string, std::weak_ptr<stream_state_t>> actor_streams;
    std::function<task_t<void> (actor_ref_t, std::uint64_t)> native_binder;
};

class session_actor_manager_access_t
{
  public:
    static void attach (session_actor_manager_t &manager, stream_t stream);
    static void set_codec (session_actor_manager_t &manager, stream_codec_t codec);
    static void bind_native (session_actor_manager_t &manager,
                             std::function<task_t<void> (actor_ref_t, std::uint64_t)> binder);
    static void disconnect (session_actor_manager_t &manager) noexcept;
};

struct relayed_frame_t
{
    actor_ref_t actor;
    stream_header_t header;
    zlink::message_t payload;
};

/* Bound for frames parked while no relay dispatcher is configured
 * (async-execution-policy §1.3): shares the session-relay waiter bound. */
inline constexpr std::size_t relayed_frame_capacity = 1024;

class actor_gateway_state_t
{
  public:
    template<typename Work>
    decltype(auto) sync (Work &&work) const
    {
        return lane.run ([work = std::forward<Work> (work)] () mutable
                         -> decltype(auto) { return std::invoke (work); }).get ();
    }

    using create_dispatcher_t = std::function<result_t<actor_ref_t> (
      std::string, std::string, const std::optional<zlink::message_t> &)>;
    using join_spot_dispatcher_t =
      std::function<task_t<actor_join_reply_t> (const actor_ref_t &,
                                                spot_id_t,
                                                const zlink::message_t &,
                                                std::string,
                                                std::string,
                                                std::chrono::milliseconds)>;
    using join_entry_spot_dispatcher_t = std::function<result_t<actor_join_reply_t> (
      const actor_ref_t &, const zlink::message_t &, std::chrono::milliseconds)>;
    using relay_dispatcher_t = std::function<task_t<std::optional<zlink::message_t>> (
      const actor_ref_t &,
      actor_context_t,
      const stream_header_t &,
      const zlink::message_t &,
      std::optional<bound_session_relay_source_t>)>;
    using disconnect_dispatcher_t = std::function<task_t<void> (const actor_ref_t &)>;
    using bound_session_registrar_t = std::function<result_t<void> (const actor_ref_t &)>;
    using bound_session_sender_t = std::function<task_t<result_t<void>> (
      const actor_ref_t &, std::uint64_t, const stream_header_t &, const zlink::message_t &)>;
    using membership_query_t = std::function<std::optional<spot_id_t> (const actor_ref_t &)>;
    using join_barrier_reserver_t =
      std::function<result_t<std::shared_ptr<deferred_barrier_t>> (const actor_ref_t &)>;

    struct pending_session_relay_t
    {
        std::function<task_t<void> ()> dispatch;
        std::shared_ptr<task_completion_source_t<void>> completion;
        std::string packet_name;
    };

    struct pending_bound_session_send_t
    {
        std::function<task_t<result_t<void>> ()> dispatch;
        std::shared_ptr<bound_session_delivery_fence_t> completion_fence;
    };

    runtime::offload_executor_t lane_executor;
    mutable runtime::state_lane_t lane{lane_executor};
    std::map<std::string, actor_record_t> actors_by_id;
    std::map<std::string, std::shared_ptr<bound_session_sink_t>> bound_session_sinks;
    std::map<std::string, std::vector<std::shared_ptr<bound_session_replacement_handler_t>>>
      bound_session_replacement_handlers;
    std::vector<relayed_frame_t> relayed_frames;
    std::vector<relayed_frame_t> bound_session_pushes;
    std::map<std::string, std::deque<pending_session_relay_t>> pending_session_relays;
    std::set<std::string> active_session_relays;
    std::map<std::string, std::deque<pending_bound_session_send_t>> pending_bound_session_sends;
    std::set<std::string> active_bound_session_sends;
    std::map<std::string, std::weak_ptr<bound_session_delivery_fence_t>>
      join_completion_delivery_fences;
    std::set<session_relay_completion_fence_t> active_session_relay_completions;
    create_dispatcher_t create_dispatcher;
    join_spot_dispatcher_t join_spot_dispatcher;
    join_entry_spot_dispatcher_t join_entry_spot_dispatcher;
    relay_dispatcher_t relay_dispatcher;
    bool offload_session_relay = false;
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

    std::weak_ptr<actor_gateway_state_t> weak_state () const noexcept { return _state; }

    session_actor_manager_t manager () const;
    std::vector<relayed_frame_t> relayed_frames () const;
    std::vector<relayed_frame_t> bound_session_pushes () const;
    std::optional<actor_bound_session_route_t>
    bound_session_route (const actor_ref_t &actor_ref) const;
    std::optional<actor_bound_session_route_t>
    resolve_bound_session_push_route (const actor_ref_t &actor_ref,
                                      const actor_bound_session_route_t &staged_route) const;
    bool actor_bound (std::string actor_id) const;
    bool actor_disconnected (std::string actor_id) const;
    actor_context_t actor_context (const actor_ref_t &actor_ref,
                                   std::uint64_t source_binding_generation = 0) const;
    bool same_context_source_fence (const actor_context_t &left,
                                    const actor_context_t &right) const noexcept;
    result_t<void> update_actor_ref (const actor_ref_t &actor_ref);
    result_t<void> destroy_actor (const actor_ref_t &actor_ref);
    actor_session_binding_snapshot_t
    bind_session_stream (std::string actor_id,
                         stream_t stream,
                         stream_codec_t codec = stream_codec_t::message_pack,
                         std::string session_id = {},
                         std::uint64_t binding_token = 0,
                         std::optional<actor_ref_t> actor_ref = std::nullopt);
    result_t<void>
    bind_session_route (actor_ref_t actor_ref,
                        route_client_t route_client,
                        std::string route_channel_name,
                        zlink::routing_id_t target_node_rid,
                        stream_codec_t codec = stream_codec_t::message_pack,
                        bool replace_existing = true,
                        std::optional<zlink::routing_id_t> session_rid = std::nullopt);
    result_t<void> bind_session_sink (
      actor_ref_t actor_ref,
      std::function<task_t<void> (std::string, stream_codec_t, const zlink::message_t &)> sink,
      stream_codec_t codec = stream_codec_t::message_pack,
      bool replace_existing = true);
    result_t<void> bind_session_route (
      actor_ref_t actor_ref,
      std::function<task_t<void> (std::string, stream_codec_t, const zlink::message_t &)> sink,
      actor_bound_session_route_t route,
      stream_codec_t codec = stream_codec_t::message_pack,
      bool replace_existing = true);
    result_t<actor_bound_session_transition_t>
    replace_session_route (actor_ref_t actor_ref,
                           bound_session_sink_t sink,
                           actor_bound_session_route_t route,
                           stream_codec_t codec = stream_codec_t::message_pack);
    result_t<void>
    record_bound_session_route (const actor_ref_t &actor_ref,
                                zlink::routing_id_t node_rid,
                                std::optional<zlink::routing_id_t> session_rid = std::nullopt,
                                std::uint64_t node_generation = 0,
                                std::uint64_t authority_owner_generation = 0,
                                std::uint64_t owner_lease_generation = 0,
                                std::uint64_t binding_generation = 0,
                                std::uint64_t binding_token = 0,
                                std::uint64_t session_sequence = 0,
                                bool session_sequence_baseline_unknown = false);
    result_t<actor_bound_session_transition_t>
    record_bound_session_route_transition (const actor_ref_t &actor_ref,
                                           actor_bound_session_route_t route);
    result_t<void> record_session_relay_source (const actor_ref_t &actor_ref,
                                                zlink::routing_id_t session_rid,
                                                std::uint64_t binding_generation);
    result_t<void>
    admit_session_relay (const actor_ref_t &actor_ref,
                         const zlink::routing_id_t &source_node_rid,
                         const zlink::routing_id_t &session_rid,
                         std::uint64_t binding_generation,
                         std::uint64_t session_sequence,
                         const runtime::protocol::actor_route_fence_t *target_route = nullptr);
    result_t<void> begin_session_relay_completion (const actor_ref_t &actor_ref,
                                                   const zlink::routing_id_t &source_node_rid,
                                                   const zlink::routing_id_t &session_rid,
                                                   std::uint64_t binding_generation,
                                                   std::uint64_t session_sequence);
    result_t<void> complete_session_relay (const actor_ref_t &actor_ref,
                                           const zlink::routing_id_t &source_node_rid,
                                           const zlink::routing_id_t &session_rid,
                                           std::uint64_t binding_generation,
                                           std::uint64_t session_sequence);
    result_t<void> retire_bound_session_route (const actor_ref_t &actor_ref,
                                               const zlink::routing_id_t &session_owner_node,
                                               const zlink::routing_id_t &session_rid,
                                               std::uint64_t retired_binding_generation);
    bool
    commit_session_relocation_route (const runtime::protocol::session_relocation_route_t &route,
                                     const runtime::stateful::stream_binding_t &previous,
                                     const runtime::stateful::stream_binding_t &target);
    bool prepare_session_relocation_target_route (
      const runtime::protocol::session_relocation_route_t &route,
      std::uint64_t target_owner_lease_generation);
    bool confirm_session_remote_tenure (const runtime::protocol::bound_session_send_t &send);
    void unbind_session_stream (std::string actor_id,
                                std::string session_id = {},
                                std::uint64_t binding_token = 0);
    void restore_session_stream (std::string actor_id,
                                 const std::string &session_id,
                                 std::uint64_t binding_token,
                                 actor_session_binding_snapshot_t snapshot);
    result_t<void> dispatch_bound_session_send (const actor_ref_t &actor_ref,
                                                std::string packet_name,
                                                stream_codec_t codec,
                                                const zlink::message_t &payload) const;
    using admitted_bound_session_delivery_t =
      std::function<result_t<void> (std::string, stream_codec_t, const zlink::message_t &)>;
    std::optional<admitted_bound_session_delivery_t>
    admit_bound_session_delivery (const actor_ref_t &actor_ref,
                                  std::uint64_t binding_generation) const;
    std::shared_ptr<bound_session_replacement_handler_t>
    register_bound_session_replacement_handler (const zlink::routing_id_t &session_rid,
                                                bound_session_replacement_handler_t handler);
    void unregister_bound_session_replacement_handler (
      const zlink::routing_id_t &session_rid,
      const std::shared_ptr<bound_session_replacement_handler_t> &handler);
    bool dispatch_bound_session_replaced (
      const runtime::protocol::bound_session_replaced_t &replacement) const;
    void on_join_spot (actor_gateway_state_t::join_spot_dispatcher_t dispatcher);
    void on_create (actor_gateway_state_t::create_dispatcher_t dispatcher);
    void on_join_entry_spot (actor_gateway_state_t::join_entry_spot_dispatcher_t dispatcher);
    void on_relay (actor_gateway_state_t::relay_dispatcher_t dispatcher);
    void offload_session_relay (bool enabled = true);
    void on_membership (actor_gateway_state_t::membership_query_t query);
    void on_join_barrier (actor_gateway_state_t::join_barrier_reserver_t reserver);
    void on_disconnect (actor_gateway_state_t::disconnect_dispatcher_t dispatcher);
    void on_bound_session (actor_gateway_state_t::bound_session_registrar_t registrar);
    void on_bound_session_send (actor_gateway_state_t::bound_session_sender_t sender);
    std::shared_ptr<bound_session_delivery_fence_t>
    begin_join_completion_delivery_fence (const actor_ref_t &actor_ref);
    void settle_join_completion_delivery_fence (
      const actor_ref_t &actor_ref,
      const std::shared_ptr<bound_session_delivery_fence_t> &fence,
      result_t<void> callback_result,
      std::function<void (result_t<void>)> settled);
    /* Stage traces emit only at detailed; callers building stage/result
     * strings must gate on trace_bound_session_send_stage_enabled() so the
     * off/errors/normal hot path pays no allocation (spec 26 §4). */
    bool trace_bound_session_send_stage_enabled () const noexcept;
    void trace_bound_session_send_stage (const std::string &actor_id,
                                         std::string_view stage,
                                         std::string_view result = {}) const;
    void bind_serializers (serializer_registry_t &serializers);
    void set_dispatch (dispatch_options_t options);

  private:
    std::shared_ptr<actor_gateway_state_t> _state;
};

} // namespace zlink::framework::detail
