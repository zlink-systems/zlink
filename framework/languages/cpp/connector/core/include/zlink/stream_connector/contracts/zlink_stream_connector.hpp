/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/stream_connector/contracts/calls/zlink_stream_calls.hpp>
#include <zlink/stream_connector/contracts/codec_registry.hpp>
#include <zlink/stream_connector/contracts/stream_payload.hpp>
#include <zlink/stream_connector/contracts/zlink_stream_connector_options.hpp>

#include <chrono>
#include <functional>
#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>
#include <typeindex>
#include <utility>

namespace zlink::stream_connector
{

class inbound_observer_registration_t
{
  public:
    inbound_observer_registration_t () = default;
    ~inbound_observer_registration_t ();

    inbound_observer_registration_t (inbound_observer_registration_t &&) noexcept;
    inbound_observer_registration_t &operator= (inbound_observer_registration_t &&) noexcept;

    inbound_observer_registration_t (const inbound_observer_registration_t &) = delete;
    inbound_observer_registration_t &operator= (const inbound_observer_registration_t &) = delete;

    void close ();
    explicit operator bool () const noexcept;

  private:
    friend class connector_t;
    inbound_observer_registration_t (std::shared_ptr<void> state, std::uint64_t id);

    std::shared_ptr<void> _state;
    std::uint64_t _id = 0;
};

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

    /// Runs one pending On(...) callback when manual dispatch mode is used.
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
        return wait_for<TMessage> (detail::message_packet_name<TMessage> ());
    }

    /// Starts a typed wait call with the given timeout.
    template <typename TMessage> wait_call_t<TMessage> wait_for (std::chrono::milliseconds timeout)
    {
        return wait_for<TMessage> (detail::message_packet_name<TMessage> ()).timeout (timeout);
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
        return expect_none<TMessage> (detail::message_packet_name<TMessage> ());
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
        return wait_for_sequence<TMessage> (detail::message_packet_name<TMessage> ());
    }

    /// Starts a typed ordered wait for the given packet name.
    template <typename TMessage>
    wait_for_sequence_call_t<TMessage> wait_for_sequence (std::string packet_name)
    {
        return wait_for_sequence_call_t<TMessage> (
          _state, std::move (packet_name), options ().wait_timeout);
    }

    /// Registers a connection state callback owned by this connector.
    connector_t &
    on_connection_state_changed (std::function<void (const connection_state_changed_t &)> handler);

    /// Registers an error callback owned by this connector.
    connector_t &on_error (std::function<void (const error_t &)> handler);

    /// Registers an inbound observer before connect().
    ///
    /// The observer receives immutable snapshots for logging, metrics, or tracing. It cannot
    /// change packet dispatch, complete requests, or reply to messages. The callback is scheduled
    /// outside the receive path.
    inbound_observer_registration_t
    observe_inbound (std::function<void (const inbound_observation_t &)> observer);

    /// Registers a disconnected callback owned by this connector.
    connector_t &on_disconnected (std::function<void ()> handler);

    /// Starts a typed send call by copying the payload into a packet.
    template <typename TMessage> send_call_t send (const TMessage &message)
    {
        auto packet = make_packet<TMessage> ();
        packet.payload = detail::to_packet_payload (message, 0);
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
        packet.payload = detail::to_packet_payload (request, 0);
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
    /// runs from the connector receive path. The connector owns the callback until it is closed or
    /// destroyed.
    template <typename TMessage>
    connector_t &on (std::string packet_name, std::function<void (const TMessage &)> callback)
    {
        return on_packet_erased (
          std::move (packet_name), [callback = std::move (callback)] (const packet_t &packet) {
              if constexpr (std::is_same_v<TMessage, packet_t>) {
                  callback (packet);
              } else {
                  TMessage message{};
                  detail::apply_packet_payload (message, packet.codec, packet.payload, 0);
                  callback (std::move (message));
              }
          });
    }

    /// Registers a packet callback for the packet name resolved from TMessage.
    template <typename TMessage> connector_t &on (std::function<void (const TMessage &)> callback)
    {
        return on<TMessage> (detail::message_packet_name<TMessage> (), std::move (callback));
    }

  private:
    friend class connector_factory_t;
    friend std::shared_ptr<void> connector_internal_handle (const connector_t &connector);

    explicit connector_t (connector_options_t options);
    template <typename TMessage> packet_t make_packet () const
    {
        return make_packet (std::type_index (typeid (TMessage)),
                            detail::message_packet_name<TMessage> ());
    }

    packet_t make_packet (std::type_index type, std::string packet_name) const;
    connector_t &on_packet_erased (std::string packet_name,
                                   std::function<void (const packet_t &)> handler);

    std::shared_ptr<void> _state;
    codec_registry_t _codecs;
};

} // namespace zlink::stream_connector
