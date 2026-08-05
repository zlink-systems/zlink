/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/stream_connector.hpp>
#include <zlink/stream_connector/codecs/auto_codec.hpp>
#include <zlink/stream_e2e_client.hpp>
#include <zlink/stream_e2e_client/codecs/auto_codec.hpp>
#include <zlink/Contracts/Sockets/stream_socket.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>

#include "runtime/connector_runtime.hpp"
#include "runtime/protocol/compression/lz4_compression_codec.hpp"
#include "runtime/protocol/framing/frame_codec.hpp"
#include "runtime/protocol/header_codec.hpp"
#include "runtime/protocol/metadata_codec.hpp"
#include "runtime/protocol/packet_name_resolver.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#ifdef ZLINK_STREAM_CONNECTOR_TEST_WITH_OPENSSL
#include <boost/asio/ssl/stream.hpp>
#include <openssl/crypto.h>
#endif

#include <chrono>
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <nlohmann/json.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <typeindex>
#include <vector>

static_assert (
  std::is_same_v<decltype (std::declval<zlink::stream_connector::connector_t &> ().wait_for (
                   "packet", std::chrono::milliseconds (1))),
                 zlink::stream_connector::result_t<zlink::stream_connector::packet_t>>);
static_assert (std::is_same_v<
               decltype (std::declval<zlink::stream_connector::connector_t &> ()
                           .expect_none<zlink::stream_connector::packet_t> ("packet")
                           .within (std::chrono::milliseconds (1))
                           .submit ()),
               zlink::stream_connector::result_t<void>>);
static_assert (std::is_same_v<
               decltype (std::declval<zlink::stream_connector::connector_t &> ()
                           .wait_for_sequence<zlink::stream_connector::packet_t> ("packet")
                           .expect ([] (const zlink::stream_connector::packet_t &) { return true; })
                           .timeout (std::chrono::milliseconds (1))
                           .submit ()),
               zlink::stream_connector::result_t<
                 std::vector<zlink::stream_connector::packet_t>>>);
namespace
{

#ifdef ZLINK_STREAM_CONNECTOR_TEST_WITH_OPENSSL
struct openssl_thread_cleanup_t
{
    ~openssl_thread_cleanup_t () { OPENSSL_thread_stop (); }
};
#endif

class prefix_compression_codec_t final : public zlink::stream_connector::compression_codec_t
{
  public:
    explicit prefix_compression_codec_t (std::string prefix) : _prefix (std::move (prefix)) {}

    zlink::message_t compress (const zlink::message_t &payload) const override
    {
        return zlink::message_t::from (_prefix + ":" + payload.to_string ());
    }

    zlink::message_t decompress (const zlink::message_t &payload, std::size_t) const override
    {
        const auto value = payload.to_string ();
        const auto marker = _prefix + ":";
        if (value.rfind (marker, 0) != 0) {
            throw std::runtime_error ("unexpected compression marker");
        }
        return zlink::message_t::from (value.substr (marker.size ()));
    }

  private:
    std::string _prefix;
};

class async_write_connection_t final : public zlink::stream_connector::detail::stream_connection_t
{
  public:
    explicit async_write_connection_t (bool fail_write = false) : _fail_write (fail_write) {}

    bool is_open () const override { return _open; }

    std::size_t available (boost::system::error_code &error) override
    {
        error.clear ();
        return 0;
    }

    std::size_t read_some (std::uint8_t *, std::size_t, boost::system::error_code &error) override
    {
        error = boost::asio::error::would_block;
        return 0;
    }

    void async_read_some (std::size_t,
                          std::function<void (boost::system::error_code, std::vector<std::uint8_t>)>
                            completion) override
    {
        read_completion = std::move (completion);
    }

    void write (const std::vector<std::uint8_t> &bytes) override { written.push_back (bytes); }

    void async_write (std::vector<std::uint8_t> bytes,
                      std::function<void (boost::system::error_code)> completion) override
    {
        written.push_back (std::move (bytes));
        completion (_fail_write ? boost::asio::error::operation_aborted
                                : boost::system::error_code{});
    }

    void shutdown_and_close () override
    {
        _open = false;
        read_completion = {};
    }

    void close (boost::system::error_code &error) override
    {
        _open = false;
        read_completion = {};
        error.clear ();
    }

    std::vector<std::vector<std::uint8_t>> written;
    std::function<void (boost::system::error_code, std::vector<std::uint8_t>)> read_completion;

  private:
    bool _open = true;
    bool _fail_write = false;
};

class early_reply_connection_t final : public zlink::stream_connector::detail::stream_connection_t
{
  public:
    explicit early_reply_connection_t (std::vector<std::uint8_t> reply) : _reply (std::move (reply))
    {
    }

    bool is_open () const override { return _open; }

    std::size_t available (boost::system::error_code &error) override
    {
        error.clear ();
        return _reply.empty () ? 0 : _reply.size ();
    }

    std::size_t read_some (std::uint8_t *, std::size_t, boost::system::error_code &error) override
    {
        error = boost::asio::error::would_block;
        return 0;
    }

    void async_read_some (std::size_t,
                          std::function<void (boost::system::error_code, std::vector<std::uint8_t>)>
                            completion) override
    {
        // A real socket keeps an idle read pending. Completing exhausted reads
        // inline with empty data re-enters the read pump on the caller stack
        // until it overflows, so hold the completion once the scripted reply
        // has been consumed.
        if (_reply.empty ()) {
            _pending_read = std::move (completion);
            return;
        }
        auto reply = std::move (_reply);
        _reply.clear ();
        completion (boost::system::error_code{}, std::move (reply));
    }

    void write (const std::vector<std::uint8_t> &bytes) override { written.push_back (bytes); }

    void async_write (std::vector<std::uint8_t> bytes,
                      std::function<void (boost::system::error_code)> completion) override
    {
        written.push_back (std::move (bytes));
        write_completion = std::move (completion);
    }

    void complete_write ()
    {
        if (write_completion) {
            auto completion = std::move (write_completion);
            completion (boost::system::error_code{});
        }
    }

    void shutdown_and_close () override
    {
        _open = false;
        _pending_read = {};
        write_completion = {};
    }

    void close (boost::system::error_code &error) override
    {
        _open = false;
        _pending_read = {};
        write_completion = {};
        error.clear ();
    }

    std::vector<std::vector<std::uint8_t>> written;
    std::function<void (boost::system::error_code)> write_completion;

  private:
    bool _open = true;
    std::vector<std::uint8_t> _reply;
    std::function<void (boost::system::error_code, std::vector<std::uint8_t>)> _pending_read;
};

class oversized_compression_codec_t final : public zlink::stream_connector::compression_codec_t
{
  public:
    zlink::message_t compress (const zlink::message_t &payload) const override { return payload; }

    zlink::message_t decompress (const zlink::message_t &,
                                 std::size_t max_decompressed_size) const override
    {
        return zlink::message_t::from (std::string (max_decompressed_size + 1, 'x'));
    }
};

class callback_latch_t
{
  public:
    void signal ()
    {
        {
            std::lock_guard<std::mutex> lock (_mutex);
            _ready = true;
        }
        _changed.notify_all ();
    }

    bool wait_for (std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock (_mutex);
        return _changed.wait_for (lock, timeout, [this] { return _ready; });
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    bool _ready = false;
};

class joining_thread_t
{
  public:
    joining_thread_t () = default;

    template <typename F, typename... Args>
    explicit joining_thread_t (F &&function, Args &&...args) :
        _thread (std::forward<F> (function), std::forward<Args> (args)...)
    {
    }

    joining_thread_t (joining_thread_t &&) noexcept = default;
    joining_thread_t &operator= (joining_thread_t &&) noexcept = default;

    joining_thread_t (const joining_thread_t &) = delete;
    joining_thread_t &operator= (const joining_thread_t &) = delete;

    ~joining_thread_t ()
    {
        if (_thread.joinable ()) {
            _thread.join ();
        }
    }

    void join ()
    {
        if (_thread.joinable ()) {
            _thread.join ();
        }
    }

  private:
    std::thread _thread;
};

bool dispatch_until (zlink::stream_connector::connector_t &connector,
                     callback_latch_t &latch,
                     std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    do {
        connector.dispatch ();
        if (latch.wait_for (std::chrono::milliseconds (1))) {
            return true;
        }
    } while (std::chrono::steady_clock::now () < deadline);
    connector.dispatch ();
    return latch.wait_for (std::chrono::milliseconds (0));
}

struct login_request_t
{
    static constexpr const char *packet_name = "LoginRequest";
};

struct login_reply_t
{
};

struct auto_payload_t
{
    static constexpr const char *packet_name = "AutoPayload";
    std::string text;
};

static_assert (std::is_same_v<decltype (std::declval<zlink::stream_connector::connector_t &> ()
                                          .wait_for<auto_payload_t> ()),
                              zlink::stream_connector::wait_call_t<auto_payload_t>>);
template <typename T> concept has_core_async_terminator = requires (T value)
{
    value.async ();
};
static_assert (!has_core_async_terminator<zlink::stream_connector::send_call_t>);
static_assert (!has_core_async_terminator<zlink::stream_connector::request_call_t>);
static_assert (!has_core_async_terminator<zlink::stream_connector::wait_call_t<auto_payload_t>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::stream_e2e_client::coroutine_connector_t &> ()
                             .wait_for<auto_payload_t> ()
                             .async ()),
                 zlink::stream_e2e_client::task_t<auto_payload_t>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::stream_e2e_client::coroutine_connector_t &> ()
                             .expect_none<auto_payload_t> ()
                             .within (std::chrono::milliseconds (1))
                             .async ()),
                 zlink::stream_e2e_client::task_t<void>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::stream_e2e_client::coroutine_connector_t &> ()
                             .wait_for_sequence<auto_payload_t> ()
                             .expect ([] (const auto_payload_t &) { return true; })
                             .async ()),
                 zlink::stream_e2e_client::task_t<std::vector<auto_payload_t>>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::stream_e2e_client::coroutine_connector_t &> ()
                             .wait_for<auto_payload_t> ()
                             .to_future ()),
                 std::future<auto_payload_t>>);
static_assert (
  std::is_same_v<decltype (std::declval<zlink::stream_e2e_client::coroutine_connector_t &> ()
                             .request (std::declval<auto_payload_t> ())
                             .async<auto_payload_t> ()),
                 zlink::stream_e2e_client::task_t<auto_payload_t>>);

void to_json (nlohmann::json &json, const auto_payload_t &payload)
{
    json = nlohmann::json{{"text", payload.text}};
}

void to_json (nlohmann::json &json, const login_request_t &)
{
    json = nlohmann::json::object ();
}

void from_json (const nlohmann::json &, login_reply_t &)
{
}

void from_json (const nlohmann::json &json, auto_payload_t &payload)
{
    payload.text = json.at ("text").get<std::string> ();
}

zlink::message_t to_stream_payload (const auto_payload_t &payload)
{
    return zlink::message_t::from_json (payload);
}

void from_stream_payload (const zlink::message_t &payload, auto_payload_t &message)
{
    message = payload.parse_json<auto_payload_t> ();
}

zlink::stream_e2e_client::task_t<void>
send_with_coroutine_submit (zlink::stream_e2e_client::coroutine_connector_t &connector)
{
    connector.send (login_request_t{}).packet_name ("coroutine.send").submit ();
    co_return;
}

zlink::stream_e2e_client::task_t<login_reply_t>
request_with_coroutine_submit (zlink::stream_e2e_client::coroutine_connector_t &connector)
{
    auto reply = co_await connector.request (login_request_t{})
                   .packet_name ("coroutine.request")
                   .timeout (std::chrono::milliseconds (100))
                   .async<login_reply_t> ();
    co_return reply;
}

zlink::stream_e2e_client::task_t<int> delayed_coroutine_child ()
{
    auto value = co_await zlink::stream_e2e_client::task_t<int> (
      [] (std::function<void (zlink::stream_connector::result_t<int>)> callback) {
          std::thread ([callback = std::move (callback)] () mutable {
              callback (zlink::stream_connector::result_t<int>::success (41));
          }).detach ();
      });
    co_return value;
}

zlink::stream_e2e_client::task_t<int> await_delayed_coroutine_child ()
{
    auto value = co_await delayed_coroutine_child ();
    co_return value + 1;
}

zlink::stream_e2e_client::task_t<bool>
result_waits_for_coroutine_frame_cleanup (std::atomic_bool &cleaned)
{
    co_await zlink::stream_e2e_client::task_t<void> (
      [] (std::function<void (zlink::stream_connector::result_t<void>)> callback) {
          std::thread ([callback = std::move (callback)] () mutable {
              callback (zlink::stream_connector::result_t<void>::success ());
          }).detach ();
      });
    struct cleanup_marker_t
    {
        std::atomic_bool *cleaned;
        ~cleanup_marker_t ()
        {
            std::this_thread::sleep_for (std::chrono::milliseconds (20));
            cleaned->store (true);
        }
    } cleanup_marker{&cleaned};
    co_return true;
}

struct server_frame_t
{
    zlink::stream_connector::detail::stream_header_t header;
    std::string payload;
    bool compressed = false;
};

std::optional<server_frame_t> try_read_server_frame (std::string &buffer)
{
    if (buffer.size () < 6) {
        return std::nullopt;
    }
    const auto header_size =
      (static_cast<std::uint8_t> (buffer[0]) << 8) | static_cast<std::uint8_t> (buffer[1]);
    const auto payload_size =
      (static_cast<std::size_t> (static_cast<std::uint8_t> (buffer[2])) << 24)
      | (static_cast<std::size_t> (static_cast<std::uint8_t> (buffer[3])) << 16)
      | (static_cast<std::size_t> (static_cast<std::uint8_t> (buffer[4])) << 8)
      | static_cast<std::uint8_t> (buffer[5]);
    if (buffer.size () < 6 + header_size + payload_size) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> header_bytes (
      buffer.begin () + 6, buffer.begin () + 6 + static_cast<std::ptrdiff_t> (header_size));
    auto decoded = zlink::stream_connector::detail::header_codec_t{}.decode (header_bytes);
    if (!decoded) {
        return std::nullopt;
    }
    std::string payload (buffer.begin () + 6 + static_cast<std::ptrdiff_t> (header_size),
                         buffer.begin () + 6 + static_cast<std::ptrdiff_t> (header_size)
                           + static_cast<std::ptrdiff_t> (payload_size));
    buffer.erase (0, 6 + header_size + payload_size);
    const bool compressed =
      (static_cast<std::uint8_t> (decoded.value ().flags)
       & static_cast<std::uint8_t> (zlink::stream_connector::header_flags_t::payload_compressed))
      != 0;
    if (compressed) {
        payload = zlink::stream_connector::detail::lz4_compression_codec_t{}
                    .decompress (zlink::message_t::from (payload), 64 * 1024)
                    .to_string ();
    }
    return server_frame_t{decoded.value (), std::move (payload), compressed};
}

std::optional<server_frame_t> try_read_server_frame_raw (std::string &buffer)
{
    if (buffer.size () < 6) {
        return std::nullopt;
    }
    const auto header_size =
      (static_cast<std::uint8_t> (buffer[0]) << 8) | static_cast<std::uint8_t> (buffer[1]);
    const auto payload_size =
      (static_cast<std::size_t> (static_cast<std::uint8_t> (buffer[2])) << 24)
      | (static_cast<std::size_t> (static_cast<std::uint8_t> (buffer[3])) << 16)
      | (static_cast<std::size_t> (static_cast<std::uint8_t> (buffer[4])) << 8)
      | static_cast<std::uint8_t> (buffer[5]);
    if (buffer.size () < 6 + header_size + payload_size) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> header_bytes (
      buffer.begin () + 6, buffer.begin () + 6 + static_cast<std::ptrdiff_t> (header_size));
    auto decoded = zlink::stream_connector::detail::header_codec_t{}.decode (header_bytes);
    if (!decoded) {
        return std::nullopt;
    }
    std::string payload (buffer.begin () + 6 + static_cast<std::ptrdiff_t> (header_size),
                         buffer.begin () + 6 + static_cast<std::ptrdiff_t> (header_size)
                           + static_cast<std::ptrdiff_t> (payload_size));
    buffer.erase (0, 6 + header_size + payload_size);
    const bool compressed =
      (static_cast<std::uint8_t> (decoded.value ().flags)
       & static_cast<std::uint8_t> (zlink::stream_connector::header_flags_t::payload_compressed))
      != 0;
    return server_frame_t{decoded.value (), std::move (payload), compressed};
}

zlink::message_t make_server_frame (zlink::stream_connector::message_kind_t kind,
                                    std::uint64_t seq,
                                    std::string name,
                                    std::string payload,
                                    bool compressed = false)
{
    const bool legacy_named_reply =
      (kind == zlink::stream_connector::message_kind_t::response
       || kind == zlink::stream_connector::message_kind_t::error)
      && !name.empty ();
    zlink::stream_connector::detail::stream_header_t header;
    header.kind = kind;
    header.codec = kind == zlink::stream_connector::message_kind_t::error
                     ? zlink::stream_connector::codec_t::json
                     : zlink::stream_connector::codec_t::raw;
    header.flags = compressed ? zlink::stream_connector::header_flags_t::payload_compressed
                              : zlink::stream_connector::header_flags_t::none;
    header.request_seq = kind == zlink::stream_connector::message_kind_t::request
                             || kind == zlink::stream_connector::message_kind_t::response
                             || kind == zlink::stream_connector::message_kind_t::error
                           ? std::optional<std::uint64_t>{seq}
                           : std::optional<std::uint64_t>{};
    header.name = legacy_named_reply ? std::string{} : name;
    auto header_bytes = zlink::stream_connector::detail::header_codec_t{}.encode (header);
    if (legacy_named_reply) {
        auto &bytes = header_bytes.value ();
        bytes[12] = static_cast<std::uint8_t> (name.size ());
        bytes.insert (bytes.begin () + 13, name.begin (), name.end ());
    }
    zlink::stream_connector::connector_options_t options;
    options.compression = zlink::stream_connector::compression_t::lz4;
    if (compressed) {
        payload = zlink::stream_connector::detail::lz4_compression_codec_t{}
                    .compress (zlink::message_t::from (payload))
                    .to_string ();
    }
    std::vector<std::uint8_t> payload_bytes (payload.begin (), payload.end ());
    options.max_send_payload_size = std::max (options.max_send_payload_size, payload_bytes.size ());
    auto frame = zlink::stream_connector::detail::frame_codec_t::encode (header_bytes.value (),
                                                                         payload_bytes, options);
    return zlink::message_t::from (std::string (frame.value ().begin (), frame.value ().end ()));
}

zlink::message_t make_frame_prefix (std::size_t header_size, std::size_t payload_size)
{
    auto prefix =
      zlink::stream_connector::detail::frame_codec_t::encode_prefix (header_size, payload_size);
    return zlink::message_t::from (std::string (prefix.value ().begin (), prefix.value ().end ()));
}

} // namespace

int main ()
{
    using zlink::stream_connector::codec_t;
    using zlink::stream_connector::header_flags_t;
    using zlink::stream_connector::message_kind_t;
    using zlink::stream_connector::detail::header_codec_t;
    using zlink::stream_connector::detail::stream_header_t;

    for (const auto kind : {message_kind_t::response, message_kind_t::error}) {
        stream_header_t reply;
        reply.kind = kind;
        reply.codec = kind == message_kind_t::error ? codec_t::json : codec_t::raw;
        reply.flags = header_flags_t::has_request_seq;
        reply.request_seq = 7;
        const auto encoded = header_codec_t{}.encode (reply);
        if (!encoded || encoded.value ().size () < 13 || encoded.value ()[12] != 0
            || !header_codec_t{}.decode (encoded.value ())) {
            return 200;
        }
        reply.name = "forbidden.reply.name";
        if (header_codec_t{}.encode (reply)) {
            return 201;
        }
    }

    {
        zlink::stream_connector::codec_registry_t codecs;
        if (!codecs.supports (zlink::stream_connector::codec_t::raw)) {
            return 142;
        }
        const auto message_pack_was_enabled =
          codecs.supports (zlink::stream_connector::codec_t::message_pack);
        bool default_rejected = false;
        if (!message_pack_was_enabled) {
            try {
                codecs.use_default_codec (zlink::stream_connector::codec_t::message_pack);
            }
            catch (const std::invalid_argument &) {
                default_rejected = true;
            }
        }
        codecs.enable_codec (zlink::stream_connector::codec_t::message_pack)
          .use_default_codec (zlink::stream_connector::codec_t::message_pack);
        zlink::stream_connector::codec_registry_t moved_codecs (std::move (codecs));
        zlink::stream_connector::codec_registry_t assigned_codecs;
        assigned_codecs = std::move (moved_codecs);
        if ((!message_pack_was_enabled && !default_rejected)
            || !assigned_codecs.supports (zlink::stream_connector::codec_t::message_pack)) {
            return 143;
        }
    }

    {
        zlink::stream_connector::connector_t default_connector;
        bool disconnected_callback_registered = false;
        default_connector.on_disconnected ([&] { disconnected_callback_registered = true; });
        if (default_connector.is_connected () || default_connector.pending_dispatch_count () != 0) {
            return 144;
        }
        (void) disconnected_callback_registered;
        zlink::stream_connector::metadata_t metadata;
        metadata.with ("trace", "unbound");
        zlink::stream_connector::send_call_t unbound_send;
        unbound_send.metadata (metadata).codec (zlink::stream_connector::codec_t::raw).submit ();
    }

    {
        auto state = std::make_shared<zlink::stream_connector::detail::connector_state_t> (
          zlink::stream_connector::connector_options_t{});
        auto connection = std::make_shared<async_write_connection_t> ();
        bool callback_called = false;
        callback_latch_t callback_latch;
        {
            std::lock_guard<std::mutex> lock (state->transport_mutex);
            state->connection = connection;
            state->pending_writes.push_back (
              zlink::stream_connector::detail::pending_write_t{
                std::vector<std::uint8_t>{'f', 'r', 'a', 'm', 'e'},
                [&callback_called, &callback_latch] (
                  zlink::stream_connector::result_t<void> result) {
                    callback_called = static_cast<bool> (result);
                    callback_latch.signal ();
                }});
        }
        zlink::stream_connector::detail::change_state (
          state, zlink::stream_connector::connection_state_t::connected);
        zlink::stream_connector::detail::resume_pending_writes_after_connect (state);
        if (!callback_latch.wait_for (std::chrono::seconds (1))) {
            return 146;
        }
        if (!callback_called || connection->written.size () != 1
            || std::string (connection->written[0].begin (), connection->written[0].end ())
                 != "frame"
            || !state->pending_writes.empty () || state->write_in_progress) {
            return 145;
        }
    }

    {
        auto nested = await_delayed_coroutine_child ().result ();
        if (!nested || nested.value () != 42) {
            return 76;
        }
    }
    {
        std::atomic_bool coroutine_frame_cleaned{false};
        auto cleanup_result =
          result_waits_for_coroutine_frame_cleanup (coroutine_frame_cleaned).result ();
        if (!cleanup_result || !cleanup_result.value () || !coroutine_frame_cleaned.load ()) {
            return 108;
        }
    }

    {
        zlink::stream_connector::detail::packet_name_resolver_t resolver;
        if (resolver.resolve (std::type_index (typeid (login_request_t)), "explicit.name")
              != "explicit.name"
            || resolver.resolve (std::type_index (typeid (login_request_t))).empty ()) {
            return 69;
        }
    }

    {
        zlink::stream_connector::detail::metadata_codec_t metadata_codec;
        zlink::stream_connector::metadata_t metadata;
        metadata.with ("trace", "abc");
        auto encoded_metadata = metadata_codec.encode (metadata);
        auto decoded_metadata =
          encoded_metadata
            ? metadata_codec.decode (encoded_metadata.value ())
            : zlink::stream_connector::result_t<zlink::stream_connector::metadata_t>::failure (
                zlink::stream_connector::error_code_t::validation_failed, "metadata encode failed");
        const auto empty_value_metadata = metadata_codec.decode ({1, 1, 'k', 0, 0});
        if (!decoded_metadata || decoded_metadata.value ().values.at ("trace") != "abc"
            || !empty_value_metadata || empty_value_metadata.value ().values.at ("k") != ""
            || metadata_codec.decode ({}).error_code ()
                 != zlink::stream_connector::error_code_t::frame_decode_failed
            || metadata_codec.decode ({1}).error_code ()
                 != zlink::stream_connector::error_code_t::frame_decode_failed
            || metadata_codec.decode ({1, 0}).error_code ()
                 != zlink::stream_connector::error_code_t::frame_decode_failed
            || metadata_codec.decode ({1, 1, 'k'}).error_code ()
                 != zlink::stream_connector::error_code_t::frame_decode_failed
            || metadata_codec.decode ({1, 1, 'k', 0, 1, 'v', 1}).error_code ()
                 != zlink::stream_connector::error_code_t::frame_decode_failed) {
            return 70;
        }
        zlink::stream_connector::metadata_t invalid_metadata;
        invalid_metadata.with ("", "value");
        if (metadata_codec.encode (invalid_metadata).error_code ()
            != zlink::stream_connector::error_code_t::validation_failed) {
            return 71;
        }
        zlink::stream_connector::metadata_t too_large_metadata;
        too_large_metadata.with ("key", std::string (65536, 'v'));
        if (metadata_codec.encode (too_large_metadata).error_code ()
            != zlink::stream_connector::error_code_t::validation_failed) {
            return 72;
        }
        zlink::stream_connector::metadata_t boundary_metadata;
        boundary_metadata.with ("k", std::string (1019, 'v'));
        zlink::stream_connector::metadata_t over_limit_metadata;
        over_limit_metadata.with ("k", std::string (1020, 'v'));
        if (!metadata_codec.encode (boundary_metadata)
            || metadata_codec.encode (over_limit_metadata).error_code ()
                 != zlink::stream_connector::error_code_t::validation_failed) {
            return 176;
        }
    }

    {
        zlink::stream_connector::connector_options_t frame_options;
        frame_options.max_send_payload_size = 2;
        frame_options.max_receive_payload_size = 4;
        const auto oversized_header =
          static_cast<std::size_t> (std::numeric_limits<std::uint16_t>::max ()) + 1;
        if (zlink::stream_connector::detail::frame_codec_t::validate_frame_size (
              oversized_header, 1, frame_options)
            || zlink::stream_connector::detail::frame_codec_t::validate_frame_size (1, 3,
                                                                                    frame_options)
            || zlink::stream_connector::detail::frame_codec_t::validate_receive_frame_size (
              oversized_header, 1, frame_options)
            || zlink::stream_connector::detail::frame_codec_t::validate_receive_frame_size (
              1, 5, frame_options)
            || zlink::stream_connector::detail::frame_codec_t::encode_prefix (
                 static_cast<std::size_t> (std::numeric_limits<std::uint16_t>::max ()) + 1, 0)
                   .error_code ()
                 != zlink::stream_connector::error_code_t::frame_too_large
            || zlink::stream_connector::detail::frame_codec_t::encode_prefix (
                 0, static_cast<std::size_t> (std::numeric_limits<std::uint32_t>::max ()) + 1)
                   .error_code ()
                 != zlink::stream_connector::error_code_t::frame_too_large) {
            return 73;
        }
    }

    {
        zlink::stream_connector::detail::lz4_compression_codec_t lz4;
        if (!lz4.available ()) {
            return 19;
        }
        const auto source =
          zlink::message_t::from (std::string ("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
        const auto compressed = lz4.compress (source);
        const auto restored = lz4.decompress (compressed, source.size ());
        if (restored.to_string () != source.to_string ()
            || compressed.to_string () == source.to_string ()) {
            return 20;
        }
        /* LZ4 pickle frame (the cross-language wire): header 0xC0 = 4-byte
         * size difference, so the declared decompressed size is the body plus
         * the difference — here far past the receive limit. */
        const std::string oversized_declared_size{
          static_cast<char> (0xC0), static_cast<char> (0x40), static_cast<char> (0x42),
          static_cast<char> (0x0f), static_cast<char> (0x00), static_cast<char> (0x01)};
        bool oversized_rejected = false;
        try {
            (void) lz4.decompress (zlink::message_t::from (oversized_declared_size), 1024);
        }
        catch (const std::runtime_error &) {
            oversized_rejected = true;
        }
        if (!oversized_rejected) {
            return 141;
        }
    }

    {
        zlink::stream_connector::detail::header_codec_t header_codec;
        zlink::stream_connector::detail::stream_header_t header;
        header.kind = zlink::stream_connector::message_kind_t::request;
        header.codec = zlink::stream_connector::codec_t::json;
        header.request_seq = 42;
        header.name = "profile.get";
        header.metadata.with ("traceId", "abc");
        auto encoded = header_codec.encode (header);
        if (!encoded) {
            return 21;
        }
        auto decoded = header_codec.decode (encoded.value ());
        if (!decoded || decoded.value ().kind != zlink::stream_connector::message_kind_t::request
            || decoded.value ().codec != zlink::stream_connector::codec_t::json
            || decoded.value ().request_seq.value_or (0) != 42
            || decoded.value ().name != "profile.get"
            || decoded.value ().metadata.values.at ("traceId") != "abc") {
            return 22;
        }
        const std::vector<std::uint8_t> invalid_flag{1, 1, 0x80, 1,
                                                     static_cast<std::uint8_t> ('x')};
        if (header_codec.decode (invalid_flag).error_code ()
            != zlink::stream_connector::error_code_t::frame_decode_failed) {
            return 23;
        }
        zlink::stream_connector::detail::stream_header_t control;
        control.kind = zlink::stream_connector::message_kind_t::control;
        control.codec = zlink::stream_connector::codec_t::raw;
        control.name = "$zlink.heartbeat.ping";
        if (!header_codec.encode (control)) {
            return 24;
        }
        control.codec = zlink::stream_connector::codec_t::json;
        if (header_codec.encode (control)
            || header_codec.encode (control).error_code ()
                 != zlink::stream_connector::error_code_t::frame_decode_failed) {
            return 25;
        }
        zlink::stream_connector::detail::stream_header_t application_name;
        application_name.kind = zlink::stream_connector::message_kind_t::send;
        application_name.codec = zlink::stream_connector::codec_t::raw;
        application_name.name = "$application.event";
        if (!header_codec.encode (application_name)) {
            return 177;
        }
        application_name.name = "$zlink.private";
        if (header_codec.encode (application_name).error_code ()
            != zlink::stream_connector::error_code_t::frame_decode_failed) {
            return 178;
        }
        zlink::stream_connector::connector_options_t frame_options;
        frame_options.max_send_payload_size = 16;
        auto frame = zlink::stream_connector::detail::frame_codec_t::encode (
          encoded.value (), std::vector<std::uint8_t>{'o', 'k'}, frame_options);
        if (!frame || frame.value ().size () != encoded.value ().size () + 8
            || frame.value ()[0] != 0 || frame.value ()[1] != encoded.value ().size ()
            || frame.value ()[2] != 0 || frame.value ()[3] != 0 || frame.value ()[4] != 0
            || frame.value ()[5] != 2) {
            return 26;
        }
    }

    zlink::context_t context;
    zlink::stream_socket_t server (context);
    server.options ().notify (false);
    server.bind ("tcp://127.0.0.1:0");
    const auto endpoint = server.options ().last_endpoint ();
    zlink::stream_connector::connector_options_t default_options;
    if (default_options.compression != zlink::stream_connector::compression_t::lz4
        || !default_options.compression_codec) {
        return 67;
    }
    std::atomic_bool compressed_send_seen{false};
    std::atomic_bool compressible_large_send_seen{false};
    std::atomic_bool uncompressed_send_seen{false};
    joining_thread_t server_thread ([&server, &compressed_send_seen, &compressible_large_send_seen,
                                     &uncompressed_send_seen] {
        int handled = 0;
        std::string buffer;
        while (handled < 4) {
            zlink::received_t inbound;
            if (server.recv (inbound) != 0) {
                return;
            }
            buffer += inbound.parts ().empty () ? std::string{} : inbound.parts ()[0].to_string ();
            while (auto frame = try_read_server_frame (buffer)) {
                if (frame->header.kind == zlink::stream_connector::message_kind_t::send
                    && frame->compressed && frame->payload == "{}") {
                    compressed_send_seen = true;
                }
                if (frame->header.kind == zlink::stream_connector::message_kind_t::send
                    && frame->compressed && frame->header.name == "compressible.large"
                    && frame->payload == std::string (128, 'a')) {
                    compressible_large_send_seen = true;
                }
                if (frame->header.kind == zlink::stream_connector::message_kind_t::send
                    && !frame->compressed && frame->header.name == "login.uncompressed"
                    && frame->payload == "{}") {
                    uncompressed_send_seen = true;
                }
                if (frame->header.kind == zlink::stream_connector::message_kind_t::request) {
                    auto push = make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                                   "server.compressed", "server-payload", true);
                    inbound.send ().message (push).submit ();
                    auto reply =
                      make_server_frame (zlink::stream_connector::message_kind_t::response,
                                         frame->header.request_seq.value (), frame->header.name,
                                         "ok");
                    inbound.send ().message (reply).submit ();
                }
                ++handled;
            }
            inbound.close ();
        }
    });

    zlink::stream_connector::connector_options_t options;
    options.endpoint = endpoint;
    options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
    options.max_send_payload_size = 16;
    options.compression = zlink::stream_connector::compression_t::lz4;

    auto connector = zlink::stream_connector::connector_factory_t::create (options);
    if (connector.state () != zlink::stream_connector::connection_state_t::created) {
        return 1;
    }

    auto states = std::make_shared<std::vector<zlink::stream_connector::connection_state_t>> ();
    connector.on_connection_state_changed (
      [states] (const zlink::stream_connector::connection_state_changed_t &state) {
          states->push_back (state.current);
      });

    if (!connector.connect () || !connector.is_connected () || !states->empty ()
        || connector.pending_dispatch_count () != 2 || !connector.dispatch ()
        || states->size () != 2
        || states->back () != zlink::stream_connector::connection_state_t::connected) {
        return 2;
    }

    {
        boost::asio::io_context lifecycle_io;
        boost::asio::ip::tcp::acceptor lifecycle_acceptor (
          lifecycle_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
        const auto lifecycle_endpoint =
          std::string ("tcp://127.0.0.1:")
          + std::to_string (lifecycle_acceptor.local_endpoint ().port ());
        callback_latch_t accepted_latch;
        callback_latch_t release_latch;
        joining_thread_t lifecycle_server (
          [&lifecycle_acceptor, &accepted_latch, &release_latch] {
              boost::asio::ip::tcp::socket socket (lifecycle_acceptor.get_executor ());
              lifecycle_acceptor.accept (socket);
              accepted_latch.signal ();
              release_latch.wait_for (std::chrono::seconds (2));
          });

        zlink::stream_connector::connector_options_t lifecycle_options;
        lifecycle_options.endpoint = lifecycle_endpoint;
        lifecycle_options.reconnect.enabled = false;
        lifecycle_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        auto lifecycle_connector =
          zlink::stream_connector::connector_factory_t::create (lifecycle_options);
        std::atomic_size_t lifecycle_state_count{0};
        callback_latch_t initial_states_delivered;
        lifecycle_connector.on_connection_state_changed (
          [&lifecycle_state_count, &initial_states_delivered] (
            const zlink::stream_connector::connection_state_changed_t &) {
              if (lifecycle_state_count.fetch_add (1) + 1 == 2) {
                  initial_states_delivered.signal ();
              }
          });
        const auto first_connect = lifecycle_connector.connect ();
        const auto initial_states_completed =
          initial_states_delivered.wait_for (std::chrono::seconds (1));
        const auto first_state_count = lifecycle_state_count.load ();
        const auto repeated_connect = lifecycle_connector.connect ();
        const auto repeated_state_count = lifecycle_state_count.load ();
        callback_latch_t repeated_async_latch;
        std::atomic_bool repeated_async_succeeded{false};
        lifecycle_connector.connect (
          [&repeated_async_latch, &repeated_async_succeeded] (
            zlink::stream_connector::result_t<void> result) {
              repeated_async_succeeded.store (static_cast<bool> (result));
              repeated_async_latch.signal ();
          });
        const auto repeated_async_completed =
          repeated_async_latch.wait_for (std::chrono::seconds (1));
        const auto repeated_async_state_count = lifecycle_state_count.load ();
        const auto closed = lifecycle_connector.close ();
        const auto connect_after_close = lifecycle_connector.connect ();
        if (connect_after_close) {
            (void) lifecycle_connector.close ();
        }
        release_latch.signal ();
        lifecycle_server.join ();
        if (!first_connect || !initial_states_completed
            || !accepted_latch.wait_for (std::chrono::seconds (1))
            || !repeated_connect || repeated_state_count != first_state_count || !closed
            || !repeated_async_completed || !repeated_async_succeeded.load ()
            || repeated_async_state_count != first_state_count
            || connect_after_close
            || connect_after_close.error_code () != zlink::stream_connector::error_code_t::closed) {
            return 168;
        }
    }

    {
        boost::asio::io_context timeout_io;
        boost::asio::ip::tcp::acceptor timeout_acceptor (
          timeout_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
        const auto timeout_endpoint =
          std::string ("ws://127.0.0.1:")
          + std::to_string (timeout_acceptor.local_endpoint ().port ()) + "/stream";
        callback_latch_t timeout_server_accepted;
        callback_latch_t timeout_server_release;
        joining_thread_t timeout_server (
          [&timeout_acceptor, &timeout_server_accepted, &timeout_server_release] {
              boost::asio::ip::tcp::socket socket (timeout_acceptor.get_executor ());
              timeout_acceptor.accept (socket);
              timeout_server_accepted.signal ();
              timeout_server_release.wait_for (std::chrono::milliseconds (500));
          });

        zlink::stream_connector::connector_options_t timeout_options;
        timeout_options.endpoint = timeout_endpoint;
        timeout_options.transport = zlink::stream_connector::transport_t::websocket;
        timeout_options.connect_timeout = std::chrono::milliseconds (50);
        timeout_options.reconnect.enabled = false;
        auto timeout_connector =
          zlink::stream_connector::connector_factory_t::create (timeout_options);
        const auto connect_started_at = std::chrono::steady_clock::now ();
        const auto timed_connect = timeout_connector.connect ();
        const auto connect_elapsed = std::chrono::steady_clock::now () - connect_started_at;
        timeout_server_release.signal ();
        timeout_server.join ();
        if (!timeout_server_accepted.wait_for (std::chrono::milliseconds (100)) || timed_connect
            || timed_connect.error_code ()
                 != zlink::stream_connector::error_code_t::connect_timeout
            || connect_elapsed >= std::chrono::milliseconds (250)) {
            return 169;
        }
    }

    {
        boost::asio::io_context timeout_io;
        boost::asio::ip::tcp::acceptor timeout_acceptor (
          timeout_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
        const auto timeout_endpoint =
          std::string ("ws://127.0.0.1:")
          + std::to_string (timeout_acceptor.local_endpoint ().port ()) + "/stream";
        callback_latch_t timeout_server_release;
        joining_thread_t timeout_server ([&timeout_acceptor, &timeout_server_release] {
            boost::asio::ip::tcp::socket socket (timeout_acceptor.get_executor ());
            timeout_acceptor.accept (socket);
            timeout_server_release.wait_for (std::chrono::milliseconds (500));
        });

        zlink::stream_connector::connector_options_t timeout_options;
        timeout_options.endpoint = timeout_endpoint;
        timeout_options.transport = zlink::stream_connector::transport_t::websocket;
        timeout_options.connect_timeout = std::chrono::milliseconds (50);
        timeout_options.reconnect.enabled = false;
        timeout_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        auto timeout_connector =
          zlink::stream_connector::connector_factory_t::create (timeout_options);
        callback_latch_t timeout_completed;
        std::optional<zlink::stream_connector::error_code_t> timeout_error;
        const auto connect_started_at = std::chrono::steady_clock::now ();
        timeout_connector.connect ([&timeout_completed, &timeout_error] (auto result) {
            timeout_error = result.error_code ();
            timeout_completed.signal ();
        });
        const auto callback_arrived =
          timeout_completed.wait_for (std::chrono::milliseconds (250));
        const auto connect_elapsed = std::chrono::steady_clock::now () - connect_started_at;
        timeout_server_release.signal ();
        timeout_server.join ();
        if (!callback_arrived
            || timeout_error != zlink::stream_connector::error_code_t::connect_timeout
            || connect_elapsed >= std::chrono::milliseconds (250)) {
            return 170;
        }
    }

    {
        boost::asio::io_context heartbeat_io;
        boost::asio::ip::tcp::acceptor heartbeat_acceptor (
          heartbeat_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
        const auto heartbeat_endpoint =
          std::string ("tcp://127.0.0.1:")
          + std::to_string (heartbeat_acceptor.local_endpoint ().port ());
        std::atomic_bool heartbeat_seen_without_dispatch{false};
        joining_thread_t heartbeat_server (
          [&heartbeat_acceptor, &heartbeat_seen_without_dispatch] {
              boost::asio::ip::tcp::socket socket (heartbeat_acceptor.get_executor ());
              heartbeat_acceptor.accept (socket);
              socket.non_blocking (true);
              std::string buffer;
              std::array<char, 512> chunk{};
              const auto deadline =
                std::chrono::steady_clock::now () + std::chrono::milliseconds (250);
              while (std::chrono::steady_clock::now () < deadline) {
                  boost::system::error_code error;
                  const auto size = socket.read_some (boost::asio::buffer (chunk), error);
                  if (!error) {
                      buffer.append (chunk.data (), size);
                      if (auto frame = try_read_server_frame (buffer)) {
                          heartbeat_seen_without_dispatch =
                            frame->header.kind
                              == zlink::stream_connector::message_kind_t::control
                            && frame->header.name == "$zlink.heartbeat.ping";
                          break;
                      }
                  } else if (error != boost::asio::error::would_block
                             && error != boost::asio::error::try_again) {
                      break;
                  }
                  std::this_thread::sleep_for (std::chrono::milliseconds (1));
              }
          });

        zlink::stream_connector::connector_options_t heartbeat_options;
        heartbeat_options.endpoint = heartbeat_endpoint;
        heartbeat_options.heartbeat.interval = std::chrono::milliseconds (20);
        heartbeat_options.heartbeat.timeout = std::chrono::milliseconds (500);
        heartbeat_options.reconnect.enabled = false;
        auto heartbeat_connector =
          zlink::stream_connector::connector_factory_t::create (heartbeat_options);
        if (!heartbeat_connector.connect ()) {
            return 171;
        }
        heartbeat_server.join ();
        heartbeat_connector.close ();
        if (!heartbeat_seen_without_dispatch) {
            return 171;
        }
    }

    {
        boost::asio::io_context lifecycle_io;
        boost::asio::ip::tcp::acceptor lifecycle_acceptor (
          lifecycle_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
        const auto lifecycle_endpoint =
          std::string ("tcp://127.0.0.1:")
          + std::to_string (lifecycle_acceptor.local_endpoint ().port ());
        callback_latch_t lifecycle_server_accepted;
        callback_latch_t lifecycle_server_release;
        joining_thread_t lifecycle_server (
          [&lifecycle_acceptor, &lifecycle_server_accepted, &lifecycle_server_release] {
              boost::asio::ip::tcp::socket socket (lifecycle_acceptor.get_executor ());
              lifecycle_acceptor.accept (socket);
              lifecycle_server_accepted.signal ();
              lifecycle_server_release.wait_for (std::chrono::milliseconds (500));
          });

        zlink::stream_connector::connector_options_t lifecycle_options;
        lifecycle_options.endpoint = lifecycle_endpoint;
        lifecycle_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
        lifecycle_options.heartbeat.enabled = false;
        lifecycle_options.reconnect.enabled = false;
        auto lifecycle_connector =
          zlink::stream_connector::connector_factory_t::create (lifecycle_options);
        if (!lifecycle_connector.connect ()
            || !lifecycle_server_accepted.wait_for (std::chrono::milliseconds (100))) {
            lifecycle_server_release.signal ();
            lifecycle_server.join ();
            return 172;
        }

        std::mutex callback_mutex;
        int state_callback_count = 0;
        int error_callback_count = 0;
        int disconnected_callback_count = 0;
        std::vector<std::thread::id> callback_threads;
        lifecycle_connector
          .on_connection_state_changed ([&] (const auto &) {
              std::lock_guard<std::mutex> lock (callback_mutex);
              ++state_callback_count;
              callback_threads.push_back (std::this_thread::get_id ());
          })
          .on_error ([&] (const auto &) {
              std::lock_guard<std::mutex> lock (callback_mutex);
              ++error_callback_count;
              callback_threads.push_back (std::this_thread::get_id ());
          })
          .on_disconnected ([&] {
              std::lock_guard<std::mutex> lock (callback_mutex);
              ++disconnected_callback_count;
              callback_threads.push_back (std::this_thread::get_id ());
          });
        lifecycle_server_release.signal ();
        lifecycle_server.join ();
        const auto disconnected_deadline =
          std::chrono::steady_clock::now () + std::chrono::milliseconds (250);
        while (lifecycle_connector.state ()
                 != zlink::stream_connector::connection_state_t::disconnected
               && std::chrono::steady_clock::now () < disconnected_deadline) {
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        bool callbacks_ran_before_dispatch = false;
        {
            std::lock_guard<std::mutex> lock (callback_mutex);
            callbacks_ran_before_dispatch = state_callback_count != 0 || error_callback_count != 0
                                            || disconnected_callback_count != 0;
        }
        if (callbacks_ran_before_dispatch) {
            lifecycle_connector.close ();
            return 173;
        }
        if (lifecycle_connector.pending_dispatch_count () == 0
            || !lifecycle_connector.dispatch ()) {
            lifecycle_connector.close ();
            return 174;
        }
        bool callbacks_invalid = false;
        {
            std::lock_guard<std::mutex> lock (callback_mutex);
            const auto dispatch_thread = std::this_thread::get_id ();
            callbacks_invalid =
              state_callback_count != 1 || error_callback_count != 1
              || disconnected_callback_count != 1 || callback_threads.size () != 3
              || std::any_of (callback_threads.begin (), callback_threads.end (),
                              [dispatch_thread] (auto thread) {
                                  return thread != dispatch_thread;
                              });
        }
        if (callbacks_invalid) {
            lifecycle_connector.close ();
            return 175;
        }
        lifecycle_connector.close ();
    }

    {
        boost::asio::io_context reconnect_io;
        boost::asio::ip::tcp::acceptor reconnect_acceptor (
          reconnect_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
        const auto reconnect_endpoint =
          std::string ("tcp://127.0.0.1:")
          + std::to_string (reconnect_acceptor.local_endpoint ().port ());
        callback_latch_t first_request_seen;
        callback_latch_t release_first_connection;
        std::atomic_bool second_connection_seen{false};
        std::atomic_bool second_connection_received_fresh_send{false};
        joining_thread_t reconnect_server (
          [&reconnect_acceptor, &first_request_seen, &release_first_connection,
           &second_connection_seen, &second_connection_received_fresh_send] {
              boost::asio::ip::tcp::socket first (reconnect_acceptor.get_executor ());
              reconnect_acceptor.accept (first);
              std::array<char, 512> first_chunk{};
              boost::system::error_code error;
              const auto first_size = first.read_some (boost::asio::buffer (first_chunk), error);
              if (!error) {
                  std::string first_buffer (first_chunk.data (), first_size);
                  if (auto frame = try_read_server_frame (first_buffer);
                      frame && frame->header.kind
                                   == zlink::stream_connector::message_kind_t::request
                      && frame->header.name == "pending.before.reconnect") {
                      first_request_seen.signal ();
                  }
              }
              release_first_connection.wait_for (std::chrono::milliseconds (500));
              first.shutdown (boost::asio::ip::tcp::socket::shutdown_both, error);
              first.close (error);

              reconnect_acceptor.non_blocking (true);
              boost::asio::ip::tcp::socket second (reconnect_acceptor.get_executor ());
              const auto accept_deadline =
                std::chrono::steady_clock::now () + std::chrono::milliseconds (700);
              while (std::chrono::steady_clock::now () < accept_deadline) {
                  reconnect_acceptor.accept (second, error);
                  if (!error) {
                      second_connection_seen = true;
                      break;
                  }
                  if (error != boost::asio::error::would_block
                      && error != boost::asio::error::try_again) {
                      return;
                  }
                  std::this_thread::sleep_for (std::chrono::milliseconds (1));
              }
              if (!second_connection_seen) {
                  return;
              }
              second.non_blocking (true);
              std::string second_buffer;
              std::array<char, 512> second_chunk{};
              const auto read_deadline =
                std::chrono::steady_clock::now () + std::chrono::milliseconds (500);
              while (std::chrono::steady_clock::now () < read_deadline) {
                  const auto size = second.read_some (boost::asio::buffer (second_chunk), error);
                  if (!error) {
                      second_buffer.append (second_chunk.data (), size);
                      if (auto frame = try_read_server_frame (second_buffer)) {
                          second_connection_received_fresh_send =
                            frame->header.kind == zlink::stream_connector::message_kind_t::send
                            && frame->header.name == "fresh.after.reconnect";
                          return;
                      }
                  } else if (error != boost::asio::error::would_block
                             && error != boost::asio::error::try_again) {
                      return;
                  }
                  std::this_thread::sleep_for (std::chrono::milliseconds (1));
              }
          });

        zlink::stream_connector::connector_options_t reconnect_options;
        reconnect_options.endpoint = reconnect_endpoint;
        reconnect_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        reconnect_options.heartbeat.enabled = false;
        reconnect_options.connect_timeout = std::chrono::milliseconds (1000);
        reconnect_options.reconnect.initial_delay = std::chrono::milliseconds (100);
        reconnect_options.reconnect.max_delay = std::chrono::milliseconds (100);
        reconnect_options.reconnect.max_attempts = 3;
        auto reconnect_connector =
          zlink::stream_connector::connector_factory_t::create (reconnect_options);
        std::atomic_bool reconnecting_seen{false};
        reconnect_connector.on_connection_state_changed ([&reconnecting_seen] (const auto &state) {
            if (state.current == zlink::stream_connector::connection_state_t::reconnecting) {
                reconnecting_seen.store (true, std::memory_order_release);
            }
        });
        if (!reconnect_connector.connect ()) {
            release_first_connection.signal ();
            reconnect_server.join ();
            return 176;
        }
        callback_latch_t pending_request_completed;
        std::atomic_int pending_request_callback_count{0};
        std::optional<zlink::stream_connector::error_code_t> pending_request_error;
        reconnect_connector.request (login_request_t{})
          .packet_name ("pending.before.reconnect")
          .timeout (std::chrono::seconds (2))
          .submit<login_reply_t> ([&] (auto result) {
              ++pending_request_callback_count;
              pending_request_error = result.error_code ();
              pending_request_completed.signal ();
          });
        if (!first_request_seen.wait_for (std::chrono::milliseconds (250))) {
            release_first_connection.signal ();
            reconnect_server.join ();
            return 176;
        }
        release_first_connection.signal ();
        const auto reconnecting_deadline =
          std::chrono::steady_clock::now () + std::chrono::milliseconds (250);
        while (reconnect_connector.state ()
                 != zlink::stream_connector::connection_state_t::reconnecting
               && std::chrono::steady_clock::now () < reconnecting_deadline) {
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        const auto request_during_reconnect =
          reconnect_connector.request (login_request_t{})
            .packet_name ("must.not.queue.during.reconnect")
            .timeout (std::chrono::seconds (1))
            .submit<login_reply_t> ();
        const auto connected_deadline =
          std::chrono::steady_clock::now () + std::chrono::milliseconds (700);
        while (reconnect_connector.state ()
                 != zlink::stream_connector::connection_state_t::connected
               && std::chrono::steady_clock::now () < connected_deadline) {
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        if (reconnect_connector.state ()
            == zlink::stream_connector::connection_state_t::connected) {
            reconnect_connector.send (login_request_t{})
              .packet_name ("fresh.after.reconnect")
              .submit ();
        }
        reconnect_server.join ();
        const auto pending_completed =
          pending_request_completed.wait_for (std::chrono::milliseconds (250));
        reconnect_connector.close ();
        if (!reconnecting_seen.load (std::memory_order_acquire) || !second_connection_seen
            || !second_connection_received_fresh_send || !pending_completed
            || pending_request_callback_count != 1
            || pending_request_error != zlink::stream_connector::error_code_t::disconnected
            || request_during_reconnect
            || request_during_reconnect.error_code ()
                 != zlink::stream_connector::error_code_t::disconnected) {
            return 176;
        }
    }

    {
        zlink::stream_connector::connector_options_t lifecycle_options;
        lifecycle_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        bool connect_lifetime_seen = false;
        callback_latch_t connect_lifetime_latch;
        {
            auto lifecycle_connector =
              zlink::stream_connector::connector_factory_t::create (lifecycle_options);
            lifecycle_connector.connect ([&] (zlink::stream_connector::result_t<void> result) {
                connect_lifetime_seen =
                  !result
                  && result.error_code ()
                       == zlink::stream_connector::error_code_t::configuration_error;
                connect_lifetime_latch.signal ();
            });
        }
        if (!connect_lifetime_latch.wait_for (std::chrono::milliseconds (100))
            || !connect_lifetime_seen) {
            return 97;
        }

        zlink::stream_connector::connector_options_t invalid_received_options;
        invalid_received_options.endpoint = endpoint;
        invalid_received_options.max_received_messages = 0;
        auto invalid_received_connector =
          zlink::stream_connector::connector_factory_t::create (invalid_received_options);
        auto invalid_received_connect = invalid_received_connector.connect ();
        if (invalid_received_connect
            || invalid_received_connect.error_code ()
                 != zlink::stream_connector::error_code_t::configuration_error) {
            return 154;
        }

        bool close_lifetime_seen = false;
        callback_latch_t close_lifetime_latch;
        {
            auto lifecycle_connector =
              zlink::stream_connector::connector_factory_t::create (lifecycle_options);
            lifecycle_connector.close ([&] (zlink::stream_connector::result_t<void> result) {
                close_lifetime_seen = static_cast<bool> (result);
                close_lifetime_latch.signal ();
            });
        }
        if (!close_lifetime_latch.wait_for (std::chrono::milliseconds (100))
            || !close_lifetime_seen) {
            return 98;
        }

        boost::asio::io_context async_connect_io;
        boost::asio::ip::tcp::acceptor async_connect_acceptor (
          async_connect_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
        const auto async_connect_endpoint =
          std::string ("tcp://127.0.0.1:")
          + std::to_string (async_connect_acceptor.local_endpoint ().port ());
        callback_latch_t async_connect_server_latch;
        /* accept한 소켓은 이 블록이 끝날 때까지 살아 있어야 한다. accept 스레드의 지역 변수로
         * 두면 스레드가 끝나는 즉시 연결이 닫히고, 클라이언트가 EOF로 disconnected가 되어
         * is_connected() 검사와 경합한다. */
        boost::asio::ip::tcp::socket async_connect_accepted (async_connect_io);
        joining_thread_t async_connect_server_thread ([&] {
            boost::system::error_code error;
            async_connect_acceptor.accept (async_connect_accepted, error);
            async_connect_server_latch.signal ();
        });
        lifecycle_options.endpoint = async_connect_endpoint;
        auto async_connect_connector =
          zlink::stream_connector::connector_factory_t::create (lifecycle_options);
        bool async_connect_seen = false;
        callback_latch_t async_connect_latch;
        const auto async_connect_started = std::chrono::steady_clock::now ();
        async_connect_connector.connect ([&] (zlink::stream_connector::result_t<void> result) {
            async_connect_seen = static_cast<bool> (result);
            async_connect_latch.signal ();
        });
        const auto async_connect_submit_elapsed =
          std::chrono::steady_clock::now () - async_connect_started;
        if (async_connect_submit_elapsed > std::chrono::milliseconds (50)) {
            async_connect_connector.close ();
            boost::system::error_code ignored;
            async_connect_acceptor.close (ignored);
            async_connect_server_thread.join ();
            return 99;
        }
        /* 비차단 여부는 위의 submit 경과 시간이 단언한다. 여기서는 완료 콜백이 실제로 오는지만
         * 본다. 완료 대기에 짧은 시한을 걸면 부하가 걸린 머신에서 그대로 flake가 된다. */
        if (!async_connect_latch.wait_for (std::chrono::seconds (5)) || !async_connect_seen
            || !async_connect_connector.is_connected ()) {
            async_connect_connector.close ();
            boost::system::error_code ignored;
            async_connect_acceptor.close (ignored);
            async_connect_server_thread.join ();
            return 100;
        }
        async_connect_connector.close ();
        async_connect_server_latch.wait_for (std::chrono::milliseconds (100));
        async_connect_server_thread.join ();

        boost::asio::io_context websocket_connect_io;
        boost::asio::ip::tcp::acceptor websocket_connect_acceptor (
          websocket_connect_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
        const auto websocket_connect_endpoint =
          std::string ("ws://127.0.0.1:")
          + std::to_string (websocket_connect_acceptor.local_endpoint ().port ()) + "/stream";
        callback_latch_t websocket_connect_server_latch;
        callback_latch_t websocket_connect_close_latch;
        joining_thread_t websocket_connect_server_thread ([&] {
            auto accepted = std::make_shared<boost::asio::ip::tcp::socket> (websocket_connect_io);
            boost::system::error_code accept_error;
            websocket_connect_acceptor.async_accept (*accepted,
                                                     [&] (boost::system::error_code error) {
                                                         accept_error = error;
                                                         websocket_connect_server_latch.signal ();
                                                     });
            websocket_connect_io.run_for (std::chrono::milliseconds (200));
            if (accept_error || !accepted->is_open ()) {
                websocket_connect_server_latch.signal ();
                websocket_connect_close_latch.wait_for (std::chrono::seconds (1));
                return;
            }
            boost::beast::websocket::stream<boost::asio::ip::tcp::socket> websocket (
              std::move (*accepted));
            boost::system::error_code error;
            websocket.accept (error);
            websocket_connect_server_latch.signal ();
            // Keep the peer open until the client observes the successful callback.
            websocket_connect_close_latch.wait_for (std::chrono::seconds (1));
            boost::system::error_code ignored;
            boost::beast::get_lowest_layer (websocket).close (ignored);
        });
        zlink::stream_connector::connector_options_t websocket_connect_options;
        websocket_connect_options.endpoint = websocket_connect_endpoint;
        websocket_connect_options.transport = zlink::stream_connector::transport_t::websocket;
        websocket_connect_options.dispatch_mode =
          zlink::stream_connector::dispatch_mode_t::immediate;
        auto websocket_connect_connector =
          zlink::stream_connector::connector_factory_t::create (websocket_connect_options);
        bool websocket_connect_seen = false;
        callback_latch_t websocket_connect_latch;
        const auto websocket_connect_started = std::chrono::steady_clock::now ();
        websocket_connect_connector.connect ([&] (zlink::stream_connector::result_t<void> result) {
            websocket_connect_seen = static_cast<bool> (result);
            websocket_connect_latch.signal ();
        });
        const auto websocket_connect_submit_elapsed =
          std::chrono::steady_clock::now () - websocket_connect_started;
        if (websocket_connect_submit_elapsed > std::chrono::milliseconds (50)) {
            websocket_connect_close_latch.signal ();
            websocket_connect_connector.close ();
            boost::system::error_code ignored;
            websocket_connect_acceptor.close (ignored);
            websocket_connect_server_thread.join ();
            return 101;
        }
        // The elapsed assertion above checks that connect() submits asynchronously.
        // Allow the server thread enough time to be scheduled before judging the
        // separate completion callback contract on a loaded test host.
        if (!websocket_connect_latch.wait_for (std::chrono::seconds (1))
            || !websocket_connect_seen || !websocket_connect_connector.is_connected ()) {
            websocket_connect_close_latch.signal ();
            websocket_connect_connector.close ();
            boost::system::error_code ignored;
            websocket_connect_acceptor.close (ignored);
            websocket_connect_server_thread.join ();
            return 102;
        }
        websocket_connect_close_latch.signal ();
        websocket_connect_connector.close ();
        websocket_connect_server_latch.wait_for (std::chrono::milliseconds (100));
        websocket_connect_server_thread.join ();
    }
    if (!connector.codecs ().supports (zlink::stream_connector::codec_t::json)) {
        return 3;
    }

    connector.send (login_request_t{}).metadata ("trace", "t1").compress ().submit ();
    auto runtime = zlink::stream_connector::detail::connector_runtime_t::from (connector);
    auto none_observed = connector.expect_none ("test.none")
                           .within (std::chrono::milliseconds (5))
                           .submit ();
    if (!none_observed) {
        return 193;
    }
    bool async_none_observed = false;
    callback_latch_t async_none_latch;
    connector.expect_none ("test.none.async")
      .within (std::chrono::milliseconds (5))
      .submit ([&] (zlink::stream_connector::result_t<void> result) {
          async_none_observed = static_cast<bool> (result);
          async_none_latch.signal ();
      });
    dispatch_until (connector, async_none_latch, std::chrono::milliseconds (100));
    if (!async_none_observed) {
        return 201;
    }
    runtime.receive_packet (
      zlink::stream_connector::packet_t{"test.unexpected",
                                        {},
                                        zlink::stream_connector::codec_t::raw,
                                        false,
                                        zlink::message_t::from (std::string ("unexpected"))});
    auto unexpected = connector.expect_none ("test.unexpected")
                        .within (std::chrono::milliseconds (5))
                        .submit ();
    if (unexpected
        || unexpected.error_code () != zlink::stream_connector::error_code_t::validation_failed) {
        return 194;
    }
    for (const auto *payload : {"first", "second", "third"}) {
        runtime.receive_packet (
          zlink::stream_connector::packet_t{"test.sequence",
                                            {},
                                            zlink::stream_connector::codec_t::raw,
                                            false,
                                            zlink::message_t::from (std::string (payload))});
    }
    auto sequence = connector.wait_for_sequence ("test.sequence")
                      .expect ([] (const zlink::stream_connector::packet_t &packet) {
                          return packet.payload.to_string () == "first";
                      })
                      .expect ([] (const zlink::stream_connector::packet_t &packet) {
                          return packet.payload.to_string () == "second";
                      })
                      .expect ([] (const zlink::stream_connector::packet_t &packet) {
                          return packet.payload.to_string () == "third";
                      })
                      .timeout (std::chrono::milliseconds (20))
                      .submit ();
    if (!sequence || sequence.value ().size () != 3) {
        return 195;
    }
    runtime.receive_packet (
      zlink::stream_connector::packet_t{"test.out-of-order",
                                        {},
                                        zlink::stream_connector::codec_t::raw,
                                        false,
                                        zlink::message_t::from (std::string ("second"))});
    auto out_of_order = connector.wait_for_sequence ("test.out-of-order")
                          .expect ([] (const zlink::stream_connector::packet_t &packet) {
                              return packet.payload.to_string () == "first";
                          })
                          .timeout (std::chrono::milliseconds (20))
                          .submit ();
    if (out_of_order
        || out_of_order.error_code ()
             != zlink::stream_connector::error_code_t::validation_failed) {
        return 196;
    }
    int action_invocations = 0;
    auto asserted_error = zlink::stream_connector::assertions::expect_failure (
      [&] {
          ++action_invocations;
          return zlink::stream_connector::result_t<void>::failure (
            zlink::stream_connector::error_code_t::validation_failed, "expected failure");
      },
      zlink::stream_connector::error_code_t::validation_failed);
    if (action_invocations != 1
        || asserted_error.code != zlink::stream_connector::error_code_t::validation_failed) {
        return 197;
    }
    auto timeout_error = zlink::stream_connector::assertions::expect_timeout ([] {
        return zlink::stream_connector::result_t<void>::failure (
          zlink::stream_connector::error_code_t::request_timeout, "expected timeout");
    });
    if (timeout_error.code != zlink::stream_connector::error_code_t::request_timeout) {
        return 198;
    }
    bool non_timeout_rethrown = false;
    try {
        (void) zlink::stream_connector::assertions::expect_timeout ([] {
            return zlink::stream_connector::result_t<void>::failure (
              zlink::stream_connector::error_code_t::disconnected, "not a timeout");
        });
    } catch (const zlink::stream_connector::assertions::failure_t &failure) {
        non_timeout_rethrown =
          failure.error ().code == zlink::stream_connector::error_code_t::disconnected;
    }
    if (!non_timeout_rethrown) {
        return 199;
    }
    bool ensure_message_required = false;
    try {
        zlink::stream_connector::assertions::ensure (true, "");
    } catch (const std::invalid_argument &) {
        ensure_message_required = true;
    }
    if (!ensure_message_required) {
        return 200;
    }
    if (runtime.sent_packets ().size () != 1
        || runtime.sent_packets ()[0].name != login_request_t::packet_name
        || runtime.sent_packets ()[0].codec != zlink::stream_connector::codec_t::json
        || runtime.sent_packets ()[0].metadata.values.at ("trace") != "t1") {
        return 5;
    }
    connector
      .send (zlink::stream_connector::packet_t{
        "compressible.large", {}, zlink::stream_connector::codec_t::raw, false,
        zlink::message_t::from (std::string (128, 'a'))})
      .compress ()
      .submit ();
    const bool compressible_large_send_accepted = runtime.sent_packets ().size () == 2;
    if (!compressible_large_send_accepted) {
        connector
          .send (zlink::stream_connector::packet_t{
            "compressible.fallback", {}, zlink::stream_connector::codec_t::raw, false,
            zlink::message_t::from (std::string ("ok"))})
          .submit ();
    }
    {
        // Async sends/requests ride the shared runner (write strand + posted
        // delivery), so completions are awaited with a bounded poll instead of
        // being asserted synchronously after submit.
        const auto eventually = [] (const std::function<bool ()> &predicate) {
            const auto deadline =
              std::chrono::steady_clock::now () + std::chrono::seconds (2);
            while (!predicate () && std::chrono::steady_clock::now () < deadline) {
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
            }
            return predicate ();
        };
        const auto no_pending_requests = [] (const auto &state) {
            std::lock_guard<std::mutex> lock (state->transport_mutex);
            return state->pending_requests.empty ();
        };
        const auto no_pending_waits = [] (const auto &state) {
            std::lock_guard<std::mutex> lock (state->transport_mutex);
            return state->pending_waits.empty ();
        };
        const auto release_state = [] (const auto &state) {
            if (!state)
                return;
            std::shared_ptr<zlink::stream_connector::detail::stream_connection_t>
              connection;
            {
                std::lock_guard<std::mutex> lock (state->transport_mutex);
                connection = std::move (state->connection);
                state->pending_requests.clear ();
                state->pending_waits.clear ();
                state->pending_sends.clear ();
                state->pending_writes.clear ();
                state->dispatch_queue.clear ();
                state->delivery_queue.clear ();
            }
            if (connection)
                connection->shutdown_and_close ();
        };
        zlink::stream_connector::connector_options_t async_send_options;
        async_send_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        auto async_send_state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (async_send_options);
        async_send_state->state = zlink::stream_connector::connection_state_t::connected;
        auto async_send_connection = std::make_shared<async_write_connection_t> ();
        async_send_state->connection = async_send_connection;
        std::atomic<bool> async_send_seen{false};
        zlink::stream_connector::detail::submit_send_async (
          async_send_state,
          zlink::stream_connector::packet_t{.name = "async.send",
                                            .payload = zlink::message_t::from ("payload")},
          [&] (zlink::stream_connector::result_t<void> result) {
              async_send_seen = static_cast<bool> (result);
          });
        if (!eventually ([&] {
                return async_send_seen.load () && !async_send_connection->written.empty ()
                       && async_send_state->sent_packets.size () == 1;
            })
            || async_send_state->sent_packets[0].name != "async.send") {
            return 155;
        }

        auto async_write_failure_state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (async_send_options);
        async_write_failure_state->state = zlink::stream_connector::connection_state_t::connected;
        async_write_failure_state->connection = std::make_shared<async_write_connection_t> (true);
        std::atomic<bool> async_write_failure_seen{false};
        zlink::stream_connector::detail::submit_send_async (
          async_write_failure_state,
          zlink::stream_connector::packet_t{.name = "async.write.fail",
                                            .payload = zlink::message_t::from ("payload")},
          [&] (zlink::stream_connector::result_t<void> result) {
              async_write_failure_seen =
                !result
                && result.error_code () == zlink::stream_connector::error_code_t::send_failed;
          });
        if (!eventually ([&] { return async_write_failure_seen.load (); })
            || !async_write_failure_state->sent_packets.empty ()) {
            return 158;
        }

        auto async_closed_state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (async_send_options);
        async_closed_state->state = zlink::stream_connector::connection_state_t::connected;
        async_closed_state->connection = std::make_shared<async_write_connection_t> ();
        async_closed_state->close_requested.store (true);
        std::atomic<bool> async_closed_seen{false};
        zlink::stream_connector::detail::submit_send_async (
          async_closed_state,
          zlink::stream_connector::packet_t{.name = "async.closed",
                                            .payload = zlink::message_t::from ("payload")},
          [&] (zlink::stream_connector::result_t<void> result) {
              async_closed_seen =
                !result && result.error_code () == zlink::stream_connector::error_code_t::closed;
          });
        if (!eventually ([&] { return async_closed_seen.load (); })) {
            return 159;
        }

        auto async_disconnected_state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (async_send_options);
        std::atomic<bool> async_disconnected_seen{false};
        zlink::stream_connector::detail::submit_send_async (
          async_disconnected_state,
          zlink::stream_connector::packet_t{.name = "async.disconnected",
                                            .payload = zlink::message_t::from ("payload")},
          [&] (zlink::stream_connector::result_t<void> result) {
              async_disconnected_seen =
                !result
                && result.error_code () == zlink::stream_connector::error_code_t::disconnected;
          });
        if (!eventually ([&] { return async_disconnected_seen.load (); })) {
            return 160;
        }

        async_send_options.max_send_payload_size = 1;
        auto async_validation_state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (async_send_options);
        async_validation_state->state = zlink::stream_connector::connection_state_t::connected;
        async_validation_state->connection = std::make_shared<async_write_connection_t> ();
        std::atomic<bool> async_validation_seen{false};
        zlink::stream_connector::detail::submit_send_async (
          async_validation_state,
          zlink::stream_connector::packet_t{.name = "async.validation",
                                            .payload = zlink::message_t::from ("too-large")},
          [&] (zlink::stream_connector::result_t<void> result) {
              async_validation_seen =
                !result
                && result.error_code () == zlink::stream_connector::error_code_t::frame_too_large;
          });
        if (!eventually ([&] { return async_validation_seen.load (); })) {
            return 161;
        }

        std::atomic<bool> async_request_unbound_seen{false};
        zlink::stream_connector::detail::submit_request_async (
          {}, zlink::stream_connector::packet_t{.name = "unbound.request"},
          std::chrono::milliseconds (1),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>
                 result) {
              async_request_unbound_seen =
                !result
                && result.error_code ()
                     == zlink::stream_connector::error_code_t::configuration_error;
          });
        if (!eventually ([&] { return async_request_unbound_seen.load (); })) {
            return 162;
        }

        std::atomic<bool> async_request_closed_seen{false};
        zlink::stream_connector::detail::submit_request_async (
          async_closed_state, zlink::stream_connector::packet_t{.name = "closed.request"},
          std::chrono::milliseconds (1),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>
                 result) {
              async_request_closed_seen =
                !result && result.error_code () == zlink::stream_connector::error_code_t::closed;
          });
        if (!eventually ([&] { return async_request_closed_seen.load (); })) {
            return 163;
        }

        std::atomic<bool> async_request_disconnected_seen{false};
        zlink::stream_connector::detail::submit_request_async (
          async_disconnected_state,
          zlink::stream_connector::packet_t{.name = "disconnected.request"},
          std::chrono::milliseconds (1),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>
                 result) {
              async_request_disconnected_seen =
                !result
                && result.error_code () == zlink::stream_connector::error_code_t::disconnected;
          });
        if (!eventually ([&] { return async_request_disconnected_seen.load (); })) {
            return 164;
        }

        std::atomic<bool> async_request_validation_seen{false};
        zlink::stream_connector::detail::submit_request_async (
          async_validation_state,
          zlink::stream_connector::packet_t{.name = "validation.request",
                                            .payload = zlink::message_t::from ("too-large")},
          std::chrono::milliseconds (1),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>
                 result) {
              async_request_validation_seen =
                !result
                && result.error_code () == zlink::stream_connector::error_code_t::frame_too_large;
          });
        if (!eventually ([&] { return async_request_validation_seen.load (); })) {
            return 165;
        }

        auto async_request_write_failure_state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (
            zlink::stream_connector::connector_options_t{
              .dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate});
        async_request_write_failure_state->state =
          zlink::stream_connector::connection_state_t::connected;
        async_request_write_failure_state->connection =
          std::make_shared<async_write_connection_t> (true);
        std::atomic<bool> async_request_write_failure_seen{false};
        zlink::stream_connector::detail::submit_request_async (
          async_request_write_failure_state,
          zlink::stream_connector::packet_t{.name = "write.failure.request",
                                            .payload = zlink::message_t::from ("payload")},
          std::chrono::milliseconds (1),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>
                 result) {
              async_request_write_failure_seen =
                !result
                && result.error_code () == zlink::stream_connector::error_code_t::send_failed;
          });
        if (!eventually ([&] {
                return async_request_write_failure_seen.load ()
                       && no_pending_requests (async_request_write_failure_state);
            })) {
            return 166;
        }

        auto early_reply_frame = make_server_frame (
          zlink::stream_connector::message_kind_t::response, 1, "early.reply.request",
          "early-reply");
        const auto early_reply_text = early_reply_frame.to_string ();
        auto early_reply_connection = std::make_shared<early_reply_connection_t> (
          std::vector<std::uint8_t> (early_reply_text.begin (), early_reply_text.end ()));
        auto early_reply_state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (
            zlink::stream_connector::connector_options_t{
              .dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate});
        early_reply_state->state = zlink::stream_connector::connection_state_t::connected;
        early_reply_state->connection = early_reply_connection;
        std::atomic<int> early_reply_callback_count{0};
        std::atomic<bool> early_reply_seen{false};
        zlink::stream_connector::detail::submit_request_async (
          early_reply_state,
          zlink::stream_connector::packet_t{.name = "early.reply.request",
                                            .payload = zlink::message_t::from ("payload")},
          std::chrono::milliseconds (25),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>
                 result) {
              ++early_reply_callback_count;
              early_reply_seen = result && result.value ().payload.to_string () == "early-reply";
          });
        if (!eventually ([&] {
                return !early_reply_connection->written.empty ()
                       && static_cast<bool> (early_reply_connection->write_completion)
                       && early_reply_seen.load () && early_reply_callback_count.load () == 1
                       && no_pending_requests (early_reply_state);
            })) {
            return 171;
        }
        early_reply_connection->complete_write ();
        std::this_thread::sleep_for (std::chrono::milliseconds (500));
        if (!early_reply_seen.load () || early_reply_callback_count.load () != 1
            || !no_pending_requests (early_reply_state)) {
            return 173;
        }

        /* §5.2: pending request 매칭은 request_seq가 정본이다. 응답의 packet name이 request와
         * 달라도 그 응답으로 완료한다 — 이름은 대조 조건이 아니다. */
        auto mismatched_reply_frame = make_server_frame (
          zlink::stream_connector::message_kind_t::response, 1, "unexpected.reply", "payload");
        const auto mismatched_reply_text = mismatched_reply_frame.to_string ();
        auto mismatched_reply_connection = std::make_shared<early_reply_connection_t> (
          std::vector<std::uint8_t> (mismatched_reply_text.begin (), mismatched_reply_text.end ()));
        auto mismatched_reply_state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (
            zlink::stream_connector::connector_options_t{
              .dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate});
        mismatched_reply_state->state = zlink::stream_connector::connection_state_t::connected;
        mismatched_reply_state->connection = mismatched_reply_connection;
        std::atomic<bool> mismatched_reply_completed{false};
        zlink::stream_connector::detail::submit_request_async (
          mismatched_reply_state,
          zlink::stream_connector::packet_t{.name = "expected.reply",
                                            .payload = zlink::message_t::from ("payload")},
          std::chrono::milliseconds (25),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>
                 result) {
              mismatched_reply_completed =
                result && result.value ().payload.to_string () == "payload";
          });
        if (!eventually ([&] {
                return mismatched_reply_completed.load ()
                       && no_pending_requests (mismatched_reply_state);
            })) {
            return 179;
        }

        auto invalid_error_frame = make_server_frame (
          zlink::stream_connector::message_kind_t::error, 1, "invalid.error.request",
          "{\"error\":\"missing code and message\"}");
        const auto invalid_error_text = invalid_error_frame.to_string ();
        auto invalid_error_connection = std::make_shared<early_reply_connection_t> (
          std::vector<std::uint8_t> (invalid_error_text.begin (), invalid_error_text.end ()));
        auto invalid_error_state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (
            zlink::stream_connector::connector_options_t{
              .dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate});
        invalid_error_state->state = zlink::stream_connector::connection_state_t::connected;
        invalid_error_state->connection = invalid_error_connection;
        std::atomic<bool> invalid_error_rejected{false};
        zlink::stream_connector::detail::submit_request_async (
          invalid_error_state,
          zlink::stream_connector::packet_t{.name = "invalid.error.request",
                                            .payload = zlink::message_t::from ("payload")},
          std::chrono::milliseconds (25),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>
                 result) {
              invalid_error_rejected =
                !result
                && result.error_code ()
                     == zlink::stream_connector::error_code_t::frame_decode_failed;
          });
        if (!eventually ([&] {
                return invalid_error_rejected.load ()
                       && no_pending_requests (invalid_error_state);
            })) {
            return 180;
        }

        auto interleaved_push_frame = make_server_frame (
          zlink::stream_connector::message_kind_t::send, 0, "interleaved.push", "push-payload");
        auto interleaved_response_frame =
          make_server_frame (zlink::stream_connector::message_kind_t::response, 1,
                             "interleaved.request", "reply-payload");
        const auto interleaved_text =
          interleaved_push_frame.to_string () + interleaved_response_frame.to_string ();
        auto interleaved_connection = std::make_shared<early_reply_connection_t> (
          std::vector<std::uint8_t> (interleaved_text.begin (), interleaved_text.end ()));
        auto interleaved_state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (
            zlink::stream_connector::connector_options_t{
              .dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate});
        interleaved_state->state = zlink::stream_connector::connection_state_t::connected;
        interleaved_state->connection = interleaved_connection;
        std::atomic<int> interleaved_wait_callback_count{0};
        std::atomic<bool> interleaved_push_seen{false};
        interleaved_state->pending_waits.emplace (
          1, zlink::stream_connector::detail::pending_wait_t{
               1,
               "interleaved.push",
               {},
               [&] (zlink::stream_connector::result_t<zlink::stream_connector::packet_t> result) {
                   ++interleaved_wait_callback_count;
                   interleaved_push_seen =
                     result && result.value ().payload.to_string () == "push-payload";
               }});
        std::atomic<bool> interleaved_reply_seen{false};
        zlink::stream_connector::detail::submit_request_async (
          interleaved_state,
          zlink::stream_connector::packet_t{.name = "interleaved.request",
                                            .payload = zlink::message_t::from ("payload")},
          std::chrono::milliseconds (25),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>
                 result) {
              interleaved_reply_seen =
                result && result.value ().payload.to_string () == "reply-payload";
          });
        if (!eventually ([&] {
                return !interleaved_connection->written.empty ()
                       && static_cast<bool> (interleaved_connection->write_completion)
                       && interleaved_push_seen.load ()
                       && interleaved_wait_callback_count.load () == 1
                       && interleaved_reply_seen.load ()
                       && no_pending_requests (interleaved_state)
                       && no_pending_waits (interleaved_state);
            })) {
            return 174;
        }
        interleaved_connection->complete_write ();
        if (!interleaved_push_seen.load () || interleaved_wait_callback_count.load () != 1
            || !interleaved_reply_seen.load () || !no_pending_requests (interleaved_state)
            || !no_pending_waits (interleaved_state)) {
            return 175;
        }

        async_send_state->dispatch_queue.push_back (zlink::stream_connector::packet_t{
          .name = "queued.receive", .payload = zlink::message_t::from ("queued")});
        const auto receive_queued = zlink::stream_connector::detail::receive_next (
          async_send_state, std::chrono::milliseconds (1));
        if (!receive_queued || receive_queued.value ().name != "queued.receive") {
            return 167;
        }
        auto receive_closed_state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (
            zlink::stream_connector::connector_options_t{});
        receive_closed_state->close_requested.store (true);
        const auto receive_closed = zlink::stream_connector::detail::receive_next (
          receive_closed_state, std::chrono::milliseconds (1));
        const auto receive_disconnected = zlink::stream_connector::detail::receive_next (
          async_disconnected_state, std::chrono::milliseconds (1));
        if (receive_closed
            || receive_closed.error_code () != zlink::stream_connector::error_code_t::closed
            || receive_disconnected
            || receive_disconnected.error_code ()
                 != zlink::stream_connector::error_code_t::disconnected) {
            return 168;
        }
        const auto receive_timeout = zlink::stream_connector::detail::receive_next (
          async_send_state, std::chrono::milliseconds (1));
        if (receive_timeout
            || receive_timeout.error_code ()
                 != zlink::stream_connector::error_code_t::request_timeout) {
            return 169;
        }

        async_send_state->dispatch_queue.push_back (zlink::stream_connector::packet_t{
          .name = "queued.wait", .payload = zlink::message_t::from ("queued")});
        const auto wait_matched = zlink::stream_connector::detail::wait_for_packet (
          async_send_state, "queued.wait",
          [] (const zlink::stream_connector::packet_t &packet) {
              return packet.payload.to_string () == "queued";
          },
          std::chrono::milliseconds (1));
        const auto wait_closed = zlink::stream_connector::detail::wait_for_packet (
          receive_closed_state, "missing", {}, std::chrono::milliseconds (1));
        if (!wait_matched || wait_closed
            || wait_closed.error_code () != zlink::stream_connector::error_code_t::closed) {
            return 170;
        }
        release_state (async_send_state);
        release_state (async_write_failure_state);
        release_state (async_closed_state);
        release_state (async_disconnected_state);
        release_state (async_validation_state);
        release_state (async_request_write_failure_state);
        release_state (early_reply_state);
        release_state (mismatched_reply_state);
        release_state (invalid_error_state);
        release_state (interleaved_state);
        release_state (receive_closed_state);
    }

    connector.send (login_request_t{}).packet_name ("login.uncompressed").submit ();

    /* 이 케이스가 확인하는 것은 reply 성공과 pending map 정리이지 응답 지연이 아니다. 짧은
     * timeout은 부하가 걸린 머신에서 그대로 flake가 된다. */
    auto request = connector.request (login_request_t{})
                     .packet_name ("login.request")
                     .timeout (std::chrono::seconds (5))
                     .submit<login_reply_t> ();
    if (!request || runtime.pending_request_count () != 0) {
        return 6;
    }
    server_thread.join ();
    if (!compressed_send_seen || !uncompressed_send_seen) {
        return 27;
    }
    if (!compressible_large_send_accepted || !compressible_large_send_seen) {
        return 179;
    }

    int compressed_dispatch_count = 0;
    connector.on<zlink::stream_connector::packet_t> (
      "server.compressed", [&] (const zlink::stream_connector::packet_t &packet) {
          if (packet.compressed && packet.payload.to_string () == "server-payload") {
              ++compressed_dispatch_count;
          }
      });
    if (connector.pending_dispatch_count () != 1) {
        return 28;
    }
    if (!connector.dispatch () || compressed_dispatch_count != 1
        || connector.pending_dispatch_count () != 0) {
        return 29;
    }

    {
        zlink::stream_connector::connector_options_t disabled_options;
        disabled_options.endpoint = endpoint;
        disabled_options.compression = zlink::stream_connector::compression_t::none;
        disabled_options.compression_codec.reset ();
        auto disabled_connector =
          zlink::stream_connector::connector_factory_t::create (disabled_options);
        if (!disabled_connector.connect ()) {
            return 147;
        }
        disabled_connector.send (login_request_t{}).compress ().submit ();
        disabled_connector.close ();
    }

    {
        zlink::stream_socket_t custom_server (context);
        custom_server.options ().notify (false);
        custom_server.bind ("tcp://127.0.0.1:0");
        const auto custom_endpoint = custom_server.options ().last_endpoint ();
        std::atomic_bool custom_payload_seen{false};
        joining_thread_t custom_server_thread ([&custom_server, &custom_payload_seen] {
            std::string buffer;
            while (!custom_payload_seen.load ()) {
                zlink::received_t inbound;
                if (custom_server.recv (inbound) != 0) {
                    return;
                }
                buffer +=
                  inbound.parts ().empty () ? std::string{} : inbound.parts ()[0].to_string ();
                while (auto frame = try_read_server_frame_raw (buffer)) {
                    if (frame->header.kind == zlink::stream_connector::message_kind_t::send
                        && frame->header.name == "custom.outbound" && frame->compressed
                        && frame->payload == "custom:{}") {
                        custom_payload_seen = true;
                    }
                }
                inbound.close ();
            }
        });
        zlink::stream_connector::connector_options_t custom_options;
        custom_options.endpoint = custom_endpoint;
        custom_options.compression_codec = std::make_shared<prefix_compression_codec_t> ("custom");
        auto custom_connector =
          zlink::stream_connector::connector_factory_t::create (custom_options);
        if (!custom_connector.connect ()) {
            return 148;
        }
        custom_connector.send (login_request_t{})
          .packet_name ("custom.outbound")
          .compress ()
          .submit ();
        custom_server_thread.join ();
        if (!custom_payload_seen) {
            return 145;
        }
        custom_connector.close ();
    }

    zlink::stream_socket_t receive_server (context);
    receive_server.options ().notify (false);
    receive_server.bind ("tcp://127.0.0.1:0");
    const auto receive_endpoint = receive_server.options ().last_endpoint ();
    joining_thread_t receive_server_thread ([&receive_server] {
        zlink::received_t inbound;
        if (receive_server.recv (inbound) != 0) {
            return;
        }
        auto first = make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                        "server.receive.one", "one");
        auto second = make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                         "server.receive.two", "two");
        inbound.send ()
          .message (zlink::message_t::from (first.to_string () + second.to_string ()))
          .submit ();
        inbound.close ();
    });
    zlink::stream_connector::connector_options_t receive_options;
    receive_options.endpoint = receive_endpoint;
    receive_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
    receive_options.request_timeout = std::chrono::milliseconds (100);
    auto receive_connector = zlink::stream_connector::connector_factory_t::create (receive_options);
    if (!receive_connector.connect ()) {
        return 56;
    }
    receive_connector.send (login_request_t{}).packet_name ("receive.trigger").submit ();
    auto received_first =
      receive_connector.wait_for ("server.receive.one", std::chrono::milliseconds (100));
    auto received_second =
      receive_connector.wait_for ("server.receive.two", std::chrono::milliseconds (100));
    receive_server_thread.join ();
    if (!received_first || !received_second || received_first.value ().name != "server.receive.one"
        || received_first.value ().payload.to_string () != "one"
        || received_second.value ().name != "server.receive.two"
        || received_second.value ().payload.to_string () != "two"
        || receive_connector.pending_dispatch_count () != 0) {
        return 58;
    }
    receive_connector.close ();

    zlink::stream_connector::connector_options_t capped_receive_options;
    capped_receive_options.endpoint = "inproc://unused-capped-receive";
    capped_receive_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
    capped_receive_options.max_received_messages = 1;
    auto capped_receive_connector =
      zlink::stream_connector::connector_factory_t::create (capped_receive_options);
    std::atomic_bool received_drop_seen{false};
    capped_receive_connector.on_error ([&] (const zlink::stream_connector::error_t &error) {
        if (error.code == zlink::stream_connector::error_code_t::received_message_dropped) {
            received_drop_seen = true;
        }
    });
    auto capped_runtime =
      zlink::stream_connector::detail::connector_runtime_t::from (capped_receive_connector);
    capped_runtime.receive_packet (zlink::stream_connector::packet_t{
      .name = "queued.one", .payload = zlink::message_t::from ("one")});
    capped_runtime.receive_packet (zlink::stream_connector::packet_t{
      .name = "dropped.two", .payload = zlink::message_t::from ("two")});
    if (capped_receive_connector.pending_dispatch_count () != 2) {
        return 152;
    }
    auto capped_first =
      capped_receive_connector.wait_for ("queued.one", std::chrono::milliseconds (10));
    const auto capped_errors_dispatched = capped_receive_connector.dispatch ();
    auto capped_second =
      capped_receive_connector.wait_for ("dropped.two", std::chrono::milliseconds (10));
    if (!capped_errors_dispatched || !received_drop_seen || !capped_first || capped_second
        || capped_second.error_code () != zlink::stream_connector::error_code_t::disconnected) {
        return 153;
    }

    zlink::stream_socket_t observer_server (context);
    observer_server.options ().notify (false);
    observer_server.bind ("tcp://127.0.0.1:0");
    const auto observer_endpoint = observer_server.options ().last_endpoint ();
    joining_thread_t observer_server_thread ([&observer_server] {
        zlink::received_t inbound;
        if (observer_server.recv (inbound) != 0) {
            return;
        }
        std::string buffer =
          inbound.parts ().empty () ? std::string{} : inbound.parts ()[0].to_string ();
        auto frame = try_read_server_frame (buffer);
        if (!frame || frame->header.kind != zlink::stream_connector::message_kind_t::request) {
            inbound.close ();
            return;
        }
        auto control = make_server_frame (zlink::stream_connector::message_kind_t::control, 0,
                                          "$zlink.heartbeat.pong", "");
        inbound.send ().message (control).submit ();
        auto push = make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                       "observer.push", "push");
        inbound.send ().message (push).submit ();
        auto reply =
          make_server_frame (zlink::stream_connector::message_kind_t::response,
                             frame->header.request_seq.value (), frame->header.name,
                             "observer-reply");
        inbound.send ().message (reply).submit ();
        inbound.close ();
    });
    zlink::stream_connector::connector_options_t observer_options;
    observer_options.endpoint = observer_endpoint;
    observer_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
    observer_options.request_timeout = std::chrono::milliseconds (100);
    observer_options.max_inbound_observer_payload_preview_bytes = 3;
    auto observer_connector =
      zlink::stream_connector::connector_factory_t::create (observer_options);
    std::mutex observed_mutex;
    std::condition_variable observed_changed;
    std::vector<zlink::stream_connector::inbound_observation_t> observations;
    auto observer_registration = observer_connector.observe_inbound (
      [&] (const zlink::stream_connector::inbound_observation_t &observation) {
          {
              std::lock_guard<std::mutex> lock (observed_mutex);
              observations.push_back (observation);
          }
          observed_changed.notify_all ();
      });
    zlink::stream_connector::inbound_observer_registration_t moved_observer_registration (
      std::move (observer_registration));
    zlink::stream_connector::inbound_observer_registration_t assigned_observer_registration;
    assigned_observer_registration = std::move (moved_observer_registration);
    if (observer_registration || moved_observer_registration || !assigned_observer_registration
        || !observer_connector.connect ()) {
        observer_server_thread.join ();
        return 80;
    }
    if (observer_connector.observe_inbound (
          [] (const zlink::stream_connector::inbound_observation_t &) {})
        || !observer_connector.request (login_request_t{})
              .packet_name ("observer.request")
              .timeout (std::chrono::milliseconds (100))
              .submit<login_reply_t> ()) {
        observer_connector.close ();
        observer_server_thread.join ();
        return 81;
    }
    {
        std::unique_lock<std::mutex> lock (observed_mutex);
        observed_changed.wait_for (lock, std::chrono::milliseconds (100), [&] {
            const auto saw_response =
              std::any_of (observations.begin (), observations.end (), [] (const auto &item) {
                  return item.kind == zlink::stream_connector::message_kind_t::response
                         && item.name == "observer.request" && item.request_seq.has_value ()
                         && item.payload_length == std::string ("observer-reply").size ()
                         && item.payload_preview == std::vector<std::uint8_t>{'o', 'b', 's'};
              });
            const auto saw_push =
              std::any_of (observations.begin (), observations.end (), [] (const auto &item) {
                  return item.kind == zlink::stream_connector::message_kind_t::send
                         && item.name == "observer.push";
              });
            const auto saw_control =
              std::any_of (observations.begin (), observations.end (), [] (const auto &item) {
                  return item.kind == zlink::stream_connector::message_kind_t::control
                         && item.name == "$zlink.heartbeat.pong";
              });
            return saw_response && saw_push && saw_control;
        });
        const auto saw_response =
          std::any_of (observations.begin (), observations.end (), [] (const auto &item) {
              return item.kind == zlink::stream_connector::message_kind_t::response
                     && item.name == "observer.request" && item.request_seq.has_value ()
                     && item.payload_length == std::string ("observer-reply").size ()
                     && item.payload_preview == std::vector<std::uint8_t>{'o', 'b', 's'};
          });
        const auto saw_push =
          std::any_of (observations.begin (), observations.end (), [] (const auto &item) {
              return item.kind == zlink::stream_connector::message_kind_t::send
                     && item.name == "observer.push";
          });
        const auto saw_control =
          std::any_of (observations.begin (), observations.end (), [] (const auto &item) {
              return item.kind == zlink::stream_connector::message_kind_t::control
                     && item.name == "$zlink.heartbeat.pong";
          });
        if (!saw_response || !saw_push || !saw_control) {
            observer_connector.close ();
            observer_server_thread.join ();
            return 82;
        }
    }
    assigned_observer_registration.close ();
    auto before_dispose_count = observations.size ();
    zlink::stream_connector::detail::connector_runtime_t::from (observer_connector)
      .receive_packet (
        zlink::stream_connector::packet_t{"after.dispose",
                                          {},
                                          zlink::stream_connector::codec_t::raw,
                                          false,
                                          zlink::message_t::from (std::string ("ignored"))});
    std::this_thread::sleep_for (std::chrono::milliseconds (10));
    if (observations.size () != before_dispose_count) {
        observer_connector.close ();
        observer_server_thread.join ();
        return 83;
    }
    observer_connector.close ();
    observer_server_thread.join ();

    zlink::stream_socket_t failing_observer_server (context);
    failing_observer_server.options ().notify (false);
    failing_observer_server.bind ("tcp://127.0.0.1:0");
    const auto failing_observer_endpoint = failing_observer_server.options ().last_endpoint ();
    joining_thread_t failing_observer_server_thread ([&failing_observer_server] {
        zlink::received_t inbound;
        if (failing_observer_server.recv (inbound) != 0) {
            return;
        }
        auto push = make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                       "observer.failure", "push");
        inbound.send ().message (push).submit ();
        inbound.close ();
    });
    zlink::stream_connector::connector_options_t failing_observer_options;
    failing_observer_options.endpoint = failing_observer_endpoint;
    failing_observer_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
    failing_observer_options.request_timeout = std::chrono::milliseconds (100);
    auto failing_observer_connector =
      zlink::stream_connector::connector_factory_t::create (failing_observer_options);
    std::atomic_bool observer_failed_seen{false};
    failing_observer_connector.on_error ([] (const zlink::stream_connector::error_t &) {
        throw std::runtime_error ("observer error handler failed");
    });
    failing_observer_connector.on_error ([&] (const zlink::stream_connector::error_t &error) {
        if (error.code == zlink::stream_connector::error_code_t::observer_failed) {
            observer_failed_seen = true;
        }
    });
    auto failing_registration = failing_observer_connector.observe_inbound (
      [] (const zlink::stream_connector::inbound_observation_t &) {
          throw std::runtime_error ("observer failed");
      });
    if (!failing_registration || !failing_observer_connector.connect ()) {
        failing_observer_connector.close ();
        failing_observer_server_thread.join ();
        return 84;
    }
    failing_observer_connector.send (login_request_t{})
      .packet_name ("observer.failure.trigger")
      .submit ();
    const auto failure_deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (100);
    while (!observer_failed_seen && std::chrono::steady_clock::now () < failure_deadline) {
        failing_observer_connector.dispatch ();
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    failing_observer_connector.close ();
    failing_observer_server_thread.join ();
    if (!observer_failed_seen) {
        return 85;
    }

    zlink::stream_socket_t dropped_observer_server (context);
    dropped_observer_server.options ().notify (false);
    dropped_observer_server.bind ("tcp://127.0.0.1:0");
    const auto dropped_observer_endpoint = dropped_observer_server.options ().last_endpoint ();
    joining_thread_t dropped_observer_server_thread ([&dropped_observer_server] {
        zlink::received_t inbound;
        if (dropped_observer_server.recv (inbound) != 0) {
            return;
        }
        for (int index = 0; index < 16; ++index) {
            auto push = make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                           "observer.overflow." + std::to_string (index), "push");
            inbound.send ().message (push).submit ();
        }
        auto reply = make_server_frame (zlink::stream_connector::message_kind_t::response, 1,
                                        "observer.overflow.trigger", "observer-overflow-reply");
        inbound.send ().message (reply).submit ();
        inbound.close ();
    });
    zlink::stream_connector::connector_options_t dropped_observer_options;
    dropped_observer_options.endpoint = dropped_observer_endpoint;
    dropped_observer_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    dropped_observer_options.request_timeout = std::chrono::milliseconds (100);
    dropped_observer_options.max_inbound_observer_notifications = 1;
    auto dropped_observer_connector =
      zlink::stream_connector::connector_factory_t::create (dropped_observer_options);
    std::atomic_bool observer_dropped_seen{false};
    std::atomic_int overflow_observations{0};
    dropped_observer_connector.on_error ([&] (const zlink::stream_connector::error_t &error) {
        if (error.code == zlink::stream_connector::error_code_t::observer_dropped) {
            observer_dropped_seen = true;
        }
    });
    auto dropped_registration = dropped_observer_connector.observe_inbound (
      [&] (const zlink::stream_connector::inbound_observation_t &) {
          ++overflow_observations;
          std::this_thread::sleep_for (std::chrono::milliseconds (20));
      });
    if (!dropped_registration || !dropped_observer_connector.connect ()) {
        dropped_observer_connector.close ();
        dropped_observer_server_thread.join ();
        return 86;
    }
    auto overflow_reply = dropped_observer_connector.request (login_request_t{})
                            .packet_name ("observer.overflow.trigger")
                            .timeout (std::chrono::milliseconds (100))
                            .submit<login_reply_t> ();
    const auto dropped_deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (100);
    while (!observer_dropped_seen && std::chrono::steady_clock::now () < dropped_deadline) {
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    dropped_observer_connector.close ();
    dropped_observer_server_thread.join ();
    if (!overflow_reply || !observer_dropped_seen || overflow_observations.load () <= 0) {
        return 87;
    }

    connector
      .send (zlink::stream_connector::packet_t{"oversized.payload",
                                               {},
                                               zlink::stream_connector::codec_t::raw,
                                               false,
                                               zlink::message_t::from (std::string (17, 'x'))})
      .submit ();

    zlink::stream_connector::metadata_t oversized_metadata;
    oversized_metadata.with ("trace", std::string (9 * 1024, 'm'));
    connector
      .send (zlink::stream_connector::packet_t{"oversized.metadata", std::move (oversized_metadata),
                                               zlink::stream_connector::codec_t::raw, false,
                                               zlink::message_t::from (std::string ("ok"))})
      .submit ();

    int manual_dispatch_count = 0;
    connector.on<zlink::stream_connector::packet_t> (
      "server.push", [&] (const zlink::stream_connector::packet_t &packet) {
          if (packet.payload.to_string () == "payload") {
              ++manual_dispatch_count;
          }
      });
    runtime.receive_packet (
      zlink::stream_connector::packet_t{"server.push",
                                        {},
                                        zlink::stream_connector::codec_t::raw,
                                        false,
                                        zlink::message_t::from (std::string ("payload"))});
    if (manual_dispatch_count != 0 || connector.pending_dispatch_count () != 1) {
        return 7;
    }
    if (!connector.dispatch () || manual_dispatch_count != 1
        || connector.pending_dispatch_count () != 0) {
        return 8;
    }

    bool immediate_wait_callback_seen = false;
    callback_latch_t immediate_wait_callback_latch;
    runtime.receive_packet (
      zlink::stream_connector::packet_t{"server.wait.immediate",
                                        {},
                                        zlink::stream_connector::codec_t::raw,
                                        false,
                                        zlink::message_t::from (std::string ("immediate"))});
    connector.wait_for<zlink::stream_connector::packet_t> ("server.wait.immediate")
      .timeout (std::chrono::milliseconds (100))
      .submit ([&] (zlink::stream_connector::result_t<zlink::stream_connector::packet_t> result) {
          immediate_wait_callback_seen =
            result && result.value ().payload.to_string () == "immediate";
          immediate_wait_callback_latch.signal ();
      });
    dispatch_until (connector, immediate_wait_callback_latch, std::chrono::milliseconds (100));
    if (!immediate_wait_callback_seen) {
        return 176;
    }

    bool pending_wait_callback_seen = false;
    callback_latch_t pending_wait_latch;
    connector.wait_for<zlink::stream_connector::packet_t> ("server.wait.callback")
      .timeout (std::chrono::milliseconds (100))
      .submit ([&] (zlink::stream_connector::result_t<zlink::stream_connector::packet_t> result) {
          pending_wait_callback_seen =
            result && result.value ().payload.to_string () == "wait-callback";
          pending_wait_latch.signal ();
      });
    runtime.receive_packet (
      zlink::stream_connector::packet_t{"server.wait.callback",
                                        {},
                                        zlink::stream_connector::codec_t::raw,
                                        false,
                                        zlink::message_t::from (std::string ("wait-callback"))});
    dispatch_until (connector, pending_wait_latch, std::chrono::milliseconds (100));
    if (!pending_wait_callback_seen) {
        return 83;
    }

    bool pending_wait_timeout_seen = false;
    callback_latch_t pending_wait_timeout_latch;
    connector.wait_for<zlink::stream_connector::packet_t> ("server.wait.missing")
      .timeout (std::chrono::milliseconds (5))
      .submit ([&] (zlink::stream_connector::result_t<zlink::stream_connector::packet_t> result) {
          pending_wait_timeout_seen =
            !result
            && result.error_code () == zlink::stream_connector::error_code_t::request_timeout;
          pending_wait_timeout_latch.signal ();
      });
    dispatch_until (connector, pending_wait_timeout_latch, std::chrono::milliseconds (100));
    if (!pending_wait_timeout_seen) {
        return 84;
    }

    zlink::stream_connector::connector_options_t immediate_options;
    immediate_options.endpoint = endpoint;
    immediate_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto immediate = zlink::stream_connector::connector_factory_t::create (immediate_options);
    if (!immediate.connect ()) {
        return 9;
    }
    auto invalid_typed_wait =
      immediate.wait_for<auto_payload_t> ("server.wait.invalid-json")
        .timeout (std::chrono::milliseconds (100))
        .to_future ("typed wait payload decode failed");
    zlink::stream_connector::detail::connector_runtime_t::from (immediate)
      .receive_packet (zlink::stream_connector::packet_t{
        "server.wait.invalid-json",
        {},
        zlink::stream_connector::codec_t::json,
        false,
        zlink::message_t::from (std::string ("{not-json"))});
    bool typed_wait_decode_failed = false;
    try {
        (void) invalid_typed_wait.get ();
    }
    catch (const std::runtime_error &) {
        typed_wait_decode_failed = true;
    }
    catch (const std::future_error &) {
        immediate.close ();
        return 203;
    }
    if (!typed_wait_decode_failed) {
        immediate.close ();
        return 203;
    }
    auto immediate_coroutine = zlink::stream_e2e_client::use (immediate);
    auto immediate_none = immediate_coroutine.expect_none ("server.none.coroutine")
                            .within (std::chrono::milliseconds (5))
                            .async ()
                            .result ();
    if (!immediate_none) {
        return 202;
    }
    int immediate_count = 0;
    immediate.on<zlink::stream_connector::packet_t> (
      "server.push", [&] (const zlink::stream_connector::packet_t &) { ++immediate_count; });
    zlink::stream_connector::detail::connector_runtime_t::from (immediate).receive_packet (
      zlink::stream_connector::packet_t{"server.push",
                                        {},
                                        zlink::stream_connector::codec_t::raw,
                                        false,
                                        zlink::message_t::from (std::string ("payload"))});
    if (immediate_count != 1 || immediate.pending_dispatch_count () != 0) {
        return 10;
    }
    immediate.close ();

    if (connector.codecs ().supports (zlink::stream_connector::codec_t::message_pack)) {
        return 11;
    }
    auto disconnected = connector.close ();
    if (!disconnected
        || connector.state () != zlink::stream_connector::connection_state_t::closed) {
        return 12;
    }

    connector.send (login_request_t{}).packet_name ("after.close").submit ();
    bool request_after_close_callback_seen = false;
    callback_latch_t request_after_close_latch;
    connector.request (login_request_t{})
      .packet_name ("after.close.request")
      .submit<login_reply_t> ([&] (zlink::stream_connector::result_t<login_reply_t> result) {
          request_after_close_callback_seen =
            !result && result.error_code () == zlink::stream_connector::error_code_t::closed;
          request_after_close_latch.signal ();
      });
    dispatch_until (connector, request_after_close_latch, std::chrono::milliseconds (100));
    if (!request_after_close_callback_seen) {
        return 59;
    }
    bool wait_after_close_callback_seen = false;
    callback_latch_t wait_after_close_latch;
    connector.wait_for<zlink::stream_connector::packet_t> ("after.close.wait")
      .timeout (std::chrono::milliseconds (5))
      .submit ([&] (zlink::stream_connector::result_t<zlink::stream_connector::packet_t> result) {
          wait_after_close_callback_seen =
            !result && result.error_code () == zlink::stream_connector::error_code_t::closed;
          wait_after_close_latch.signal ();
      });
    dispatch_until (connector, wait_after_close_latch, std::chrono::milliseconds (100));
    if (!wait_after_close_callback_seen) {
        return 158;
    }
    auto missing_endpoint = zlink::stream_connector::connector_factory_t::create (
      zlink::stream_connector::connector_options_t{});
    if (missing_endpoint.connect ()
        || missing_endpoint.connect ().error_code ()
             != zlink::stream_connector::error_code_t::configuration_error) {
        return 14;
    }

    zlink::stream_socket_t auto_server (context);
    auto_server.options ().notify (false);
    auto_server.bind ("tcp://127.0.0.1:0");
    const auto auto_endpoint = auto_server.options ().last_endpoint ();
    std::atomic_bool auto_json_seen{false};
    joining_thread_t auto_server_thread ([&auto_server, &auto_json_seen] {
        zlink::received_t inbound;
        if (auto_server.recv (inbound) != 0) {
            return;
        }
        std::string buffer =
          inbound.parts ().empty () ? std::string{} : inbound.parts ()[0].to_string ();
        if (auto frame = try_read_server_frame (buffer)) {
            auto_json_seen =
              frame->header.kind == zlink::stream_connector::message_kind_t::send
              && frame->header.codec == zlink::stream_connector::codec_t::json
              && frame->header.name == auto_payload_t::packet_name
              && nlohmann::json::parse (frame->payload).at ("text").get<std::string> () == "auto";
        }
        inbound.close ();
    });

    zlink::stream_connector::connector_options_t auto_options;
    auto_options.endpoint = auto_endpoint;
    auto_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
    auto auto_connector = zlink::stream_connector::connector_factory_t::create (auto_options);
    if (!auto_connector.connect ()) {
        return 30;
    }
    auto_connector.send (auto_payload_t{"auto"}).submit ();
    auto_server_thread.join ();
    if (!auto_json_seen) {
        return 32;
    }

    int auto_dispatch_count = 0;
    zlink::stream_connector::codecs::on<auto_payload_t> (auto_connector,
                                                         [&] (const auto_payload_t &payload) {
                                                             if (payload.text == "callback") {
                                                                 ++auto_dispatch_count;
                                                             }
                                                         });
    zlink::stream_connector::detail::connector_runtime_t::from (auto_connector)
      .receive_packet (zlink::stream_connector::packet_t{
        auto_payload_t::packet_name,
        {},
        zlink::stream_connector::codec_t::json,
        false,
        zlink::message_t::from_json (auto_payload_t{"callback"})});
    if (auto_connector.pending_dispatch_count () != 1 || auto_dispatch_count != 0) {
        return 33;
    }
    if (!auto_connector.dispatch () || auto_dispatch_count != 1
        || auto_connector.pending_dispatch_count () != 0) {
        return 34;
    }
    zlink::stream_connector::detail::connector_runtime_t::from (auto_connector)
      .receive_packet (zlink::stream_connector::packet_t{
        auto_payload_t::packet_name,
        {},
        zlink::stream_connector::codec_t::json,
        false,
        zlink::message_t::from_json (auto_payload_t{"typed-wait"})});
    auto auto_typed_wait = auto_connector.wait_for<auto_payload_t> ().submit ();
    if (!auto_typed_wait || auto_typed_wait.value ().text != "typed-wait") {
        return 78;
    }
    zlink::stream_connector::detail::connector_runtime_t::from (auto_connector)
      .receive_packet (zlink::stream_connector::packet_t{
        auto_payload_t::packet_name,
        {},
        zlink::stream_connector::codec_t::json,
        false,
        zlink::message_t::from_json (auto_payload_t{"typed-filter-skipped"})});
    zlink::stream_connector::detail::connector_runtime_t::from (auto_connector)
      .receive_packet (zlink::stream_connector::packet_t{
        auto_payload_t::packet_name,
        {},
        zlink::stream_connector::codec_t::json,
        false,
        zlink::message_t::from_json (auto_payload_t{"typed-filtered"})});
    auto auto_filtered_wait =
      auto_connector.wait_for<auto_payload_t> ()
        .where ([] (const auto_payload_t &payload) { return payload.text == "typed-filtered"; })
        .submit ();
    if (!auto_filtered_wait || auto_filtered_wait.value ().text != "typed-filtered") {
        return 79;
    }
    zlink::stream_connector::detail::connector_runtime_t::from (auto_connector)
      .receive_packet (zlink::stream_connector::packet_t{
        auto_payload_t::packet_name,
        {},
        zlink::stream_connector::codec_t::json,
        false,
        zlink::message_t::from_json (auto_payload_t{"member-filter-skipped"})});
    zlink::stream_connector::detail::connector_runtime_t::from (auto_connector)
      .receive_packet (zlink::stream_connector::packet_t{
        auto_payload_t::packet_name,
        {},
        zlink::stream_connector::codec_t::json,
        false,
        zlink::message_t::from_json (auto_payload_t{"member-filtered"})});
    auto auto_member_filtered_wait =
      auto_connector.wait_for<auto_payload_t> ()
        .where (&auto_payload_t::text, std::string ("member-filtered"))
        .submit ();
    if (!auto_member_filtered_wait
        || auto_member_filtered_wait.value ().text != "member-filtered") {
        return 107;
    }
    auto_connector.close ();

    zlink::stream_socket_t timeout_server (context);
    timeout_server.options ().notify (false);
    timeout_server.bind ("tcp://127.0.0.1:0");
    const auto timeout_endpoint = timeout_server.options ().last_endpoint ();
    std::atomic_bool timed_request_seen{false};
    joining_thread_t timeout_server_thread ([&timeout_server, &timed_request_seen] {
        zlink::received_t inbound;
        if (timeout_server.recv (inbound) != 0) {
            return;
        }
        timed_request_seen = true;
        inbound.close ();
    });
    zlink::stream_connector::connector_options_t timeout_options;
    timeout_options.endpoint = timeout_endpoint;
    timeout_options.request_timeout = std::chrono::milliseconds (5);
    auto timeout_connector = zlink::stream_connector::connector_factory_t::create (timeout_options);
    if (!timeout_connector.connect ()) {
        return 35;
    }
    auto timeout_reply = timeout_connector.request (login_request_t{})
                           .packet_name ("timeout.request")
                           .timeout (std::chrono::milliseconds (5))
                           .submit<login_reply_t> ();
    timeout_server_thread.join ();
    if (timeout_reply
        || timeout_reply.error_code () != zlink::stream_connector::error_code_t::request_timeout
        || !timed_request_seen) {
        return 36;
    }
    timeout_connector.close ();

    zlink::stream_socket_t error_reply_server (context);
    error_reply_server.options ().notify (false);
    error_reply_server.bind ("tcp://127.0.0.1:0");
    const auto error_reply_endpoint = error_reply_server.options ().last_endpoint ();
    joining_thread_t error_reply_server_thread ([&error_reply_server] {
        zlink::received_t inbound;
        if (error_reply_server.recv (inbound) != 0) {
            return;
        }
        std::string buffer =
          inbound.parts ().empty () ? std::string{} : inbound.parts ()[0].to_string ();
        if (auto frame = try_read_server_frame (buffer)) {
            auto reply = make_server_frame (zlink::stream_connector::message_kind_t::error,
                                            frame->header.request_seq.value (), frame->header.name,
                                            "{\"code\":\"server_closed\","
                                            "\"message\":\"server closed request\"}");
            inbound.send ().message (reply).submit ();
        }
        inbound.close ();
    });
    zlink::stream_connector::connector_options_t error_reply_options;
    error_reply_options.endpoint = error_reply_endpoint;
    error_reply_options.request_timeout = std::chrono::milliseconds (100);
    auto error_reply_connector =
      zlink::stream_connector::connector_factory_t::create (error_reply_options);
    if (!error_reply_connector.connect ()) {
        return 150;
    }
    auto error_reply = error_reply_connector.request (login_request_t{})
                         .packet_name ("error.reply.request")
                         .timeout (std::chrono::milliseconds (100))
                         .submit<login_reply_t> ();
    error_reply_server_thread.join ();
    if (error_reply
        || error_reply.error_code () != zlink::stream_connector::error_code_t::remote_error
        || !error_reply.error ()
        || error_reply.error ()->message.find ("server closed request") == std::string::npos) {
        return 151;
    }
    error_reply_connector.close ();

    zlink::stream_socket_t callback_response_server (context);
    callback_response_server.options ().notify (false);
    callback_response_server.bind ("tcp://127.0.0.1:0");
    const auto callback_response_endpoint = callback_response_server.options ().last_endpoint ();
    joining_thread_t callback_response_thread ([&callback_response_server] {
        zlink::received_t inbound;
        if (callback_response_server.recv (inbound) != 0) {
            return;
        }
        std::string buffer =
          inbound.parts ().empty () ? std::string{} : inbound.parts ()[0].to_string ();
        if (auto frame = try_read_server_frame (buffer)) {
            auto reply = make_server_frame (zlink::stream_connector::message_kind_t::response,
                                            frame->header.request_seq.value (), frame->header.name,
                                            "ok");
            inbound.send ().message (reply).submit ();
        }
        inbound.close ();
    });
    zlink::stream_connector::connector_options_t callback_response_options;
    callback_response_options.endpoint = callback_response_endpoint;
    callback_response_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto callback_response_connector =
      zlink::stream_connector::connector_factory_t::create (callback_response_options);
    if (!callback_response_connector.connect ()) {
        return 60;
    }
    bool request_callback_response_seen = false;
    callback_latch_t request_callback_response_latch;
    callback_response_connector.request (login_request_t{})
      .packet_name ("callback.response.request")
      .timeout (std::chrono::milliseconds (100))
      .submit<login_reply_t> ([&] (zlink::stream_connector::result_t<login_reply_t> result) {
          request_callback_response_seen = static_cast<bool> (result);
          request_callback_response_latch.signal ();
      });
    callback_response_thread.join ();
    request_callback_response_latch.wait_for (std::chrono::milliseconds (100));
    if (!request_callback_response_seen) {
        return 61;
    }
    callback_response_connector.close ();

    zlink::stream_socket_t callback_timeout_server (context);
    callback_timeout_server.options ().notify (false);
    callback_timeout_server.bind ("tcp://127.0.0.1:0");
    const auto callback_timeout_endpoint = callback_timeout_server.options ().last_endpoint ();
    std::atomic_bool callback_timeout_request_seen{false};
    joining_thread_t callback_timeout_thread (
      [&callback_timeout_server, &callback_timeout_request_seen] {
          zlink::received_t inbound;
          if (callback_timeout_server.recv (inbound) != 0) {
              return;
          }
          callback_timeout_request_seen = true;
          inbound.close ();
      });
    zlink::stream_connector::connector_options_t callback_timeout_options;
    callback_timeout_options.endpoint = callback_timeout_endpoint;
    callback_timeout_options.request_timeout = std::chrono::milliseconds (5);
    callback_timeout_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto callback_timeout_connector =
      zlink::stream_connector::connector_factory_t::create (callback_timeout_options);
    if (!callback_timeout_connector.connect ()) {
        return 62;
    }
    bool request_callback_timeout_seen = false;
    callback_latch_t request_callback_timeout_latch;
    callback_timeout_connector.request (login_request_t{})
      .packet_name ("callback.timeout.request")
      .timeout (std::chrono::milliseconds (5))
      .submit<login_reply_t> ([&] (zlink::stream_connector::result_t<login_reply_t> result) {
          request_callback_timeout_seen =
            !result
            && result.error_code () == zlink::stream_connector::error_code_t::request_timeout;
          request_callback_timeout_latch.signal ();
      });
    callback_timeout_thread.join ();
    request_callback_timeout_latch.wait_for (std::chrono::milliseconds (100));
    if (!request_callback_timeout_seen || !callback_timeout_request_seen) {
        return 63;
    }
    callback_timeout_connector.close ();

    zlink::stream_socket_t async_pump_server (context);
    async_pump_server.options ().notify (false);
    async_pump_server.bind ("tcp://127.0.0.1:0");
    const auto async_pump_endpoint = async_pump_server.options ().last_endpoint ();
    joining_thread_t async_pump_thread ([&async_pump_server] {
        zlink::received_t inbound;
        if (async_pump_server.recv (inbound) != 0) {
            return;
        }
        std::string buffer =
          inbound.parts ().empty () ? std::string{} : inbound.parts ()[0].to_string ();
        if (auto frame = try_read_server_frame (buffer)) {
            auto push = make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                           "async.pump.push", "push");
            inbound.send ().message (push).submit ();
            auto reply =
              make_server_frame (zlink::stream_connector::message_kind_t::response,
                                 frame->header.request_seq.value (), frame->header.name,
                                 "async-pump-reply");
            inbound.send ().message (reply).submit ();
        }
        inbound.close ();
    });
    zlink::stream_connector::connector_options_t async_pump_options;
    async_pump_options.endpoint = async_pump_endpoint;
    async_pump_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    async_pump_options.request_timeout = std::chrono::milliseconds (100);
    auto async_pump_connector =
      zlink::stream_connector::connector_factory_t::create (async_pump_options);
    if (!async_pump_connector.connect ()) {
        async_pump_thread.join ();
        return 156;
    }
    bool async_pump_wait_seen = false;
    callback_latch_t async_pump_wait_latch;
    async_pump_connector.wait_for<zlink::stream_connector::packet_t> ("async.pump.push")
      .timeout (std::chrono::milliseconds (100))
      .submit ([&] (zlink::stream_connector::result_t<zlink::stream_connector::packet_t> result) {
          async_pump_wait_seen = result && result.value ().payload.to_string () == "push";
          async_pump_wait_latch.signal ();
      });
    auto async_pump_reply = async_pump_connector.request (login_request_t{})
                              .packet_name ("async.pump.request")
                              .timeout (std::chrono::milliseconds (100))
                              .submit<login_reply_t> ();
    async_pump_wait_latch.wait_for (std::chrono::milliseconds (100));
    async_pump_thread.join ();
    async_pump_connector.close ();
    if (!async_pump_reply || !async_pump_wait_seen) {
        return 157;
    }

    zlink::stream_socket_t close_cleanup_server (context);
    close_cleanup_server.options ().notify (false);
    close_cleanup_server.bind ("tcp://127.0.0.1:0");
    const auto close_cleanup_endpoint = close_cleanup_server.options ().last_endpoint ();
    callback_latch_t close_cleanup_request_latch;
    callback_latch_t close_cleanup_release_latch;
    joining_thread_t close_cleanup_thread (
      [&close_cleanup_server, &close_cleanup_request_latch, &close_cleanup_release_latch] {
          zlink::received_t inbound;
          if (close_cleanup_server.recv (inbound) != 0) {
              return;
          }
          close_cleanup_request_latch.signal ();
          close_cleanup_release_latch.wait_for (std::chrono::milliseconds (100));
          inbound.close ();
      });
    zlink::stream_connector::connector_options_t close_cleanup_options;
    close_cleanup_options.endpoint = close_cleanup_endpoint;
    close_cleanup_options.request_timeout = std::chrono::milliseconds (1000);
    close_cleanup_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto close_cleanup_connector =
      zlink::stream_connector::connector_factory_t::create (close_cleanup_options);
    if (!close_cleanup_connector.connect ()) {
        close_cleanup_release_latch.signal ();
        close_cleanup_thread.join ();
        return 80;
    }
    bool close_cleanup_seen = false;
    callback_latch_t close_cleanup_callback_latch;
    const auto close_cleanup_submit_started = std::chrono::steady_clock::now ();
    close_cleanup_connector.request (login_request_t{})
      .packet_name ("close.cleanup.request")
      .timeout (std::chrono::milliseconds (1000))
      .submit<login_reply_t> ([&] (zlink::stream_connector::result_t<login_reply_t> result) {
          close_cleanup_seen =
            !result && result.error_code () == zlink::stream_connector::error_code_t::closed;
          close_cleanup_callback_latch.signal ();
      });
    const auto close_cleanup_submit_elapsed =
      std::chrono::steady_clock::now () - close_cleanup_submit_started;
    if (close_cleanup_submit_elapsed > std::chrono::milliseconds (50)) {
        close_cleanup_release_latch.signal ();
        close_cleanup_thread.join ();
        return 96;
    }
    if (!close_cleanup_request_latch.wait_for (std::chrono::milliseconds (100))) {
        close_cleanup_release_latch.signal ();
        close_cleanup_thread.join ();
        return 81;
    }
    close_cleanup_connector.close ();
    close_cleanup_release_latch.signal ();
    close_cleanup_thread.join ();
    close_cleanup_callback_latch.wait_for (std::chrono::milliseconds (100));
    if (!close_cleanup_seen) {
        return 82;
    }

    zlink::stream_socket_t coroutine_server (context);
    coroutine_server.options ().notify (false);
    coroutine_server.bind ("tcp://127.0.0.1:0");
    const auto coroutine_endpoint = coroutine_server.options ().last_endpoint ();
    std::atomic_bool coroutine_send_seen{false};
    std::atomic_bool coroutine_request_seen{false};
    joining_thread_t coroutine_thread ([&coroutine_server, &coroutine_send_seen,
                                        &coroutine_request_seen] {
        std::string buffer;
        while (!coroutine_send_seen || !coroutine_request_seen) {
            zlink::received_t inbound;
            if (coroutine_server.recv (inbound) != 0) {
                return;
            }
            buffer += inbound.parts ().empty () ? std::string{} : inbound.parts ()[0].to_string ();
            while (auto frame = try_read_server_frame (buffer)) {
                if (frame->header.kind == zlink::stream_connector::message_kind_t::send
                    && frame->header.name == "coroutine.send") {
                    coroutine_send_seen = true;
                }
                if (frame->header.kind == zlink::stream_connector::message_kind_t::request
                    && frame->header.name == "coroutine.request") {
                    coroutine_request_seen = true;
                    auto reply =
                      make_server_frame (zlink::stream_connector::message_kind_t::response,
                                         frame->header.request_seq.value (), frame->header.name,
                                         "ok");
                    inbound.send ().message (reply).submit ();
                }
            }
            inbound.close ();
        }
    });
    zlink::stream_connector::connector_options_t coroutine_options;
    coroutine_options.endpoint = coroutine_endpoint;
    coroutine_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto coroutine_connector =
      zlink::stream_connector::connector_factory_t::create (coroutine_options);
    if (!coroutine_connector.connect ()) {
        return 73;
    }
    auto coroutine_adapter = zlink::stream_e2e_client::use (coroutine_connector);
    auto coroutine_send = send_with_coroutine_submit (coroutine_adapter).result ();
    if (!coroutine_send) {
        return 74;
    }
    auto coroutine_request = request_with_coroutine_submit (coroutine_adapter).result ();
    coroutine_thread.join ();
    if (!coroutine_request || !coroutine_send_seen || !coroutine_request_seen) {
        return 75;
    }
    coroutine_connector.close ();

    boost::asio::io_context partial_io;
    boost::asio::ip::tcp::acceptor partial_acceptor (
      partial_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
    const auto partial_endpoint = std::string ("tcp://127.0.0.1:")
                                  + std::to_string (partial_acceptor.local_endpoint ().port ());
    std::atomic_bool partial_write_seen{false};
    joining_thread_t partial_server_thread ([&partial_acceptor, &partial_write_seen] {
        boost::asio::ip::tcp::socket socket (partial_acceptor.get_executor ());
        partial_acceptor.accept (socket);
        std::array<char, 256> request_buffer{};
        boost::system::error_code error;
        socket.read_some (boost::asio::buffer (request_buffer), error);
        if (error) {
            return;
        }
        const auto frame = make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                              "server.partial", "split")
                             .to_string ();
        socket.write_some (boost::asio::buffer (frame.data (), 3), error);
        if (error) {
            return;
        }
        callback_latch_t partial_frame_delay;
        partial_frame_delay.wait_for (std::chrono::milliseconds (2));
        socket.write_some (boost::asio::buffer (frame.data () + 3, frame.size () - 3), error);
        partial_write_seen = !error;
    });
    zlink::stream_connector::connector_options_t partial_options;
    partial_options.endpoint = partial_endpoint;
    partial_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
    auto partial_connector = zlink::stream_connector::connector_factory_t::create (partial_options);
    if (!partial_connector.connect ()) {
        return 64;
    }
    partial_connector.send (login_request_t{}).packet_name ("partial.trigger").submit ();
    auto partial_packet =
      partial_connector.wait_for ("server.partial", std::chrono::milliseconds (100));
    partial_server_thread.join ();
    if (!partial_packet || !partial_write_seen || partial_packet.value ().name != "server.partial"
        || partial_packet.value ().payload.to_string () != "split") {
        return 66;
    }
    partial_connector.close ();

    zlink::stream_socket_t large_receive_server (context);
    large_receive_server.options ().notify (false);
    large_receive_server.bind ("tcp://127.0.0.1:0");
    const auto large_receive_endpoint = large_receive_server.options ().last_endpoint ();
    const std::string large_receive_payload (70 * 1024, 'l');
    joining_thread_t large_receive_server_thread ([&large_receive_server, &large_receive_payload] {
        zlink::received_t inbound;
        if (large_receive_server.recv (inbound) != 0) {
            return;
        }
        auto frame = make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                        "server.large", large_receive_payload);
        inbound.send ().message (frame).submit ();
        inbound.close ();
    });
    zlink::stream_connector::connector_options_t large_receive_options;
    large_receive_options.endpoint = large_receive_endpoint;
    large_receive_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
    large_receive_options.max_send_payload_size = 16;
    large_receive_options.max_receive_payload_size = 80 * 1024;
    auto large_receive_connector =
      zlink::stream_connector::connector_factory_t::create (large_receive_options);
    if (!large_receive_connector.connect ()) {
        return 70;
    }
    large_receive_connector.send (login_request_t{})
      .packet_name ("large.receive.trigger")
      .submit ();
    auto large_received =
      large_receive_connector.wait_for ("server.large", std::chrono::milliseconds (100));
    large_receive_server_thread.join ();
    if (!large_received || large_received.value ().name != "server.large"
        || large_received.value ().payload.to_string ().size () != large_receive_payload.size ()) {
        return 72;
    }
    large_receive_connector.close ();

    zlink::stream_socket_t oversized_receive_server (context);
    oversized_receive_server.options ().notify (false);
    oversized_receive_server.bind ("tcp://127.0.0.1:0");
    const auto oversized_receive_endpoint = oversized_receive_server.options ().last_endpoint ();
    joining_thread_t oversized_receive_server_thread ([&oversized_receive_server] {
        zlink::received_t inbound;
        if (oversized_receive_server.recv (inbound) != 0) {
            return;
        }
        inbound.send ().message (make_frame_prefix (0, 17)).submit ();
        // Allow the frame to reach the connector before closing the test peer.
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
        inbound.close ();
    });
    zlink::stream_connector::connector_options_t oversized_receive_options;
    oversized_receive_options.endpoint = oversized_receive_endpoint;
    oversized_receive_options.max_receive_payload_size = 16;
    auto oversized_receive_connector =
      zlink::stream_connector::connector_factory_t::create (oversized_receive_options);
    if (!oversized_receive_connector.connect ()) {
        return 120;
    }
    auto oversized_receive_result = oversized_receive_connector.request (login_request_t{})
                                      .packet_name ("oversized.receive.request")
                                      .timeout (std::chrono::milliseconds (100))
                                      .submit<login_reply_t> ();
    oversized_receive_server_thread.join ();
    if (oversized_receive_result
        || oversized_receive_result.error_code ()
             != zlink::stream_connector::error_code_t::frame_too_large) {
        return 121;
    }
    oversized_receive_connector.close ();

    zlink::stream_socket_t async_oversized_receive_server (context);
    async_oversized_receive_server.options ().notify (false);
    async_oversized_receive_server.bind ("tcp://127.0.0.1:0");
    const auto async_oversized_receive_endpoint =
      async_oversized_receive_server.options ().last_endpoint ();
    joining_thread_t async_oversized_receive_server_thread ([&async_oversized_receive_server] {
        zlink::received_t inbound;
        if (async_oversized_receive_server.recv (inbound) != 0) {
            return;
        }
        inbound.send ().message (make_frame_prefix (0, 17)).submit ();
        inbound.close ();
    });
    zlink::stream_connector::connector_options_t async_oversized_receive_options;
    async_oversized_receive_options.endpoint = async_oversized_receive_endpoint;
    async_oversized_receive_options.max_receive_payload_size = 16;
    async_oversized_receive_options.dispatch_mode =
      zlink::stream_connector::dispatch_mode_t::immediate;
    auto async_oversized_receive_connector =
      zlink::stream_connector::connector_factory_t::create (async_oversized_receive_options);
    if (!async_oversized_receive_connector.connect ()) {
        return 122;
    }
    bool async_oversized_receive_seen = false;
    callback_latch_t async_oversized_receive_latch;
    async_oversized_receive_connector.request (login_request_t{})
      .packet_name ("async.oversized.receive.request")
      .timeout (std::chrono::milliseconds (100))
      .submit<login_reply_t> ([&] (zlink::stream_connector::result_t<login_reply_t> result) {
          async_oversized_receive_seen =
            !result
            && result.error_code () == zlink::stream_connector::error_code_t::frame_too_large;
          async_oversized_receive_latch.signal ();
      });
    async_oversized_receive_server_thread.join ();
    async_oversized_receive_latch.wait_for (std::chrono::milliseconds (100));
    if (!async_oversized_receive_seen) {
        return 123;
    }
    async_oversized_receive_connector.close ();

    zlink::stream_socket_t oversized_wait_server (context);
    oversized_wait_server.options ().notify (false);
    oversized_wait_server.bind ("tcp://127.0.0.1:0");
    const auto oversized_wait_endpoint = oversized_wait_server.options ().last_endpoint ();
    std::atomic_bool oversized_wait_release{false};
    joining_thread_t oversized_wait_server_thread (
      [&oversized_wait_server, &oversized_wait_release] {
          zlink::received_t inbound;
          if (oversized_wait_server.recv (inbound) != 0) {
              return;
          }
          inbound.send ().message (make_frame_prefix (0, 17)).submit ();
          const auto deadline =
            std::chrono::steady_clock::now () + std::chrono::seconds (3);
          while (!oversized_wait_release
                 && std::chrono::steady_clock::now () < deadline) {
              std::this_thread::sleep_for (std::chrono::milliseconds (1));
          }
          inbound.close ();
      });
    zlink::stream_connector::connector_options_t oversized_wait_options;
    oversized_wait_options.endpoint = oversized_wait_endpoint;
    oversized_wait_options.max_receive_payload_size = 16;
    auto oversized_wait_connector =
      zlink::stream_connector::connector_factory_t::create (oversized_wait_options);
    if (!oversized_wait_connector.connect ()) {
        return 124;
    }
    oversized_wait_connector.send (login_request_t{})
      .packet_name ("oversized.wait.trigger")
      .submit ();
    auto oversized_wait_result =
      oversized_wait_connector.wait_for ("oversized.wait", std::chrono::milliseconds (1500));
    oversized_wait_release = true;
    oversized_wait_server_thread.join ();
    if (oversized_wait_result
        || oversized_wait_result.error_code ()
             != zlink::stream_connector::error_code_t::frame_too_large) {
        return 126;
    }
    oversized_wait_connector.close ();

    zlink::stream_socket_t heartbeat_server (context);
    heartbeat_server.options ().notify (false);
    heartbeat_server.bind ("tcp://127.0.0.1:0");
    const auto heartbeat_endpoint = heartbeat_server.options ().last_endpoint ();
    std::atomic_bool heartbeat_seen{false};
    joining_thread_t heartbeat_server_thread ([&heartbeat_server, &heartbeat_seen] {
        zlink::received_t inbound;
        if (heartbeat_server.recv (inbound) != 0) {
            return;
        }
        std::string buffer =
          inbound.parts ().empty () ? std::string{} : inbound.parts ()[0].to_string ();
        if (auto frame = try_read_server_frame (buffer)) {
            heartbeat_seen = frame->header.kind == zlink::stream_connector::message_kind_t::control
                             && frame->header.name == "$zlink.heartbeat.ping";
            auto pong = make_server_frame (zlink::stream_connector::message_kind_t::control, 0,
                                           "$zlink.heartbeat.pong", "");
            inbound.send ().message (pong).submit ();
        }
        inbound.close ();
    });
    zlink::stream_connector::connector_options_t heartbeat_options;
    heartbeat_options.endpoint = heartbeat_endpoint;
    heartbeat_options.heartbeat.interval = std::chrono::milliseconds (0);
    auto heartbeat_connector =
      zlink::stream_connector::connector_factory_t::create (heartbeat_options);
    if (!heartbeat_connector.connect () || !heartbeat_connector.dispatch ()) {
        return 37;
    }
    heartbeat_server_thread.join ();
    if (!heartbeat_seen) {
        return 38;
    }
    bool heartbeat_control_delivered = false;
    heartbeat_connector.on<zlink::stream_connector::packet_t> (
      "$zlink.heartbeat.pong",
      [&] (const zlink::stream_connector::packet_t &) { heartbeat_control_delivered = true; });
    if (!heartbeat_connector.dispatch () || heartbeat_control_delivered
        || heartbeat_connector.pending_dispatch_count () != 0) {
        return 44;
    }
    heartbeat_connector.close ();

    /* graceful-drain-handoff §7.2: 동기 request가 응답을 기다리는 동안에도 server ping에
     * pong으로 답해야 한다. pong을 dispatch() 경로에만 두면, 응답이 heartbeat timeout보다
     * 오래 걸리는 정상 요청에서 서버가 세션을 끊는다(E2E ATD-C3B). */
    zlink::stream_socket_t pong_during_request_server (context);
    pong_during_request_server.options ().notify (false);
    pong_during_request_server.bind ("tcp://127.0.0.1:0");
    const auto pong_during_request_endpoint =
      pong_during_request_server.options ().last_endpoint ();
    std::atomic_bool pong_during_request_seen{false};
    joining_thread_t pong_during_request_thread (
      [&pong_during_request_server, &pong_during_request_seen] {
          zlink::received_t inbound;
          if (pong_during_request_server.recv (inbound) != 0) {
              return;
          }
          std::string buffer =
            inbound.parts ().empty () ? std::string{} : inbound.parts ()[0].to_string ();
          auto request = try_read_server_frame (buffer);
          if (!request || !request->header.request_seq) {
              inbound.close ();
              return;
          }
          /* 요청을 받아 두고 응답을 미룬 채 ping을 보낸다. */
          auto ping = make_server_frame (zlink::stream_connector::message_kind_t::control, 0,
                                         "$zlink.heartbeat.ping", "");
          inbound.send ().message (ping).submit ();

          /* 클라이언트가 dispatch()를 부르지 않는 동안 pong이 오는지 본다. */
          const auto deadline =
            std::chrono::steady_clock::now () + std::chrono::milliseconds (3000);
          std::string pong_buffer;
          while (std::chrono::steady_clock::now () < deadline && !pong_during_request_seen) {
              zlink::received_t pong_inbound;
              if (pong_during_request_server.recv (pong_inbound) != 0) {
                  break;
              }
              pong_buffer += pong_inbound.parts ().empty ()
                               ? std::string{}
                               : pong_inbound.parts ()[0].to_string ();
              if (auto frame = try_read_server_frame (pong_buffer)) {
                  pong_during_request_seen =
                    frame->header.kind == zlink::stream_connector::message_kind_t::control
                    && frame->header.name == "$zlink.heartbeat.pong";
              }
          }

          auto reply = make_server_frame (zlink::stream_connector::message_kind_t::response,
                                          *request->header.request_seq, request->header.name, "{}");
          inbound.send ().message (reply).submit ();
          inbound.close ();
      });
    zlink::stream_connector::connector_options_t pong_during_request_options;
    pong_during_request_options.endpoint = pong_during_request_endpoint;
    pong_during_request_options.heartbeat.interval = std::chrono::milliseconds (0);
    /* manual dispatch: application이 dispatch()를 부를 때까지 pump가 돌지 않는다. 동기 request가
     * 응답을 기다리는 동안 pong을 dispatch() 경로에만 두면 이 모드에서 답이 나가지 않는다. */
    pong_during_request_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
    auto pong_during_request_connector =
      zlink::stream_connector::connector_factory_t::create (pong_during_request_options);
    if (!pong_during_request_connector.connect ()) {
        return 190;
    }
    auto pong_during_request_reply = pong_during_request_connector.request (login_request_t{})
                                       .packet_name ("slow.request")
                                       .timeout (std::chrono::milliseconds (5000))
                                       .submit<login_reply_t> ();
    pong_during_request_thread.join ();
    pong_during_request_connector.close ();
    if (!pong_during_request_seen) {
        return 191;
    }
    if (!pong_during_request_reply) {
        return 192;
    }

    zlink::stream_socket_t heartbeat_timeout_server (context);
    heartbeat_timeout_server.options ().notify (false);
    heartbeat_timeout_server.bind ("tcp://127.0.0.1:0");
    zlink::stream_connector::connector_options_t heartbeat_timeout_options;
    heartbeat_timeout_options.endpoint = heartbeat_timeout_server.options ().last_endpoint ();
    heartbeat_timeout_options.heartbeat.timeout = std::chrono::milliseconds (0);
    // Keep the reconnecting state observable long enough to assert the
    // timeout transition instead of racing an immediate reconnect.
    heartbeat_timeout_options.reconnect.initial_delay = std::chrono::milliseconds (100);
    auto heartbeat_timeout_connector =
      zlink::stream_connector::connector_factory_t::create (heartbeat_timeout_options);
    if (!heartbeat_timeout_connector.connect ()) {
        return 45;
    }
    const auto heartbeat_timeout_deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (250);
    while (heartbeat_timeout_connector.state ()
             != zlink::stream_connector::connection_state_t::reconnecting
           && std::chrono::steady_clock::now () < heartbeat_timeout_deadline) {
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    if (heartbeat_timeout_connector.state ()
        != zlink::stream_connector::connection_state_t::reconnecting) {
        return 46;
    }
    heartbeat_timeout_connector.close ();

    boost::asio::io_context websocket_io;
    boost::asio::ip::tcp::acceptor websocket_acceptor (
      websocket_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
    const auto websocket_endpoint = std::string ("ws://127.0.0.1:")
                                    + std::to_string (websocket_acceptor.local_endpoint ().port ())
                                    + "/stream";
    std::atomic_bool websocket_send_seen{false};
    joining_thread_t websocket_server_thread ([&websocket_acceptor, &websocket_send_seen] {
        boost::asio::ip::tcp::socket socket (websocket_acceptor.get_executor ());
        websocket_acceptor.accept (socket);
        boost::beast::websocket::stream<boost::asio::ip::tcp::socket> websocket (
          std::move (socket));
        websocket.accept ();
        boost::beast::flat_buffer buffer;
        websocket.read (buffer);
        auto frame_text = boost::beast::buffers_to_string (buffer.data ());
        if (auto frame = try_read_server_frame (frame_text)) {
            websocket_send_seen =
              frame->header.kind == zlink::stream_connector::message_kind_t::send
              && frame->header.name == login_request_t::packet_name && websocket.got_binary ();
        }
        boost::system::error_code ignored;
        websocket.close (boost::beast::websocket::close_code::normal, ignored);
    });

    zlink::stream_connector::connector_options_t websocket_options;
    websocket_options.endpoint = websocket_endpoint;
    websocket_options.transport = zlink::stream_connector::transport_t::websocket;
    auto websocket_connector =
      zlink::stream_connector::connector_factory_t::create (websocket_options);
    if (!websocket_connector.connect ()) {
        return 47;
    }
    websocket_connector.send (login_request_t{}).submit ();
    const auto websocket_send_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (!websocket_send_seen && std::chrono::steady_clock::now () < websocket_send_deadline) {
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    websocket_connector.close ();
    websocket_server_thread.join ();
    if (!websocket_send_seen) {
        return 49;
    }

#ifdef ZLINK_STREAM_CONNECTOR_TEST_WITH_OPENSSL
    boost::asio::io_context tls_io;
    boost::asio::ip::tcp::acceptor tls_acceptor (tls_io,
                                                 {boost::asio::ip::make_address ("127.0.0.1"), 0});
    const auto tls_endpoint =
      std::string ("tls://localhost:") + std::to_string (tls_acceptor.local_endpoint ().port ());
    std::atomic_bool tls_send_seen{false};
    joining_thread_t tls_server_thread ([&tls_acceptor, &tls_send_seen] {
        openssl_thread_cleanup_t openssl_cleanup;
        boost::asio::ssl::context tls_context (boost::asio::ssl::context::tls_server);
        tls_context.use_certificate_chain_file (ZLINK_STREAM_CONNECTOR_TEST_CERT);
        tls_context.use_private_key_file (ZLINK_STREAM_CONNECTOR_TEST_KEY,
                                          boost::asio::ssl::context::pem);
        boost::asio::ip::tcp::socket socket (tls_acceptor.get_executor ());
        tls_acceptor.accept (socket);
        boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream (std::move (socket),
                                                                       tls_context);
        boost::system::error_code error;
        stream.handshake (boost::asio::ssl::stream_base::server, error);
        if (error) {
            return;
        }
        std::string buffer;
        std::array<char, 1024> chunk{};
        while (!tls_send_seen) {
            const auto read = stream.read_some (boost::asio::buffer (chunk), error);
            if (error) {
                return;
            }
            buffer.append (chunk.data (), read);
            if (auto frame = try_read_server_frame (buffer)) {
                tls_send_seen = frame->header.kind == zlink::stream_connector::message_kind_t::send
                                && frame->header.name == login_request_t::packet_name;
            }
        }
        stream.shutdown (error);
    });

    zlink::stream_connector::connector_options_t tls_options;
    tls_options.endpoint = tls_endpoint;
    tls_options.transport = zlink::stream_connector::transport_t::tls;
    tls_options.skip_server_certificate_validation = true;
    auto tls_connector = zlink::stream_connector::connector_factory_t::create (tls_options);
    if (!tls_connector.connect ()) {
        return 50;
    }
    tls_connector.send (login_request_t{}).submit ();
    const auto tls_send_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (!tls_send_seen && std::chrono::steady_clock::now () < tls_send_deadline) {
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    tls_connector.close ();
    tls_server_thread.join ();
    if (!tls_send_seen) {
        return 52;
    }

    boost::asio::io_context wss_io;
    boost::asio::ip::tcp::acceptor wss_acceptor (wss_io,
                                                 {boost::asio::ip::make_address ("127.0.0.1"), 0});
    const auto wss_endpoint = std::string ("wss://localhost:")
                              + std::to_string (wss_acceptor.local_endpoint ().port ()) + "/stream";
    std::atomic_bool wss_send_seen{false};
    joining_thread_t wss_server_thread ([&wss_acceptor, &wss_send_seen] {
        openssl_thread_cleanup_t openssl_cleanup;
        boost::asio::ssl::context tls_context (boost::asio::ssl::context::tls_server);
        tls_context.use_certificate_chain_file (ZLINK_STREAM_CONNECTOR_TEST_CERT);
        tls_context.use_private_key_file (ZLINK_STREAM_CONNECTOR_TEST_KEY,
                                          boost::asio::ssl::context::pem);
        boost::asio::ip::tcp::socket socket (wss_acceptor.get_executor ());
        wss_acceptor.accept (socket);
        boost::asio::ssl::stream<boost::asio::ip::tcp::socket> tls_stream (std::move (socket),
                                                                           tls_context);
        boost::system::error_code error;
        tls_stream.handshake (boost::asio::ssl::stream_base::server, error);
        if (error) {
            return;
        }
        boost::beast::websocket::stream<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>
          websocket (std::move (tls_stream));
        websocket.accept ();
        boost::beast::flat_buffer buffer;
        websocket.read (buffer);
        auto frame_text = boost::beast::buffers_to_string (buffer.data ());
        if (auto frame = try_read_server_frame (frame_text)) {
            wss_send_seen = frame->header.kind == zlink::stream_connector::message_kind_t::send
                            && frame->header.name == login_request_t::packet_name
                            && websocket.got_binary ();
        }
        websocket.close (boost::beast::websocket::close_code::normal, error);
    });

    zlink::stream_connector::connector_options_t wss_options;
    wss_options.endpoint = wss_endpoint;
    wss_options.transport = zlink::stream_connector::transport_t::websocket_secure;
    wss_options.skip_server_certificate_validation = true;
    auto wss_connector = zlink::stream_connector::connector_factory_t::create (wss_options);
    if (!wss_connector.connect ()) {
        return 53;
    }
    wss_connector.send (login_request_t{}).submit ();
    const auto wss_send_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (!wss_send_seen && std::chrono::steady_clock::now () < wss_send_deadline) {
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    wss_connector.close ();
    wss_server_thread.join ();
    if (!wss_send_seen) {
        return 55;
    }
#endif

    boost::asio::io_context reconnect_success_io;
    boost::asio::ip::tcp::acceptor reserved_reconnect_acceptor (reconnect_success_io);
    reserved_reconnect_acceptor.open (boost::asio::ip::tcp::v4 ());
    reserved_reconnect_acceptor.set_option (boost::asio::socket_base::reuse_address (true));
    reserved_reconnect_acceptor.bind ({boost::asio::ip::make_address ("127.0.0.1"), 0});
    const auto reconnect_success_port = reserved_reconnect_acceptor.local_endpoint ().port ();
    reserved_reconnect_acceptor.close ();
    const auto reconnect_success_endpoint =
      std::string ("tcp://127.0.0.1:") + std::to_string (reconnect_success_port);
    std::atomic_bool reconnect_success_send_seen{false};
    joining_thread_t reconnect_success_server_thread (
      [&reconnect_success_io, reconnect_success_port, &reconnect_success_send_seen] {
          callback_latch_t retry_delay_latch;
          retry_delay_latch.wait_for (std::chrono::milliseconds (5));
          boost::asio::ip::tcp::acceptor acceptor (reconnect_success_io);
          acceptor.open (boost::asio::ip::tcp::v4 ());
          acceptor.set_option (boost::asio::socket_base::reuse_address (true));
          acceptor.bind ({boost::asio::ip::make_address ("127.0.0.1"), reconnect_success_port});
          acceptor.listen ();
          boost::asio::ip::tcp::socket socket (acceptor.get_executor ());
          acceptor.accept (socket);
          std::array<char, 512> request_buffer{};
          boost::system::error_code error;
          const auto read_size = socket.read_some (boost::asio::buffer (request_buffer), error);
          if (error) {
              return;
          }
          std::string frame_text (request_buffer.data (), read_size);
          if (auto frame = try_read_server_frame (frame_text)) {
              reconnect_success_send_seen =
                frame->header.kind == zlink::stream_connector::message_kind_t::send
                && frame->header.name == login_request_t::packet_name;
          }
      });
    zlink::stream_connector::connector_options_t reconnect_success_options;
    reconnect_success_options.endpoint = reconnect_success_endpoint;
    reconnect_success_options.reconnect.initial_delay = std::chrono::milliseconds (10);
    reconnect_success_options.reconnect.max_delay = std::chrono::milliseconds (10);
    reconnect_success_options.reconnect.max_attempts = 4;
    auto reconnect_success_connector =
      zlink::stream_connector::connector_factory_t::create (reconnect_success_options);
    auto reconnect_success_states =
      std::make_shared<std::vector<zlink::stream_connector::connection_state_t>> ();
    reconnect_success_connector.on_connection_state_changed (
      [reconnect_success_states] (
        const zlink::stream_connector::connection_state_changed_t &state) {
          reconnect_success_states->push_back (state.current);
      });
    const auto reconnect_success_result = reconnect_success_connector.connect ();
    if (!reconnect_success_result || !reconnect_success_connector.dispatch ()
        || std::find (reconnect_success_states->begin (), reconnect_success_states->end (),
                   zlink::stream_connector::connection_state_t::reconnecting)
        == reconnect_success_states->end ()) {
        reconnect_success_server_thread.join ();
        return 67;
    }
    reconnect_success_connector.send (login_request_t{}).submit ();
    reconnect_success_connector.close ();
    reconnect_success_server_thread.join ();
    if (!reconnect_success_send_seen) {
        return 69;
    }

    zlink::stream_connector::connector_options_t reconnect_options;
    reconnect_options.endpoint = "tcp://127.0.0.1:1";
    reconnect_options.reconnect.initial_delay = std::chrono::milliseconds (1);
    reconnect_options.reconnect.max_delay = std::chrono::milliseconds (1);
    reconnect_options.reconnect.max_attempts = 2;
    auto reconnect_connector =
      zlink::stream_connector::connector_factory_t::create (reconnect_options);
    auto reconnect_states =
      std::make_shared<std::vector<zlink::stream_connector::connection_state_t>> ();
    reconnect_connector.on_connection_state_changed (
      [reconnect_states] (const zlink::stream_connector::connection_state_changed_t &state) {
          reconnect_states->push_back (state.current);
      });
    auto reconnect_result = reconnect_connector.connect ();
    if (reconnect_result
        || reconnect_result.error_code () != zlink::stream_connector::error_code_t::connect_timeout
        || !reconnect_connector.dispatch ()
        || std::find (reconnect_states->begin (), reconnect_states->end (),
                      zlink::stream_connector::connection_state_t::reconnecting)
             == reconnect_states->end ()) {
        return 39;
    }

    struct endpoint_mismatch_case_t
    {
        zlink::stream_connector::transport_t transport;
        const char *endpoint;
        const char *message_part;
    };

    const endpoint_mismatch_case_t endpoint_mismatch_cases[] = {
      {zlink::stream_connector::transport_t::tcp, "ws://127.0.0.1:1/stream", "tcp://host:port"},
      {zlink::stream_connector::transport_t::websocket, "tcp://127.0.0.1:1", "ws://host:port/path"},
#ifdef ZLINK_STREAM_CONNECTOR_TEST_WITH_OPENSSL
      {zlink::stream_connector::transport_t::tls, "tcp://127.0.0.1:1", "tls://host:port"},
      {zlink::stream_connector::transport_t::websocket_secure, "tcp://127.0.0.1:1",
       "wss://host:port/path"}
#else
      {zlink::stream_connector::transport_t::tls, "tcp://127.0.0.1:1", "does not support"},
      {zlink::stream_connector::transport_t::websocket_secure, "tcp://127.0.0.1:1",
       "does not support"}
#endif
    };
    for (const auto &mismatch_case : endpoint_mismatch_cases) {
        zlink::stream_connector::connector_options_t invalid_transport_options;
        invalid_transport_options.endpoint = mismatch_case.endpoint;
        invalid_transport_options.transport = mismatch_case.transport;
        auto invalid_transport =
          zlink::stream_connector::connector_factory_t::create (invalid_transport_options);
        auto invalid_transport_result = invalid_transport.connect ();
        if (invalid_transport_result
            || invalid_transport_result.error_code ()
                 != zlink::stream_connector::error_code_t::configuration_error
            || !invalid_transport_result.error ()
            || invalid_transport_result.error ()->message.find (mismatch_case.message_part)
                 == std::string::npos
            || invalid_transport.is_connected ()) {
            return 42;
        }
    }

    auto request_after_reconnect_failure = reconnect_connector.request (login_request_t{})
                                             .packet_name ("after.reconnect.failure")
                                             .submit<login_reply_t> ();
    if (request_after_reconnect_failure
        || request_after_reconnect_failure.error_code ()
             != zlink::stream_connector::error_code_t::disconnected) {
        return 43;
    }
    return 0;
}
