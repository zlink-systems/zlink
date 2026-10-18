/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <zlink/framework/contracts/detail/call_facade.hpp>
#include <zlink/framework/contracts/dispatch/request_task.hpp>
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

// Reject runtime execution contexts before a blocking terminal starts work.
void ensure_blocking_submit_allowed ();

class submit_once_t
{
  public:
    bool try_claim () noexcept { return !_claimed.exchange (true, std::memory_order_acq_rel); }

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
    using submit_fn_t = std::function<request_task_t<TReply> (
      const std::string &, std::chrono::milliseconds, const metadata_map_t &)>;
    using preflight_fn_t = std::function<result_t<void> (bool)>;

    explicit request_call_t (result_t<TReply> result) : _immediate (std::move (result)) {}

    request_call_t (std::string packet_name, submit_fn_t submit, preflight_fn_t preflight = {}) :
        _packet_name (std::move (packet_name)),
        _submit (std::move (submit)),
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

    request_task_t<TReply> async () { return start (false); }

    TReply submit ()
    {
        detail::ensure_blocking_submit_allowed ();
        return async ().result ().value ();
    }

    task_t<TReply> yield ()
    {
        return detail::yield_call<TReply> ([this] { return start (true); });
    }

  private:
    request_task_t<TReply> start (bool release_turn)
    {
        if (_preflight) {
            const auto admitted = _preflight (release_turn);
            if (!admitted) {
                return request_task_t<TReply> (
                  admitted.error ()
                    ? detail::result_access_t::failure<TReply> (*admitted.error ())
                    : result_t<TReply>::failure (framework_error_kind_t::internal_failure,
                                                 "request preflight failed"));
            }
        }
        if (_immediate) {
            return request_task_t<TReply> (*_immediate);
        }
        if (!_submit) {
            return request_task_t<TReply> (
              result_t<TReply>::failure (framework_error_kind_t::protocol_error,
                                         "request call is not bound to a channel client"));
        }
        // The handler awaits in its held turn. A Yield handler re-enters through
        // its queue instead, so only the submission it wraps resumes in that turn.
        auto held = detail::held_serial_turn_await_scheduler ();
        if (release_turn) {
            return submit_owned (_submit, _packet_name, _timeout, _metadata, std::move (held));
        }
        auto pending = submit_owned (_submit, _packet_name, _timeout, _metadata, {});
        if (!held) {
            return pending;
        }
        return detail::reschedule_task (std::move (pending), std::move (held));
    }

    static request_task_t<TReply> submit_owned (submit_fn_t submit,
                                                std::string packet_name,
                                                std::chrono::milliseconds timeout,
                                                metadata_map_t metadata,
                                                detail::task_scheduler_t submission_scheduler)
    {
        auto pending = submit (packet_name, timeout, metadata);
        if (submission_scheduler) {
            pending =
              detail::reschedule_task (std::move (pending), std::move (submission_scheduler));
        }
        co_return co_await detail::request_submission (std::move (pending));
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
    using submit_fn_t = std::function<request_task_t<zlink::message_t> (
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

    template <typename TReply> request_task_t<TReply> async () { return start<TReply> (); }

    template <typename TReply> TReply submit ()
    {
        detail::ensure_blocking_submit_allowed ();
        return async<TReply> ().result ().value ();
    }

    template <typename TReply> task_t<TReply> yield ()
    {
        return detail::yield_call<TReply> ([this] { return start<TReply> (); });
    }

  protected:
    template <typename TReply> request_task_t<TReply> start ()
    {
        /* `async()` and `yield()` are commonly called on a temporary returned
         * by MessageBus::request(). Copy every value needed by the coroutine
         * before the call object can be destroyed. The suspended operation must
         * own its request state instead of retaining this object's address. */
        return start_owned<TReply> (_packet_name, _serializers, _submit, _timeout, _metadata);
    }

    template <typename TReply>
    static request_task_t<TReply> start_owned (std::string packet_name,
                                               serializer_registry_t *serializers,
                                               submit_fn_t submit,
                                               std::chrono::milliseconds timeout,
                                               metadata_map_t metadata)
    {
        if (!submit) {
            co_return result_t<TReply>::failure (framework_error_kind_t::protocol_error,
                                                 "request call is not bound to a channel client");
        }
        zlink::message_t reply;
        auto held = detail::held_serial_turn_await_scheduler ();
        auto pending = submit (packet_name, timeout, metadata);
        if (held) {
            pending = detail::reschedule_task (std::move (pending), std::move (held));
        }
        reply = co_await detail::request_submission (std::move (pending));
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
            return result_t<TReply>::success (
              serializers->get<TReply> ().deserialize (detail::encoded_payload_from_raw (reply)));
        }
        catch (const framework_exception_t &error) {
            return detail::result_access_t::failure<TReply> (error);
        }
    }
    request_task_t<zlink::message_t> async_raw ()
    {
        if (!_submit) {
            return request_task_t<zlink::message_t> (result_t<zlink::message_t>::failure (
              framework_error_kind_t::protocol_error,
              "request call is not bound to a channel client"));
        }
        return _submit (_packet_name, _timeout, _metadata);
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
task_t<void> submit_one_way_task (std::function<result_t<void> ()> submit);

task_t<void> submit_logical_multicast_task (std::function<result_t<void> ()> submit,
                                            std::chrono::milliseconds timeout);

task_t<void> submit_logical_multicast_async_task (std::function<task_t<void> ()> submit,
                                                  std::chrono::milliseconds timeout);
} // namespace detail

class send_call_t
{
  public:
    using metadata_map_t = std::map<std::string, std::string>;
    using submit_fn_t = std::function<result_t<void> (const std::string &, const metadata_map_t &)>;

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

    void submit ()
    {
        detail::ensure_blocking_submit_allowed ();
        async ().result ().value ();
    }

    task_t<void> async ()
    {
        if (!_submission->try_claim ()) {
            return task_t<void> (
              result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                       "one-way call has already been submitted"));
        }
        if (_immediate) {
            return detail::submit_one_way_task ([immediate = *_immediate] { return immediate; });
        }
        if (_async_submit) {
            return _async_submit (_packet_name, _metadata);
        }
        if (!_submit) {
            return task_t<void> (
              result_t<void>::failure (framework_error_kind_t::protocol_error,
                                       "send call is not bound to a channel client"));
        }
        return detail::submit_one_way_task (
          [packet_name = _packet_name, metadata = _metadata, submit = _submit] () {
              return submit (packet_name, metadata);
          });
    }

  private:
    using async_submit_fn_t =
      std::function<task_t<void> (const std::string &, const metadata_map_t &)>;

    send_call_t (std::string packet_name, async_submit_fn_t submit) :
        _packet_name (std::move (packet_name)), _async_submit (std::move (submit))
    {
    }

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
    async_submit_fn_t _async_submit;
    std::shared_ptr<detail::submit_once_t> _submission = std::make_shared<detail::submit_once_t> ();

    friend class message_bus_t;
    friend class bound_session_t;
    friend class spot_context_t;
};

class publish_call_t
{
  public:
    using metadata_map_t = std::map<std::string, std::string>;
    using submit_fn_t = std::function<result_t<void> (const metadata_map_t &)>;
    using async_submit_fn_t = std::function<task_t<void> (const metadata_map_t &)>;

    explicit publish_call_t (result_t<void> result) : _immediate (std::move (result)) {}

    explicit publish_call_t (submit_fn_t submit,
                             std::chrono::milliseconds timeout = std::chrono::seconds (1)) :
        _submit (std::move (submit)), _timeout (timeout)
    {
    }

    explicit publish_call_t (async_submit_fn_t submit,
                             std::chrono::milliseconds timeout = std::chrono::seconds (1)) :
        _async_submit (std::move (submit)), _timeout (timeout)
    {
    }

    publish_call_t &metadata (std::string key, std::string value)
    {
        _metadata[std::move (key)] = std::move (value);
        return *this;
    }

    task_t<void> async ()
    {
        if (!_submission->try_claim ()) {
            return task_t<void> (
              result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                       "logical multicast call has already been submitted"));
        }
        if (_preflight) {
            const auto admitted = _preflight ();
            if (!admitted) {
                return task_t<void> (admitted);
            }
        }
        if (_immediate) {
            return task_t<void> (*_immediate);
        }
        if (_async_submit) {
            return detail::submit_logical_multicast_async_task (
              [submit = _async_submit, metadata = _metadata,
               context_lifetime = _context_lifetime] () mutable {
                  (void) context_lifetime;
                  if (!submit) {
                      throw framework_exception_t (
                        framework_error_kind_t::protocol_error,
                        "logical multicast call is not bound to a publisher");
                  }
                  return submit (metadata);
              },
              _timeout);
        }
        return detail::submit_logical_multicast_task (
          [submit = _submit, metadata = _metadata,
           context_lifetime = _context_lifetime] () mutable {
              (void) context_lifetime;
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
    friend class spot_context_t;

    std::optional<result_t<void>> _immediate;
    metadata_map_t _metadata;
    submit_fn_t _submit;
    async_submit_fn_t _async_submit;
    std::function<result_t<void> ()> _preflight;
    // Retain the source Context through submission, as its erased callback did.
    std::shared_ptr<void> _context_lifetime;
    std::chrono::milliseconds _timeout{std::chrono::seconds (1)};
    std::shared_ptr<detail::submit_once_t> _submission = std::make_shared<detail::submit_once_t> ();
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

    task_t<void> async () { return _call.async (); }

    void submit () { _call.submit (); }

  private:
    send_call_t _call;
};

class fanout_publish_call_t
{
  public:
    explicit fanout_publish_call_t (send_call_t call) : _call (std::move (call)) {}

    task_t<void> async () { return _call.async (); }

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
    task_t<void> async ();
    void submit ();

  private:
    using submit_fn_t =
      std::function<task_t<void> (const detail::stream_header_t &, const zlink::message_t &)>;

    friend class stream_t;
    friend class detail::stream_write_call_state_t;

    stream_write_call_t (detail::stream_header_t header,
                         zlink::message_t payload,
                         std::shared_ptr<const stream_compression_codec_t> compression_codec,
                         submit_fn_t submit);

    task_t<void> submit_now ();

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
    task_t<void> async ();
    void submit ();

  private:
    using submit_fn_t =
      std::function<task_t<void> (const detail::stream_header_t &, const zlink::message_t &)>;

    friend class stream_t;

    stream_send_call_t (detail::stream_header_t header,
                        zlink::message_t payload,
                        std::shared_ptr<const stream_compression_codec_t> compression_codec,
                        submit_fn_t submit);

    std::shared_ptr<detail::stream_write_call_state_t> _state;
};

} // namespace zlink::framework
