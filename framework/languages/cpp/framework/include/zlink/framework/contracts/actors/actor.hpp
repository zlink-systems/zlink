/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/framework/contracts/channels/call.hpp>
#include <zlink/framework/contracts/detail/message_payload.hpp>
#include <zlink/framework/contracts/detail/message_name.hpp>
#include <zlink/framework/contracts/errors/result.hpp>
#include <zlink/framework/contracts/messaging/message.hpp>
#include <zlink/framework/contracts/spots/spot.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>

#include <cstdint>
#include <climits>
#include <chrono>
#include <compare>
#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <variant>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace zlink::framework
{

namespace detail
{
inline bool is_valid_actor_id (std::string_view value) noexcept;
class actor_ref_access_t;
}

class actor_id_t final
{
  public:
    explicit actor_id_t (std::string value) : _value (std::move (value))
    {
        if (!detail::is_valid_actor_id (_value)) {
            throw std::invalid_argument (
              "ActorId must be valid UTF-8 and contain from 1 through 255 bytes");
        }
    }

    std::string_view value () const noexcept { return _value; }
    auto operator<=> (const actor_id_t &) const = default;

  private:
    std::string _value;
};

namespace detail
{
inline bool is_valid_actor_id (std::string_view value) noexcept
{
    if (value.empty () || value.size () > 255)
        return false;

    const auto *bytes = reinterpret_cast<const unsigned char *> (value.data ());
    std::size_t index = 0;
    while (index < value.size ()) {
        const auto first = bytes[index++];
        if (first <= 0x7f)
            continue;

        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            continuation_count = 1;
            code_point = first & 0x1f;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            continuation_count = 2;
            code_point = first & 0x0f;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            continuation_count = 3;
            code_point = first & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }

        if (index + continuation_count > value.size ())
            return false;
        for (std::size_t offset = 0; offset < continuation_count; ++offset) {
            const auto next = bytes[index++];
            if ((next & 0xc0) != 0x80)
                return false;
            code_point = (code_point << 6) | (next & 0x3f);
        }
        if (code_point < minimum || code_point > 0x10ffff
            || (code_point >= 0xd800 && code_point <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

class actor_gateway_state_t;
class actor_gateway_runtime_t;
class session_actor_binding_context_t;
class session_actor_manager_access_t;
class actor_manager_access_t;
class actor_manager_state_t;
class actor_create_call_state_t;
class spot_node_runtime_t;
} // namespace detail

class actor_ref_t final
{
  public:
    actor_ref_t (actor_id_t actor_id,
                 std::uint64_t object_generation,
                 std::string mesh_name,
                 node_rid_t node_rid);

    const node_rid_t &node_rid () const noexcept;
    const actor_id_t &actor_id () const noexcept;
    std::uint64_t object_generation () const noexcept;
    std::string_view mesh_name () const noexcept;

  private:
    friend class detail::actor_ref_access_t;
    actor_id_t _actor_id;
    std::uint64_t _object_generation = 0;
    std::string _mesh_name;
    node_rid_t _node_rid;
    std::string _actor_type;
};

class actor_client_t;
class route_client_t;
class actor_context_t;

struct actor_join_accepted_t
{
    std::uint64_t operation_id_high = 0;
    std::uint64_t operation_id_low = 0;
    actor_ref_t actor;
    std::optional<message_t> reply;
};

struct actor_join_rejected_t
{
    std::uint64_t operation_id_high = 0;
    std::uint64_t operation_id_low = 0;
    std::optional<message_t> reply;
};

struct actor_join_failed_t
{
    std::uint64_t operation_id_high = 0;
    std::uint64_t operation_id_low = 0;
    framework_error_kind_t error_kind = framework_error_kind_t::internal_failure;
};

using actor_join_completion_t =
  std::variant<actor_join_accepted_t,
               actor_join_rejected_t,
               actor_join_failed_t>;

namespace detail
{
inline actor_join_completion_t
actor_join_completion_from_erased (
  actor_join_completion_outcome_t outcome,
  std::uint64_t operation_id_high,
  std::uint64_t operation_id_low,
  const actor_ref_t *committed,
  const std::optional<message_t> &reply,
  framework_error_kind_t error_kind)
{
    if (outcome == actor_join_completion_outcome_t::accepted
        && committed != nullptr) {
        return actor_join_accepted_t{
          operation_id_high, operation_id_low, *committed, reply};
    }
    if (outcome == actor_join_completion_outcome_t::rejected) {
        return actor_join_rejected_t{
          operation_id_high, operation_id_low, reply};
    }
    return actor_join_failed_t{
      operation_id_high, operation_id_low, error_kind};
}
} // namespace detail

class actor_t
{
  public:
    virtual ~actor_t () = default;
    virtual actor_context_t &context () noexcept = 0;
    virtual const actor_context_t &context () const noexcept = 0;
    virtual void configure () {}
    virtual task_t<void>
    on_join_completed (const actor_join_completion_t &)
    {
        return task_t<void> (result_t<void>::success ());
    }
};

template <typename TActor>
requires std::derived_from<TActor, actor_t>
class actor_factory_t
{
  public:
    virtual ~actor_factory_t () = default;
    virtual task_t<std::shared_ptr<TActor>>
    create (actor_context_t context,
            std::stop_token operation_cancellation) = 0;
};

template <typename TActor>
class actor_relocation_adapter_t
{
  public:
    virtual ~actor_relocation_adapter_t () = default;
    virtual task_t<std::vector<std::byte>>
    capture (TActor &actor, std::stop_token operation_cancellation) = 0;
    virtual task_t<void>
    restore (TActor &actor,
             std::vector<std::byte> payload,
             std::stop_token operation_cancellation) = 0;
};

template <typename TActor>
class actor_factory_builder_t
{
  public:
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
    requires std::derived_from<TAdapter, actor_relocation_adapter_t<TActor>>
    void preserve_state_with ()
    {
        ensure_mutable ();
        static_assert (
          std::is_default_constructible_v<TAdapter>,
          "C++ Actor relocation adapters must be default constructible");
        select (detail::factory_relocation_kind_t::preserve_state,
                typeid (TAdapter));
        _capture = [] (void *actor,
                       std::stop_token operation_cancellation)
          -> task_t<std::vector<std::byte>> {
            auto adapter = std::make_shared<TAdapter> ();
            auto payload = co_await adapter->capture (
              *static_cast<TActor *> (actor), operation_cancellation);
            co_return payload;
        };
        _restore = [] (void *actor,
                       std::vector<std::byte> payload,
                       std::stop_token operation_cancellation)
          -> task_t<void> {
            auto adapter = std::make_shared<TAdapter> ();
            co_await adapter->restore (
              *static_cast<TActor *> (actor), std::move (payload),
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
              "Actor factory builder cannot be changed after the configure callback returns");
        }
    }

    void validate () const
    {
        if (_relocation.kind
            == detail::factory_relocation_kind_t::unspecified) {
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "Actor factory must select exactly one relocation policy");
        }
    }

    void select (detail::factory_relocation_kind_t kind,
                 std::type_index adapter_type)
    {
        if (_relocation.kind
            != detail::factory_relocation_kind_t::unspecified) {
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "Actor factory must select exactly one relocation policy");
        }
        _relocation = {kind, adapter_type};
    }

    detail::factory_relocation_configuration_t _relocation;
    std::function<task_t<std::vector<std::byte>> (
      void *, std::stop_token)> _capture;
    std::function<task_t<void> (
      void *, std::vector<std::byte>, std::stop_token)> _restore;
    bool _sealed = false;
};

class actor_send_call_t
{
  public:
    using metadata_map_t = std::map<std::string, std::string>;

    actor_send_call_t (actor_client_t &client,
                       actor_id_t actor_id,
                       std::string packet_name,
                       message_t message);

    actor_send_call_t &metadata (std::string key, std::string value);
    task_t<void> submit ();

  private:
    actor_client_t *_client;
    actor_id_t _actor_id;
    std::string _packet_name;
    message_t _message;
    metadata_map_t _metadata;
    std::shared_ptr<detail::submit_once_t> _submission =
      std::make_shared<detail::submit_once_t> ();
};

class actor_request_call_t
{
  public:
    using metadata_map_t = std::map<std::string, std::string>;

    actor_request_call_t (actor_client_t &client,
                          actor_id_t actor_id,
                          std::string packet_name,
                          message_t request);

    actor_request_call_t &timeout (std::chrono::milliseconds timeout);
    actor_request_call_t &metadata (std::string key, std::string value);

    template <typename TReply> task_t<TReply> submit ()
    {
        auto reply = co_await start (false);
        co_return reply.template decode<TReply> (serializers ());
    }

    template <typename TReply> task_t<TReply> yield ()
    {
        auto reply = co_await start (true);
        co_return reply.template decode<TReply> (serializers ());
    }

    task_t<message_t> submit_message ();
    task_t<message_t> yield_message ();

  private:
    task_t<message_t> start (bool release_turn);
    serializer_registry_t &serializers () const;

    actor_client_t *_client;
    actor_id_t _actor_id;
    std::string _packet_name;
    message_t _request;
    std::optional<std::chrono::milliseconds> _timeout;
    metadata_map_t _metadata;
};

class actor_client_t
{
  public:
    virtual ~actor_client_t () = default;

    template <typename TMessage>
    actor_send_call_t send (actor_id_t actor_id, TMessage message)
    {
        using message_type = std::remove_cvref_t<TMessage>;
        return actor_send_call_t (*this, std::move (actor_id),
                                  detail::message_name<message_type> (),
                                  message_t::from (std::move (message)));
    }

    template <typename TRequest>
    actor_request_call_t request (actor_id_t actor_id, TRequest request)
    {
        using request_type = std::remove_cvref_t<TRequest>;
        return actor_request_call_t (*this, std::move (actor_id),
                                     detail::message_name<request_type> (),
                                     message_t::from (std::move (request)));
    }

  protected:
    virtual task_t<void> send_erased (actor_id_t actor_id,
                                      std::string packet_name,
                                      message_t message,
                                      const actor_send_call_t::metadata_map_t &metadata) = 0;
    virtual task_t<message_t> request_erased (
      actor_id_t actor_id,
      std::string packet_name,
      message_t request,
      std::optional<std::chrono::milliseconds> timeout,
      const actor_request_call_t::metadata_map_t &metadata) = 0;
    virtual serializer_registry_t &actor_client_serializers () = 0;

  private:
    friend class actor_send_call_t;
    friend class actor_request_call_t;
};

class actor_directory_t
{
  public:
    virtual ~actor_directory_t () = default;
    virtual task_t<std::optional<actor_ref_t>> find (std::string actor_id) = 0;
};

struct actor_create_existing_t
{
    actor_ref_t actor;
};

struct actor_create_created_t
{
    actor_ref_t actor;
    std::optional<message_t> reply;
};

struct actor_create_rejected_t
{
    std::optional<message_t> reply;
};

using actor_create_result_t =
  std::variant<actor_create_existing_t,
               actor_create_created_t,
               actor_create_rejected_t>;

class actor_create_call_t
{
  public:
    actor_create_call_t (actor_create_call_t &&) noexcept;
    actor_create_call_t &operator= (actor_create_call_t &&) noexcept;
    actor_create_call_t (const actor_create_call_t &) = delete;
    actor_create_call_t &operator= (const actor_create_call_t &) = delete;
    ~actor_create_call_t ();

    actor_create_call_t &in_mesh (std::string mesh_name);
    actor_create_call_t &creation_request (message_t request);
    template <typename TCreation>
    requires (!std::is_same_v<std::remove_cvref_t<TCreation>, zlink::message_t>
              && !std::is_same_v<std::remove_cvref_t<TCreation>, message_t>)
    actor_create_call_t &creation_request (TCreation request)
    {
        return creation_request (message_t::from (std::move (request)));
    }
    actor_create_call_t &timeout (std::chrono::milliseconds timeout);
    task_t<actor_create_result_t> submit ();
    task_t<actor_create_result_t> yield ();

  private:
    friend class actor_manager_t;
    explicit actor_create_call_t (
      std::shared_ptr<detail::actor_create_call_state_t> state);
    std::shared_ptr<detail::actor_create_call_state_t> _state;
};

class actor_manager_t
{
  public:
    actor_manager_t ();
    ~actor_manager_t ();
    actor_manager_t (actor_manager_t &&) noexcept;
    actor_manager_t &operator= (actor_manager_t &&) noexcept;
    actor_manager_t (const actor_manager_t &) = default;
    actor_manager_t &operator= (const actor_manager_t &) = default;

    actor_create_call_t create (actor_id_t actor_id,
                                std::string stable_type);
    actor_create_call_t get_or_create (actor_id_t actor_id,
                                       std::string stable_type);
    task_t<std::optional<actor_ref_t>> find (actor_id_t actor_id) const;
    task_t<std::optional<spot_ref_t>> find_spot (actor_id_t actor_id) const;
    task_t<bool> destroy (actor_ref_t actor);

  private:
    friend class detail::actor_manager_access_t;
    explicit actor_manager_t (
      std::shared_ptr<detail::actor_manager_state_t> state);
    std::shared_ptr<detail::actor_manager_state_t> _state;
};

namespace detail
{
struct actor_join_reply_t
{
    int result_code = 0;
    actor_ref_t actor;
    zlink::message_t reply;
};
} // namespace detail

class actor_join_call_t
{
  public:
    using deferred_fn_t = std::function<void (std::chrono::milliseconds)>;

    explicit actor_join_call_t (deferred_fn_t deferred) :
        _deferred (std::move (deferred))
    {}
    actor_join_call_t (actor_join_call_t &&) noexcept = default;
    actor_join_call_t &operator= (actor_join_call_t &&) noexcept = default;
    actor_join_call_t (const actor_join_call_t &) = delete;
    actor_join_call_t &operator= (const actor_join_call_t &) = delete;

    actor_join_call_t &timeout (std::chrono::milliseconds timeout)
    {
        if (timeout.count () < 1 || timeout.count () > INT_MAX) {
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "Actor join timeout must be from 1 through INT_MAX milliseconds");
        }
        _timeout = timeout;
        return *this;
    }

    void defer ()
    {
        if (_deferred_once) {
            throw framework_exception_t (
              framework_error_kind_t::invalid_operation,
              "Actor join call was already deferred");
        }
        _deferred_once = true;
        if (!_deferred) {
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "Actor join call has no deferred operation");
        }
        const auto deadline = std::chrono::steady_clock::now () + _timeout;
        std::shared_ptr<detail::deferred_barrier_t> barrier;
        if (_reserve_barrier) {
            auto reserved = _reserve_barrier ();
            if (!reserved) {
                const auto *error = reserved.error ();
                throw framework_exception_t (
                  reserved.error_kind (),
                  error != nullptr ? error->what ()
                                   : "Actor join barrier reservation failed");
            }
            barrier = std::move (reserved.value ());
        }
        auto registered = detail::defer_current_serial_turn (
          [deferred = std::move (_deferred), deadline,
           barrier] () mutable {
              auto run = [deferred = std::move (deferred),
                          deadline] () mutable {
                  const auto now = std::chrono::steady_clock::now ();
                  if (now >= deadline) {
                      throw framework_exception_t (
                        framework_error_kind_t::deadline_exceeded,
                        "Deferred Actor join deadline elapsed before activation");
                  }
                  deferred (
                    std::chrono::duration_cast<std::chrono::milliseconds> (
                      deadline - now));
              };
              if (barrier) {
                  const auto activated = barrier->activate (
                    std::move (run));
                  if (!activated) {
                      const auto *error = activated.error ();
                      throw framework_exception_t (
                        activated.error_kind (),
                        error != nullptr ? error->what ()
                                         : "Actor join barrier activation failed");
                  }
                  return;
              }
              run ();
          },
          [barrier] {
              if (barrier)
                  barrier->cancel ();
          });
        if (!registered) {
            if (barrier)
                barrier->cancel ();
            const auto *error = registered.error ();
            throw framework_exception_t (
              registered.error_kind (),
              error != nullptr ? error->what ()
                               : "Actor join defer registration failed");
        }
    }

  private:
    friend class actor_context_t;

    actor_join_call_t (
      deferred_fn_t deferred,
      detail::deferred_barrier_reserver_t reserve_barrier) :
        _deferred (std::move (deferred)),
        _reserve_barrier (std::move (reserve_barrier))
    {}

    deferred_fn_t _deferred;
    detail::deferred_barrier_reserver_t _reserve_barrier;
    std::chrono::milliseconds _timeout{5000};
    bool _deferred_once = false;
};


class relay_request_call_t : private detail::call_facade_t<relay_request_call_t, zlink::message_t>
{
  private:
    using base_t = detail::call_facade_t<relay_request_call_t, zlink::message_t>;

  public:
    explicit relay_request_call_t (result_t<zlink::message_t> result) : base_t (std::move (result))
    {
    }

    using base_t::submit;
    using base_t::timeout;
    using base_t::yield;
};

class bound_session_t
{
  public:
    bound_session_t ();
    ~bound_session_t ();

    bound_session_t (bound_session_t &&) noexcept;
    bound_session_t &operator= (bound_session_t &&) noexcept;
    bound_session_t (const bound_session_t &) = default;
    bound_session_t &operator= (const bound_session_t &) = default;

    bound_session_send_call_t send (const message_t &payload);

    template <typename TMessage>
    requires (!std::is_same_v<std::remove_cvref_t<TMessage>, message_t>
              && !std::is_same_v<std::remove_cvref_t<TMessage>, zlink::message_t>)
      bound_session_send_call_t send (const TMessage &message)
    {
        using message_type = std::remove_cvref_t<TMessage>;
        return send_typed (detail::message_name<message_type> (),
                           std::type_index (typeid (message_type)),
                           [&message] (serializer_registry_t &serializers) {
                               return serializers.template get<message_type> ().serialize (message);
                           });
    }
    task_t<void> disconnect ();

  private:
    friend class actor_context_t;
    friend class session_actor_t;
    friend class session_actor_manager_t;
    friend class detail::actor_gateway_runtime_t;

    explicit bound_session_t (std::shared_ptr<detail::actor_gateway_state_t> state,
                              actor_ref_t actor_ref,
                              std::uint64_t expected_binding_generation = 0);

    bound_session_send_call_t
    send_typed (std::string packet_name,
                std::type_index message_type,
                std::function<encoded_payload_t (serializer_registry_t &)> encode_payload);
    bound_session_send_call_t
    send_typed (std::string packet_name, std::type_index message_type, const void *message);
    bound_session_send_call_t send_erased (std::string packet_name,
                                           stream_codec_t codec,
                                           const zlink::message_t &payload);

    std::shared_ptr<detail::actor_gateway_state_t> _state;
    std::shared_ptr<actor_ref_t> _actor_ref;
    std::uint64_t _expected_binding_generation = 0;
};

class actor_context_t
{
  public:
    ~actor_context_t ();

    actor_context_t (actor_context_t &&) noexcept;
    actor_context_t &operator= (actor_context_t &&) = delete;
    actor_context_t (const actor_context_t &) = delete;
    actor_context_t &operator= (const actor_context_t &) = delete;

    const actor_ref_t &actor_ref () const noexcept;
    const actor_id_t &actor_id () const noexcept;
    std::uint64_t object_generation () const noexcept;
    std::string_view mesh_name () const noexcept;
    std::optional<spot_id_t> spot_id () const;
    bound_session_t bound_session () const;

  private:
    actor_join_call_t join_spot_payload (spot_id_t spot_id, const zlink::message_t &request)
    {
        auto context = std::shared_ptr<actor_context_t> (
          new actor_context_t (
            _state, *_actor_ref, _source_binding_generation, _mesh_name));
        context->_actor_ref = _actor_ref;
        return actor_join_call_t (
          [context, spot_id = std::move (spot_id), request] (
            std::chrono::milliseconds timeout) mutable {
              const auto erased =
                context->join_spot_erased (std::move (spot_id), request, timeout);
              if (!erased) {
                  const auto *error = erased.error ();
                  throw framework_exception_t (
                    erased.error_kind (),
                    error != nullptr ? error->what () : "actor join spot failed");
              }
          },
          [context] {
              return context->reserve_join_barrier ();
          });
    }

  public:
    actor_join_call_t join_spot (spot_id_t spot_id, const message_t &request)
    {
        auto *serializers = serializer_registry ();
        if (serializers == nullptr) {
            throw framework_exception_t (
              framework_error_kind_t::protocol_error,
              "actor join spot requires a serializer registry");
        }
        return join_spot_payload (std::move (spot_id), request.to_raw (*serializers));
    }

    template <typename TRequest>
    requires (!std::is_same_v<std::remove_cvref_t<TRequest>, message_t>) actor_join_call_t
      join_spot (spot_id_t spot_id, const TRequest &request)
    {
        auto *serializers = serializer_registry ();
        if (serializers == nullptr) {
            throw framework_exception_t (
              framework_error_kind_t::protocol_error,
              "actor join spot requires a serializer registry");
        }
        return join_spot_payload (
          std::move (spot_id),
          detail::encoded_payload_to_raw (serializers->get<TRequest> ().serialize (request)));
    }

    actor_join_call_t join_spot (spot_id_t spot_id)
    {
        return join_spot_payload (std::move (spot_id), zlink::message_t{});
    }

  private:
    actor_join_call_t join_entry_spot_payload (const zlink::message_t &request);

  public:
    actor_join_call_t join_entry_spot (const message_t &request)
    {
        auto *serializers = serializer_registry ();
        if (serializers == nullptr) {
            throw framework_exception_t (
              framework_error_kind_t::protocol_error,
              "actor join entry spot requires a serializer registry");
        }
        return join_entry_spot_payload (request.to_raw (*serializers));
    }

    template <typename TRequest>
    requires (!std::is_same_v<std::remove_cvref_t<TRequest>, message_t>)
      actor_join_call_t
      join_entry_spot (const TRequest &request)
    {
        auto *serializers = serializer_registry ();
        if (serializers == nullptr) {
            throw framework_exception_t (
              framework_error_kind_t::protocol_error,
              "actor join entry spot requires a serializer registry");
        }
        return join_entry_spot_payload (
          detail::encoded_payload_to_raw (serializers->get<TRequest> ().serialize (request)));
    }

    actor_join_call_t join_entry_spot ()
    {
        return join_entry_spot_payload (zlink::message_t{});
    }

  private:
    friend class spot_node_builder_t;
    friend class detail::spot_node_runtime_t;
    friend class session_actor_t;
    friend class session_actor_manager_t;
    friend class detail::actor_gateway_runtime_t;
    actor_context_t ();
    explicit actor_context_t (std::shared_ptr<detail::actor_gateway_state_t> state,
                              actor_ref_t actor_ref,
                              std::uint64_t source_binding_generation = 0,
                              std::string mesh_name = {});

    bool has_same_source_fence (const actor_context_t &other) const noexcept;

    result_t<detail::actor_join_reply_t> join_spot_erased (spot_id_t spot_id,
                                                           const zlink::message_t &request,
                                                           std::chrono::milliseconds timeout);
    result_t<std::shared_ptr<detail::deferred_barrier_t>>
    reserve_join_barrier () const;
    serializer_registry_t *serializer_registry () const noexcept;
    std::optional<zlink::message_t> create_payload () const;

    std::shared_ptr<detail::actor_gateway_state_t> _state;
    std::shared_ptr<actor_ref_t> _actor_ref;
    std::uint64_t _source_binding_generation = 0;
    std::string _mesh_name;
};

class session_actor_t
{
  public:
    ~session_actor_t ();

    session_actor_t (session_actor_t &&) noexcept;
    session_actor_t &operator= (session_actor_t &&) noexcept;
    session_actor_t (const session_actor_t &) = default;
    session_actor_t &operator= (const session_actor_t &) = default;

    const actor_ref_t &ref () const noexcept;
    std::string_view actor_id () const noexcept;
    actor_context_t context () const;
    bound_session_t bound_session () const;
    task_t<void> relay (const zlink::message_t &payload);
    task_t<void> relay (std::string packet_name, const zlink::message_t &payload);
    relay_request_call_t relay_request (const zlink::message_t &payload);
    relay_request_call_t relay_request (std::string packet_name, const zlink::message_t &payload);
    task_t<void> notify_disconnected ();

  private:
    friend class session_actor_manager_t;
    friend class detail::actor_gateway_runtime_t;

    explicit session_actor_t (std::shared_ptr<detail::actor_gateway_state_t> state,
                              actor_ref_t ref,
                              std::uint64_t binding_token = 0);
    task_t<void> relay_internal (const zlink::message_t &payload);

    std::shared_ptr<detail::actor_gateway_state_t> _state;
    actor_ref_t _ref;
    std::uint64_t _binding_token = 0;
};

class session_actor_manager_t
{
  public:
    session_actor_manager_t ();
    ~session_actor_manager_t ();

    session_actor_manager_t (session_actor_manager_t &&) noexcept;
    session_actor_manager_t &operator= (session_actor_manager_t &&) noexcept;
    session_actor_manager_t (const session_actor_manager_t &) = default;
    session_actor_manager_t &operator= (const session_actor_manager_t &) = default;

    result_t<session_actor_t> create (std::string actor_type, std::string actor_id);
    result_t<session_actor_t>
    create (std::string actor_type, std::string actor_id, const zlink::message_t &request);
    result_t<session_actor_t>
    create (std::string actor_type, std::string actor_id, const message_t &request);
    template <typename TRequest>
    requires (!std::is_same_v<std::remove_cvref_t<TRequest>, zlink::message_t>
              && !std::is_same_v<std::remove_cvref_t<TRequest>, message_t>)
      result_t<session_actor_t> create (std::string actor_type,
                                        std::string actor_id,
                                        const TRequest &request)
    {
        try {
            return create (std::move (actor_type), std::move (actor_id), message_t::from (request));
        }
        catch (const framework_exception_t &error) {
            return detail::result_access_t::failure<session_actor_t> (error);
        }
    }
    std::optional<session_actor_t> find (std::string actor_id) const;
    result_t<session_actor_t> get_or_create (std::string actor_type, std::string actor_id);
    result_t<session_actor_t>
    get_or_create (std::string actor_type, std::string actor_id, const zlink::message_t &request);
    result_t<session_actor_t>
    get_or_create (std::string actor_type, std::string actor_id, const message_t &request);
    template <typename TRequest>
    requires (!std::is_same_v<std::remove_cvref_t<TRequest>, zlink::message_t>
              && !std::is_same_v<std::remove_cvref_t<TRequest>, message_t>)
      result_t<session_actor_t> get_or_create (std::string actor_type,
                                               std::string actor_id,
                                               const TRequest &request)
    {
        try {
            return get_or_create (std::move (actor_type), std::move (actor_id),
                                  message_t::from (request));
        }
        catch (const framework_exception_t &error) {
            return detail::result_access_t::failure<session_actor_t> (error);
        }
    }
    request_call_t<session_actor_t> bind (actor_ref_t actor_ref);
    request_call_t<session_actor_t> bind_or_get (actor_ref_t actor_ref);

  private:
    friend class detail::actor_gateway_runtime_t;
    friend class detail::session_actor_manager_access_t;
    explicit session_actor_manager_t (std::shared_ptr<detail::actor_gateway_state_t> state);
    result_t<session_actor_t> create_erased (std::string actor_type,
                                             std::string actor_id,
                                             std::optional<zlink::message_t> request);
    result_t<session_actor_t> get_or_create_erased (std::string actor_type,
                                                    std::string actor_id,
                                                    std::optional<zlink::message_t> request);
    zlink::message_t serialize_request (std::type_index request_type, const void *request) const;
    std::uint64_t bind_current_session (const actor_ref_t &actor_ref);

    std::shared_ptr<detail::actor_gateway_state_t> _state;
    std::shared_ptr<detail::session_actor_binding_context_t> _binding_context;
};

template <typename TActor, typename TActorFactory>
spot_node_builder_t &
spot_node_builder_t::add_actor_factory (
  std::string actor_type,
  std::shared_ptr<TActorFactory> factory,
  std::function<void (actor_factory_builder_t<TActor> &)> configure)
{
    static_assert (std::derived_from<TActor, actor_t>);
    static_assert (
      std::derived_from<TActorFactory, actor_factory_t<TActor>>);
    if (!factory || !configure) {
        throw framework_exception_t (
          framework_error_kind_t::not_configured,
          "Actor factory and configure callback must not be empty");
    }
    auto factory_builder =
      std::make_shared<actor_factory_builder_t<TActor>> ();
    retain_factory_builder (factory_builder);
    try {
        configure (*factory_builder);
        factory_builder->validate ();
    }
    catch (...) {
        factory_builder->seal ();
        throw;
    }
    const auto relocation = factory_builder->_relocation;
    auto capture = factory_builder->_capture;
    auto restore = factory_builder->_restore;
    factory_builder->seal ();
    return add_actor_factory_erased (
      std::move (actor_type), std::type_index (typeid (TActor)),
      {},
      [] (void *actor, const actor_ref_t &,
          void *actor_context) {
          if (actor_context == nullptr
              || !static_cast<TActor *> (actor)->context ()
                    .has_same_source_fence (
                      *static_cast<actor_context_t *> (
                        actor_context))) {
              throw framework_exception_t (
                framework_error_kind_t::not_configured,
                "Actor factory must return an Actor that exposes the provided Context");
          }
      },
      [] (void *, serializer_registry_t &)
        -> std::optional<zlink::message_t> { return std::nullopt; },
      [] (void *, const zlink::message_t &, serializer_registry_t &) {},
      [factory = std::move (factory)] (
        actor_context_t context) -> std::shared_ptr<void> {
          auto expected = actor_context_t (
            context._state, context.actor_ref (),
            context._source_binding_generation, context._mesh_name);
          auto created = factory->create (std::move (context), {}).result ();
          if (!created) {
              const auto *error = created.error ();
              throw framework_exception_t (
                created.error_kind (),
                error != nullptr ? error->what ()
                                 : "Actor factory failed");
          }
          if (!created.value ()) {
              throw framework_exception_t (
                framework_error_kind_t::not_found,
                "Actor factory returned null");
          }
          if (!created.value ()->context ().has_same_source_fence (expected)) {
              throw framework_exception_t (
                framework_error_kind_t::not_configured,
                "Actor factory must return an Actor that exposes the provided Context");
          }
          created.value ()->configure ();
          return std::static_pointer_cast<void> (
            std::move (created.value ()));
      },
      [] (void *actor,
          detail::actor_join_completion_outcome_t outcome,
          std::uint64_t operation_id_high,
          std::uint64_t operation_id_low,
          const actor_ref_t *committed,
          const std::optional<message_t> &reply,
          framework_error_kind_t error_kind,
          bool) {
          const auto completion =
            detail::actor_join_completion_from_erased (
              outcome, operation_id_high, operation_id_low, committed,
              reply, error_kind);
          return static_cast<TActor *> (actor)
            ->on_join_completed (completion);
      },
      relocation, std::move (capture), std::move (restore));
}

} // namespace zlink::framework
