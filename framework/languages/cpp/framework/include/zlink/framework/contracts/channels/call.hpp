/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/detail/call_facade.hpp>
#include <zlink/framework/contracts/codecs/serializer.hpp>

#include <chrono>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace zlink
{
class message_t;
} // namespace zlink

namespace zlink::framework
{

class stream_compression_codec_t;

namespace detail
{
class stream_write_call_state_t;
class stream_header_t;

class submit_once_t
{
  public:
    bool try_claim () noexcept
    {
        return !_claimed.exchange (true, std::memory_order_acq_rel);
    }

  private:
    std::atomic_bool _claimed{false};
};

/* Runs a transport call outside the caller's serial turn and owns the work
 * through the Framework offload executor. The implementation is provided by
 * the runtime so this public call surface does not create an unmanaged thread. */
bool submit_blocking_call (std::function<void ()> work);
} // namespace detail

template <typename TReply> class request_call_t
{
  public:
    using metadata_map_t = std::map<std::string, std::string>;
    using submit_fn_t = std::function<task_t<TReply> (
      const std::string &, std::chrono::milliseconds, const metadata_map_t &)>;
    using preflight_fn_t = std::function<result_t<void> (bool)>;

    explicit request_call_t (result_t<TReply> result) : _immediate (std::move (result)) {}

    request_call_t (std::string packet_name,
                    submit_fn_t submit,
                    preflight_fn_t preflight = {}) :
        _packet_name (std::move (packet_name)), _submit (std::move (submit)),
        _preflight (std::move (preflight))
    {
    }

    request_call_t &timeout (std::chrono::milliseconds timeout)
    {
        _timeout = timeout;
        return *this;
    }

    request_call_t &metadata (std::string key, std::string value)
    {
        _metadata[std::move (key)] = std::move (value);
        return *this;
    }

    task_t<TReply> submit () { return start (false); }

    task_t<TReply> yield () { return start (true); }

  private:
    task_t<TReply> start (bool release_turn)
    {
        if (release_turn && !detail::current_serial_turn_allows_yield ()) {
            return detail::unsupported_yield_task<TReply> ();
        }
        if (_preflight) {
            const auto admitted = _preflight (release_turn);
            if (!admitted) {
                return task_t<TReply> (
                  admitted.error ()
                    ? detail::result_access_t::failure<TReply> (*admitted.error ())
                    : result_t<TReply>::failure (
                        framework_error_kind_t::internal_failure,
                        "request preflight failed"));
            }
        }
        if (_immediate) {
            return task_t<TReply> (*_immediate);
        }
        if (!_submit) {
            return task_t<TReply> (
              result_t<TReply>::failure (framework_error_kind_t::protocol_error,
                                         "request call is not bound to a channel client"));
        }
        auto turn_plan = detail::prepare_serial_turn_await (release_turn);
        if (!turn_plan) {
            return _submit (_packet_name, _timeout, _metadata);
        }
        const auto ambient_context = detail::capture_ambient_context ();
        // The transport submit may block its calling thread. It therefore runs
        // outside the serial executor even when submit keeps the logical turn.
        auto source =
          std::make_shared<detail::task_completion_source_t<TReply>> (
            std::move (turn_plan->scheduler));
        auto pending = source->task ();
        if (!detail::submit_blocking_call (
              [source, submit = _submit, packet_name = _packet_name,
               timeout = _timeout, metadata = _metadata,
               ambient_context] () mutable {
            const auto ambient_guard = detail::enter_ambient_context (ambient_context);
            try {
                source->complete (submit (packet_name, timeout, metadata).result ());
            }
            catch (const framework_exception_t &error) {
                source->complete (detail::result_access_t::failure<TReply> (error));
            }
            catch (...) {
                source->complete (result_t<TReply>::failure (
                  framework_error_kind_t::internal_failure, "awaited request failed"));
            }
        })) {
            source->complete (result_t<TReply>::failure (
              framework_error_kind_t::internal_failure,
              "request offload executor rejected the call"));
        }
        return pending;
    }

    std::optional<result_t<TReply>> _immediate;
    std::string _packet_name;
    std::chrono::milliseconds _timeout{0};
    metadata_map_t _metadata;
    submit_fn_t _submit;
    preflight_fn_t _preflight;
};

class channel_request_call_t
{
  public:
    using metadata_map_t = std::map<std::string, std::string>;
    using submit_fn_t = std::function<task_t<zlink::message_t> (
      const std::string &, std::chrono::milliseconds, const metadata_map_t &)>;

    channel_request_call_t () = default;

    channel_request_call_t (std::string packet_name,
                            serializer_registry_t *serializers,
                            submit_fn_t submit) :
        _packet_name (std::move (packet_name)),
        _serializers (serializers),
        _submit (std::move (submit))
    {
    }

    channel_request_call_t &timeout (std::chrono::milliseconds timeout)
    {
        _timeout = timeout;
        return *this;
    }

    channel_request_call_t &metadata (std::string key, std::string value)
    {
        _metadata[std::move (key)] = std::move (value);
        return *this;
    }

    template <typename TReply> task_t<TReply> submit () { return start<TReply> (false); }

    template <typename TReply> task_t<TReply> yield () { return start<TReply> (true); }

  protected:
    template <typename TReply> task_t<TReply> start (bool release_turn)
    {
        /* `submit()` and `yield()` are commonly called on a temporary returned
         * by MessageBus::request(). Copy every value needed by the coroutine
         * before the call object can be destroyed. The suspended operation must
         * own its request state instead of retaining this object's address. */
        return start_owned<TReply> (
          release_turn, _packet_name, _serializers, _submit, _timeout, _metadata);
    }

    template <typename TReply>
    static task_t<TReply> start_owned (bool release_turn,
                                      std::string packet_name,
                                      serializer_registry_t *serializers,
                                      submit_fn_t submit,
                                      std::chrono::milliseconds timeout,
                                      metadata_map_t metadata)
    {
        if (release_turn && !detail::current_serial_turn_allows_yield ()) {
            co_return co_await detail::unsupported_yield_task<TReply> ();
        }
        if (!submit) {
            co_return result_t<TReply>::failure (framework_error_kind_t::protocol_error,
                                                 "request call is not bound to a channel client");
        }
        zlink::message_t reply;
        auto turn_plan = detail::prepare_serial_turn_await (release_turn);
        if (!turn_plan) {
            reply = co_await submit (packet_name, timeout, metadata);
        } else {
            const auto ambient_context = detail::capture_ambient_context ();
            auto source = std::make_shared<detail::task_completion_source_t<zlink::message_t>> (
              std::move (turn_plan->scheduler));
            auto pending = source->task ();
            auto blocking_submit = [submit = std::move (submit),
                                    packet_name = std::move (packet_name),
                                    timeout, metadata = std::move (metadata)] () mutable {
                return submit (packet_name, timeout, metadata).result ();
            };
            if (!detail::submit_blocking_call ([source,
                                                submit = std::move (blocking_submit),
                                                ambient_context] () mutable {
                const auto ambient_guard = detail::enter_ambient_context (ambient_context);
                try {
                    source->complete (submit ());
                }
                catch (const framework_exception_t &error) {
                    source->complete (detail::result_access_t::failure<zlink::message_t> (error));
                }
                catch (...) {
                    source->complete (result_t<zlink::message_t>::failure (
                      framework_error_kind_t::internal_failure, "awaited channel request failed"));
                }
            })) {
                source->complete (result_t<zlink::message_t>::failure (
                  framework_error_kind_t::internal_failure,
                  "request offload executor rejected the call"));
            }
            reply = co_await pending;
        }
        co_return decode<TReply> (serializers, reply);
    }

    template <typename TReply>
    static result_t<TReply> decode (serializer_registry_t *serializers,
                                    const zlink::message_t &reply)
    {
        if (serializers == nullptr) {
            return result_t<TReply>::failure (framework_error_kind_t::protocol_error,
                                              "channel request has no serializer registry");
        }
        try {
            return result_t<TReply>::success (serializers->get<TReply> ().deserialize (
              detail::encoded_payload_from_raw (reply)));
        }
        catch (const framework_exception_t &error) {
            return detail::result_access_t::failure<TReply> (error);
        }
    }
    task_t<zlink::message_t> submit_raw ()
    {
        if (!_submit) {
            return task_t<zlink::message_t> (result_t<zlink::message_t>::failure (
              framework_error_kind_t::protocol_error,
              "request call is not bound to a channel client"));
        }
        return _submit (_packet_name, _timeout, _metadata);
    }

    // Copyable blocking submit closure that can run on a thread other than the
    // caller's Spot serial turn. Captures copies of the request parameters so it
    // stays valid even after the originating call object is gone.
    std::function<result_t<zlink::message_t> ()> blocking_submit () const
    {
        if (!_submit) {
            return [] {
                return result_t<zlink::message_t>::failure (
                  framework_error_kind_t::protocol_error,
                  "request call is not bound to a channel client");
            };
        }
        return [submit = _submit, packet_name = _packet_name, timeout = _timeout,
                metadata = _metadata] () { return submit (packet_name, timeout, metadata).result (); };
    }

    const std::string &packet_name_value () const noexcept { return _packet_name; }
    std::chrono::milliseconds timeout_value () const noexcept { return _timeout; }
    const metadata_map_t &metadata_values () const noexcept { return _metadata; }
    serializer_registry_t *serializers () const noexcept { return _serializers; }

  private:
    std::string _packet_name;
    std::chrono::milliseconds _timeout{0};
    metadata_map_t _metadata;
    serializer_registry_t *_serializers = nullptr;
    submit_fn_t _submit;
};


namespace detail
{
task_t<void>
submit_one_way_task (std::function<result_t<void> ()> submit);

task_t<void>
submit_logical_multicast_task (std::function<result_t<void> ()> submit,
                               std::chrono::milliseconds timeout);
} // namespace detail

class send_call_t
{
  public:
    using metadata_map_t = std::map<std::string, std::string>;
    using submit_fn_t =
      std::function<result_t<void> (const std::string &, const metadata_map_t &)>;

    explicit send_call_t (result_t<void> result) : _immediate (std::move (result)) {}

    send_call_t (std::string packet_name, submit_fn_t submit) :
        _packet_name (std::move (packet_name)), _submit (std::move (submit))
    {
    }

    send_call_t &metadata (std::string key, std::string value)
    {
        _metadata[std::move (key)] = std::move (value);
        return *this;
    }

    task_t<void> submit ()
    {
        if (!_submission->try_claim ()) {
            return task_t<void> (result_t<void>::failure (
              framework_error_kind_t::invalid_operation,
              "one-way call has already been submitted"));
        }
        if (_immediate) {
            return detail::submit_one_way_task (
              [immediate = *_immediate] { return immediate; });
        }
        if (!_submit) {
            return task_t<void> (result_t<void>::failure (
              framework_error_kind_t::protocol_error,
              "send call is not bound to a channel client"));
        }
        return detail::submit_one_way_task (
          [packet_name = _packet_name, metadata = _metadata,
           submit = _submit] () { return submit (packet_name, metadata); });
    }

  private:
    result_t<void> submit_now ()
    {
        if (_immediate) {
            return *_immediate;
        }
        if (!_submit) {
            return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                            "send call is not bound to a channel client");
        }
        return _submit (_packet_name, _metadata);
    }

    std::optional<result_t<void>> _immediate;
    std::string _packet_name;
    metadata_map_t _metadata;
    submit_fn_t _submit;
    std::shared_ptr<detail::submit_once_t> _submission =
      std::make_shared<detail::submit_once_t> ();
};

class publish_call_t
{
  public:
    using metadata_map_t = std::map<std::string, std::string>;
    using submit_fn_t = std::function<result_t<void> (const metadata_map_t &)>;

    explicit publish_call_t (result_t<void> result) :
        _immediate (std::move (result))
    {
    }

    explicit publish_call_t (
      submit_fn_t submit,
      std::chrono::milliseconds timeout = std::chrono::seconds (1)) :
        _submit (std::move (submit)), _timeout (timeout)
    {
    }

    publish_call_t &metadata (std::string key, std::string value)
    {
        _metadata[std::move (key)] = std::move (value);
        return *this;
    }

    task_t<void> submit ()
    {
        if (!_submission->try_claim ()) {
            return task_t<void> (result_t<void>::failure (
              framework_error_kind_t::invalid_operation,
              "logical multicast call has already been submitted"));
        }
        if (_immediate) {
            return task_t<void> (*_immediate);
        }
        return detail::submit_logical_multicast_task (
          [submit = _submit, metadata = _metadata] () mutable {
              if (!submit) {
                  throw framework_exception_t (
                    framework_error_kind_t::protocol_error,
                    "logical multicast call is not bound to a publisher");
              }
              return submit (metadata);
          },
          _timeout);
    }

  private:
    std::optional<result_t<void>> _immediate;
    metadata_map_t _metadata;
    submit_fn_t _submit;
    std::chrono::milliseconds _timeout{std::chrono::seconds (1)};
    std::shared_ptr<detail::submit_once_t> _submission =
      std::make_shared<detail::submit_once_t> ();
};

class bound_session_send_call_t
{
  public:
    explicit bound_session_send_call_t (send_call_t call) : _call (std::move (call)) {}

    bound_session_send_call_t &metadata (std::string key, std::string value)
    {
        _call.metadata (std::move (key), std::move (value));
        return *this;
    }

    task_t<void> submit () { return _call.submit (); }

  private:
    send_call_t _call;
};

class fanout_publish_call_t
{
  public:
    explicit fanout_publish_call_t (send_call_t call) : _call (std::move (call)) {}

    task_t<void> submit () { return _call.submit (); }

  private:
    send_call_t _call;
};

class stream_write_call_t
{
  public:
    using metadata_map_t = std::map<std::string, std::string>;

    explicit stream_write_call_t (result_t<void> result);
    ~stream_write_call_t ();

    stream_write_call_t (stream_write_call_t &&) noexcept;
    stream_write_call_t &operator= (stream_write_call_t &&) noexcept;
    stream_write_call_t (const stream_write_call_t &) = delete;
    stream_write_call_t &operator= (const stream_write_call_t &) = delete;

    stream_write_call_t &metadata (std::string key, std::string value);
    stream_write_call_t &compress ();
    task_t<void> submit ();

  private:
    using submit_fn_t =
      std::function<result_t<void> (const detail::stream_header_t &, const zlink::message_t &)>;

    friend class stream_t;
    friend class detail::stream_write_call_state_t;

    stream_write_call_t (detail::stream_header_t header,
                         zlink::message_t payload,
                         std::shared_ptr<const stream_compression_codec_t> compression_codec,
                         submit_fn_t submit);

    result_t<void> submit_now ();

    std::shared_ptr<detail::stream_write_call_state_t> _state;
};

class stream_send_call_t
{
  public:
    explicit stream_send_call_t (result_t<void> result);
    ~stream_send_call_t ();

    stream_send_call_t (stream_send_call_t &&) noexcept;
    stream_send_call_t &operator= (stream_send_call_t &&) noexcept;
    stream_send_call_t (const stream_send_call_t &) = delete;
    stream_send_call_t &operator= (const stream_send_call_t &) = delete;

    stream_send_call_t &metadata (std::string key, std::string value);
    stream_send_call_t &packet_name (std::string packet_name);
    stream_send_call_t &compress ();
    task_t<void> submit ();

  private:
    using submit_fn_t =
      std::function<result_t<void> (const detail::stream_header_t &, const zlink::message_t &)>;

    friend class stream_t;

    stream_send_call_t (detail::stream_header_t header,
                        zlink::message_t payload,
                        std::shared_ptr<const stream_compression_codec_t> compression_codec,
                        submit_fn_t submit);

    std::shared_ptr<detail::stream_write_call_state_t> _state;
};

} // namespace zlink::framework
