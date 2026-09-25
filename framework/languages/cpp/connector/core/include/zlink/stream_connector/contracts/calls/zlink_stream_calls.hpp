/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/stream_connector/contracts/result.hpp>
#include <zlink/stream_connector/contracts/stream_payload.hpp>
#include <zlink/stream_connector/contracts/zlink_stream_connector_options.hpp>
#include <zlink/stream_connector/contracts/zlink_stream_interfaces.hpp>
#include <zlink/stream_connector/contracts/zlink_stream_models.hpp>

#include <chrono>
#include <atomic>
#include <functional>
#if ZLINK_HAS_EXCEPTIONS
#include <future>
#include <stdexcept>
#endif
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace zlink::stream_connector
{

namespace detail
{
struct actor_binding_ref_t
{
    std::uint16_t slot = 0;
    std::shared_ptr<std::atomic_bool> bound;
    std::string actor_id;
};

struct request_reply_t
{
    packet_t packet;
};

/* Applies the connector's typed codec (stream-connector §5.4) to a received
 * payload before the payload type decodes it. */
std::vector<std::uint8_t> encode_typed_payload (const std::shared_ptr<void> &state,
                                                const std::vector<std::uint8_t> &payload);
std::vector<std::uint8_t> decode_typed_payload (const std::shared_ptr<void> &state,
                                                const packet_t &packet);
std::vector<std::uint8_t> decode_typed_reply (const std::shared_ptr<void> &state,
                                              codec_t codec,
                                              const std::vector<std::uint8_t> &payload);

/* Rebuilds the received message from the packet the connector queued
 * (stream-connector §5.5): the wait surfaces hand the caller a message, not a
 * bare payload, so a predicate also sees the packet name, the metadata and the
 * actor identity. */
template <typename TMessage>
result_t<message_t<TMessage>> decode_message (const std::shared_ptr<void> &state, packet_t packet)
{
    message_t<TMessage> message;
    message.packet_name = packet.name;
    message.metadata = packet.metadata;
    message.actor_id = packet.actor_id;
    if constexpr (std::is_same_v<TMessage, packet_t>) {
        message.payload = std::move (packet);
    } else {
        auto decoded =
          decode_typed_message<TMessage> (packet.codec, decode_typed_payload (state, packet));
        if (!decoded) {
            return result_t<message_t<TMessage>>::failure (
              decoded.error_code ().value_or (error_code_t::frame_decode_failed),
              decoded.error () ? decoded.error ()->message
                               : "stream connector payload decode failed");
        }
        message.payload = std::move (decoded.value ());
    }
    return result_t<message_t<TMessage>>::success (std::move (message));
}

result_t<request_reply_t>
submit_request (std::shared_ptr<void> state, packet_t packet, std::chrono::milliseconds timeout);
result_t<request_reply_t> submit_request (std::shared_ptr<void> state,
                                          packet_t packet,
                                          std::chrono::milliseconds timeout,
                                          std::optional<actor_binding_ref_t> actor_binding);
void submit_request_async (std::shared_ptr<void> state,
                           packet_t packet,
                           std::chrono::milliseconds timeout,
                           std::function<void (result_t<request_reply_t>)> callback,
                           bool deliver_direct = false,
                           std::optional<actor_binding_ref_t> actor_binding = std::nullopt,
                           std::shared_ptr<std::vector<std::uint64_t>> reply_hook_ids = {});
result_t<packet_t> submit_wait (std::shared_ptr<void> state,
                                std::string packet_name,
                                std::function<bool (const packet_t &)> predicate,
                                std::chrono::milliseconds timeout);
void submit_wait_async (std::shared_ptr<void> state,
                        std::string packet_name,
                        std::function<bool (const packet_t &)> predicate,
                        std::chrono::milliseconds timeout,
                        std::function<void (result_t<packet_t>)> callback);
void post_runtime_operation (std::function<void ()> operation);
void schedule_delivery (std::shared_ptr<void> state, std::function<void ()> callback);
void run_request_sending (const std::shared_ptr<void> &state, request_sending_context_t &context);
void run_reply_received (const std::shared_ptr<void> &state,
                         const reply_received_context_t &context,
                         const std::vector<std::uint64_t> &handler_ids);
std::vector<std::uint64_t> capture_reply_hook_ids (const std::shared_ptr<void> &state);
void schedule_reply_received (const std::shared_ptr<void> &state, reply_received_context_t context);
} // namespace detail

class send_call_t
{
  public:
    /// Creates an unbound send call that fails with configuration_error when submitted.
    send_call_t ();
    ~send_call_t ();

    send_call_t (send_call_t &&) noexcept;
    send_call_t &operator= (send_call_t &&) noexcept;
    send_call_t (const send_call_t &) = default;
    send_call_t &operator= (const send_call_t &) = default;

    /// Overrides the packet name sent with this call.
    send_call_t &packet_name (std::string name);

    /// Adds or replaces one metadata value copied into the outbound packet.
    send_call_t &metadata (std::string key, std::string value);

    /// Replaces the outbound packet metadata.
    send_call_t &metadata (metadata_t metadata);

    /* stream-connector §5.4 keeps the codec on the creation options; there is
     * deliberately no per-operation codec on this builder. */

    /// Marks the outbound packet for compression when compression is available.
    send_call_t &compress ();

    /// Gives the packet to the connector for delivery.
    void submit ();

  private:
    friend class connector_t;
    friend class actor_t;
    send_call_t (std::shared_ptr<void> state, packet_t packet);

    std::shared_ptr<void> _state;
    packet_t _packet;
    std::optional<detail::actor_binding_ref_t> _actor_binding;
};

class request_call_t
{
  public:
    /// Creates an unbound request call that fails with configuration_error when submitted.
    request_call_t () = default;

    /// Overrides the packet name sent with this request.
    request_call_t &packet_name (std::string name)
    {
        _packet.name = std::move (name);
        return *this;
    }

    /// Adds or replaces one metadata value copied into the outbound request packet.
    request_call_t &metadata (std::string key, std::string value)
    {
        _packet.metadata.with (std::move (key), std::move (value));
        return *this;
    }

    /// Replaces the outbound request metadata.
    request_call_t &metadata (metadata_t metadata)
    {
        _packet.metadata = std::move (metadata);
        return *this;
    }

    /* stream-connector §5.4 keeps the codec on the creation options; there is
     * deliberately no per-operation codec on this builder. */

    /// Sets the request timeout used by submit.
    request_call_t &timeout (std::chrono::milliseconds timeout)
    {
        _timeout = timeout;
        return *this;
    }

    /// Marks the outbound request packet for compression when compression is available.
    request_call_t &compress ()
    {
        _packet.compressed = true;
        return *this;
    }

    /// Sends the request, waits for the correlated reply, and decodes it as TReply.
    template <typename TReply> result_t<TReply> submit ()
    {
        if (!_state) {
            return result_t<TReply>::failure (error_code_t::configuration_error,
                                              "request call has no connector");
        }
        const auto started = std::chrono::steady_clock::now ();
        const auto request_name = _packet.name;
        const auto actor_id =
          _actor_binding ? std::optional<std::string> (_actor_binding->actor_id) : std::nullopt;
        request_sending_context_t sending{request_name, actor_id, _packet.metadata};
        detail::run_request_sending (_state, sending);
        auto reply = detail::submit_request (_state, std::move (_packet), _timeout, _actor_binding);
        auto result = erased_result_t (_state, reply).template as<TReply> ();
        auto context = reply_context (request_name, actor_id, reply, result, started);
        detail::schedule_reply_received (_state, std::move (context));
        return result;
    }

    /// Sends the request and invokes the callback with the decoded reply result.
    template <typename TReply> void submit (std::function<void (result_t<TReply>)> callback)
    {
        if (!_state) {
            if (callback) {
                callback (result_t<TReply>::failure (error_code_t::configuration_error,
                                                     "request call has no connector"));
            }
            return;
        }
        auto state = _state;
        const auto started = std::chrono::steady_clock::now ();
        const auto request_name = _packet.name;
        const auto actor_id =
          _actor_binding ? std::optional<std::string> (_actor_binding->actor_id) : std::nullopt;
        request_sending_context_t sending{request_name, actor_id, _packet.metadata};
        detail::run_request_sending (state, sending);
        auto reply_hook_ids = std::make_shared<std::vector<std::uint64_t>> ();
        auto packet = std::move (_packet);
        const auto timeout = _timeout;
        detail::submit_request_async (
          state, std::move (packet), timeout,
          [state, request_name, actor_id, started, reply_hook_ids,
           callback = std::move (callback)] (result_t<detail::request_reply_t> reply) mutable {
              erased_result_t erased (state, reply);
              auto result = erased.template as<TReply> ();
              detail::run_reply_received (
                state, reply_context (request_name, actor_id, reply, result, started),
                *reply_hook_ids);
              if (callback) {
                  callback (std::move (result));
              }
          },
          false, _actor_binding, reply_hook_ids);
    }

  private:
    friend class connector_t;
    friend class actor_t;

    template <typename TReply>
    static reply_received_context_t reply_context (const std::string &request_name,
                                                   const std::optional<std::string> &actor_id,
                                                   const result_t<detail::request_reply_t> &reply,
                                                   const result_t<TReply> &result,
                                                   std::chrono::steady_clock::time_point started)
    {
        reply_received_context_t context;
        context.request_packet_name = request_name;
        context.actor_id = actor_id;
        context.succeeded = static_cast<bool> (result);
        context.elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (
          std::chrono::steady_clock::now () - started);
        if (result) {
            context.reply = reply.value ().packet;
        } else {
            context.error = result.error ();
        }
        return context;
    }

    class erased_result_t
    {
      public:
        erased_result_t (std::shared_ptr<void> state, result_t<detail::request_reply_t> result) :
            _state (std::move (state)), _result (std::move (result))
        {
        }

        template <typename T> result_t<T> as () const
        {
            if (!_result) {
                return result_t<T>::failure (
                  _result.error_code ().value_or (error_code_t::disconnected),
                  _result.error () ? _result.error ()->message : "request failed");
            }
            if constexpr (std::is_same_v<T, std::vector<std::uint8_t>>) {
                return result_t<T>::success (_result.value ().packet.payload);
            } else {
                return detail::decode_typed_message<T> (
                  _result.value ().packet.codec,
                  detail::decode_typed_reply (_state, _result.value ().packet.codec,
                                              _result.value ().packet.payload));
            }
        }

      private:
        std::shared_ptr<void> _state;
        result_t<detail::request_reply_t> _result;
    };

    request_call_t (std::shared_ptr<void> state,
                    packet_t packet,
                    std::chrono::milliseconds default_timeout) :
        _state (std::move (state)), _packet (std::move (packet)), _timeout (default_timeout)
    {
    }

    std::shared_ptr<void> _state;
    packet_t _packet;
    std::chrono::milliseconds _timeout{0};
    std::optional<detail::actor_binding_ref_t> _actor_binding;
};

template <typename TMessage> class wait_call_t
{
  public:
    /// Creates an unbound wait call that fails with configuration_error when submitted.
    wait_call_t () = default;

    /// Overrides the packet name used to match received packets.
    wait_call_t &packet_name (std::string name)
    {
        _packet_name = std::move (name);
        return *this;
    }

    /// Sets the wait timeout used by submit.
    wait_call_t &timeout (std::chrono::milliseconds timeout)
    {
        _timeout = timeout;
        return *this;
    }

    /// Restricts the wait to a received message that satisfies the predicate.
    ///
    /// The predicate reads the message, not the payload alone
    /// (stream-connector §10.1).
    wait_call_t &where (std::function<bool (const message_t<TMessage> &)> predicate)
    {
        _predicate = std::move (predicate);
        return *this;
    }

    template <typename TValue, typename TExpected>
    wait_call_t &where (TValue TMessage::*member, TExpected &&expected)
    {
        auto expected_value = std::decay_t<TExpected> (std::forward<TExpected> (expected));
        return where ([member, expected_value =
                                 std::move (expected_value)] (const message_t<TMessage> &message) {
            return std::invoke (member, message.payload) == expected_value;
        });
    }

    /// Waits for a matching packet, consumes it, and decodes it as a message.
    result_t<message_t<TMessage>> submit ()
    {
        using result_type = result_t<message_t<TMessage>>;
        if (!_state) {
            return result_type::failure (error_code_t::configuration_error,
                                         "wait call has no connector");
        }

        auto packet = detail::submit_wait (_state, _packet_name, packet_predicate (), _timeout);
        if (!packet) {
            return result_type::failure (observation_failure_code (packet.error_code ()),
                                         packet.error () ? packet.error ()->message
                                                         : "stream connector wait failed");
        }
        return detail::decode_message<TMessage> (_state, std::move (packet.value ()));
    }

    /// Waits for a matching packet and invokes the callback with the message.
    void submit (std::function<void (result_t<message_t<TMessage>>)> callback)
    {
        using result_type = result_t<message_t<TMessage>>;
        if (!_state) {
            if (callback) {
                callback (result_type::failure (error_code_t::configuration_error,
                                                "wait call has no connector"));
            }
            return;
        }

        auto state = _state;
        auto packet_name = std::move (_packet_name);
        const auto timeout = _timeout;
        detail::submit_wait_async (
          state, std::move (packet_name), packet_predicate (), timeout,
          [state, callback = std::move (callback)] (result_t<packet_t> packet) mutable {
              result_type result =
                result_type::failure (error_code_t::disconnected, "stream connector wait failed");
              if (!packet) {
                  result = result_type::failure (observation_failure_code (packet.error_code ()),
                                                 packet.error () ? packet.error ()->message
                                                                 : "stream connector wait failed");
              } else {
                  result = detail::decode_message<TMessage> (state, std::move (packet.value ()));
              }
              if (callback) {
                  callback (std::move (result));
              }
          });
    }

#if ZLINK_HAS_EXCEPTIONS
    /* Convenience for callers that already work in futures. It is compiled out
     * where exceptions are disabled, so the core keeps its no-exception
     * boundary for engine builds (cpp stream-connector §5). */
    std::future<message_t<TMessage>>
    to_future (std::string failure_message = "stream connector wait failed")
    {
        auto promise = std::make_shared<std::promise<message_t<TMessage>>> ();
        auto future = promise->get_future ();
        submit ([promise, failure_message = std::move (failure_message)] (
                  result_t<message_t<TMessage>> result) mutable {
            try {
                if (!result) {
                    promise->set_exception (
                      std::make_exception_ptr (std::runtime_error (failure_message)));
                    return;
                }
                promise->set_value (std::move (result.value ()));
            }
            catch (...) {
                promise->set_exception (std::current_exception ());
            }
        });
        return future;
    }
#endif

  private:
    friend class connector_t;

    /* stream-connector §10.1: an observation the surface itself rejects — here,
     * nothing arriving inside the window — is validation_failed. A connection
     * that ended keeps its own code, because the observation did not go wrong;
     * the place to observe went away. */
    static error_code_t observation_failure_code (std::optional<error_code_t> code)
    {
        if (!code || *code == error_code_t::request_timeout) {
            return error_code_t::validation_failed;
        }
        return *code;
    }

    std::function<bool (const packet_t &)> packet_predicate () const
    {
        if (!_predicate) {
            return {};
        }
        /* The connector parks this predicate in its own pending-wait list, so
         * it holds a weak reference back instead of keeping that state alive
         * through the list. */
        return [weak_state = std::weak_ptr<void> (_state),
                predicate = _predicate] (const packet_t &packet) {
            auto message = detail::decode_message<TMessage> (weak_state.lock (), packet);
            if (!message) {
                return false;
            }
            return predicate (message.value ());
        };
    }

    wait_call_t (std::shared_ptr<void> state,
                 std::string packet_name,
                 std::chrono::milliseconds default_timeout) :
        _state (std::move (state)),
        _packet_name (std::move (packet_name)),
        _timeout (default_timeout)
    {
    }

    std::shared_ptr<void> _state;
    std::string _packet_name;
    std::chrono::milliseconds _timeout{0};
    std::function<bool (const message_t<TMessage> &)> _predicate;
};

template <typename TMessage> class expect_none_call_t
{
  public:
    expect_none_call_t () = default;

    /// Sets the observation window in which the packet must not arrive.
    expect_none_call_t &within (std::chrono::milliseconds window)
    {
        _window = window;
        _has_window = true;
        return *this;
    }

    /// Observes the receive queue for the configured window.
    result_t<void> submit ()
    {
        if (auto invalid = validation_error ()) {
            return result_t<void>::failure (invalid->code, std::move (invalid->message));
        }

        auto packet = detail::submit_wait (_state, _packet_name, {}, _window);
        return evaluate (std::move (packet));
    }

    /// Observes the receive queue and invokes the callback with the assertion result.
    void submit (std::function<void (result_t<void>)> callback)
    {
        if (auto invalid = validation_error ()) {
            if (callback) {
                callback (result_t<void>::failure (invalid->code, std::move (invalid->message)));
            }
            return;
        }
        detail::submit_wait_async (
          _state, std::move (_packet_name), {}, _window,
          [callback = std::move (callback)] (result_t<packet_t> packet) mutable {
              if (callback) {
                  callback (evaluate (std::move (packet)));
              }
          });
    }

  private:
    friend class connector_t;

    expect_none_call_t (std::shared_ptr<void> state, std::string packet_name) :
        _state (std::move (state)), _packet_name (std::move (packet_name))
    {
    }

    static result_t<void> evaluate (result_t<packet_t> packet)
    {
        if (!packet && packet.error_code () == error_code_t::request_timeout) {
            return result_t<void>::success ();
        }
        if (!packet) {
            return result_t<void>::failure (
              packet.error_code ().value_or (error_code_t::disconnected),
              packet.error () ? packet.error ()->message : "expect-none observation failed");
        }
        return result_t<void>::failure (
          error_code_t::validation_failed,
          "an unexpected packet arrived during the observation window");
    }

    std::optional<error_t> validation_error () const
    {
        if (!_state) {
            return error_t{error_code_t::configuration_error, "expect-none call has no connector"};
        }
        if (!_has_window || _window <= std::chrono::milliseconds::zero ()) {
            return error_t{error_code_t::validation_failed,
                           "expect-none requires a positive observation window"};
        }
        return std::nullopt;
    }

    std::shared_ptr<void> _state;
    std::string _packet_name;
    std::chrono::milliseconds _window{0};
    bool _has_window = false;
};

template <typename TMessage> class wait_for_sequence_call_t
{
  public:
    wait_for_sequence_call_t () = default;

    /// Appends the next message predicate in required arrival order.
    ///
    /// The predicate reads the message, not the payload alone
    /// (stream-connector §10.1).
    wait_for_sequence_call_t &expect (std::function<bool (const message_t<TMessage> &)> predicate)
    {
        _predicates.push_back (std::move (predicate));
        return *this;
    }

    /// Sets one overall deadline for the complete sequence.
    wait_for_sequence_call_t &timeout (std::chrono::milliseconds timeout)
    {
        _timeout = timeout;
        return *this;
    }

    /// Waits for same-name packets and verifies each message in arrival order.
    result_t<std::vector<message_t<TMessage>>> submit ()
    {
        if (auto invalid = validation_error (_state, _predicates, _timeout)) {
            return failure (invalid->code, std::move (invalid->message));
        }

        std::vector<message_t<TMessage>> messages;
        messages.reserve (_predicates.size ());
        const auto deadline = std::chrono::steady_clock::now () + _timeout;
        for (const auto &predicate : _predicates) {
            const auto now = std::chrono::steady_clock::now ();
            if (now >= deadline) {
                return failure (error_code_t::validation_failed,
                                "stream connector sequence wait timed out");
            }
            auto packet = detail::submit_wait (
              _state, _packet_name, {},
              std::chrono::duration_cast<std::chrono::milliseconds> (deadline - now));
            if (!packet) {
                return failure (observation_failure_code (packet.error_code ()),
                                packet.error () ? packet.error ()->message
                                                : "stream connector sequence wait failed");
            }
            auto accepted =
              accept_packet (_state, std::move (packet.value ()), predicate, messages);
            if (!accepted) {
                return failure (accepted.error_code ().value_or (error_code_t::validation_failed),
                                accepted.error ()->message);
            }
        }
        return result_t<std::vector<message_t<TMessage>>>::success (std::move (messages));
    }

    /// Waits for the sequence and invokes the callback with the ordered messages.
    void submit (std::function<void (result_t<std::vector<message_t<TMessage>>>)> callback)
    {
        auto operation = std::make_shared<async_operation_t> (_state, std::move (_packet_name),
                                                              std::move (_predicates), _timeout,
                                                              std::move (callback));
        operation->start ();
    }

  private:
    friend class connector_t;

    using sequence_result_t = result_t<std::vector<message_t<TMessage>>>;

    struct async_operation_t : std::enable_shared_from_this<async_operation_t>
    {
        async_operation_t (
          std::shared_ptr<void> state_value,
          std::string packet_name_value,
          std::vector<std::function<bool (const message_t<TMessage> &)>> predicates_value,
          std::chrono::milliseconds timeout_value,
          std::function<void (sequence_result_t)> callback_value) :
            state (std::move (state_value)),
            packet_name (std::move (packet_name_value)),
            predicates (std::move (predicates_value)),
            timeout (timeout_value),
            callback (std::move (callback_value))
        {
        }

        void start ()
        {
            if (auto invalid = validation_error (state, predicates, timeout)) {
                complete (failure (invalid->code, std::move (invalid->message)));
                return;
            }
            messages.reserve (predicates.size ());
            deadline = std::chrono::steady_clock::now () + timeout;
            advance ();
        }

        void advance ()
        {
            const auto now = std::chrono::steady_clock::now ();
            if (now >= deadline) {
                complete (failure (error_code_t::validation_failed,
                                   "stream connector sequence wait timed out"));
                return;
            }
            auto self = this->shared_from_this ();
            detail::submit_wait_async (
              state, packet_name, {},
              std::chrono::duration_cast<std::chrono::milliseconds> (deadline - now),
              [self = std::move (self)] (result_t<packet_t> packet) mutable {
                  self->received (std::move (packet));
              });
        }

        void received (result_t<packet_t> packet)
        {
            if (!packet) {
                complete (failure (observation_failure_code (packet.error_code ()),
                                   packet.error () ? packet.error ()->message
                                                   : "stream connector sequence wait failed"));
                return;
            }
            const auto &predicate = predicates[index];
            auto accepted = accept_packet (state, std::move (packet.value ()), predicate, messages);
            if (!accepted) {
                complete (
                  failure (accepted.error_code ().value_or (error_code_t::validation_failed),
                           accepted.error ()->message));
                return;
            }
            ++index;
            if (index == predicates.size ()) {
                complete (sequence_result_t::success (std::move (messages)));
                return;
            }
            advance ();
        }

        void complete (sequence_result_t result)
        {
            auto completed = std::move (callback);
            if (completed) {
                completed (std::move (result));
            }
        }

        std::shared_ptr<void> state;
        std::string packet_name;
        std::vector<std::function<bool (const message_t<TMessage> &)>> predicates;
        std::chrono::milliseconds timeout;
        std::function<void (sequence_result_t)> callback;
        std::chrono::steady_clock::time_point deadline{};
        std::vector<message_t<TMessage>> messages;
        std::size_t index = 0;
    };

    wait_for_sequence_call_t (std::shared_ptr<void> state,
                              std::string packet_name,
                              std::chrono::milliseconds default_timeout) :
        _state (std::move (state)),
        _packet_name (std::move (packet_name)),
        _timeout (default_timeout)
    {
    }

    /* stream-connector §10.1: an observation that the surface itself rejects is
     * validation_failed. A transport failure that ended the wait keeps its own
     * code, because it is not the observation that failed. */
    static error_code_t observation_failure_code (std::optional<error_code_t> code)
    {
        if (!code || *code == error_code_t::request_timeout) {
            return error_code_t::validation_failed;
        }
        return *code;
    }

    static result_t<void>
    accept_packet (const std::shared_ptr<void> &state,
                   packet_t packet,
                   const std::function<bool (const message_t<TMessage> &)> &predicate,
                   std::vector<message_t<TMessage>> &messages)
    {
        auto decoded = detail::decode_message<TMessage> (state, std::move (packet));
        if (!decoded) {
            return result_t<void>::failure (
              decoded.error_code ().value_or (error_code_t::frame_decode_failed),
              decoded.error ()->message);
        }
        if (!predicate || !predicate (decoded.value ())) {
            return result_t<void>::failure (error_code_t::validation_failed,
                                            "packet payload arrived out of the expected sequence");
        }
        messages.push_back (std::move (decoded.value ()));
        return result_t<void>::success ();
    }

    static std::optional<error_t> validation_error (
      const std::shared_ptr<void> &state,
      const std::vector<std::function<bool (const message_t<TMessage> &)>> &predicates,
      std::chrono::milliseconds timeout)
    {
        if (!state) {
            return error_t{error_code_t::configuration_error,
                           "sequence wait call has no connector"};
        }
        if (predicates.empty ()) {
            return error_t{error_code_t::validation_failed,
                           "sequence wait requires at least one expectation"};
        }
        if (timeout <= std::chrono::milliseconds::zero ()) {
            return error_t{error_code_t::validation_failed,
                           "sequence wait requires a positive timeout"};
        }
        return std::nullopt;
    }

    static sequence_result_t failure (error_code_t code, std::string message)
    {
        return sequence_result_t::failure (code, std::move (message));
    }

    std::shared_ptr<void> _state;
    std::string _packet_name;
    std::vector<std::function<bool (const message_t<TMessage> &)>> _predicates;
    std::chrono::milliseconds _timeout{0};
};

} // namespace zlink::stream_connector
