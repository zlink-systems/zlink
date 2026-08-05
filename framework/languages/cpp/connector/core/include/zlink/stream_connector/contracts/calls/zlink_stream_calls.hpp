/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/stream_connector/contracts/result.hpp>
#include <zlink/stream_connector/contracts/stream_payload.hpp>
#include <zlink/stream_connector/contracts/zlink_stream_connector_options.hpp>
#include <zlink/stream_connector/contracts/zlink_stream_interfaces.hpp>
#include <zlink/stream_connector/contracts/zlink_stream_models.hpp>

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace zlink::stream_connector
{

namespace detail
{
struct request_reply_t
{
    codec_t codec = codec_t::raw;
    zlink::message_t payload;
};

result_t<request_reply_t>
submit_request (std::shared_ptr<void> state, packet_t packet, std::chrono::milliseconds timeout);
void submit_request_async (std::shared_ptr<void> state,
                           packet_t packet,
                           std::chrono::milliseconds timeout,
                           std::function<void (result_t<request_reply_t>)> callback,
                           bool deliver_direct = false);
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

    /// Sets the outbound payload codec.
    send_call_t &codec (codec_t codec);

    /// Marks the outbound packet for compression when compression is available.
    send_call_t &compress ();

    /// Gives the packet to the connector for delivery.
    void submit ();

  private:
    friend class connector_t;
    send_call_t (std::shared_ptr<void> state, packet_t packet);

    std::shared_ptr<void> _state;
    packet_t _packet;
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

    /// Sets the outbound request payload codec.
    request_call_t &codec (codec_t codec)
    {
        _packet.codec = codec;
        return *this;
    }

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
        return submit_erased ().template as<TReply> ();
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
        auto packet = std::move (_packet);
        const auto timeout = _timeout;
        detail::submit_request_async (
          state, std::move (packet), timeout,
          [callback = std::move (callback)] (result_t<detail::request_reply_t> reply) mutable {
              erased_result_t erased (std::move (reply));
              auto result = erased.template as<TReply> ();
              if (callback) {
                  callback (std::move (result));
              }
          });
    }

  private:
    friend class connector_t;

    class erased_result_t
    {
      public:
        explicit erased_result_t (result_t<detail::request_reply_t> result) :
            _result (std::move (result))
        {
        }

        template <typename T> result_t<T> as () const
        {
            if (!_result) {
                return result_t<T>::failure (_result.error_code (), _result.error ()
                                                                      ? _result.error ()->message
                                                                      : "request failed");
            }
            if constexpr (std::is_same_v<T, zlink::message_t>) {
                return result_t<T>::success (_result.value ().payload);
            } else {
                T value{};
                try {
                    detail::apply_packet_payload (value, _result.value ().codec,
                                                  _result.value ().payload, 0);
                } catch (const std::exception &ex) {
                    return result_t<T>::failure (error_code_t::frame_decode_failed, ex.what ());
                }
                return result_t<T>::success (std::move (value));
            }
        }

      private:
        result_t<detail::request_reply_t> _result;
    };

    request_call_t (std::shared_ptr<void> state,
                    packet_t packet,
                    std::chrono::milliseconds default_timeout) :
        _state (std::move (state)), _packet (std::move (packet)), _timeout (default_timeout)
    {
    }

    erased_result_t submit_erased ()
    {
        return erased_result_t (detail::submit_request (_state, std::move (_packet), _timeout));
    }

    std::shared_ptr<void> _state;
    packet_t _packet;
    std::chrono::milliseconds _timeout{0};
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

    /// Restricts the wait to a decoded message that satisfies the predicate.
    wait_call_t &where (std::function<bool (const TMessage &)> predicate)
    {
        _predicate = std::move (predicate);
        return *this;
    }

    template <typename TValue, typename TExpected>
    wait_call_t &where (TValue TMessage::*member, TExpected &&expected)
    {
        auto expected_value = std::decay_t<TExpected> (std::forward<TExpected> (expected));
        return where (
          [member, expected_value = std::move (expected_value)] (const TMessage &message) {
              return std::invoke (member, message) == expected_value;
          });
    }

    /// Waits for a matching packet, consumes it, and decodes it as TMessage.
    result_t<TMessage> submit ()
    {
        if (!_state) {
            return result_t<TMessage>::failure (error_code_t::configuration_error,
                                                "wait call has no connector");
        }

        std::function<bool (const packet_t &)> packet_predicate;
        if (_predicate) {
            packet_predicate = [predicate = _predicate] (const packet_t &packet) {
                if constexpr (std::is_same_v<TMessage, packet_t>) {
                    return predicate (packet);
                } else {
                    TMessage message{};
                    detail::apply_packet_payload (message, packet.codec, packet.payload, 0);
                    return predicate (message);
                }
            };
        }

        auto packet =
          detail::submit_wait (_state, _packet_name, std::move (packet_predicate), _timeout);
        if (!packet) {
            return result_t<TMessage>::failure (packet.error_code (),
                                                packet.error () ? packet.error ()->message
                                                                : "stream connector wait failed");
        }
        if constexpr (std::is_same_v<TMessage, packet_t>) {
            return result_t<TMessage>::success (std::move (packet.value ()));
        } else {
            try {
                TMessage message{};
                detail::apply_packet_payload (
                  message, packet.value ().codec, packet.value ().payload, 0);
                return result_t<TMessage>::success (std::move (message));
            }
            catch (const std::exception &error) {
                return result_t<TMessage>::failure (
                  error_code_t::frame_decode_failed, error.what ());
            }
            catch (...) {
                return result_t<TMessage>::failure (
                  error_code_t::frame_decode_failed,
                  "stream connector wait payload decode failed");
            }
        }
    }

    /// Waits for a matching packet and invokes the callback with the decoded result.
    void submit (std::function<void (result_t<TMessage>)> callback)
    {
        if (!_state) {
            if (callback) {
                callback (result_t<TMessage>::failure (error_code_t::configuration_error,
                                                       "wait call has no connector"));
            }
            return;
        }

        std::function<bool (const packet_t &)> packet_predicate;
        if (_predicate) {
            packet_predicate = [predicate = _predicate] (const packet_t &packet) {
                if constexpr (std::is_same_v<TMessage, packet_t>) {
                    return predicate (packet);
                } else {
                    TMessage message{};
                    detail::apply_packet_payload (message, packet.codec, packet.payload, 0);
                    return predicate (message);
                }
            };
        }

        auto state = _state;
        auto packet_name = std::move (_packet_name);
        const auto timeout = _timeout;
        detail::submit_wait_async (
          state, std::move (packet_name), std::move (packet_predicate), timeout,
          [callback = std::move (callback)] (result_t<packet_t> packet) mutable {
              result_t<TMessage> result = result_t<TMessage>::failure (
                error_code_t::configuration_error, "stream connector wait failed");
              if (!packet) {
                  result = result_t<TMessage>::failure (
                    packet.error_code (),
                    packet.error () ? packet.error ()->message : "stream connector wait failed");
              } else if constexpr (std::is_same_v<TMessage, packet_t>) {
                  result = result_t<TMessage>::success (std::move (packet.value ()));
              } else {
                  try {
                      TMessage message{};
                      detail::apply_packet_payload (
                        message, packet.value ().codec, packet.value ().payload, 0);
                      result = result_t<TMessage>::success (std::move (message));
                  }
                  catch (const std::exception &error) {
                      result = result_t<TMessage>::failure (
                        error_code_t::frame_decode_failed, error.what ());
                  }
                  catch (...) {
                      result = result_t<TMessage>::failure (
                        error_code_t::frame_decode_failed,
                        "stream connector wait payload decode failed");
                  }
              }
              if (callback) {
                  callback (std::move (result));
              }
          });
    }

    std::future<TMessage> to_future (std::string failure_message = "stream connector wait failed")
    {
        auto promise = std::make_shared<std::promise<TMessage>> ();
        auto future = promise->get_future ();
        submit ([promise, failure_message = std::move (failure_message)] (
                  result_t<TMessage> result) mutable {
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

  private:
    friend class connector_t;

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
    std::function<bool (const TMessage &)> _predicate;
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
              packet.error_code (),
              packet.error () ? packet.error ()->message : "expect-none observation failed");
        }
        return result_t<void>::failure (error_code_t::validation_failed,
                                        "an unexpected packet arrived during the observation window");
    }

    std::optional<error_t> validation_error () const
    {
        if (!_state) {
            return error_t{error_code_t::configuration_error,
                           "expect-none call has no connector"};
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

    /// Appends the next payload predicate in required arrival order.
    wait_for_sequence_call_t &expect (std::function<bool (const TMessage &)> predicate)
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

    /// Waits for same-name packets and verifies each payload in arrival order.
    result_t<std::vector<TMessage>> submit ()
    {
        if (auto invalid = validation_error (_state, _predicates, _timeout)) {
            return failure (invalid->code, std::move (invalid->message));
        }

        std::vector<TMessage> messages;
        messages.reserve (_predicates.size ());
        const auto deadline = std::chrono::steady_clock::now () + _timeout;
        for (const auto &predicate : _predicates) {
            const auto now = std::chrono::steady_clock::now ();
            if (now >= deadline) {
                return failure (error_code_t::request_timeout,
                                "stream connector sequence wait timed out");
            }
            auto packet = detail::submit_wait (
              _state, _packet_name, {},
              std::chrono::duration_cast<std::chrono::milliseconds> (deadline - now));
            if (!packet) {
                return failure (packet.error_code (),
                                packet.error () ? packet.error ()->message
                                                : "stream connector sequence wait failed");
            }
            auto accepted = accept_packet (std::move (packet.value ()), predicate, messages);
            if (!accepted) {
                return failure (accepted.error_code (), accepted.error ()->message);
            }
        }
        return result_t<std::vector<TMessage>>::success (std::move (messages));
    }

    /// Waits for the sequence and invokes the callback with the ordered payloads.
    void submit (std::function<void (result_t<std::vector<TMessage>>)> callback)
    {
        auto operation = std::make_shared<async_operation_t> (
          _state, std::move (_packet_name), std::move (_predicates), _timeout,
          std::move (callback));
        operation->start ();
    }

  private:
    friend class connector_t;

    using sequence_result_t = result_t<std::vector<TMessage>>;

    struct async_operation_t : std::enable_shared_from_this<async_operation_t>
    {
        async_operation_t (std::shared_ptr<void> state_value,
                           std::string packet_name_value,
                           std::vector<std::function<bool (const TMessage &)>> predicates_value,
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
                complete (failure (error_code_t::request_timeout,
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
                complete (failure (packet.error_code (),
                                   packet.error () ? packet.error ()->message
                                                   : "stream connector sequence wait failed"));
                return;
            }
            const auto &predicate = predicates[index];
            auto accepted = accept_packet (std::move (packet.value ()), predicate, messages);
            if (!accepted) {
                complete (failure (accepted.error_code (), accepted.error ()->message));
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
        std::vector<std::function<bool (const TMessage &)>> predicates;
        std::chrono::milliseconds timeout;
        std::function<void (sequence_result_t)> callback;
        std::chrono::steady_clock::time_point deadline{};
        std::vector<TMessage> messages;
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

    static result_t<TMessage> decode (packet_t packet)
    {
        if constexpr (std::is_same_v<TMessage, packet_t>) {
            return result_t<TMessage>::success (std::move (packet));
        } else {
            try {
                TMessage message{};
                detail::apply_packet_payload (message, packet.codec, packet.payload, 0);
                return result_t<TMessage>::success (std::move (message));
            } catch (const std::exception &error) {
                return result_t<TMessage>::failure (error_code_t::frame_decode_failed,
                                                    error.what ());
            }
        }
    }

    static result_t<void> accept_packet (
      packet_t packet,
      const std::function<bool (const TMessage &)> &predicate,
      std::vector<TMessage> &messages)
    {
        auto decoded = decode (std::move (packet));
        if (!decoded) {
            return result_t<void>::failure (decoded.error_code (), decoded.error ()->message);
        }
        if (!predicate || !predicate (decoded.value ())) {
            return result_t<void>::failure (
              error_code_t::validation_failed,
              "packet payload arrived out of the expected sequence");
        }
        messages.push_back (std::move (decoded.value ()));
        return result_t<void>::success ();
    }

    static std::optional<error_t> validation_error (
      const std::shared_ptr<void> &state,
      const std::vector<std::function<bool (const TMessage &)>> &predicates,
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
    std::vector<std::function<bool (const TMessage &)>> _predicates;
    std::chrono::milliseconds _timeout{0};
};

} // namespace zlink::stream_connector
