/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/framework/contracts/channels/call.hpp>
#include <zlink/framework/contracts/channels/channel.hpp>
#include <zlink/framework/contracts/codecs/serializer.hpp>
#include <zlink/framework/contracts/configuration/services.hpp>
#include <zlink/framework/contracts/detail/handler_invocation.hpp>
#include <zlink/framework/contracts/detail/message_name.hpp>
#include <zlink/framework/contracts/messaging/message.hpp>
#include <zlink/framework/contracts/messaging/message_context.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/errors/result.hpp>
#include <zlink/framework/contracts/placement.hpp>
#include <zlink/framework/contracts/spots/spot_identity.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>
#include <zlink/framework/contracts/timers/timer.hpp>
#include <zlink/framework/contracts/workers/worker.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <concepts>
#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <stop_token>
#include <thread>
#include <tuple>
#include <typeindex>
#include <type_traits>
#include <utility>
#include <vector>

namespace zlink::framework
{

namespace detail
{
class spot_node_builder_state_t;
class spot_context_state_t;
class spot_context_access_t;
struct mesh_node_builder_state_t;
void drain_spot_node_executors (spot_node_builder_state_t &node);
void cancel_spot_node_dispatch_queues (spot_node_builder_state_t &node);

template <typename THandler, typename TDependencies>
struct timer_handler_factory_t;

template <typename THandler, typename... TDependencies>
struct timer_handler_factory_t<THandler, dependency_list_t<TDependencies...>>
{
    static std::shared_ptr<void> create (service_provider_t *services)
    {
        if constexpr (sizeof...(TDependencies) == 0) {
            static_assert (std::is_default_constructible_v<THandler>,
                           "SPOT timer handler without dependency_types "
                           "must be default constructible");
            return std::make_shared<THandler> ();
        } else {
            static_assert (std::is_constructible_v<THandler, TDependencies &...>,
                           "SPOT timer handler constructor must accept "
                           "dependency_types by reference");
            if (services == nullptr) {
                throw framework_exception_t (
                  framework_error_kind_t::not_configured,
                  "SPOT timer handler dependencies require an activation service scope");
            }
            return std::make_shared<THandler> (
              services->template get_required<TDependencies> ()...);
        }
    }
};
} // namespace detail

class actor_context_t;
class actor_ref_t;
class actor_t;
template <typename TActor>
requires std::derived_from<TActor, actor_t>
class actor_factory_t;
template <typename TActor>
class actor_factory_builder_t;
class spot_publisher_client_t;
class spot_manager_t;
class spot_context_t;
class entry_spot_context_t;
class instance_spot_context_t;
struct spot_actor_join_result_t;
struct actor_create_response_t;
struct spot_create_response_t;

enum class spot_close_reason_t
{
    explicit_close = 0,
    host_shutdown = 1,
    relocation_out = 2,
    idle_evicted = 3
};

struct spot_closing_context_t final
{
    spot_close_reason_t reason = spot_close_reason_t::explicit_close;
    std::chrono::system_clock::time_point deadline;
};

enum class spot_relocation_ready_outcome_t : std::uint8_t
{
    continued = 0,
    relocated = 1
};

struct spot_relocation_ready_completion_t final
{
    spot_relocation_ready_outcome_t outcome =
      spot_relocation_ready_outcome_t::continued;
};

class spot_relocation_ready_call_t
{
  public:
    ~spot_relocation_ready_call_t ();
    spot_relocation_ready_call_t (
      spot_relocation_ready_call_t &&) noexcept;
    spot_relocation_ready_call_t &
    operator= (spot_relocation_ready_call_t &&) = delete;
    spot_relocation_ready_call_t (
      const spot_relocation_ready_call_t &) = delete;
    spot_relocation_ready_call_t &
    operator= (const spot_relocation_ready_call_t &) = delete;

    void defer ();

  private:
    friend class spot_context_t;
    explicit spot_relocation_ready_call_t (
      std::shared_ptr<detail::spot_context_state_t> state);

    std::shared_ptr<detail::spot_context_state_t> _state;
};

template <typename TActor> class spot_t;
template <typename TActor> class entry_spot_t;
class instance_spot_t;

namespace detail
{
enum class actor_join_completion_outcome_t : std::uint8_t
{
    accepted,
    rejected,
    failed
};

using actor_join_completion_callback_t = std::function<task_t<void> (
  void *, actor_join_completion_outcome_t, std::uint64_t, std::uint64_t,
  const actor_ref_t *, const std::optional<message_t> &,
  framework_error_kind_t, bool)>;
} // namespace detail

namespace detail
{
struct spot_accept_reject_result_t
{
    bool accepted = false;
    std::optional<message_t> reply;

    static spot_accept_reject_result_t accept (std::optional<message_t> reply = std::nullopt)
    {
        return spot_accept_reject_result_t{true, std::move (reply)};
    }

    template <typename TReply>
    requires (!std::is_same_v<std::remove_cvref_t<TReply>, message_t>
              && !std::is_same_v<std::remove_cvref_t<TReply>,
                                 zlink::message_t>) static spot_accept_reject_result_t
      accept (TReply reply)
    {
        return accept (message_t::from (std::move (reply)));
    }

    static spot_accept_reject_result_t reject (std::optional<message_t> reply = std::nullopt)
    {
        return spot_accept_reject_result_t{false, std::move (reply)};
    }

    template <typename TReply>
    requires (!std::is_same_v<std::remove_cvref_t<TReply>, message_t>
              && !std::is_same_v<std::remove_cvref_t<TReply>,
                                 zlink::message_t>) static spot_accept_reject_result_t
      reject (TReply reply)
    {
        return reject (message_t::from (std::move (reply)));
    }
};
} // namespace detail

struct spot_actor_join_result_t
{
    bool accepted = false;
    std::optional<message_t> reply;

    static spot_actor_join_result_t accept (std::optional<message_t> reply = std::nullopt)
    {
        auto result = detail::spot_accept_reject_result_t::accept (std::move (reply));
        return spot_actor_join_result_t{result.accepted, std::move (result.reply)};
    }

    template <typename TReply>
    requires (!std::is_same_v<std::remove_cvref_t<TReply>, message_t>
              && !std::is_same_v<std::remove_cvref_t<TReply>,
                                 zlink::message_t>) static spot_actor_join_result_t
      accept (TReply reply)
    {
        return accept (message_t::from (std::move (reply)));
    }

    static spot_actor_join_result_t reject (std::optional<message_t> reply = std::nullopt)
    {
        auto result = detail::spot_accept_reject_result_t::reject (std::move (reply));
        return spot_actor_join_result_t{result.accepted, std::move (result.reply)};
    }

    template <typename TReply>
    requires (!std::is_same_v<std::remove_cvref_t<TReply>, message_t>
              && !std::is_same_v<std::remove_cvref_t<TReply>,
                                 zlink::message_t>) static spot_actor_join_result_t
      reject (TReply reply)
    {
        return reject (message_t::from (std::move (reply)));
    }
};

struct actor_create_response_t
{
    bool accepted = true;
    std::optional<message_t> reply;

    static actor_create_response_t accept (std::optional<message_t> reply = std::nullopt)
    {
        auto result = detail::spot_accept_reject_result_t::accept (std::move (reply));
        return actor_create_response_t{result.accepted, std::move (result.reply)};
    }

    template <typename TReply>
    requires (!std::is_same_v<std::remove_cvref_t<TReply>, message_t>
              && !std::is_same_v<std::remove_cvref_t<TReply>,
                                 zlink::message_t>) static actor_create_response_t
      accept (TReply reply)
    {
        return accept (message_t::from (std::move (reply)));
    }

    static actor_create_response_t reject (std::optional<message_t> reply = std::nullopt)
    {
        auto result = detail::spot_accept_reject_result_t::reject (std::move (reply));
        return actor_create_response_t{result.accepted, std::move (result.reply)};
    }

    template <typename TReply>
    requires (!std::is_same_v<std::remove_cvref_t<TReply>, message_t>
              && !std::is_same_v<std::remove_cvref_t<TReply>,
                                 zlink::message_t>) static actor_create_response_t
      reject (TReply reply)
    {
        return reject (message_t::from (std::move (reply)));
    }
};

enum class spot_create_state_t
{
    existing = 0,
    created = 1,
    rejected = 2
};

class spot_ref_t final
{
  public:
    spot_ref_t (spot_id_t spot_id,
                std::uint64_t object_generation,
                std::string mesh_name,
                node_rid_t node_rid) :
        _spot_id (std::move (spot_id)),
        _object_generation (object_generation),
        _mesh_name (std::move (mesh_name)),
        _node_rid (std::move (node_rid))
    {
        detail::require_spot_id (_spot_id);
        if (_object_generation == 0
            || _object_generation
                 > static_cast<std::uint64_t> (std::numeric_limits<std::int64_t>::max ()))
            throw std::invalid_argument (
              "SpotRef ObjectGeneration must be from 1 through INT64_MAX");
    }

    const spot_id_t &spot_id () const noexcept { return _spot_id; }
    std::uint64_t object_generation () const noexcept
    {
        return _object_generation;
    }
    std::string_view mesh_name () const noexcept { return _mesh_name; }
    const node_rid_t &node_rid () const noexcept { return _node_rid; }

  private:
    spot_id_t _spot_id;
    std::uint64_t _object_generation = 0;
    std::string _mesh_name;
    node_rid_t _node_rid;
};

struct spot_create_response_t
{
    bool accepted = true;
    std::optional<message_t> reply;

    static spot_create_response_t accept (std::optional<message_t> reply = std::nullopt)
    {
        auto result = detail::spot_accept_reject_result_t::accept (std::move (reply));
        return spot_create_response_t{result.accepted, std::move (result.reply)};
    }

    template <typename TReply>
    requires (!std::is_same_v<std::remove_cvref_t<TReply>, message_t>
              && !std::is_same_v<std::remove_cvref_t<TReply>,
                                 zlink::message_t>) static spot_create_response_t
      accept (TReply reply)
    {
        return accept (message_t::from (std::move (reply)));
    }

    static spot_create_response_t reject (std::optional<message_t> reply = std::nullopt)
    {
        auto result = detail::spot_accept_reject_result_t::reject (std::move (reply));
        return spot_create_response_t{result.accepted, std::move (result.reply)};
    }

    template <typename TReply>
    requires (!std::is_same_v<std::remove_cvref_t<TReply>, message_t>
              && !std::is_same_v<std::remove_cvref_t<TReply>,
                                 zlink::message_t>) static spot_create_response_t
      reject (TReply reply)
    {
        return reject (message_t::from (std::move (reply)));
    }
};

/// Runtime-private projection of one inbound Spot or Spot Actor message. Dispatch fills it from the
/// wire envelope and then builds the public `message_context_t` the handler declared, so the frame
/// layout and the internal metadata staging map never reach the public handler surface.
struct spot_inbound_message_t
{
    std::optional<std::string_view> find (std::string_view key) const
    {
        const auto iterator = std::lower_bound (
          values.begin (), values.end (), key,
          [] (const auto &entry, std::string_view value) {
              return std::string_view (entry.first) < value;
          });
        if (iterator == values.end () || iterator->first != key) {
            return std::nullopt;
        }
        return iterator->second;
    }

    bool contains (std::string_view key) const
    {
        const auto iterator = std::lower_bound (
          values.begin (), values.end (), key,
          [] (const auto &entry, std::string_view value) {
              return std::string_view (entry.first) < value;
          });
        return iterator != values.end () && iterator->first == key;
    }

    bool empty () const noexcept { return values.empty (); }

    message_context_t to_message_context (std::string packet_name) const
    {
        message_context_t context;
        context.mesh_name = mesh_name;
        context.packet_name = std::move (packet_name);
        context.content_type = content_type;
        std::map<std::string, std::string> application_metadata;
        for (const auto &[key, value] : values) {
            if (!key.starts_with ("__zlink."))
                application_metadata.emplace (key, value);
        }
        context.metadata = message_metadata_t (std::move (application_metadata));
        context.correlation_id = correlation_id;
        return context;
    }

    publish_message_context_t to_publish_context (std::string packet_name,
                                                  std::string subscription_topic) const
    {
        publish_message_context_t context{to_message_context (std::move (packet_name))};
        context.topic = std::move (subscription_topic);
        context.source = source;
        return context;
    }

    std::string content_type = "application/json";
    std::map<std::string, std::string> values;
    std::optional<std::string> mesh_name;
    std::optional<std::string> correlation_id;
    std::optional<std::string> source;
};

class message_metadata_policy_t
{
  public:
    message_metadata_policy_t &add_forwarded_metadata_key (std::string key)
    {
        if (key.empty () || is_blank (key)) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "metadata key must not be empty");
        }
        _forwarded_keys.insert (std::move (key));
        return *this;
    }

    bool can_forward (std::string_view key) const
    {
        return _forwarded_keys.find (std::string (key)) != _forwarded_keys.end ();
    }

    spot_inbound_message_t project (const std::map<std::string, std::string> &metadata) const
    {
        spot_inbound_message_t projected;
        for (const auto &[key, value] : metadata) {
            if (can_forward (key)) {
                projected.values.emplace (key, value);
            }
        }
        return projected;
    }

  private:
    static bool is_blank (const std::string &value)
    {
        return std::all_of (value.begin (), value.end (),
                            [] (unsigned char ch) { return std::isspace (ch) != 0; });
    }

    std::set<std::string> _forwarded_keys;
};

namespace detail
{
class spot_context_state_t;
class spot_node_builder_state_t;
class spot_node_runtime_state_t;
class spot_node_runtime_t;

// Internal state distinguishes the three Spot lifecycles without parallel
// boolean flags that can represent contradictory combinations.
enum class spot_runtime_kind_t : std::uint8_t
{
    entry,
    user,
    instance
};

struct spot_actor_admission_callbacks_t
{
    std::function<spot_actor_join_result_t (
      void *, std::string_view, const zlink::message_t &, serializer_registry_t &)>
      join;
    std::function<task_t<void> (void *, void *)> on_actor_joined;
    std::function<task_t<actor_create_response_t> (
      void *, void *, const zlink::message_t &, serializer_registry_t &)>
      on_create_actor;
    std::function<task_t<void> (void *, void *)> on_leave_actor;
    std::function<task_t<void> (void *, void *)> on_disconnect_actor;
    spot_runtime_kind_t kind = spot_runtime_kind_t::user;
};

template <typename T> struct spot_member_function_traits_t;

template <typename TResult, typename TSpot, typename... TArgs>
struct spot_member_function_traits_t<TResult (TSpot::*) (TArgs...)>
{
    using spot_type = TSpot;
    using result_type = TResult;
    using args_tuple = std::tuple<TArgs...>;
    static constexpr std::size_t arg_count = sizeof...(TArgs);

    template <std::size_t Index> using arg_t = std::tuple_element_t<Index, args_tuple>;
};

template <typename TResult, typename TSpot, typename... TArgs>
struct spot_member_function_traits_t<TResult (TSpot::*) (TArgs...) const>
    : spot_member_function_traits_t<TResult (TSpot::*) (TArgs...)>
{
};

template <auto Method>
using spot_member_traits_t = spot_member_function_traits_t<decltype (Method)>;

template <typename T> using unqualified_spot_arg_t = std::remove_cvref_t<T>;

template <typename TTraits, std::size_t ArgCount> struct spot_handler_payload_arg;

template <typename TTraits> struct spot_handler_payload_arg<TTraits, 1>
{
    using type = unqualified_spot_arg_t<typename TTraits::template arg_t<0>>;
};

template <typename TTraits> struct spot_handler_payload_arg<TTraits, 2>
{
    using type = unqualified_spot_arg_t<typename TTraits::template arg_t<1>>;
};

template <typename TTraits>
using spot_handler_payload_arg_t =
  typename spot_handler_payload_arg<TTraits, TTraits::arg_count>::type;

template <typename T> struct spot_member_reply_payload_t
{
    using type = std::remove_cvref_t<T>;
};

template <typename T> struct spot_member_reply_payload_t<task_t<T>>
{
    using type = T;
};

template <typename TResult>
task_t<zlink::message_t> complete_spot_member_call (TResult &&result,
                                                    serializer_registry_t &serializers)
{
    using result_type = std::remove_cvref_t<TResult>;
    if constexpr (is_task_v<result_type>) {
        using value_type = typename task_value_type_t<result_type>::type;
        try {
            if constexpr (std::is_void_v<value_type>) {
                co_await result;
                co_return result_t<zlink::message_t>::success (zlink::message_t{});
            } else {
                auto value = co_await result;
                co_return result_t<zlink::message_t>::success (detail::encoded_payload_to_raw (
                  serializers.get<value_type> ().serialize (value)));
            }
        }
        catch (const framework_exception_t &error) {
            co_return detail::result_access_t::failure<zlink::message_t> (error);
        }
        catch (const std::exception &error) {
            co_return result_t<zlink::message_t>::failure (framework_error_kind_t::internal_failure,
                                                           error.what ());
        }
    } else if constexpr (std::is_void_v<result_type>) {
        co_return result_t<zlink::message_t>::success (zlink::message_t{});
    } else {
        co_return (co_await serialize_handler_result (std::forward<TResult> (result), serializers));
    }
}

template <typename TCall>
task_t<zlink::message_t> invoke_spot_member (TCall &&call, serializer_registry_t &serializers)
{
    using result_type = std::invoke_result_t<TCall>;
    try {
        if constexpr (std::is_void_v<result_type>) {
            call ();
            co_return result_t<zlink::message_t>::success (zlink::message_t{});
        } else {
            co_return (co_await complete_spot_member_call (call (), serializers));
        }
    }
    catch (...) {
        co_return current_exception_to_message_result ("spot handler threw an exception");
    }
}

template <typename TCall, typename... TKeepAlive>
task_t<zlink::message_t> invoke_spot_member_keepalive (TCall call,
                                                       serializer_registry_t &serializers,
                                                       TKeepAlive... keep_alive)
{
    (void) sizeof...(keep_alive);
    auto handler_task = invoke_spot_member (call, serializers);
    co_return co_await handler_task;
}
} // namespace detail

enum class spot_handler_kind_t
{
    packet = 0,
    subscription = 1,
    actor_send = 2,
    actor_request = 3
};

struct spot_route_t
{
    node_rid_t node_rid;
    spot_id_t spot_id;
    std::string spot_name;
};

struct accepted_spot_route_channel_t
{
    std::string channel_name;
    std::vector<std::string> manual_connections;
};

struct spot_packet_descriptor_t
{
    std::string packet_name;
    std::type_index payload_type;
};

struct spot_handler_descriptor_t
{
    spot_handler_kind_t kind;
    std::string packet_name;
    std::string topic;
    std::type_index handler_type;
    std::type_index payload_type;
    std::type_index actor_type;
    std::type_index reply_type;
};

struct spot_info_t
{
    spot_id_t spot_id;
    std::string spot_name;
};

enum class user_spot_execution_mode_t
{
    spot_wide = 0,
    per_actor = 1
};

enum class spot_relocation_readiness_mode_t
{
    any_turn_boundary = 0,
    application_signaled = 1
};

namespace detail
{
enum class factory_relocation_kind_t
{
    unspecified = 0,
    disabled = 1,
    recreate = 2,
    preserve_state = 3
};

struct factory_relocation_configuration_t
{
    factory_relocation_kind_t kind{factory_relocation_kind_t::unspecified};
    std::type_index adapter_type{typeid (void)};
    std::function<task_t<std::vector<std::byte>> (
      void *, std::stop_token)> capture;
    std::function<task_t<void> (
      void *, std::vector<std::byte>, std::stop_token)> restore;
};
} // namespace detail

template <typename TSpot>
class spot_relocation_adapter_t
{
  public:
    virtual ~spot_relocation_adapter_t () = default;
    virtual task_t<std::vector<std::byte>>
    capture (TSpot &spot, std::stop_token operation_cancellation) = 0;
    virtual task_t<void>
    restore (TSpot &spot,
             std::vector<std::byte> payload,
             std::stop_token operation_cancellation) = 0;
};

template <typename TSpot>
class user_spot_factory_builder_t
{
  public:
    user_spot_factory_builder_t &set_stable_type_limit (std::int32_t limit)
    {
        ensure_mutable ();
        if (limit < 1) {
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "User Spot stable type limit must be positive");
        }
        _stable_type_limit = limit;
        return *this;
    }

    user_spot_factory_builder_t &
    set_execution_mode (user_spot_execution_mode_t mode)
    {
        ensure_mutable ();
        _execution_mode = mode;
        return *this;
    }

    user_spot_factory_builder_t &
    set_relocation_readiness (spot_relocation_readiness_mode_t mode)
    {
        ensure_mutable ();
        _relocation_readiness = mode;
        return *this;
    }

    void disable_relocation ()
    {
        ensure_mutable ();
        select (detail::factory_relocation_kind_t::disabled, typeid (void));
    }

    void recreate_on_relocation ()
    {
        ensure_mutable ();
        select (detail::factory_relocation_kind_t::recreate, typeid (void));
    }

    template <typename TAdapter>
    requires std::derived_from<TAdapter, spot_relocation_adapter_t<TSpot>>
    void preserve_state_with ()
    {
        ensure_mutable ();
        static_assert (
          std::is_default_constructible_v<TAdapter>,
          "C++ Spot relocation adapters must be default constructible");
        select (detail::factory_relocation_kind_t::preserve_state,
                typeid (TAdapter));
        _relocation.capture = [] (
          void *spot,
          std::stop_token operation_cancellation)
          -> task_t<std::vector<std::byte>> {
            auto adapter = std::make_shared<TAdapter> ();
            co_return co_await adapter->capture (
              *static_cast<TSpot *> (spot),
              operation_cancellation);
        };
        _relocation.restore = [] (
          void *spot,
          std::vector<std::byte> payload,
          std::stop_token operation_cancellation)
          -> task_t<void> {
            auto adapter = std::make_shared<TAdapter> ();
            co_await adapter->restore (
              *static_cast<TSpot *> (spot),
              std::move (payload),
              operation_cancellation);
        };
    }

  private:
    friend class spot_node_builder_t;

    void seal () noexcept { _sealed = true; }

    void ensure_mutable () const
    {
        if (_sealed) {
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "User Spot factory builder cannot be changed after the configure callback returns");
        }
    }

    void validate () const
    {
        if (_relocation.kind
            == detail::factory_relocation_kind_t::unspecified) {
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "User Spot factory must select exactly one relocation policy");
        }
        if (_execution_mode == user_spot_execution_mode_t::per_actor
            && _relocation.kind
                 != detail::factory_relocation_kind_t::recreate) {
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "Per-Actor User Spots require recreate_on_relocation");
        }
        if (_execution_mode == user_spot_execution_mode_t::per_actor
            && _relocation_readiness
                 == spot_relocation_readiness_mode_t::application_signaled) {
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "Application-signaled relocation readiness requires Spot-Wide execution");
        }
    }

    void select (detail::factory_relocation_kind_t kind,
                 std::type_index adapter_type)
    {
        if (_relocation.kind
            != detail::factory_relocation_kind_t::unspecified) {
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "User Spot factory must select exactly one relocation policy");
        }
        _relocation = {kind, adapter_type};
    }

    std::int32_t _stable_type_limit = 0;
    user_spot_execution_mode_t _execution_mode =
      user_spot_execution_mode_t::spot_wide;
    spot_relocation_readiness_mode_t _relocation_readiness =
      spot_relocation_readiness_mode_t::any_turn_boundary;
    detail::factory_relocation_configuration_t _relocation;
    bool _sealed = false;
};

template <typename TSpot>
class instance_spot_factory_builder_t
{
  public:
    instance_spot_factory_builder_t &set_stable_type_limit (std::int32_t limit)
    {
        ensure_mutable ();
        if (limit < 1) {
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "Instance Spot stable type limit must be positive");
        }
        _stable_type_limit = limit;
        return *this;
    }

    void disable_relocation ()
    {
        ensure_mutable ();
        select (detail::factory_relocation_kind_t::disabled, typeid (void));
    }

    void recreate_on_relocation ()
    {
        ensure_mutable ();
        select (detail::factory_relocation_kind_t::recreate, typeid (void));
    }

    template <typename TAdapter>
    requires std::derived_from<TAdapter, spot_relocation_adapter_t<TSpot>>
    void preserve_state_with ()
    {
        ensure_mutable ();
        static_assert (
          std::is_default_constructible_v<TAdapter>,
          "C++ Spot relocation adapters must be default constructible");
        select (detail::factory_relocation_kind_t::preserve_state,
                typeid (TAdapter));
        _relocation.capture = [] (
          void *spot,
          std::stop_token operation_cancellation)
          -> task_t<std::vector<std::byte>> {
            auto adapter = std::make_shared<TAdapter> ();
            co_return co_await adapter->capture (
              *static_cast<TSpot *> (spot),
              operation_cancellation);
        };
        _relocation.restore = [] (
          void *spot,
          std::vector<std::byte> payload,
          std::stop_token operation_cancellation)
          -> task_t<void> {
            auto adapter = std::make_shared<TAdapter> ();
            co_await adapter->restore (
              *static_cast<TSpot *> (spot),
              std::move (payload),
              operation_cancellation);
        };
    }

  private:
    friend class spot_node_builder_t;

    void seal () noexcept { _sealed = true; }

    void ensure_mutable () const
    {
        if (_sealed) {
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "Instance Spot factory builder cannot be changed after the configure callback returns");
        }
    }

    void validate () const
    {
        if (_relocation.kind
            == detail::factory_relocation_kind_t::unspecified) {
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "Instance Spot factory must select exactly one relocation policy");
        }
    }

    void select (detail::factory_relocation_kind_t kind,
                 std::type_index adapter_type)
    {
        if (_relocation.kind
            != detail::factory_relocation_kind_t::unspecified) {
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "Instance Spot factory must select exactly one relocation policy");
        }
        _relocation = {kind, adapter_type};
    }

    std::int32_t _stable_type_limit = 0;
    detail::factory_relocation_configuration_t _relocation;
    bool _sealed = false;
};

struct spot_node_snapshot_t
{
    std::string name;
    std::string bind_endpoint;
    std::optional<std::string> router_bind_endpoint;
    std::optional<std::string> pub_bind_endpoint;
    std::optional<zlink::routing_id_t> routing_id;
    std::vector<std::string> router_manual_connections;
    std::vector<std::pair<zlink::routing_id_t, std::string>> router_manual_rid_connections;
    std::vector<std::string> pub_sub_manual_connections;
    std::optional<std::string> discovery_channel_name;
    std::vector<std::string> spot_names;
    std::vector<std::string> instance_spot_names;
    std::map<std::string, user_spot_execution_mode_t> spot_execution_modes;
    std::optional<std::string> entry_spot_name;
    std::vector<accepted_spot_route_channel_t> accepted_route_channels;
    std::optional<std::string> spot_route_channel_name;
    std::vector<std::string> actor_types;
};

class spot_handler_registry_t;
struct spot_create_result_t;

namespace detail
{
inline bool is_default_target_routing_id (const zlink::routing_id_t &rid)
{
    const auto bytes = rid.to_bytes ();
    return bytes.size () == sizeof (std::uint32_t)
           && std::all_of (bytes.begin (), bytes.end (), [] (std::uint8_t byte) {
                  return byte == 0;
              });
}

inline node_rid_t node_rid_from_target (const zlink::routing_id_t &rid)
{
    if (is_default_target_routing_id (rid)) {
        return node_rid_t {};
    }
    return node_rid_t::from_string (rid.to_string ());
}

inline spot_id_t spot_id_from_target (spot_id_t spot_id)
{
    return spot_id;
}
} // namespace detail

class spot_context_t
{
  public:
    ~spot_context_t ();

    spot_context_t (spot_context_t &&) noexcept;
    spot_context_t &operator= (spot_context_t &&) = delete;
    spot_context_t (const spot_context_t &) = delete;
    spot_context_t &operator= (const spot_context_t &) = delete;

    node_rid_t node_rid () const;
    std::string_view mesh_name () const;
    spot_id_t spot_id () const;
    std::uint64_t object_generation () const noexcept;
    std::string spot_name () const;
    spot_handler_registry_t handlers ();
    spot_relocation_ready_call_t relocation_ready ();
    spot_manager_t manager () const;
    channel_client_t outbound () const;
    task_t<bool> close ();

    template <typename TEvent> send_call_t publish (std::string topic, TEvent event)
    {
        auto *serializers = serializer_registry ();
        if (serializers == nullptr) {
            return send_call_t (
              result_t<void>::failure (framework_error_kind_t::protocol_error,
                                       "spot publish requires a serializer registry"));
        }
        try {
            auto payload =
              detail::encoded_payload_to_raw (serializers->get<TEvent> ().serialize (event));
            return publish_erased (std::move (topic), detail::message_name<TEvent> (),
                                   std::move (payload));
        }
        catch (const framework_exception_t &error) {
            return send_call_t (
              detail::result_access_t::failure<void> (error));
        }
    }

    template <typename TRequest>
    spot_request_call_t request_to_spot (spot_id_t target_spot_id,
                                         TRequest request)
    {
        ensure_submission_open ();
        return spot_route_client ().request_to_spot (
          std::move (target_spot_id), std::move (request));
    }

    template <typename TMessage>
    spot_send_call_t send_to_spot (spot_id_t target_spot_id,
                                   TMessage message)
    {
        ensure_submission_open ();
        return spot_route_client ().send_to_spot (
          std::move (target_spot_id), std::move (message));
    }

    template <typename TPayload> spot_context_t &register_packet (std::string packet_name)
    {
        return register_packet_erased (std::move (packet_name),
                                       std::type_index (typeid (TPayload)));
    }

    template <typename TWork> auto run_cpu_worker (TWork work)
    {
        using result_type = detail::worker_sync_result_t<TWork>;
        auto scheduler = _worker_scheduler;
        auto preflight = submission_preflight ();
        return worker_call_t<result_type> (
          [scheduler, preflight = std::move (preflight),
           work = std::move (work)] (
            std::stop_token cancellation) mutable -> task_t<result_type> {
              if (preflight) {
                  const auto admitted = preflight ();
                  if (!admitted) {
                      return task_t<result_type> (
                        result_t<result_type>::failure (
                          admitted.error_kind (),
                          admitted.error ()
                            ? admitted.error ()->what ()
                            : "Spot worker preflight failed"));
                  }
              }
              if (!scheduler) {
                  return task_t<result_type> (result_t<result_type>::failure (
                    framework_error_kind_t::internal_failure, "worker runtime is not configured"));
              }

              detail::task_completion_source_t<result_type> completion;
              auto task = completion.task ();
              auto shared_work = std::make_shared<TWork> (std::move (work));
              auto completed = std::make_shared<std::atomic_bool> (false);
              const auto scheduled =
                scheduler->try_schedule ([scheduler, shared_work, completion,
                                          completed, cancellation] (std::stop_token) mutable {
                    auto result = detail::run_worker_body<result_type> (
                      *shared_work, cancellation);
                    if (cancellation.stop_requested ()) {
                        completed->store (true);
                        return;
                    }
                    if (!completed->exchange (true)) {
                        auto complete_result = [completion,
                                                result = std::move (result)] () mutable {
                            completion.complete (std::move (result));
                        };
                        scheduler->post_owner (std::move (complete_result));
                    }
                });
              if (!scheduled) {
                  completed->store (true);
                  auto complete_full = [completion] () mutable {
                      completion.complete (result_t<result_type>::failure (
                        framework_error_kind_t::capacity_exceeded, "worker queue is full"));
                  };
                  scheduler->post_owner (std::move (complete_full));
              }
              return task;
          }, scheduler ? scheduler->stop_token () : std::stop_token{});
    }

    template <typename TWork> auto run_io_worker (TWork work)
    {
        using task_type = detail::worker_async_task_t<TWork>;
        using result_type = detail::task_result_t<task_type>;
        auto scheduler = _worker_scheduler;
        auto preflight = submission_preflight ();
        return worker_call_t<result_type> (
          [scheduler, preflight = std::move (preflight),
           work = std::move (work)] (
            std::stop_token cancellation) mutable -> task_t<result_type> {
              if (preflight) {
                  const auto admitted = preflight ();
                  if (!admitted) {
                      return task_t<result_type> (
                        result_t<result_type>::failure (
                          admitted.error_kind (),
                          admitted.error ()
                            ? admitted.error ()->what ()
                            : "Spot worker preflight failed"));
                  }
              }
              if (!scheduler) {
                  return task_t<result_type> (result_t<result_type>::failure (
                    framework_error_kind_t::internal_failure, "worker runtime is not configured"));
              }
              auto completion =
                std::make_shared<detail::task_completion_source_t<result_type>> ();
              auto result = completion->task ();
              auto completed = std::make_shared<std::atomic_bool> (false);
              auto shared_work = std::make_shared<TWork> (std::move (work));
              const auto scheduled = scheduler->try_schedule (
                [shared_work, completion, completed,
                 cancellation] (std::stop_token) mutable {
                    try {
                        auto pending =
                          detail::invoke_worker_async (*shared_work, cancellation);
                        /* The inner task state owns this observer. Capturing
                         * pending in the observer would make an incomplete
                         * I/O task retain its own state after the wrapper has
                         * timed out. */
                        observe_task_completion (
                          pending,
                          [completion, completed, cancellation] (
                            const result_t<result_type> &value) mutable {
                              if (cancellation.stop_requested ()) {
                                  completed->store (true);
                                  return;
                              }
                              if (!completed->exchange (true)) {
                                  completion->complete (value);
                              }
                          });
                    }
                    catch (const framework_exception_t &error) {
                        if (!completed->exchange (true)) {
                            completion->complete (
                              detail::result_access_t::failure<result_type> (error));
                        }
                    }
                    catch (const std::exception &error) {
                        if (!completed->exchange (true)) {
                            completion->complete (result_t<result_type>::failure (
                              framework_error_kind_t::internal_failure, error.what ()));
                        }
                    }
                    catch (...) {
                        if (!completed->exchange (true)) {
                            completion->complete (result_t<result_type>::failure (
                              framework_error_kind_t::internal_failure, "I/O worker failed"));
                        }
                    }
                });
              if (!scheduled) {
                  completed->store (true);
                  scheduler->post_owner ([completion] {
                      completion->complete (result_t<result_type>::failure (
                        framework_error_kind_t::capacity_exceeded, "worker queue is full"));
                  });
              }
              return result;
          }, scheduler ? scheduler->stop_token () : std::stop_token{});
    }

    std::vector<spot_packet_descriptor_t> packet_registry () const;

    template <typename TActor>
    task_t<actor_ref_t> leave_actor (const actor_ref_t &actor_ref, TActor &actor)
    {
        return leave_actor_erased (
          actor_ref, std::type_index (typeid (TActor)), &actor,
          [] (void *actor_instance, const actor_ref_t &committed) {
              auto &typed_actor = *static_cast<TActor *> (actor_instance);
              if constexpr (requires { typed_actor.set_actor_ref (committed); }) {
                  typed_actor.set_actor_ref (committed);
              }
          });
    }

    template <typename THandler>
    timer_t
    add_timer (std::string name, std::chrono::milliseconds period, timer_options_t options = {})
    {
        using traits = detail::spot_member_traits_t<&THandler::handle>;
        static_assert (traits::arg_count == 2,
                       "SPOT timer handler must accept spot and timer_tick_t arguments");
        using spot_type = detail::unqualified_spot_arg_t<typename traits::template arg_t<0>>;
        using tick_type = detail::unqualified_spot_arg_t<typename traits::template arg_t<1>>;
        static_assert (std::is_same_v<tick_type, timer_tick_t>,
                       "SPOT timer handler second argument must be timer_tick_t");
        return add_timer_erased (
          std::move (name), period, std::move (options), std::type_index (typeid (THandler)),
          [] (service_provider_t *services) {
              return detail::timer_handler_factory_t<
                THandler,
                typename detail::handler_dependencies_t<THandler>::type>::create (services);
          },
          [] (void *spot, void *handler_instance,
              serializer_registry_t &serializers, const timer_tick_t &tick) {
              auto *typed_spot = static_cast<spot_type *> (spot);
              auto *handler = static_cast<THandler *> (handler_instance);
              auto captured_tick = std::make_shared<timer_tick_t> (tick);
              return detail::invoke_spot_member_keepalive (
                [typed_spot, handler, captured_tick] {
                    return handler->handle (*typed_spot, *captured_tick);
                },
                serializers, captured_tick);
          });
    }

  private:
    friend class spot_node_builder_t;
    friend class entry_spot_context_t;
    friend class instance_spot_context_t;
    friend class detail::spot_node_runtime_t;
    friend class detail::timer_runtime_t;
    friend class detail::spot_context_access_t;

    spot_context_t ();
    explicit spot_context_t (std::shared_ptr<detail::spot_context_state_t> state);

  protected:
    class erased_request_call_t
    {
      public:
        explicit erased_request_call_t (framework_exception_t error);
        erased_request_call_t (std::string packet_name,
                               serializer_registry_t *serializers,
                               std::function<task_t<zlink::message_t> (
                                 const std::string &,
                                 std::chrono::milliseconds,
                                 const request_call_t<zlink::message_t>::metadata_map_t &)> submit,
                               request_call_t<zlink::message_t>::preflight_fn_t preflight = {});

        template <typename TReply> request_call_t<TReply> as () const
        {
            if (_error) {
                return request_call_t<TReply> (
                  detail::result_access_t::failure<TReply> (*_error));
            }
            auto serializers = _serializers;
            auto submit = _submit;
            auto preflight = _preflight;
            return request_call_t<TReply> (
              _packet_name,
              [serializers,
               submit] (const std::string &packet_name, std::chrono::milliseconds timeout,
                        const request_call_t<TReply>::metadata_map_t &metadata) -> task_t<TReply> {
                  if (!submit) {
                      co_return result_t<TReply>::failure (
                        framework_error_kind_t::protocol_error,
                        "spot request is not bound to a route channel");
                  }
                  if (serializers == nullptr) {
                      co_return result_t<TReply>::failure (
                        framework_error_kind_t::protocol_error,
                        "spot request has no serializer registry");
                  }
                  try {
                      auto reply = co_await submit (packet_name, timeout, metadata);
                      co_return serializers->get<TReply> ().deserialize (
                        detail::encoded_payload_from_raw (reply));
                  }
                  catch (const framework_exception_t &error) {
                      co_return detail::result_access_t::failure<TReply> (error);
                  }
              },
              std::move (preflight));
        }

      private:
        std::optional<framework_exception_t> _error;
        std::string _packet_name;
        serializer_registry_t *_serializers = nullptr;
        std::function<task_t<zlink::message_t> (
          const std::string &,
          std::chrono::milliseconds,
          const request_call_t<zlink::message_t>::metadata_map_t &)>
          _submit;
        request_call_t<zlink::message_t>::preflight_fn_t _preflight;
    };

    bool has_same_source_fence (const spot_context_t &other) const noexcept;
    void ensure_submission_open () const;
    std::function<result_t<void> ()>
    submission_preflight () const;

    send_call_t
    publish_erased (std::string topic, std::string packet_name, zlink::message_t payload);
    serializer_registry_t *serializer_registry () const noexcept;
    route_client_t spot_route_client () const;
    send_call_t send_to_erased (node_rid_t node_rid,
                                spot_id_t spot_id,
                                std::string packet_name,
                                zlink::message_t payload);
    erased_request_call_t request_to_erased (node_rid_t node_rid,
                                             spot_id_t spot_id,
                                             std::string packet_name,
                                             zlink::message_t payload);
    spot_context_t &register_packet_erased (std::string packet_name, std::type_index payload_type);
    task_t<actor_ref_t>
    leave_actor_erased (const actor_ref_t &actor_ref,
                        std::type_index actor_type,
                        void *actor,
                        std::function<void (void *, const actor_ref_t &)> update_actor_ref);
    timer_t
    add_timer_erased (std::string name,
                      std::chrono::milliseconds period,
                      timer_options_t options,
                      std::type_index handler_type,
                      std::function<std::shared_ptr<void> (service_provider_t *)> handler_factory,
                      std::function<task_t<zlink::message_t> (
                        void *, void *, serializer_registry_t &,
                        const timer_tick_t &)> handler_invoker);
    task_t<bool> close_erased ();

    friend void detail::drain_spot_node_executors (detail::spot_node_builder_state_t &node);
    friend void detail::cancel_spot_node_dispatch_queues (
      detail::spot_node_builder_state_t &node);

    std::shared_ptr<detail::spot_context_state_t> _state;
    std::shared_ptr<detail::worker_scheduler_t> _worker_scheduler;
};

class entry_spot_context_t : public spot_context_t
{
  public:
    ~entry_spot_context_t ();

    entry_spot_context_t (entry_spot_context_t &&) noexcept;
    entry_spot_context_t &operator= (entry_spot_context_t &&) = delete;
    entry_spot_context_t (const entry_spot_context_t &) = delete;
    entry_spot_context_t &operator= (const entry_spot_context_t &) = delete;

    /* Resolves the actor's identity from the live instance registered on
     * this node, so a stale instance (already destroyed or superseded by a
     * newer generation) is a successful no-op and never destroys the new
     * actor. */
    template <typename TActor> task_t<void> destroy_actor (TActor &actor)
    {
        return destroy_actor_instance_erased (std::addressof (actor));
    }

  private:
    friend class detail::spot_node_runtime_t;
    entry_spot_context_t ();
    explicit entry_spot_context_t (const spot_context_t &context);
    explicit entry_spot_context_t (std::shared_ptr<detail::spot_context_state_t> state);

    task_t<void> destroy_actor_instance_erased (const void *instance);
    task_t<void> destroy_actor_erased (const actor_ref_t &actor_ref);
};

class instance_spot_context_t : public spot_context_t
{
  public:
    ~instance_spot_context_t ();

    instance_spot_context_t (instance_spot_context_t &&) noexcept;
    instance_spot_context_t &operator= (instance_spot_context_t &&) = delete;
    instance_spot_context_t (const instance_spot_context_t &) = delete;
    instance_spot_context_t &operator= (const instance_spot_context_t &) = delete;

  private:
    friend class detail::spot_node_runtime_t;
    instance_spot_context_t ();
    explicit instance_spot_context_t (const spot_context_t &context);
    explicit instance_spot_context_t (
      std::shared_ptr<detail::spot_context_state_t> state);
};

template <typename TActor>
class spot_t
{
  public:
    using actor_type = TActor;

    virtual ~spot_t () = default;
    virtual spot_context_t &context () noexcept = 0;
    virtual const spot_context_t &context () const noexcept = 0;
    virtual void configure () = 0;
    virtual task_t<spot_create_response_t> on_create (
      const message_t &request)
    {
        (void) request;
        co_return spot_create_response_t::accept ();
    }
    virtual task_t<void> on_initialize () { co_return; }
    virtual task_t<void> on_closing (
      const spot_closing_context_t &context,
      std::stop_token cleanup_cancellation)
    {
        (void) context;
        (void) cleanup_cancellation;
        co_return;
    }
    virtual task_t<void> on_relocation_ready_completed (
      const spot_relocation_ready_completion_t &completion)
    {
        (void) completion;
        co_return;
    }
    virtual task_t<spot_actor_join_result_t> on_actor_join (
      std::string_view actor_id,
      const message_t &request) = 0;
    virtual task_t<void> on_actor_joined (TActor &actor) = 0;
    virtual task_t<void> on_leave_actor (TActor &actor) = 0;
    virtual task_t<void> on_disconnect_actor (TActor &actor)
    {
        (void) actor;
        co_return;
    }
};

template <typename TActor>
class entry_spot_t
{
  public:
    using actor_type = TActor;

    virtual ~entry_spot_t () = default;
    virtual entry_spot_context_t &context () noexcept = 0;
    virtual const entry_spot_context_t &context () const noexcept = 0;
    virtual void configure () = 0;
    virtual task_t<void> on_initialize () { co_return; }
    virtual task_t<void> on_closing (
      const spot_closing_context_t &context,
      std::stop_token cleanup_cancellation)
    {
        (void) context;
        (void) cleanup_cancellation;
        co_return;
    }
    virtual task_t<actor_create_response_t> on_create_actor (
      TActor &actor,
      const message_t &create_request)
    {
        (void) actor;
        (void) create_request;
        co_return actor_create_response_t::accept ();
    }
    virtual task_t<spot_actor_join_result_t> on_actor_join (
      std::string_view actor_id,
      const message_t &request) = 0;
    virtual task_t<void> on_actor_joined (TActor &actor) = 0;
    virtual task_t<void> on_leave_actor (TActor &actor) = 0;
    virtual task_t<void> on_disconnect_actor (TActor &actor)
    {
        (void) actor;
        co_return;
    }
};

class instance_spot_t
{
  public:
    virtual ~instance_spot_t () = default;
    virtual instance_spot_context_t &context () noexcept = 0;
    virtual const instance_spot_context_t &context () const noexcept = 0;
    virtual void configure () = 0;
    virtual task_t<void> on_initialize () { co_return; }
    virtual task_t<void> on_closing (
      const spot_closing_context_t &context,
      std::stop_token cleanup_cancellation)
    {
        (void) context;
        (void) cleanup_cancellation;
        co_return;
    }
};

namespace detail
{
template <typename T>
concept user_spot_type =
  requires { typename T::actor_type; }
  && std::derived_from<T, spot_t<typename T::actor_type>>;

template <typename T>
concept entry_spot_type =
  requires { typename T::actor_type; }
  && std::derived_from<T, entry_spot_t<typename T::actor_type>>;
} // namespace detail

struct spot_create_result_t
{
    spot_ref_t spot;
    spot_create_state_t state = spot_create_state_t::created;
    std::optional<message_t> reply;
};

namespace detail
{
struct local_spot_create_result_t
{
    spot_id_t spot_id;
    spot_create_state_t state = spot_create_state_t::created;
    std::optional<message_t> reply;
    spot_context_t context;
};
struct spot_create_call_state_t;
class spot_route_internal_dispatcher_t;
} // namespace detail

class spot_handler_registry_t
{
  public:
    using invoker_t =
      std::function<task_t<zlink::message_t> (void *,
                                              void *,
                                              service_provider_t &,
                                              serializer_registry_t &,
                                              const zlink::message_t &,
                                              const spot_inbound_message_t &)>;

    spot_handler_registry_t ();
    ~spot_handler_registry_t ();

    spot_handler_registry_t (spot_handler_registry_t &&) noexcept;
    spot_handler_registry_t &operator= (spot_handler_registry_t &&) noexcept;
    spot_handler_registry_t (const spot_handler_registry_t &) = default;
    spot_handler_registry_t &operator= (const spot_handler_registry_t &) = default;

    template <auto Method>
    spot_handler_registry_t &
    add_handler (std::string packet_name = detail::message_name<
                   detail::spot_handler_payload_arg_t<detail::spot_member_traits_t<Method>>> ())
    {
        using traits = detail::spot_member_traits_t<Method>;
        static_assert (traits::arg_count == 1 || traits::arg_count == 2,
                       "SPOT packet member must accept payload or context plus payload");
        using spot_type = typename traits::spot_type;
        using message_type = detail::spot_handler_payload_arg_t<traits>;
        if constexpr (traits::arg_count == 2) {
            using context_type = detail::unqualified_spot_arg_t<typename traits::template arg_t<0>>;
            static_assert (std::is_same_v<context_type, message_context_t>,
                           "SPOT packet context must be message_context_t");
        }
        auto registered_packet_name = packet_name;
        return add_handler_erased (
          spot_handler_kind_t::packet, std::move (packet_name), {},
          std::type_index (typeid (spot_type)), std::type_index (typeid (message_type)),
          std::type_index (typeid (void)), std::type_index (typeid (void)),
          [registered_packet_name = std::move (registered_packet_name)] (
            void *spot, void *, service_provider_t &, serializer_registry_t &serializers,
            const zlink::message_t &message, const spot_inbound_message_t &metadata) {
              auto &typed_spot = *static_cast<spot_type *> (spot);
              auto payload =
                std::make_shared<message_type> (serializers.get<message_type> ().deserialize (
                  detail::encoded_payload_from_raw (message)));
              if constexpr (traits::arg_count == 2) {
                  auto context = std::make_shared<message_context_t> (
                    metadata.to_message_context (registered_packet_name));
                  return detail::invoke_spot_member_keepalive (
                    [&typed_spot, context, payload] {
                        return (typed_spot.*Method) (*context, *payload);
                    },
                    serializers, context, payload);
              } else {
                  return detail::invoke_spot_member_keepalive (
                    [&typed_spot, payload] { return (typed_spot.*Method) (*payload); }, serializers,
                    payload);
              }
          });
    }

    template <auto Method> spot_handler_registry_t &add_subscribe (std::string topic)
    {
        using traits = detail::spot_member_traits_t<Method>;
        static_assert (traits::arg_count == 1 || traits::arg_count == 2,
                       "SPOT subscription member must accept event or context plus event");
        using spot_type = typename traits::spot_type;
        using event_type = detail::spot_handler_payload_arg_t<traits>;
        if constexpr (traits::arg_count == 2) {
            using context_type = detail::unqualified_spot_arg_t<typename traits::template arg_t<0>>;
            static_assert (std::is_same_v<context_type, publish_message_context_t>,
                           "SPOT subscription context must be publish_message_context_t");
        }
        auto registered_packet_name = detail::message_name<event_type> ();
        auto registered_topic = topic;
        return add_handler_erased (
          spot_handler_kind_t::subscription, detail::message_name<event_type> (), std::move (topic),
          std::type_index (typeid (spot_type)), std::type_index (typeid (event_type)),
          std::type_index (typeid (void)), std::type_index (typeid (void)),
          [registered_packet_name = std::move (registered_packet_name),
           registered_topic = std::move (registered_topic)] (
            void *spot, void *, service_provider_t &, serializer_registry_t &serializers,
            const zlink::message_t &message, const spot_inbound_message_t &metadata) {
              auto &typed_spot = *static_cast<spot_type *> (spot);
              auto payload =
                std::make_shared<event_type> (serializers.get<event_type> ().deserialize (
                  detail::encoded_payload_from_raw (message)));
              if constexpr (traits::arg_count == 2) {
                  auto context = std::make_shared<publish_message_context_t> (
                    metadata.to_publish_context (registered_packet_name, registered_topic));
                  return detail::invoke_spot_member_keepalive (
                    [&typed_spot, context, payload] {
                        return (typed_spot.*Method) (*context, *payload);
                    },
                    serializers, context, payload);
              } else {
                  return detail::invoke_spot_member_keepalive (
                    [&typed_spot, payload] { return (typed_spot.*Method) (*payload); }, serializers,
                    payload);
              }
          });
    }

    template <auto Method>
    spot_handler_registry_t &
    add_actor_send (std::string packet_name = detail::message_name<detail::unqualified_spot_arg_t<
                      typename detail::spot_member_traits_t<Method>::template arg_t<2>>> ())
    {
        return add_actor_handler<Method, spot_handler_kind_t::actor_send> (std::move (packet_name));
    }

    template <auto Method>
    spot_handler_registry_t &add_actor_request (
      std::string packet_name = detail::message_name<detail::unqualified_spot_arg_t<
        typename detail::spot_member_traits_t<Method>::template arg_t<2>>> ())
    {
        return add_actor_handler<Method, spot_handler_kind_t::actor_request> (
          std::move (packet_name));
    }

    std::vector<spot_handler_descriptor_t> descriptors () const;

    template <typename TSpot>
    result_t<zlink::message_t> invoke_packet (std::string_view packet_name,
                                              TSpot &spot,
                                              service_provider_t &services,
                                              serializer_registry_t &serializers,
                                              const zlink::message_t &message) const
    {
        return invoke_erased (spot_handler_kind_t::packet, packet_name, {},
                              std::type_index (typeid (void)), &spot, nullptr, services,
                              serializers, message)
          .result ();
    }

    template <typename TSpot, typename TActor>
    result_t<zlink::message_t> invoke_actor_packet (std::string_view packet_name,
                                                    TSpot &spot,
                                                    TActor &actor,
                                                    service_provider_t &services,
                                                    serializer_registry_t &serializers,
                                                    const zlink::message_t &message) const
    {
        const auto kind =
          resolve_actor_packet_kind (packet_name, std::type_index (typeid (TActor)));
        return invoke_erased (kind, packet_name, {}, std::type_index (typeid (TActor)), &spot,
                              &actor, services, serializers, message)
          .result ();
    }

    template <typename TSpot, typename TActor>
    result_t<zlink::message_t> invoke_actor_packet (std::string_view packet_name,
                                                    TSpot &spot,
                                                    TActor &actor,
                                                    service_provider_t &services,
                                                    serializer_registry_t &serializers,
                                                    const zlink::message_t &message,
                                                    spot_inbound_message_t metadata) const
    {
        const auto kind =
          resolve_actor_packet_kind (packet_name, std::type_index (typeid (TActor)));
        return invoke_erased (kind, packet_name, {}, std::type_index (typeid (TActor)), &spot,
                              &actor, services, serializers, message, std::move (metadata))
          .result ();
    }

  private:
    friend class spot_context_t;
    friend class detail::spot_node_runtime_t;
    friend class detail::spot_route_internal_dispatcher_t;
    explicit spot_handler_registry_t (std::shared_ptr<detail::spot_context_state_t> state);

    template <typename TSpot, typename TActor> void register_actor_admission ()
    {
        static_assert (
          std::same_as<TActor, typename TSpot::actor_type>,
          "SPOT actor handler type must match the Spot base actor_type");
        detail::spot_actor_admission_callbacks_t callbacks;
        callbacks.kind = detail::entry_spot_type<TSpot>
                           ? detail::spot_runtime_kind_t::entry
                           : detail::spot_runtime_kind_t::user;
        callbacks.join = [] (void *spot, std::string_view actor_id,
                             const zlink::message_t &request,
                             serializer_registry_t &serializers) {
            auto &typed_spot = *static_cast<TSpot *> (spot);
            return typed_spot
              .on_actor_join (
                actor_id, message_t::from_raw (request, &serializers))
              .result ()
              .value ();
        };
        callbacks.on_actor_joined = [] (void *spot, void *actor) {
            return static_cast<TSpot *> (spot)->on_actor_joined (
              *static_cast<TActor *> (actor));
        };
        callbacks.on_create_actor = [] (void *spot, void *actor, const zlink::message_t &request,
                                      serializer_registry_t &serializers) {
            if constexpr (detail::entry_spot_type<TSpot>) {
                return static_cast<TSpot *> (spot)->on_create_actor (
                  *static_cast<TActor *> (actor),
                  message_t::from_raw (request, &serializers));
            } else {
                return task_t<actor_create_response_t> (
                  result_t<actor_create_response_t>::success (
                    actor_create_response_t::accept ()));
            }
        };
        callbacks.on_leave_actor = [] (void *spot, void *actor) {
            return static_cast<TSpot *> (spot)->on_leave_actor (
              *static_cast<TActor *> (actor));
        };
        callbacks.on_disconnect_actor = [] (void *spot, void *actor) {
            return static_cast<TSpot *> (spot)->on_disconnect_actor (
              *static_cast<TActor *> (actor));
        };
        register_actor_admission_erased (std::type_index (typeid (TActor)), std::move (callbacks));
    }

    spot_handler_kind_t resolve_actor_packet_kind (std::string_view packet_name,
                                                   std::type_index actor_type) const;

    spot_handler_registry_t &add_handler_erased (spot_handler_kind_t kind,
                                                 std::string packet_name,
                                                 std::string topic,
                                                 std::type_index handler_type,
                                                 std::type_index payload_type,
                                                 std::type_index actor_type,
                                                 std::type_index reply_type,
                                                 invoker_t invoker);

    task_t<zlink::message_t> invoke_erased (spot_handler_kind_t kind,
                                            std::string_view packet_name,
                                            std::string_view topic,
                                            std::type_index actor_type,
                                            void *spot,
                                            void *actor,
                                            service_provider_t &services,
                                            serializer_registry_t &serializers,
                                            zlink::message_t message,
                                            spot_inbound_message_t metadata = {},
                                            bool serial_dispatch = true,
                                            std::string actor_execution_key = {},
                                            std::string actor_execution_spot_id = {}) const;

    void register_actor_admission_erased (std::type_index actor_type,
                                          detail::spot_actor_admission_callbacks_t callbacks);

    template <auto Method, spot_handler_kind_t Kind>
    spot_handler_registry_t &add_actor_handler (std::string packet_name)
    {
        using traits = detail::spot_member_traits_t<Method>;
        static_assert (
          traits::arg_count == 3,
          "SPOT actor handler member must accept actor, context, and payload arguments");
        using spot_type = typename traits::spot_type;
        using actor_type = detail::unqualified_spot_arg_t<typename traits::template arg_t<0>>;
        using context_type = detail::unqualified_spot_arg_t<typename traits::template arg_t<1>>;
        using message_type = detail::unqualified_spot_arg_t<typename traits::template arg_t<2>>;
        static_assert (std::is_same_v<context_type, message_context_t>,
                       "SPOT actor handler context must be message_context_t");
        auto registered_packet_name = packet_name;
        auto &registry = add_handler_erased (
          Kind, std::move (packet_name), {}, std::type_index (typeid (spot_type)),
          std::type_index (typeid (message_type)), std::type_index (typeid (actor_type)),
          std::type_index (typeid (void)),
          [registered_packet_name = std::move (registered_packet_name)] (
            void *spot, void *actor, service_provider_t &, serializer_registry_t &serializers,
            const zlink::message_t &message, const spot_inbound_message_t &metadata) {
              auto &typed_spot = *static_cast<spot_type *> (spot);
              auto &typed_actor = *static_cast<actor_type *> (actor);
              auto payload =
                std::make_shared<message_type> (serializers.get<message_type> ().deserialize (
                  detail::encoded_payload_from_raw (message)));
              auto context = std::make_shared<message_context_t> (
                metadata.to_message_context (registered_packet_name));
              return detail::invoke_spot_member_keepalive (
                [&typed_spot, &typed_actor, context, payload] {
                    return (typed_spot.*Method) (typed_actor, *context, *payload);
                },
                serializers, context, payload);
          });
        registry.template register_actor_admission<spot_type, actor_type> ();
        return registry;
    }

    std::shared_ptr<detail::spot_context_state_t> _state;
};

class spot_create_call_t
{
  public:
    spot_create_call_t (spot_create_call_t &&) noexcept;
    spot_create_call_t &operator= (spot_create_call_t &&) noexcept;
    spot_create_call_t (const spot_create_call_t &) = delete;
    spot_create_call_t &operator= (const spot_create_call_t &) = delete;
    ~spot_create_call_t ();

    spot_create_call_t &in_mesh (std::string mesh_name);
    spot_create_call_t &creation_request (message_t request);
    template <typename TRequest>
    requires (!std::is_same_v<std::remove_cvref_t<TRequest>, zlink::message_t>
              && !std::is_same_v<std::remove_cvref_t<TRequest>, message_t>)
    spot_create_call_t &creation_request (TRequest request)
    {
        return creation_request (message_t::from (std::move (request)));
    }
    spot_create_call_t &timeout (std::chrono::milliseconds timeout);
    task_t<spot_create_result_t> submit ();
    task_t<spot_create_result_t> yield ();

  private:
    friend class spot_manager_t;
    explicit spot_create_call_t (
      std::shared_ptr<detail::spot_create_call_state_t> state);
    std::shared_ptr<detail::spot_create_call_state_t> _state;
};

class spot_manager_t
{
  public:
    spot_manager_t ();
    ~spot_manager_t ();

    spot_manager_t (spot_manager_t &&) noexcept;
    spot_manager_t &operator= (spot_manager_t &&) noexcept;
    spot_manager_t (const spot_manager_t &) = default;
    spot_manager_t &operator= (const spot_manager_t &) = default;

    spot_create_call_t create (std::string stable_type);
    spot_create_call_t get_or_create (spot_id_t spot_id,
                                      std::string stable_type);
    task_t<std::optional<spot_ref_t>> find (spot_id_t spot_id) const;
    task_t<bool> close (spot_ref_t spot);

  private:
    friend class spot_context_t;
    friend class spot_publisher_client_t;
    friend class detail::spot_node_runtime_t;
    friend class detail::spot_route_internal_dispatcher_t;
    explicit spot_manager_t (
      std::shared_ptr<detail::spot_node_builder_state_t> state);
    spot_manager_t (
      std::shared_ptr<detail::spot_node_builder_state_t> state,
      std::weak_ptr<detail::spot_context_state_t> source);
    std::optional<actor_ref_t> current_actor_ref (const actor_ref_t &actor_ref) const;
    result_t<std::optional<zlink::message_t>>
    relay_actor_packet (const actor_ref_t &actor_ref,
                        actor_context_t actor_context,
                        std::string_view packet_name,
                        const zlink::message_t &message,
                        service_provider_t &services,
                        serializer_registry_t &serializers,
                        spot_inbound_message_t metadata = {});
    result_t<std::optional<zlink::message_t>>
    relay_actor_packet (const actor_ref_t &actor_ref,
                        actor_context_t actor_context,
                        detail::stream_message_kind_t message_kind,
                        std::string_view packet_name,
                        const zlink::message_t &message,
                        service_provider_t &services,
                        serializer_registry_t &serializers,
                        spot_inbound_message_t metadata = {});

    std::shared_ptr<detail::spot_node_builder_state_t> _state;
    std::weak_ptr<detail::spot_context_state_t> _source;
};

class spot_publisher_client_t
{
  public:
    spot_publisher_client_t (spot_manager_t manager, serializer_registry_t &serializers);

    template <typename TEvent>
    publish_call_t publish (std::string channel_name,
                            std::string topic,
                            const TEvent &event) const
    {
        if (!_serializers) {
            return publish_call_t (
              result_t<void>::failure (
                framework_error_kind_t::not_configured,
                "logical multicast has no serializer registry"));
        }
        try {
            auto payload =
              detail::encoded_payload_to_raw (_serializers->get<TEvent> ().serialize (event));
            return publish_raw (std::move (channel_name), std::move (topic),
                                detail::message_name<TEvent> (), std::move (payload));
        }
        catch (const framework_exception_t &error) {
            return publish_call_t (
              detail::result_access_t::failure<void> (error));
        }
    }

  private:
    publish_call_t
    publish_raw (std::string channel_name,
                 std::string topic,
                 std::string packet_name,
                 zlink::message_t payload) const;

    spot_manager_t _manager;
    serializer_registry_t *_serializers = nullptr;
};

namespace detail
{
struct spot_lifecycle_callbacks_t
{
    std::function<std::shared_ptr<void> (spot_context_t)> create_spot_context_instance;
    std::function<std::shared_ptr<void> (entry_spot_context_t)> create_entry_context_instance;
    std::function<std::shared_ptr<void> (instance_spot_context_t)> create_instance_context_instance;
    std::function<task_t<spot_create_response_t> (
      void *, const zlink::message_t &, serializer_registry_t &)>
      on_create;
    std::function<void (void *)> on_initialize;
    std::function<void (
      void *, const spot_closing_context_t &, std::stop_token)> on_closing;
    std::function<void (
      void *, const spot_relocation_ready_completion_t &)>
      on_relocation_ready_completed;
};

} // namespace detail

class spot_node_builder_t
{
  public:
    spot_node_builder_t ();
    ~spot_node_builder_t ();

    spot_node_builder_t (spot_node_builder_t &&) noexcept;
    spot_node_builder_t &operator= (spot_node_builder_t &&) noexcept;
    spot_node_builder_t (const spot_node_builder_t &) = default;
    spot_node_builder_t &operator= (const spot_node_builder_t &) = default;

    // How long this node keeps the Message Follow route after a completed
    // relocation. The default is 30 seconds; zero disables Message Follow.
    spot_node_builder_t &set_message_follow_duration (std::chrono::milliseconds duration);
    template <typename TEntrySpot>
    requires detail::entry_spot_type<TEntrySpot>
    spot_node_builder_t &add_entry_spot (
      std::function<std::shared_ptr<TEntrySpot> (entry_spot_context_t)> factory)
    {
        if (!factory) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "Entry SPOT factory must not be empty");
        }
        auto &builder =
          add_spot_factory_erased (
            std::string ("entry"), std::type_index (typeid (TEntrySpot)),
            detail::spot_runtime_kind_t::entry,
            user_spot_execution_mode_t::spot_wide);
        register_context_lifecycle<TEntrySpot> ("entry", std::move (factory));
        return builder;
    }

    template <typename TSpot>
    requires detail::user_spot_type<TSpot>
    spot_node_builder_t &add_spot_factory (
      std::string stable_type,
      std::function<std::shared_ptr<TSpot> (spot_context_t)> factory,
      std::function<void (user_spot_factory_builder_t<TSpot> &)> configure)
    {
        if (!factory || !configure) {
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "User Spot factory and configure callback must not be empty");
        }
        auto factory_builder =
          std::make_shared<user_spot_factory_builder_t<TSpot>> ();
        retain_factory_builder (factory_builder);
        try {
            configure (*factory_builder);
            factory_builder->validate ();
        }
        catch (...) {
            factory_builder->seal ();
            throw;
        }
        const auto execution_mode = factory_builder->_execution_mode;
        const auto stable_type_limit = factory_builder->_stable_type_limit;
        const auto relocation_readiness =
          factory_builder->_relocation_readiness;
        const auto relocation = factory_builder->_relocation;
        factory_builder->seal ();
        const auto registered_type = stable_type;
        auto &builder = add_spot_factory_erased (
          std::move (stable_type), std::type_index (typeid (TSpot)),
          detail::spot_runtime_kind_t::user, execution_mode,
          stable_type_limit, relocation_readiness,
          relocation);
        register_context_lifecycle<TSpot> (
          registered_type, std::move (factory));
        return builder;
    }

    template <typename TSpot>
    requires std::derived_from<TSpot, instance_spot_t>
    spot_node_builder_t &add_instance_spot_factory (
      std::string stable_type,
      std::function<std::shared_ptr<TSpot> (instance_spot_context_t)> factory,
      std::function<void (instance_spot_factory_builder_t<TSpot> &)> configure)
    {
        if (!factory || !configure) {
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "Instance Spot factory and configure callback must not be empty");
        }
        auto factory_builder =
          std::make_shared<instance_spot_factory_builder_t<TSpot>> ();
        retain_factory_builder (factory_builder);
        try {
            configure (*factory_builder);
            factory_builder->validate ();
        }
        catch (...) {
            factory_builder->seal ();
            throw;
        }
        const auto stable_type_limit = factory_builder->_stable_type_limit;
        const auto relocation = factory_builder->_relocation;
        factory_builder->seal ();
        const auto registered_type = stable_type;
        auto &builder = add_spot_factory_erased (
          std::move (stable_type), std::type_index (typeid (TSpot)),
          detail::spot_runtime_kind_t::instance,
          user_spot_execution_mode_t::spot_wide,
          stable_type_limit,
          spot_relocation_readiness_mode_t::any_turn_boundary,
          relocation);
        register_context_lifecycle<TSpot> (registered_type, std::move (factory));
        return builder;
    }

    template <typename TActor, typename TActorFactory>
    spot_node_builder_t &
    add_actor_factory (std::string actor_type,
                       std::shared_ptr<TActorFactory> factory,
                       std::function<void (actor_factory_builder_t<TActor> &)> configure);

    spot_node_builder_t &
    add_spot_resolver (std::string name,
                       std::function<std::optional<spot_route_t> (spot_id_t)> resolver);

    spot_node_snapshot_t snapshot () const;

  private:
    friend class zlink_builder_t;
    friend class mesh_node_builder_t;
    friend struct detail::mesh_node_builder_state_t;
    friend class detail::spot_node_runtime_t;
    explicit spot_node_builder_t (std::shared_ptr<detail::spot_node_builder_state_t> state);
    detail::local_spot_create_result_t create_spot (std::string spot_name);
    detail::local_spot_create_result_t create_spot (std::string spot_name, const message_t &request);
    detail::local_spot_create_result_t get_or_create_spot (std::string spot_name, spot_id_t spot_id);
    detail::local_spot_create_result_t
    get_or_create_spot (std::string spot_name, spot_id_t spot_id, const message_t &request);
    task_t<std::optional<spot_info_t>> find_spot (spot_id_t spot_id) const;
    task_t<std::vector<spot_info_t>> list_spots () const;
    task_t<bool> close_spot (spot_id_t spot_id);
    void retain_factory_builder (std::shared_ptr<void> builder);
    std::optional<std::string> spot_name_for (spot_id_t spot_id) const;
    std::optional<spot_route_t> resolve_spot (spot_id_t spot_id) const;
    detail::local_spot_create_result_t create_spot_raw (
      std::string spot_name, zlink::message_t request);
    detail::local_spot_create_result_t
    get_or_create_spot_raw (std::string spot_name, spot_id_t spot_id, zlink::message_t request);

    spot_node_builder_t &
    add_spot_factory_erased (
      std::string spot_name,
      std::type_index spot_type,
      detail::spot_runtime_kind_t kind,
      user_spot_execution_mode_t execution_mode,
      std::int32_t stable_type_limit = 0,
      spot_relocation_readiness_mode_t relocation_readiness =
        spot_relocation_readiness_mode_t::any_turn_boundary,
      detail::factory_relocation_configuration_t relocation = {});
    spot_node_builder_t &
    accept_implicit_route_mesh (std::string route_channel_name,
                                std::vector<std::string> manual_connections = {});
    spot_node_builder_t &add_actor_factory_erased (
      std::string actor_type,
      std::type_index actor_instance_type,
      std::function<std::shared_ptr<void> (std::string)> create_instance,
      std::function<void (void *, const actor_ref_t &, void *)> configure_instance,
      std::function<std::optional<zlink::message_t> (void *, serializer_registry_t &)>
        serialize_instance,
      std::function<void (void *, const zlink::message_t &, serializer_registry_t &)>
        deserialize_instance,
      std::function<std::shared_ptr<void> (actor_context_t)>
        create_context_instance = {},
      detail::actor_join_completion_callback_t on_join_completed = {},
      detail::factory_relocation_configuration_t relocation = {},
      std::function<task_t<std::vector<std::byte>> (
        void *, std::stop_token)> capture = {},
      std::function<task_t<void> (
        void *, std::vector<std::byte>, std::stop_token)> restore = {});

    template <typename TSpot, typename TContext>
    void register_context_lifecycle (
      std::string spot_name,
      std::function<std::shared_ptr<TSpot> (TContext)> factory)
    {
        detail::spot_lifecycle_callbacks_t callbacks;
        auto create = [factory = std::move (factory)] (TContext context) {
            const auto expected_state = context._state;
            auto instance = factory (std::move (context));
            if (!instance) {
                return std::shared_ptr<void>{};
            }
            if (!instance->context ().has_same_source_fence (
                  spot_context_t (expected_state))) {
                throw framework_exception_t (
                  framework_error_kind_t::not_configured,
                  "Spot factory must return a Spot that exposes the provided Context");
            }
            instance->configure ();
            return std::static_pointer_cast<void> (std::move (instance));
        };
        if constexpr (std::is_same_v<TContext, entry_spot_context_t>) {
            callbacks.create_entry_context_instance = std::move (create);
        } else if constexpr (std::is_same_v<TContext, instance_spot_context_t>) {
            callbacks.create_instance_context_instance = std::move (create);
        } else {
            callbacks.create_spot_context_instance = std::move (create);
        }
        if constexpr (detail::user_spot_type<TSpot>) {
            callbacks.on_create =
              [] (void *spot, const zlink::message_t &request,
                  serializer_registry_t &serializers) {
                  return static_cast<TSpot *> (spot)->on_create (
                    message_t::from_raw (request, &serializers));
              };
        }
        callbacks.on_initialize = [] (void *spot) {
            static_cast<TSpot *> (spot)->on_initialize ().result ().value ();
        };
        callbacks.on_closing =
          [] (void *spot,
              const spot_closing_context_t &context,
              std::stop_token cleanup_cancellation) {
            static_cast<TSpot *> (spot)
              ->on_closing (context, cleanup_cancellation)
              .result ()
              .value ();
        };
        if constexpr (detail::user_spot_type<TSpot>) {
            callbacks.on_relocation_ready_completed =
              [] (void *spot,
                  const spot_relocation_ready_completion_t &completion) {
                  static_cast<TSpot *> (spot)
                    ->on_relocation_ready_completed (completion)
                    .result ()
                    .value ();
              };
        }
        register_lifecycle_erased (std::move (spot_name), std::move (callbacks));
    }
    void register_lifecycle_erased (std::string spot_name,
                                    detail::spot_lifecycle_callbacks_t callbacks);

    std::shared_ptr<detail::spot_node_builder_state_t> _state;
};

} // namespace zlink::framework
