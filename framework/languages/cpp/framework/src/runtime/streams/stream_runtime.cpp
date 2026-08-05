/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "stream_runtime.hpp"

#include <zlink/framework/contracts/configuration/zlink_builder.hpp>

#include "runtime/channels/channel_runtime.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <limits>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace zlink::framework
{

using detail::stream_header_flags_t;
using detail::stream_header_t;
using detail::stream_metadata_t;
using detail::stream_message_kind_t;

namespace detail
{

std::mutex &stream_dispatch_executor_mutex ()
{
    static std::mutex mutex;
    return mutex;
}

std::shared_ptr<runtime::offload_executor_t> &stream_dispatch_executor_ref ()
{
    static std::shared_ptr<runtime::offload_executor_t> executor;
    return executor;
}

std::shared_ptr<runtime::offload_executor_t> stream_dispatch_executor ()
{
    std::lock_guard lock (stream_dispatch_executor_mutex ());
    return stream_dispatch_executor_ref ();
}

void ensure_stream_dispatch_executor ()
{
    std::lock_guard lock (stream_dispatch_executor_mutex ());
    if (!stream_dispatch_executor_ref ()) {
        stream_dispatch_executor_ref () = std::make_shared<runtime::offload_executor_t> (
          0, std::max<std::size_t> (1, std::thread::hardware_concurrency ()), 4096,
          std::chrono::milliseconds (100), "zlink-stream-ex");
    }
}

class stream_write_call_state_t
{
  public:
    explicit stream_write_call_state_t (result_t<void> result) : _immediate (std::move (result)) {}

    stream_write_call_state_t (stream_header_t header,
                               zlink::message_t payload,
                               std::shared_ptr<const stream_compression_codec_t> compression_codec,
                               std::function<result_t<void> (const stream_header_t &,
                                                            const zlink::message_t &)> submit) :
        _header (std::move (header)),
        _payload (std::move (payload)),
        _submit (std::move (submit)),
        _compression_codec (std::move (compression_codec))
    {
    }

    void metadata (std::string key, std::string value)
    {
        _metadata[std::move (key)] = std::move (value);
    }

    void packet_name (std::string packet_name) { _packet_name = std::move (packet_name); }

    void compress () { _compressed = true; }

    void reply_submission (std::shared_ptr<submit_once_t> submission)
    {
        _reply_submission = std::move (submission);
    }

    result_t<void> claim_submit ()
    {
        if (!_submission.try_claim ()) {
            return result_t<void>::failure (
              framework_error_kind_t::protocol_error,
              "STREAM write call has already been submitted");
        }
        if (_reply_submission && !_reply_submission->try_claim ()) {
            return result_t<void>::failure (
              framework_error_kind_t::protocol_error,
              "STREAM reply token has already been consumed");
        }
        return result_t<void>::success ();
    }

    result_t<void> submit_now ()
    {
        if (_immediate) {
            return *_immediate;
        }
        if (!_submit || !_header || !_payload) {
            return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                            "STREAM write call is not bound to a stream");
        }

        auto metadata = _header->metadata ().values ();
        for (const auto &[key, value] : _metadata) {
            metadata[key] = value;
        }
        auto flags = _header->flags ();
        auto payload = *_payload;
        if (_compressed) {
            if (!_compression_codec) {
                return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                                "STREAM compression codec is not configured");
            }
            try {
                payload = _compression_codec->compress (payload);
            }
            catch (const std::exception &error) {
                return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                                error.what ());
            }
            flags = flags | stream_header_flags_t::payload_compressed;
        }
        const auto packet_name =
          _packet_name.empty () ? std::string (_header->packet_name ()) : _packet_name;
        auto header =
          stream_header_t (_header->kind (), _header->codec (), flags, _header->request_seq (),
                           packet_name, stream_metadata_t (std::move (metadata)));
        if (auto correlation = _header->correlation_id ()) {
            header.with_correlation_id (std::string (*correlation));
        }
        return _submit (header, payload);
    }

  private:
    std::optional<result_t<void>> _immediate;
    std::optional<stream_header_t> _header;
    std::optional<zlink::message_t> _payload;
    std::function<result_t<void> (const stream_header_t &, const zlink::message_t &)> _submit;
    std::shared_ptr<const stream_compression_codec_t> _compression_codec;
    std::map<std::string, std::string> _metadata;
    std::string _packet_name;
    bool _compressed = false;
    submit_once_t _submission;
    std::shared_ptr<submit_once_t> _reply_submission;
};

class stream_session_dispatcher_t
{
  public:
    using dispatch_callback_t = std::function<task_t<void> ()>;

    explicit stream_session_dispatcher_t (stream_state_t &state) : _stream (state) {}

    result_t<void> dispatch (std::string operation, dispatch_callback_t callback) const
    {
        const std::lock_guard<std::mutex> dispatch_lock (_stream.dispatch_mutex);
        record_operation (std::move (operation));
        task_completion_source_t<void> completion;
        auto task = completion.task ();
        auto executor = stream_dispatch_executor ();
        if (!executor) {
            return detail::boundary_failure<void> (detail::boundary_error_t::shutdown,
                                            "stream dispatch executor is not running");
        }
        auto shared_completion =
          std::make_shared<detail::task_completion_source_t<void>> (std::move (completion));
        try {
            executor->submit ([callback = std::move (callback), shared_completion] () mutable {
                try {
                    auto callback_task = callback ();
                    detail::observe_task_completion (
                      callback_task,
                      [shared_completion] (const result_t<void> &result) mutable {
                          shared_completion->complete (result);
                      });
                }
                catch (const framework_exception_t &error) {
                    shared_completion->complete (detail::result_access_t::failure<void> (error));
                }
                catch (...) {
                    shared_completion->complete (result_t<void>::failure (
                      framework_error_kind_t::internal_failure,
                      "stream session callback threw an exception"));
                }
            });
        }
        catch (const std::exception &error) {
            shared_completion->complete (
              result_t<void>::failure (framework_error_kind_t::internal_failure, error.what ()));
        }
        catch (...) {
            shared_completion->complete (result_t<void>::failure (
                framework_error_kind_t::internal_failure, "stream dispatch executor rejected work"));
        }
        return task.result ();
    }

    result_t<void> dispatch_async (
      std::string operation,
      dispatch_callback_t callback,
      stream_runtime_t::async_dispatch_completion_t completion,
      stream_runtime_t::async_dispatch_started_t started,
      stream_runtime_t::async_dispatch_cancel_t cancelled) const
    {
        auto executor = stream_dispatch_executor ();
        if (!executor) {
            return detail::boundary_failure<void> (
              detail::boundary_error_t::shutdown,
              "stream dispatch executor is not running");
        }
        std::shared_ptr<runtime::serial_execution_queue_t> queue;
        {
            const std::lock_guard<std::mutex> dispatch_lock (_stream.dispatch_mutex);
            if (!_stream.dispatch_queue) {
                _stream.dispatch_queue =
                  std::make_shared<runtime::serial_execution_queue_t> (
                    *executor, runtime::serial_execution_queue_options_t{});
            }
            queue = _stream.dispatch_queue;
        }
        record_operation (operation);
        const bool posted = queue->post_async_wait (
          std::move (operation),
          [queue, callback = std::move (callback),
           completion = std::move (completion),
           started = std::move (started)] (auto complete) mutable {
              auto finish = [queue, complete = std::move (complete),
                             completion = std::move (completion)] (
                              const result_t<void> &result) mutable {
                  complete ([queue, completion = std::move (completion), result] () mutable {
                      if (completion) {
                          completion (result);
                      }
                  });
              };
              try {
                  if (started) {
                      started ();
                  }
                  auto callback_task = callback ();
                  detail::observe_task_completion (
                    callback_task,
                    [finish = std::move (finish)] (const result_t<void> &result) mutable {
                        finish (result);
                    });
              }
              catch (const framework_exception_t &error) {
                  finish (detail::result_access_t::failure<void> (error));
              }
              catch (const std::exception &error) {
                  finish (result_t<void>::failure (
                    framework_error_kind_t::internal_failure, error.what ()));
              }
              catch (...) {
                  finish (result_t<void>::failure (
                    framework_error_kind_t::internal_failure,
                    "stream session callback threw an exception"));
              }
          }, std::move (cancelled));
        if (!posted) {
            return result_t<void>::failure (
              framework_error_kind_t::capacity_exceeded,
              "stream serial dispatch queue is full or closed");
        }
        return result_t<void>::success ();
    }

    void drain_async () const
    {
        std::shared_ptr<runtime::serial_execution_queue_t> queue;
        {
            const std::lock_guard<std::mutex> dispatch_lock (_stream.dispatch_mutex);
            queue = _stream.dispatch_queue;
        }
        if (queue) {
            queue->drain ();
        }
    }

  private:
    void record_operation (std::string operation) const
    {
        const std::lock_guard<std::mutex> lock (_stream.state_mutex);
        _stream.serial_log.push_back (std::move (operation));
    }

    stream_state_t &_stream;
};

task_t<void> dispatch_packet_session (
  packet_stream_session_t *session,
  stream_t stream,
  std::shared_ptr<stream_header_t> header,
  std::shared_ptr<session_message_context_t> context,
  std::shared_ptr<zlink::message_t> payload)
{
    auto task = [&] {
        const stream_relay_dispatch_scope_t relay_scope (*header);
        return session->on_packet (stream, *context, *payload);
    } ();
    co_return co_await task;
}

void configure_stream_dispatch_executor ()
{
    ensure_stream_dispatch_executor ();
    const char *trace_value = std::getenv ("ZLINK_CPP_HOST_STOP_TRACE");
    if (trace_value != nullptr && std::string_view (trace_value) != "0"
        && std::string_view (trace_value) != "") {
        std::cerr << "zlink-cpp-host-stop stage=configure-stream-executor" << std::endl;
    }
}

void shutdown_stream_dispatch_executor () noexcept
{
    std::shared_ptr<runtime::offload_executor_t> executor;
    {
        std::lock_guard lock (stream_dispatch_executor_mutex ());
        executor = std::move (stream_dispatch_executor_ref ());
    }
    const char *trace_value = std::getenv ("ZLINK_CPP_HOST_STOP_TRACE");
    if (trace_value != nullptr && std::string_view (trace_value) != "0"
        && std::string_view (trace_value) != "") {
        std::cerr << "zlink-cpp-host-stop stage=shutdown-stream-executor" << std::endl;
    }
    if (executor) {
        executor->drain ();
    }
}

} // namespace detail

stream_write_call_t::stream_write_call_t (result_t<void> result) :
    _state (std::make_shared<detail::stream_write_call_state_t> (std::move (result)))
{
}

stream_write_call_t::stream_write_call_t (
  detail::stream_header_t header,
  zlink::message_t payload,
  std::shared_ptr<const stream_compression_codec_t> compression_codec,
  submit_fn_t submit) :
    _state (std::make_shared<detail::stream_write_call_state_t> (
      std::move (header), std::move (payload), std::move (compression_codec), std::move (submit)))
{
}

stream_write_call_t::~stream_write_call_t () = default;
stream_write_call_t::stream_write_call_t (stream_write_call_t &&) noexcept = default;
stream_write_call_t &stream_write_call_t::operator= (stream_write_call_t &&) noexcept = default;

stream_write_call_t &stream_write_call_t::metadata (std::string key, std::string value)
{
    _state->metadata (std::move (key), std::move (value));
    return *this;
}

stream_write_call_t &stream_write_call_t::compress ()
{
    _state->compress ();
    return *this;
}

task_t<void> stream_write_call_t::submit ()
{
    auto state = _state;
    auto claimed = state->claim_submit ();
    if (!claimed) {
        return task_t<void> (
          detail::result_access_t::failure<void> (*claimed.error ()));
    }
    return detail::submit_one_way_task ([state] { return state->submit_now (); });
}

result_t<void> stream_write_call_t::submit_now ()
{
    return _state->submit_now ();
}

stream_send_call_t::stream_send_call_t (result_t<void> result) :
    _state (std::make_shared<detail::stream_write_call_state_t> (std::move (result)))
{
}

stream_send_call_t::stream_send_call_t (
  detail::stream_header_t header,
  zlink::message_t payload,
  std::shared_ptr<const stream_compression_codec_t> compression_codec,
  submit_fn_t submit) :
    _state (std::make_shared<detail::stream_write_call_state_t> (
      std::move (header), std::move (payload), std::move (compression_codec), std::move (submit)))
{
}

stream_send_call_t::~stream_send_call_t () = default;
stream_send_call_t::stream_send_call_t (stream_send_call_t &&) noexcept = default;
stream_send_call_t &stream_send_call_t::operator= (stream_send_call_t &&) noexcept = default;

stream_send_call_t &stream_send_call_t::metadata (std::string key, std::string value)
{
    _state->metadata (std::move (key), std::move (value));
    return *this;
}

stream_send_call_t &stream_send_call_t::packet_name (std::string packet_name)
{
    _state->packet_name (std::move (packet_name));
    return *this;
}

stream_send_call_t &stream_send_call_t::compress ()
{
    _state->compress ();
    return *this;
}

task_t<void> stream_send_call_t::submit ()
{
    auto state = _state;
    auto claimed = state->claim_submit ();
    if (!claimed) {
        return task_t<void> (
          detail::result_access_t::failure<void> (*claimed.error ()));
    }
    return detail::submit_one_way_task ([state] { return state->submit_now (); });
}

stream_error_t::stream_error_t (stream_session_error_t error, std::string message) :
    _error (error), _message (std::move (message))
{
}

stream_session_error_t stream_error_t::error () const noexcept
{
    return _error;
}

std::string_view stream_error_t::message () const noexcept
{
    return _message;
}

stream_metadata_t::stream_metadata_t (std::map<std::string, std::string> values) :
    _values (std::move (values))
{
}

stream_metadata_t &stream_metadata_t::with (std::string key, std::string value)
{
    _values[std::move (key)] = std::move (value);
    return *this;
}

std::optional<std::string_view> stream_metadata_t::find (std::string_view key) const
{
    const auto found = _values.find (std::string (key));
    if (found == _values.end ()) {
        return std::nullopt;
    }
    return std::string_view (found->second);
}

bool stream_metadata_t::empty () const noexcept
{
    return _values.empty ();
}

const std::map<std::string, std::string> &stream_metadata_t::values () const noexcept
{
    return _values;
}

stream_header_t::stream_header_t () = default;

stream_header_t::stream_header_t (stream_message_kind_t kind,
                                  stream_codec_t codec,
                                  stream_header_flags_t flags,
                                  std::optional<std::uint64_t> request_seq,
                                  std::string packet_name,
                                  stream_metadata_t metadata) :
    _kind (kind),
    _codec (codec),
    _flags (flags),
    _request_seq (request_seq),
    _packet_name (std::move (packet_name)),
    _metadata (std::move (metadata))
{
}

stream_message_kind_t stream_header_t::kind () const noexcept
{
    return _kind;
}

stream_codec_t stream_header_t::codec () const noexcept
{
    return _codec;
}

stream_header_flags_t stream_header_t::flags () const noexcept
{
    return _flags;
}

std::optional<std::uint64_t> stream_header_t::request_seq () const noexcept
{
    return _request_seq;
}

std::string_view stream_header_t::packet_name () const noexcept
{
    return _packet_name;
}

std::optional<std::string_view> stream_header_t::metadata (std::string_view key) const
{
    return _metadata.find (key);
}

const stream_metadata_t &stream_header_t::metadata () const noexcept
{
    return _metadata;
}

std::optional<std::string_view> stream_header_t::correlation_id () const
{
    if (_correlation_id.empty ()) {
        return std::nullopt;
    }
    return _correlation_id;
}

stream_header_t &stream_header_t::with_correlation_id (std::string correlation_id)
{
    _correlation_id = std::move (correlation_id);
    return *this;
}

std::optional<std::string_view> stream_header_t::flow_id () const
{
    if (_flow_id.empty ()) {
        return std::nullopt;
    }
    return _flow_id;
}

std::optional<flow_origin_t> stream_header_t::flow_origin () const noexcept
{
    return _flow_origin;
}

stream_header_t &stream_header_t::with_flow (std::string flow_id, flow_origin_t origin)
{
    _flow_id = std::move (flow_id);
    _flow_origin = origin;
    return *this;
}

std::optional<std::string_view> stream_header_t::content_type () const
{
    return metadata ("content_type");
}

stream_t::stream_t () : _state (std::make_shared<detail::stream_state_t> ())
{
}

stream_t::stream_t (std::shared_ptr<detail::stream_state_t> state) : _state (std::move (state))
{
}

stream_t::~stream_t () = default;
stream_t::stream_t (stream_t &&) noexcept = default;
stream_t &stream_t::operator= (stream_t &&) noexcept = default;

std::string stream_t::session_id () const
{
    return _state->session_id;
}

std::optional<zlink::routing_id_t> stream_t::routing_id () const
{
    return _state->routing_id;
}

std::optional<std::string> stream_t::local_address () const
{
    return _state->local_address;
}

std::optional<std::string> stream_t::remote_address () const
{
    return _state->remote_address;
}

session_actor_manager_t &stream_t::actors ()
{
    auto *actors = _state->actors.load (std::memory_order_acquire);
    if (!actors) {
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "STREAM session Actor manager is not attached");
    }
    return *actors;
}

task_t<void> stream_t::close ()
{
    _state->closed.store (true, std::memory_order_release);
    return task_t<void> (result_t<void>::success ());
}

namespace
{

std::function<result_t<void> (const stream_header_t &, const zlink::message_t &)>
stream_submitter (std::shared_ptr<detail::stream_state_t> state)
{
    return [state = std::move (state)] (const stream_header_t &submitted_header,
                                        const zlink::message_t &submitted_payload) {
        if (state->closed.load (std::memory_order_acquire)) {
            return detail::boundary_failure<void> (detail::boundary_error_t::disconnected,
                                                    "STREAM session is disconnected");
        }
        {
            const std::lock_guard<std::mutex> lock (state->transport_writer_mutex);
            if (state->closed.load (std::memory_order_acquire)) {
                return detail::boundary_failure<void> (detail::boundary_error_t::disconnected,
                                                        "STREAM session is disconnected");
            }
            if (state->transport_writer)
                return state->transport_writer (submitted_header, submitted_payload);
        }
        if (state->closed.load (std::memory_order_acquire)) {
            return detail::boundary_failure<void> (detail::boundary_error_t::disconnected,
                                                    "STREAM session is disconnected");
        }
        const std::lock_guard<std::mutex> lock (state->state_mutex);
        state->written_headers.push_back (submitted_header);
        state->written_payloads.push_back (submitted_payload);
        return result_t<void>::success ();
    };
}

} // namespace

stream_send_call_t stream_t::write_packet (const zlink::message_t &payload)
{
    stream_header_t header (stream_message_kind_t::send, stream_codec_t::raw,
                            stream_header_flags_t::none, std::nullopt, "", {});
    return stream_send_call_t (std::move (header), payload, _state->compression_codec,
                               stream_submitter (_state));
}

stream_write_call_t stream_t::write_packet_with_header (detail::stream_header_t header,
                                                        zlink::message_t payload)
{
    /* Stream writes propagate the ambient flow (flow-correlation §3.2);
     * control packets never carry the pair. */
    if (!header.flow_id () && header.kind () != stream_message_kind_t::control) {
        if (const auto &flow = runtime::flow_context_t::current ()) {
            header.with_flow (flow->flow_id, flow->origin);
        }
    }
    return stream_write_call_t (
      std::move (header), std::move (payload), _state->compression_codec,
      stream_submitter (_state));
}

stream_write_call_t stream_t::reply_packet (const zlink::message_t &payload)
{
    const auto request_header = _reply_header;
    if (!request_header) {
        return stream_write_call_t (
          result_t<void>::failure (framework_error_kind_t::protocol_error,
                                   "STREAM reply requires current dispatch state"));
    }
    if (!request_header->request_seq ()) {
        return stream_write_call_t (
          result_t<void>::failure (framework_error_kind_t::protocol_error,
                                   "STREAM reply requires request sequence"));
    }
    stream_header_t reply_header (stream_message_kind_t::response, request_header->codec (),
                                  stream_header_flags_t::has_request_seq,
                                  request_header->request_seq (), "", {});
    if (auto correlation = request_header->correlation_id ()) {
        reply_header.with_correlation_id (std::string (*correlation));
    }
    auto call = write_packet_with_header (std::move (reply_header), payload);
    call._state->reply_submission (_reply_submission);
    return call;
}

stream_builder_t::stream_builder_t () :
    _state (std::make_shared<detail::stream_builder_state_t> (""))
{
}

stream_builder_t::stream_builder_t (std::shared_ptr<detail::stream_builder_state_t> state) :
    _state (std::move (state))
{
}

stream_builder_t::~stream_builder_t () = default;
stream_builder_t::stream_builder_t (stream_builder_t &&) noexcept = default;
stream_builder_t &stream_builder_t::operator= (stream_builder_t &&) noexcept = default;

stream_builder_t &stream_builder_t::bind (std::string endpoint)
{
    if (endpoint.empty ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "STREAM bind endpoint must not be empty");
    }
    _state->snapshot.bind_endpoint = std::move (endpoint);
    return *this;
}

stream_builder_t &stream_builder_t::bind (std::uint16_t port)
{
    return bind ("tcp://0.0.0.0:" + std::to_string (port));
}

stream_builder_t &stream_builder_t::set_bind_host (std::string host)
{
    if (host.empty ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "STREAM bind host must not be empty");
    }
    auto endpoint = _state->snapshot.bind_endpoint;
    const auto separator = endpoint.rfind (':');
    const auto port = separator == std::string::npos
                        ? std::string ("0")
                        : endpoint.substr (separator + 1);
    const auto scheme = endpoint.rfind ("ws://", 0) == 0
                          ? std::string ("ws://")
                          : endpoint.rfind ("tls://", 0) == 0
                              ? std::string ("tls://")
                              : std::string ("tcp://");
    return bind (scheme + std::move (host) + ":" + port);
}

stream_builder_t &stream_builder_t::set_advertise_host (std::string host)
{
    if (host.empty ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "STREAM advertise host must not be empty");
    }
    _state->advertise_host = std::move (host);
    return *this;
}

stream_builder_t &stream_builder_t::set_max_message_size (std::int64_t value)
{
    if (value < 0) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "STREAM MaxMessageSize must be zero or positive");
    }
    _state->snapshot.max_message_size = value;
    return *this;
}

stream_builder_t &stream_builder_t::configure_tls_server (
  std::string certificate_file,
  std::string private_key_file,
  bool require_client_certificate)
{
    if (certificate_file.empty () || private_key_file.empty ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "STREAM TLS server requires certificate and private key");
    }
    _state->snapshot.tls_certificate_file = std::move (certificate_file);
    _state->snapshot.tls_private_key_file = std::move (private_key_file);
    _state->snapshot.tls_require_client_certificate = require_client_certificate;
    return *this;
}

stream_builder_t &stream_builder_t::register_session (std::string session_name)
{
    if (session_name.empty ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "STREAM packet session name must not be empty");
    }
    _state->snapshot.packet_session_name = std::move (session_name);
    return *this;
}

stream_snapshot_t stream_builder_t::snapshot () const
{
    return _state->snapshot;
}

stream_builder_t zlink_builder_t::stream (std::string stream_name)
{
    auto state = std::make_shared<detail::stream_builder_state_t> (std::move (stream_name));
    _state->stream_runtime->streams[state->snapshot.name] = state;
    return stream_builder_t (state);
}

} // namespace zlink::framework

namespace zlink::framework::detail
{

stream_runtime_t::stream_runtime_t (std::shared_ptr<stream_runtime_state_t> state) :
    _state (std::move (state))
{
}

stream_runtime_t stream_runtime_t::from (const zlink_builder_t &builder)
{
    return stream_runtime_t (builder._state->stream_runtime);
}

std::vector<stream_snapshot_t> stream_runtime_t::snapshots () const
{
    std::vector<stream_snapshot_t> result;
    result.reserve (_state->streams.size ());
    for (const auto &[_, state] : _state->streams) {
        result.push_back (state->snapshot);
    }
    return result;
}

void bind_stream_serializers (zlink_builder_t &builder, serializer_registry_t &serializers)
{
    builder._state->stream_runtime->serializers = &serializers;
}

void apply_stream_compression_codec (zlink_builder_t &builder,
                                     std::shared_ptr<const stream_compression_codec_t> codec)
{
    builder._state->stream_runtime->compression_codec = std::move (codec);
}

namespace
{

bool has_flag (stream_header_flags_t flags, stream_header_flags_t flag)
{
    return (static_cast<std::uint8_t> (flags) & static_cast<std::uint8_t> (flag)) != 0;
}

constexpr std::size_t max_stream_decompressed_payload_size = 64 * 1024;

bool known_kind (stream_message_kind_t kind)
{
    switch (kind) {
        case stream_message_kind_t::send:
        case stream_message_kind_t::request:
        case stream_message_kind_t::response:
        case stream_message_kind_t::error:
        case stream_message_kind_t::control:
            return true;
    }
    return false;
}

bool known_codec (stream_codec_t codec)
{
    switch (codec) {
        case stream_codec_t::raw:
        case stream_codec_t::json:
        case stream_codec_t::message_pack:
        case stream_codec_t::protobuf:
            return true;
    }
    return false;
}

result_t<void> validate_name (std::string_view name, bool allow_reserved)
{
    if (name.empty () || name.size () > 255) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "STREAM packet name is invalid");
    }
    if (!allow_reserved && name.rfind ("__zlink.", 0) == 0) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "STREAM packet name uses a reserved prefix");
    }
    return result_t<void>::success ();
}

void append_u64 (std::vector<std::uint8_t> &bytes, std::uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8) {
        bytes.push_back (static_cast<std::uint8_t> ((value >> shift) & 0xff));
    }
}

void append_u16 (std::vector<std::uint8_t> &bytes, std::uint16_t value)
{
    bytes.push_back (static_cast<std::uint8_t> ((value >> 8) & 0xff));
    bytes.push_back (static_cast<std::uint8_t> (value & 0xff));
}

std::uint16_t read_u16 (const std::vector<std::uint8_t> &bytes, std::size_t &offset)
{
    const auto value = static_cast<std::uint16_t> ((bytes[offset] << 8) | bytes[offset + 1]);
    offset += 2;
    return value;
}

std::uint64_t read_u64 (const std::vector<std::uint8_t> &bytes, std::size_t &offset)
{
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | bytes[offset++];
    }
    return value;
}

} // namespace

result_t<void> stream_runtime_t::validate_header (const stream_header_t &header) const
{
    if (!known_kind (header.kind ()) || !known_codec (header.codec ())) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "STREAM header contains unknown kind or codec");
    }

    const auto raw_flags = static_cast<std::uint8_t> (header.flags ());
    constexpr auto known_flags =
      static_cast<std::uint8_t> (stream_header_flags_t::has_request_seq)
      | static_cast<std::uint8_t> (stream_header_flags_t::has_metadata)
      | static_cast<std::uint8_t> (stream_header_flags_t::payload_compressed)
      | static_cast<std::uint8_t> (stream_header_flags_t::has_correlation_id)
      | static_cast<std::uint8_t> (stream_header_flags_t::has_flow_id);
    if ((raw_flags & ~known_flags) != 0) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "STREAM header contains unknown flags");
    }
    if (header.flow_id ().has_value () != header.flow_origin ().has_value ()) {
        return result_t<void>::failure (
          framework_error_kind_t::protocol_error,
          "STREAM header flow id and origin must be present together");
    }
    if (header.flow_id () && !runtime::flow_id_t::is_valid (*header.flow_id ())) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "STREAM header flow id must be UUIDv7");
    }
    if (header.flow_origin ()) {
        const auto raw_origin = static_cast<std::uint8_t> (*header.flow_origin ());
        if (raw_origin < 1 || raw_origin > 4) {
            return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                            "STREAM header flow origin is invalid");
        }
        if (header.kind () == stream_message_kind_t::control) {
            return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                            "STREAM control packet must not carry flow fields");
        }
    }

    const bool is_reply = header.kind () == stream_message_kind_t::response
                          || header.kind () == stream_message_kind_t::error;
    if (!is_reply) {
        if (auto valid_name = validate_name (
              header.packet_name (), header.kind () == stream_message_kind_t::control);
            !valid_name) {
            return valid_name;
        }
    }

    const bool has_request_seq = header.request_seq ().has_value ();
    const bool has_metadata = !header.metadata ().empty ();
    if (header.kind () == stream_message_kind_t::send && has_request_seq) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "STREAM send packet must not contain request sequence");
    }
    if ((header.kind () == stream_message_kind_t::request
         || header.kind () == stream_message_kind_t::response)
        && !has_request_seq) {
        return result_t<void>::failure (
          framework_error_kind_t::protocol_error,
          "STREAM request and response packets require request sequence");
    }
    if (has_request_seq && *header.request_seq () == 0) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "STREAM request sequence must not be zero");
    }
    if (header.kind () == stream_message_kind_t::error && header.codec () != stream_codec_t::json) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "STREAM error packet must use JSON codec");
    }
    if (header.kind () == stream_message_kind_t::control) {
        if (header.flags () != stream_header_flags_t::none || header.codec () != stream_codec_t::raw
            || has_request_seq || has_metadata) {
            return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                            "STREAM control packet must be raw and flagless");
        }
    }
    return result_t<void>::success ();
}

result_t<std::vector<std::uint8_t>>
stream_runtime_t::encode_header (const stream_header_t &header) const
{
    if ((header.kind () == stream_message_kind_t::response
         || header.kind () == stream_message_kind_t::error)
        && !header.packet_name ().empty ()) {
        return result_t<std::vector<std::uint8_t>>::failure (
          framework_error_kind_t::protocol_error,
          "STREAM response and error packet names must be empty");
    }
    if (auto valid = validate_header (header); !valid) {
        return result_t<std::vector<std::uint8_t>>::failure (valid.error_kind (),
                                                             valid.error ()->what ());
    }

    auto flags = header.flags ();
    if (header.request_seq ()) {
        flags = flags | stream_header_flags_t::has_request_seq;
    }
    if (!header.metadata ().empty ()) {
        flags = flags | stream_header_flags_t::has_metadata;
    }
    const auto correlation = header.correlation_id ();
    if (correlation) {
        flags = flags | stream_header_flags_t::has_correlation_id;
    }
    if (correlation && correlation->size () > std::numeric_limits<std::uint8_t>::max ()) {
        return result_t<std::vector<std::uint8_t>>::failure (
          framework_error_kind_t::protocol_error, "STREAM correlation id is too large");
    }
    const auto flow = header.flow_id ();
    if (flow) {
        flags = flags | stream_header_flags_t::has_flow_id;
    }

    std::vector<std::uint8_t> bytes;
    bytes.push_back (runtime::flow_id_t::format_marker);
    bytes.push_back (static_cast<std::uint8_t> (header.kind ()));
    bytes.push_back (static_cast<std::uint8_t> (header.codec ()));
    bytes.push_back (static_cast<std::uint8_t> (flags));
    if (header.request_seq ()) {
        append_u64 (bytes, *header.request_seq ());
    }
    bytes.push_back (static_cast<std::uint8_t> (header.packet_name ().size ()));
    bytes.insert (bytes.end (), header.packet_name ().begin (), header.packet_name ().end ());
    if (!header.metadata ().empty ()) {
        if (header.metadata ().values ().size () > std::numeric_limits<std::uint8_t>::max ()) {
            return result_t<std::vector<std::uint8_t>>::failure (
              framework_error_kind_t::protocol_error,
              "STREAM metadata item count is too large");
        }
        std::vector<std::uint8_t> metadata_bytes;
        metadata_bytes.push_back (static_cast<std::uint8_t> (header.metadata ().values ().size ()));
        for (const auto &[key, value] : header.metadata ().values ()) {
            if (key.empty () || key.size () > std::numeric_limits<std::uint8_t>::max ()
                || value.size () > std::numeric_limits<std::uint16_t>::max ()) {
                return result_t<std::vector<std::uint8_t>>::failure (
                  framework_error_kind_t::protocol_error,
                  "STREAM metadata key or value is too large");
            }
            metadata_bytes.push_back (static_cast<std::uint8_t> (key.size ()));
            metadata_bytes.insert (metadata_bytes.end (), key.begin (), key.end ());
            append_u16 (metadata_bytes, static_cast<std::uint16_t> (value.size ()));
            metadata_bytes.insert (metadata_bytes.end (), value.begin (), value.end ());
        }
        if (metadata_bytes.size () > std::numeric_limits<std::uint16_t>::max ()) {
            return result_t<std::vector<std::uint8_t>>::failure (
              framework_error_kind_t::protocol_error, "STREAM metadata is too large");
        }
        append_u16 (bytes, static_cast<std::uint16_t> (metadata_bytes.size ()));
        bytes.insert (bytes.end (), metadata_bytes.begin (), metadata_bytes.end ());
    }
    if (correlation) {
        bytes.push_back (static_cast<std::uint8_t> (correlation->size ()));
        bytes.insert (bytes.end (), correlation->begin (), correlation->end ());
    }
    if (flow) {
        bytes.insert (bytes.end (), flow->begin (), flow->end ());
        bytes.push_back (static_cast<std::uint8_t> (*header.flow_origin ()));
    }
    return result_t<std::vector<std::uint8_t>>::success (std::move (bytes));
}

result_t<stream_header_t>
stream_runtime_t::decode_header (const std::vector<std::uint8_t> &bytes) const
{
    if (bytes.size () < 5) {
        return result_t<stream_header_t>::failure (framework_error_kind_t::protocol_error,
                                                   "STREAM header is too short");
    }
    std::size_t offset = 0;
    if (bytes[offset++] != runtime::flow_id_t::format_marker) {
        return result_t<stream_header_t>::failure (framework_error_kind_t::protocol_error,
                                                   "STREAM format marker is invalid");
    }
    const auto kind = static_cast<stream_message_kind_t> (bytes[offset++]);
    const auto codec = static_cast<stream_codec_t> (bytes[offset++]);
    auto flags = static_cast<stream_header_flags_t> (bytes[offset++]);
    std::optional<std::uint64_t> request_seq;
    if (has_flag (flags, stream_header_flags_t::has_request_seq)) {
        if (bytes.size () - offset < 8) {
            return result_t<stream_header_t>::failure (
              framework_error_kind_t::protocol_error,
              "STREAM request sequence is incomplete");
        }
        request_seq = read_u64 (bytes, offset);
    }
    if (offset >= bytes.size ()) {
        return result_t<stream_header_t>::failure (framework_error_kind_t::protocol_error,
                                                   "STREAM packet name length is missing");
    }
    const auto name_size = bytes[offset++];
    if (bytes.size () - offset < name_size) {
        return result_t<stream_header_t>::failure (framework_error_kind_t::protocol_error,
                                                   "STREAM packet name is invalid");
    }
    std::string name (bytes.begin () + static_cast<std::ptrdiff_t> (offset),
                      bytes.begin () + static_cast<std::ptrdiff_t> (offset + name_size));
    offset += name_size;

    stream_metadata_t metadata;
    if (has_flag (flags, stream_header_flags_t::has_metadata)) {
        if (bytes.size () - offset < 2) {
            return result_t<stream_header_t>::failure (
              framework_error_kind_t::protocol_error, "STREAM metadata length is missing");
        }
        const auto metadata_size = read_u16 (bytes, offset);
        if (bytes.size () - offset < metadata_size) {
            return result_t<stream_header_t>::failure (
              framework_error_kind_t::protocol_error, "STREAM metadata is incomplete");
        }
        const auto metadata_end = offset + metadata_size;
        if (offset >= metadata_end) {
            return result_t<stream_header_t>::failure (
              framework_error_kind_t::protocol_error, "STREAM metadata count is missing");
        }
        const auto count = bytes[offset++];
        for (std::uint8_t i = 0; i < count; ++i) {
            if (offset >= metadata_end) {
                return result_t<stream_header_t>::failure (
                  framework_error_kind_t::protocol_error,
                  "STREAM metadata key length is missing");
            }
            const auto key_size = bytes[offset++];
            if (key_size == 0 || metadata_end - offset < key_size) {
                return result_t<stream_header_t>::failure (
                  framework_error_kind_t::protocol_error,
                  "STREAM metadata key is incomplete");
            }
            std::string key (bytes.begin () + static_cast<std::ptrdiff_t> (offset),
                             bytes.begin () + static_cast<std::ptrdiff_t> (offset + key_size));
            offset += key_size;
            if (metadata_end - offset < 2) {
                return result_t<stream_header_t>::failure (
                  framework_error_kind_t::protocol_error,
                  "STREAM metadata value length is missing");
            }
            const auto value_size = read_u16 (bytes, offset);
            if (metadata_end - offset < value_size) {
                return result_t<stream_header_t>::failure (
                  framework_error_kind_t::protocol_error,
                  "STREAM metadata value is incomplete");
            }
            std::string value (bytes.begin () + static_cast<std::ptrdiff_t> (offset),
                               bytes.begin () + static_cast<std::ptrdiff_t> (offset + value_size));
            offset += value_size;
            metadata.with (std::move (key), std::move (value));
        }
        if (offset != metadata_end) {
            return result_t<stream_header_t>::failure (
              framework_error_kind_t::protocol_error, "STREAM metadata has trailing bytes");
        }
    }
    std::string correlation;
    if (has_flag (flags, stream_header_flags_t::has_correlation_id)) {
        if (offset >= bytes.size ()) {
            return result_t<stream_header_t>::failure (
              framework_error_kind_t::protocol_error,
              "STREAM correlation id length is missing");
        }
        const auto correlation_size = bytes[offset++];
        if (correlation_size == 0 || bytes.size () - offset < correlation_size) {
            return result_t<stream_header_t>::failure (
              framework_error_kind_t::protocol_error, "STREAM correlation id is incomplete");
        }
        correlation =
          std::string (bytes.begin () + static_cast<std::ptrdiff_t> (offset),
                       bytes.begin () + static_cast<std::ptrdiff_t> (offset + correlation_size));
        offset += correlation_size;
    }
    std::string flow_id;
    std::optional<flow_origin_t> flow_origin;
    if (has_flag (flags, stream_header_flags_t::has_flow_id)) {
        if (bytes.size () - offset < runtime::flow_id_t::encoded_length + 1) {
            return result_t<stream_header_t>::failure (
              framework_error_kind_t::protocol_error,
              "STREAM header flow fields are incomplete");
        }
        flow_id = std::string (
          bytes.begin () + static_cast<std::ptrdiff_t> (offset),
          bytes.begin () + static_cast<std::ptrdiff_t> (offset + runtime::flow_id_t::encoded_length));
        offset += runtime::flow_id_t::encoded_length;
        flow_origin = static_cast<flow_origin_t> (bytes[offset++]);
    }
    if (offset != bytes.size ()) {
        return result_t<stream_header_t>::failure (framework_error_kind_t::protocol_error,
                                                   "STREAM header has trailing bytes");
    }

    stream_header_t header (kind, codec, flags, request_seq, std::move (name),
                            std::move (metadata));
    if (!correlation.empty ()) {
        header.with_correlation_id (std::move (correlation));
    }
    if (!flow_id.empty () && flow_origin) {
        header.with_flow (std::move (flow_id), *flow_origin);
    }
    if (auto valid = validate_header (header); !valid) {
        return result_t<stream_header_t>::failure (framework_error_kind_t::protocol_error,
                                                   valid.error ()->what ());
    }
    return result_t<stream_header_t>::success (std::move (header));
}

std::vector<std::uint8_t>
stream_runtime_t::encode_session_closing_payload (stream_close_reason_t reason,
                                                  std::string_view diagnostic)
{
    if (diagnostic.size () > 512) {
        diagnostic = diagnostic.substr (0, 512);
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve (4 + diagnostic.size ());
    bytes.push_back (1);
    bytes.push_back (static_cast<std::uint8_t> (reason));
    bytes.push_back (static_cast<std::uint8_t> ((diagnostic.size () >> 8) & 0xff));
    bytes.push_back (static_cast<std::uint8_t> (diagnostic.size () & 0xff));
    bytes.insert (bytes.end (), diagnostic.begin (), diagnostic.end ());
    return bytes;
}

void stream_runtime_t::send_session_closing (stream_t &stream,
                                             stream_close_reason_t reason,
                                             std::string_view diagnostic) const noexcept
{
    try {
        const auto payload_bytes = encode_session_closing_payload (reason, diagnostic);
        stream_header_t closing (stream_message_kind_t::control, stream_codec_t::raw,
                                 stream_header_flags_t::none, std::nullopt, "session-closing",
                                 {});
        stream
          .write_packet_with_header (
            std::move (closing),
            zlink::message_t::from (std::string (payload_bytes.begin (), payload_bytes.end ())))
          .submit ().result ().value ();
    }
    catch (...) {
    }
}

void stream_runtime_t::send_heartbeat_ping (stream_t &stream) const noexcept
{
    try {
        stream_header_t ping (stream_message_kind_t::control, stream_codec_t::raw,
                              stream_header_flags_t::none, std::nullopt, "$zlink.heartbeat.ping",
                              {});
        stream.write_packet_with_header (std::move (ping), zlink::message_t{})
          .submit ().result ().value ();
    }
    catch (...) {
    }
}

void stream_runtime_t::send_heartbeat_pong (stream_t &stream) const noexcept
{
    try {
        stream_header_t pong (stream_message_kind_t::control, stream_codec_t::raw,
                              stream_header_flags_t::none, std::nullopt,
                              "$zlink.heartbeat.pong", {});
        stream.write_packet_with_header (std::move (pong), zlink::message_t{})
          .submit ().result ().value ();
    }
    catch (...) {
    }
}

stream_t stream_runtime_t::open_session (std::string stream_name) const
{
    if (_state->streams.find (stream_name) == _state->streams.end ()) {
        throw framework_exception_t (framework_error_kind_t::not_found,
                                     "STREAM endpoint is not registered");
    }
    auto state = std::make_shared<stream_state_t> ();
    state->session_id = std::move (stream_name) + ":" + std::to_string (_state->next_session_id++);
    state->serializers = _state->serializers;
    state->compression_codec = _state->compression_codec;
    return stream_t (state);
}

void stream_runtime_t::set_session_identity (
  stream_t &stream,
  std::optional<zlink::routing_id_t> routing_id,
  std::optional<std::string> local_address,
  std::optional<std::string> remote_address) const
{
    stream._state->routing_id = std::move (routing_id);
    stream._state->local_address = std::move (local_address);
    stream._state->remote_address = std::move (remote_address);
}

result_t<void> stream_runtime_t::dispatch_serial (stream_t &stream,
                                                  std::string operation,
                                                  std::function<task_t<void> ()> callback) const
{
    return stream_session_dispatcher_t (*stream._state)
      .dispatch (std::move (operation), std::move (callback));
}

result_t<void> stream_runtime_t::dispatch_serial_async (
  stream_t &stream,
  std::string operation,
  std::function<task_t<void> ()> callback,
  async_dispatch_completion_t completion,
  async_dispatch_started_t started,
  async_dispatch_cancel_t cancelled) const
{
    return stream_session_dispatcher_t (*stream._state)
      .dispatch_async (std::move (operation), std::move (callback),
                       std::move (completion), std::move (started),
                       std::move (cancelled));
}

result_t<void> stream_runtime_t::dispatch_connected (packet_stream_session_t &session,
                                                     stream_t &stream) const
{
    return dispatch_serial (stream, "connected", [&] { return session.on_connected (stream); });
}

result_t<void> stream_runtime_t::dispatch_connected_async (
  packet_stream_session_t &session,
  stream_t &stream,
  async_dispatch_completion_t completion) const
{
    auto dispatch_stream = stream;
    return dispatch_serial_async (
      stream, "connected",
      [session = &session, dispatch_stream = std::move (dispatch_stream)] () mutable {
          return session->on_connected (dispatch_stream);
      },
      std::move (completion));
}

result_t<void> stream_runtime_t::dispatch_packet (packet_stream_session_t &session,
                                                  stream_t &stream,
                                                  const stream_header_t &header,
                                                  const zlink::message_t &payload) const
{
    if (auto valid = validate_header (header); !valid) {
        return valid;
    }
    auto handler_payload = payload;
    if (has_flag (header.flags (), stream_header_flags_t::payload_compressed)) {
        if (!_state->compression_codec) {
            return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                            "STREAM compression codec is not configured");
        }
        try {
            handler_payload =
              _state->compression_codec->decompress (payload, max_stream_decompressed_payload_size);
        }
        catch (const std::exception &error) {
            return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                            error.what ());
        }
        if (handler_payload.bytes ().size () > max_stream_decompressed_payload_size) {
            return result_t<void>::failure (
              framework_error_kind_t::protocol_error,
              "STREAM decompressed payload exceeds configured receive limit");
        }
    }
    std::optional<std::string> inbound_flow_id;
    if (auto id = header.flow_id ()) {
        inbound_flow_id = std::string (*id);
    }
    auto flow_scope = runtime::flow_context_t::enter (
      std::move (inbound_flow_id), header.flow_origin (),
      detail::message_flow_tracer_t (_state->dispatch).capture_enabled (),
      flow_origin_t::inbound);
    detail::message_flow_tracer_t (_state->dispatch).trace (message_flow_outcome_t::received, [&] {
        std::optional<std::string> correlation;
        if (auto id = header.correlation_id ()) {
            correlation = std::string (*id);
        }
        return message_flow_event_t{message_flow_outcome_t::received,
                                    dispatch_error_surface_t::stream_session,
                                    dispatch_message_kind_t::request,
                                    std::string (header.packet_name ()),
                                    std::nullopt,
                                    std::nullopt,
                                    correlation,
                                    std::nullopt,
                                    std::nullopt,
                                    std::nullopt,
                                    std::nullopt};
    });
    auto current_flow = runtime::flow_context_t::current ();
    auto dispatch_stream = stream;
    dispatch_stream._reply_header = header;
    dispatch_stream._reply_submission = std::make_shared<submit_once_t> ();
    auto dispatch_header = std::make_shared<stream_header_t> (header);
    auto dispatch_context = std::make_shared<session_message_context_t> ();
    dispatch_context->packet_name = std::string (header.packet_name ());
    dispatch_context->metadata = message_metadata_t (header.metadata ().values ());
    dispatch_context->can_reply = header.request_seq ().has_value ();
    auto dispatch_payload = std::make_shared<zlink::message_t> (std::move (handler_payload));
    return dispatch_serial (stream, "packet:" + std::string (header.packet_name ()),
                            [session = &session,
                             dispatch_stream = std::move (dispatch_stream),
                             dispatch_header = std::move (dispatch_header),
                             dispatch_context = std::move (dispatch_context),
                             dispatch_payload = std::move (dispatch_payload),
                             current_flow = std::move (current_flow)] () mutable {
        runtime::flow_context_t::scope_t callback_flow (std::move (current_flow));
        return dispatch_packet_session (
          session, std::move (dispatch_stream), std::move (dispatch_header),
          std::move (dispatch_context), std::move (dispatch_payload));
    });
}

result_t<void> stream_runtime_t::dispatch_packet_async (
  packet_stream_session_t &session,
  stream_t &stream,
  const stream_header_t &header,
  const zlink::message_t &payload,
  async_dispatch_completion_t completion,
  async_dispatch_started_t started,
  async_dispatch_cancel_t cancelled) const
{
    if (auto valid = validate_header (header); !valid) {
        return valid;
    }
    auto handler_payload = payload;
    if (has_flag (header.flags (), stream_header_flags_t::payload_compressed)) {
        if (!_state->compression_codec) {
            return result_t<void>::failure (
              framework_error_kind_t::protocol_error,
              "STREAM compression codec is not configured");
        }
        try {
            handler_payload = _state->compression_codec->decompress (
              payload, max_stream_decompressed_payload_size);
        }
        catch (const std::exception &error) {
            return result_t<void>::failure (
              framework_error_kind_t::protocol_error, error.what ());
        }
        if (handler_payload.bytes ().size () > max_stream_decompressed_payload_size) {
            return result_t<void>::failure (
              framework_error_kind_t::protocol_error,
              "STREAM decompressed payload exceeds configured receive limit");
        }
    }
    std::optional<std::string> inbound_flow_id;
    if (auto id = header.flow_id ()) {
        inbound_flow_id = std::string (*id);
    }
    auto flow_scope = runtime::flow_context_t::enter (
      std::move (inbound_flow_id), header.flow_origin (),
      detail::message_flow_tracer_t (_state->dispatch).capture_enabled (),
      flow_origin_t::inbound);
    detail::message_flow_tracer_t (_state->dispatch).trace (
      message_flow_outcome_t::received, [&] {
          std::optional<std::string> correlation;
          if (auto id = header.correlation_id ()) {
              correlation = std::string (*id);
          }
          return message_flow_event_t{message_flow_outcome_t::received,
                                      dispatch_error_surface_t::stream_session,
                                      dispatch_message_kind_t::request,
                                      std::string (header.packet_name ()),
                                      std::nullopt,
                                      std::nullopt,
                                      correlation,
                                      std::nullopt,
                                      std::nullopt,
                                      std::nullopt,
                                      std::nullopt};
      });
    auto current_flow = runtime::flow_context_t::current ();
    auto dispatch_stream = stream;
    dispatch_stream._reply_header = header;
    dispatch_stream._reply_submission = std::make_shared<submit_once_t> ();
    auto dispatch_header = std::make_shared<stream_header_t> (header);
    auto dispatch_context = std::make_shared<session_message_context_t> ();
    dispatch_context->packet_name = std::string (header.packet_name ());
    dispatch_context->metadata = message_metadata_t (header.metadata ().values ());
    dispatch_context->can_reply = header.request_seq ().has_value ();
    auto dispatch_payload =
      std::make_shared<zlink::message_t> (std::move (handler_payload));
    return dispatch_serial_async (
      stream, "packet:" + std::string (header.packet_name ()),
      [session = &session, dispatch_stream = std::move (dispatch_stream),
       dispatch_header = std::move (dispatch_header),
       dispatch_context = std::move (dispatch_context),
       dispatch_payload = std::move (dispatch_payload),
       current_flow = std::move (current_flow)] () mutable {
          runtime::flow_context_t::scope_t callback_flow (std::move (current_flow));
          return dispatch_packet_session (
            session, std::move (dispatch_stream), std::move (dispatch_header),
            std::move (dispatch_context), std::move (dispatch_payload));
      },
      std::move (completion), std::move (started), std::move (cancelled));
}

result_t<void> stream_runtime_t::dispatch_disconnected (packet_stream_session_t &session,
                                                        stream_t &stream) const
{
    stream._state->closed.store (true, std::memory_order_release);
    return dispatch_serial (stream, "disconnected",
                            [&] { return session.on_disconnected (stream); });
}

result_t<void> stream_runtime_t::dispatch_disconnected_async (
  packet_stream_session_t &session,
  stream_t &stream,
  async_dispatch_completion_t completion) const
{
    auto dispatch_stream = stream;
    return dispatch_serial_async (
      stream, "disconnected",
      [session = &session, dispatch_stream = std::move (dispatch_stream)] () mutable {
          dispatch_stream._state->closed.store (true, std::memory_order_release);
          return session->on_disconnected (dispatch_stream);
      },
      std::move (completion));
}

void stream_runtime_t::drain_async_dispatch (stream_t &stream) const
{
    stream_session_dispatcher_t (*stream._state).drain_async ();
}

void stream_runtime_t::mark_disconnected (stream_t &stream) const
{
    stream._state->closed.store (true, std::memory_order_release);
    const std::lock_guard<std::mutex> lock (stream._state->transport_writer_mutex);
    stream._state->transport_writer = {};
}

result_t<void> stream_runtime_t::dispatch_error (packet_stream_session_t &session,
                                                 stream_t &stream,
                                                 const stream_error_t &error) const
{
    return dispatch_serial (stream, "error", [&] { return session.on_error (stream, error); });
}

void stream_runtime_t::attach_transport_writer (
  stream_t &stream,
  std::function<result_t<void> (const stream_header_t &, const zlink::message_t &)> writer) const
{
    stream._state->transport_writer = std::move (writer);
}

std::vector<std::string> stream_runtime_t::serial_log (const stream_t &stream) const
{
    const std::lock_guard<std::mutex> lock (stream._state->state_mutex);
    return {stream._state->serial_log.begin (), stream._state->serial_log.end ()};
}

std::vector<stream_header_t> stream_runtime_t::written_headers (const stream_t &stream) const
{
    const std::lock_guard<std::mutex> lock (stream._state->state_mutex);
    return stream._state->written_headers;
}

std::vector<zlink::message_t> stream_runtime_t::written_payloads (const stream_t &stream) const
{
    const std::lock_guard<std::mutex> lock (stream._state->state_mutex);
    return stream._state->written_payloads;
}

} // namespace zlink::framework::detail
