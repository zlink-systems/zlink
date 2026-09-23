/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/stream_connector/contracts/calls/zlink_stream_calls.hpp>
#include <zlink/stream_connector/contracts/codec_registry.hpp>
#include <zlink/stream_connector/contracts/stream_payload.hpp>
#include <zlink/stream_connector/contracts/zlink_stream_connector_options.hpp>
#include <zlink/stream_connector/contracts/zlink_stream_subscription.hpp>

#include <chrono>
#include <atomic>
#include <cstdint>
#include <functional>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

namespace zlink::stream_connector
{

namespace detail
{
class actor_access_t;
}

class connector_t
{
  public:
    /// Creates a disconnected connector with default options.
    connector_t ();
    ~connector_t ();

    connector_t (connector_t &&) noexcept;
    connector_t &operator= (connector_t &&) noexcept;
    connector_t (const connector_t &) = default;
    connector_t &operator= (const connector_t &) = default;

    /// Returns true only while the underlying stream is connected.
    bool is_connected () const;

    /// Returns the current connection state snapshot.
    connection_state_t state () const;

    /// Returns why the connection last ended (stream-connector §6.2).
    ///
    /// Empty until the connection has ended once, including a first connect
    /// that failed. Reconnecting does not clear it: the value keeps the reason
    /// of the last ending, so code that never saw the disconnect event reads
    /// the same value afterwards.
    std::optional<close_reason_t> close_reason () const;

    /// Returns how many packets with this name arrived on the current
    /// connection (stream-connector §10).
    ///
    /// The count is of packets received. Consuming one does not lower it,
    /// whether a handler dispatched it or a wait surface took it, and the
    /// dispatch mode does not change it. A connection that is established
    /// restarts the count at zero.
    std::size_t received_count (std::string_view packet_name) const;

    template <typename TMessage> std::size_t received_count () const
    {
        return received_count (resolve_packet_name<TMessage> ());
    }

    /// Returns a copy of the options used by this connector.
    connector_options_t options () const;

    /// Returns the number of received packets waiting for manual callback dispatch.
    std::size_t pending_dispatch_count () const;

    /// Returns the codec registry owned by this connector.
    codec_registry_t &codecs ();

    /// Opens the stream connection and blocks until it succeeds or fails.
    result_t<void> connect ();

    /// Starts opening the stream connection and returns before completion is known.
    void connect (std::function<void (result_t<void>)> callback);

    /// Closes the stream connection and clears pending requests and received packets.
    result_t<void> close ();

    /// Starts closing the stream connection and returns before completion is delivered.
    void close (std::function<void (result_t<void>)> callback);

    /// Runs every pending On(...) callback when manual dispatch mode is used.
    ///
    /// In manual mode, received push packets do not invoke registered callbacks until dispatch()
    /// is called. wait_for(...) consumes matching received packets directly and does not require
    /// dispatch() to be called.
    result_t<void> dispatch ();

    /// Waits for the next unread packet with the given name and consumes it.
    result_t<packet_t> wait_for (std::string packet_name)
    {
        return wait_for (std::move (packet_name), options ().wait_timeout);
    }

    /// Waits for the next unread packet with the given name and consumes it.
    result_t<packet_t> wait_for (std::string packet_name, std::chrono::milliseconds timeout);

    /// Starts a typed wait call for the packet name resolved from TMessage.
    template <typename TMessage> wait_call_t<TMessage> wait_for ()
    {
        return wait_for<TMessage> (resolve_packet_name<TMessage> ());
    }

    /// Starts a typed wait call with the given timeout.
    template <typename TMessage> wait_call_t<TMessage> wait_for (std::chrono::milliseconds timeout)
    {
        return wait_for<TMessage> (resolve_packet_name<TMessage> ()).timeout (timeout);
    }

    /// Starts a typed wait call with the given packet name.
    template <typename TMessage> wait_call_t<TMessage> wait_for (std::string packet_name)
    {
        return wait_call_t<TMessage> (_state, std::move (packet_name), options ().wait_timeout);
    }

    /// Starts a typed wait call with the given packet name and timeout.
    template <typename TMessage>
    wait_call_t<TMessage> wait_for (std::string packet_name, std::chrono::milliseconds timeout)
    {
        return wait_for<TMessage> (std::move (packet_name)).timeout (timeout);
    }

    /// Starts a negative observation for an untyped packet name.
    expect_none_call_t<packet_t> expect_none (std::string packet_name)
    {
        return expect_none<packet_t> (std::move (packet_name));
    }

    /// Starts a negative observation for the packet name resolved from TMessage.
    template <typename TMessage> expect_none_call_t<TMessage> expect_none ()
    {
        return expect_none<TMessage> (resolve_packet_name<TMessage> ());
    }

    /// Starts a typed negative observation for the given packet name.
    template <typename TMessage> expect_none_call_t<TMessage> expect_none (std::string packet_name)
    {
        return expect_none_call_t<TMessage> (_state, std::move (packet_name));
    }

    /// Starts an ordered wait for untyped packets with the given name.
    wait_for_sequence_call_t<packet_t> wait_for_sequence (std::string packet_name)
    {
        return wait_for_sequence<packet_t> (std::move (packet_name));
    }

    /// Starts an ordered wait for the packet name resolved from TMessage.
    template <typename TMessage> wait_for_sequence_call_t<TMessage> wait_for_sequence ()
    {
        return wait_for_sequence<TMessage> (resolve_packet_name<TMessage> ());
    }

    /// Starts a typed ordered wait for the given packet name.
    template <typename TMessage>
    wait_for_sequence_call_t<TMessage> wait_for_sequence (std::string packet_name)
    {
        return wait_for_sequence_call_t<TMessage> (_state, std::move (packet_name),
                                                   options ().wait_timeout);
    }

    /// Registers a connection state callback (stream-connector §7).
    ///
    /// Keep the returned handle for as long as the handler must run; letting it
    /// go removes the registration.
    [[nodiscard]] subscription_t
    on_connection_state_changed (std::function<void (const connection_state_changed_t &)> handler);

    /// Registers an error callback (stream-connector §7).
    [[nodiscard]] subscription_t on_error (std::function<void (const error_t &)> handler);

    [[nodiscard]] subscription_t
    on_request_sending (std::function<void (request_sending_context_t &)> handler);
    [[nodiscard]] subscription_t
    on_reply_received (std::function<void (const reply_received_context_t &)> handler);

    /// Registers a disconnected callback (stream-connector §6.2, §7).
    ///
    /// The handler receives the close reason. The same value stays readable
    /// from close_reason() afterwards.
    [[nodiscard]] subscription_t
    on_disconnected (std::function<void (std::optional<close_reason_t>)> handler);

    std::vector<std::shared_ptr<actor_t>> actors () const;
    std::shared_ptr<actor_t> actor (std::string_view actor_id) const;
    [[nodiscard]] subscription_t
    on_actor_bound (std::function<void (const std::shared_ptr<actor_t> &)> handler);
    [[nodiscard]] subscription_t
    on_actor_unbound (std::function<void (const std::shared_ptr<actor_t> &)> handler);

    /// Starts a typed send call by copying the payload into a packet.
    template <typename TMessage> send_call_t send (const TMessage &message)
    {
        auto packet = make_packet<TMessage> ();
        packet.payload =
          detail::encode_typed_payload (_state, detail::to_packet_payload (message, 0));
        return send_call_t (_state, std::move (packet));
    }

    /// Starts a send call and transfers the packet into the call object.
    send_call_t send (packet_t packet)
    {
        if (packet.name.empty ()) {
            packet.name = "packet";
        }
        return send_call_t (_state, std::move (packet));
    }

    /// Starts a request call by copying the request payload into a packet.
    template <typename TRequest> request_call_t request (const TRequest &request)
    {
        auto packet = make_packet<TRequest> ();
        packet.payload =
          detail::encode_typed_payload (_state, detail::to_packet_payload (request, 0));
        return request_call_t (_state, std::move (packet), options ().request_timeout);
    }

    /// Starts a request call and transfers the packet into the call object.
    request_call_t request (packet_t packet)
    {
        if (packet.name.empty ()) {
            packet.name = "packet";
        }
        return request_call_t (_state, std::move (packet), options ().request_timeout);
    }

    /// Registers a packet callback for the given packet name.
    ///
    /// In manual dispatch mode the callback runs from dispatch(). In immediate dispatch mode it
    /// runs from the connector receive path. Keep the returned handle for as
    /// long as the handler must run; letting it go removes the registration
    /// (stream-connector §7).
    template <typename TMessage>
    [[nodiscard]] subscription_t on (std::string packet_name,
                                     std::function<void (const message_t<TMessage> &)> callback)
    {
        /* The connector owns this handler, so the handler holds a weak
         * reference back: a strong one would keep the connector state alive
         * through its own handler list. */
        std::weak_ptr<void> weak_state = _state;
        return on_packet_erased (
          std::move (packet_name),
          [weak_state, callback = std::move (callback)] (const packet_t &packet) {
              auto message = detail::decode_message<TMessage> (weak_state.lock (), packet);
              if (!message) {
                  return;
              }
              callback (message.value ());
          });
    }

    /// Registers a packet callback for the packet name resolved from TMessage.
    template <typename TMessage>
    [[nodiscard]] subscription_t on (std::function<void (const message_t<TMessage> &)> callback)
    {
        return on<TMessage> (resolve_packet_name<TMessage> (), std::move (callback));
    }

  private:
    friend class connector_factory_t;
    friend class actor_t;
    friend class detail::actor_access_t;
    friend std::shared_ptr<void> connector_internal_handle (const connector_t &connector);

    explicit connector_t (connector_options_t options);
    explicit connector_t (std::shared_ptr<void> state);

    /* stream-connector §5: a name declared on the payload type wins, a
     * configured name resolver comes next, and the type's simple name is the
     * fallback. A caller who names the packet on the builder still wins over
     * all three. */
    template <typename TMessage> std::string resolve_packet_name () const
    {
        if constexpr (detail::has_static_packet_name<TMessage> ()) {
            return detail::message_packet_name<TMessage> ();
        } else {
            return resolve_type_packet_name (detail::simple_type_name<TMessage> ());
        }
    }

    std::string resolve_type_packet_name (std::string type_name) const;

    template <typename TMessage> packet_t make_packet () const
    {
        return make_packet (std::type_index (typeid (TMessage)), resolve_packet_name<TMessage> ());
    }

    packet_t make_packet (std::type_index type, std::string packet_name) const;
    subscription_t on_packet_erased (std::string packet_name,
                                     std::function<void (const packet_t &)> handler);
    subscription_t on_actor_packet_erased (std::string packet_name,
                                           std::uint16_t actor_slot,
                                           std::function<void (const packet_t &)> handler);

    std::shared_ptr<void> _state;
    codec_registry_t _codecs;
};

class actor_t
{
  public:
    const std::string &actor_id () const noexcept { return _actor_id; }
    bool is_bound () const noexcept { return _bound->load (std::memory_order_acquire); }

    template <typename TMessage> send_call_t send (const TMessage &message)
    {
        auto call = _connector.send (message);
        call._actor_binding = detail::actor_binding_ref_t{_actor_slot, _bound, _actor_id};
        return call;
    }

    template <typename TRequest> request_call_t request (const TRequest &request)
    {
        auto call = _connector.request (request);
        call._actor_binding = detail::actor_binding_ref_t{_actor_slot, _bound, _actor_id};
        return call;
    }

    template <typename TMessage>
    [[nodiscard]] subscription_t on (std::string packet_name,
                                     std::function<void (const message_t<TMessage> &)> callback)
    {
        const auto actor_slot = _actor_slot;
        std::weak_ptr<void> weak_state = _connector._state;
        return _connector.on_actor_packet_erased (
          std::move (packet_name), actor_slot,
          [weak_state, callback = std::move (callback)] (const packet_t &packet) {
              auto message = detail::decode_message<TMessage> (weak_state.lock (), packet);
              if (message)
                  callback (message.value ());
          });
    }

    template <typename TMessage>
    [[nodiscard]] subscription_t on (std::function<void (const message_t<TMessage> &)> callback)
    {
        return on<TMessage> (_connector.template resolve_packet_name<TMessage> (),
                             std::move (callback));
    }

  private:
    friend class detail::actor_access_t;
    actor_t (connector_t connector,
             std::string actor_id,
             std::uint16_t actor_slot,
             std::shared_ptr<std::atomic_bool> bound) :
        _connector (std::move (connector)),
        _actor_id (std::move (actor_id)),
        _actor_slot (actor_slot),
        _bound (std::move (bound))
    {
    }

    connector_t _connector;
    std::string _actor_id;
    std::uint16_t _actor_slot = 0;
    std::shared_ptr<std::atomic_bool> _bound;
};

} // namespace zlink::stream_connector
