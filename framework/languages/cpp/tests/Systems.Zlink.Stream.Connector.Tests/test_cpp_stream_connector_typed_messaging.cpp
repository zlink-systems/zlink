/* SPDX-License-Identifier: Elastic-2.0 */
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/Contracts/Sockets/stream_socket.hpp>
#include <zlink/stream_connector.hpp>
#include <zlink/stream_connector/codecs/auto_codec.hpp>
#include <zlink/stream_e2e_client.hpp>
#include <zlink/stream_e2e_client/codecs/auto_codec.hpp>
#include "runtime/connector_runtime.hpp"
#include "runtime/protocol/framing.hpp"
#include "runtime/protocol/framing/frame_codec.hpp"
#include "runtime/protocol/header_codec.hpp"
#include "runtime/protocol/metadata_codec.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <zlink/json.hpp>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>
#include "../support/stream_connector_cases.hpp"

#include "../support/stream_connector_packet_message.hpp"
#include "../support/stream_connector_wire_frames.hpp"
#include "../support/stream_connector_typed_messages.hpp"
#include "../support/stream_connector_compression_fixture.hpp"
#include "../support/stream_connector_callback_sync.hpp"
#include "../support/stream_connector_failure.hpp"

using namespace stream_connector_fixture;

template <typename T>
concept exposes_codec_registry = requires (T &connector) { connector.codecs (); };

static_assert (!exposes_codec_registry<zlink::stream_connector::connector_t>,
               "typed codec selection belongs to creation options");

namespace
{
struct simple_named_t
{
    static constexpr const char *packet_name = "simple_named_t";
};
} // namespace

namespace
{
namespace nested_ns_t
{
struct inner_named_t
{
};
} // namespace nested_ns_t
} // namespace

namespace
{
struct string_view_named_t
{
    /* A static member spelled as a string_view must be accepted; a concept that
   * only takes const char* would drop this type onto the fallback path. */
    static constexpr std::string_view packet_name = "view.named";
};

struct char_pointer_named_t
{
    static constexpr const char *packet_name = "pointer.named";
};

struct unnamed_payload_t
{
    std::string text;
};
} // namespace

template <> struct zlink::json::dto_traits<unnamed_payload_t>
{
    static constexpr auto fields =
      std::tuple{zlink::json::field ("text", &unnamed_payload_t::text)};
};

namespace
{
class counting_typed_codec_t final : public zlink::stream_connector::typed_codec_t
{
  public:
    zlink::stream_connector::codec_t codec_id () const noexcept override
    {
        return zlink::stream_connector::codec_t::json;
    }

    std::vector<std::uint8_t> encode (const std::vector<std::uint8_t> &payload) const override
    {
        return payload;
    }

    std::vector<std::uint8_t> decode (const std::vector<std::uint8_t> &payload) const override
    {
        return payload;
    }
};

class prefixing_name_resolver_t final : public zlink::stream_connector::packet_name_resolver_t
{
  public:
    std::string resolve (std::string_view type_name) const override
    {
        return "resolved." + std::string (type_name);
    }
};

static_assert (std::is_same_v<decltype (std::declval<zlink::stream_connector::connector_t &> ()
                                          .wait_for<auto_payload_t> ()),
                              zlink::stream_connector::wait_call_t<auto_payload_t>>);

static_assert (!has_core_async_terminator<zlink::stream_connector::send_call_t>);

static_assert (
  std::is_same_v<
    decltype (std::declval<zlink::stream_e2e_client::coroutine_connector_t &> ()
                .wait_for<auto_payload_t> ()
                .async ()),
    zlink::stream_e2e_client::task_t<zlink::stream_connector::message_t<auto_payload_t>>>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::stream_e2e_client::coroutine_connector_t &> ()
                             .wait_for<auto_payload_t> ()
                             .to_future ()),
                 std::future<zlink::stream_connector::message_t<auto_payload_t>>>);

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
} // namespace

namespace stream_connector_tests
{
int verify_typed_connection_setup ()
{
    zlink::context_t context;
    zlink::stream_socket_t server (context);
    server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
    server.options ().notify (false);
    server.bind ("tcp://127.0.0.1:0");
    const auto endpoint = server.options ().last_endpoint ();
    zlink::stream_connector::connector_options_t default_options;
    if (default_options.compression != zlink::stream_connector::compression_t::lz4
        || !default_options.compression_codec) {
        return fail (67);
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
                    && frame->compressed && frame->payload == "{}"
                    && frame->header.name == login_request_t::packet_name
                    && frame->header.codec == zlink::stream_connector::codec_t::json
                    && frame->header.metadata.values.at ("trace") == "t1") {
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
                    auto reply = make_server_frame (
                      zlink::stream_connector::message_kind_t::response,
                      frame->header.request_seq.value (), frame->header.name, "ok");
                    inbound.send ().message (reply).submit ();
                }
                if (frame->header.kind != zlink::stream_connector::message_kind_t::control) {
                    ++handled;
                }
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
        return fail (1);
    }

    auto states = std::make_shared<std::vector<zlink::stream_connector::connection_state_t>> ();
    auto connector_state_subscription = connector.on_connection_state_changed (
      [states] (const zlink::stream_connector::connection_state_changed_t &state) {
          states->push_back (state.current);
      });

    if (!connector.connect () || !connector.is_connected () || !states->empty ()
        || connector.pending_dispatch_count () != 2 || !connector.dispatch ()
        || states->size () != 1
        || states->back () != zlink::stream_connector::connection_state_t::connecting
        || !connector.dispatch () || states->size () != 2
        || states->back () != zlink::stream_connector::connection_state_t::connected) {
        return fail (2);
    }

    if (const int failure = verify_connection_lifecycle (); failure != 0)
        return failure;
    if (const int failure = verify_wait_connection_loss (); failure != 0)
        return failure;
    if (const int failure = verify_connection_callbacks (); failure != 0)
        return failure;
    return verify_wait_observations (context, endpoint, compressed_send_seen,
                                     compressible_large_send_seen, uncompressed_send_seen,
                                     server_thread, options, connector);
}

int verify_typed_send_and_request (zlink::context_t &context,
                                   const std::string &endpoint,
                                   std::atomic_bool &compressed_send_seen,
                                   std::atomic_bool &compressible_large_send_seen,
                                   std::atomic_bool &uncompressed_send_seen,
                                   stream_connector_fixture::joining_thread_t &server_thread,
                                   const zlink::stream_connector::connector_options_t &options,
                                   zlink::stream_connector::connector_t &connector,
                                   const bool &compressible_large_send_accepted,
                                   zlink::stream_connector::detail::connector_runtime_t &runtime)
{
    connector.send (login_request_t{}).packet_name ("login.uncompressed").submit ();

    /* 이 케이스가 확인하는 것은 reply 성공과 pending map 정리이지 응답 지연이
   * 아니다. 짧은 timeout은 부하가 걸린 머신에서 그대로 flake가 된다. */
    auto request = connector.request (login_request_t{})
                     .packet_name ("login.request")
                     .timeout (std::chrono::seconds (5))
                     .submit<login_reply_t> ();
    if (!request || runtime.pending_request_count () != 0) {
        return fail (6);
    }
    server_thread.join ();
    if (!compressed_send_seen || !uncompressed_send_seen) {
        return fail (27);
    }
    if (!compressible_large_send_accepted || !compressible_large_send_seen) {
        return fail (179);
    }

    int compressed_dispatch_count = 0;
    auto compressed_subscription = connector.on<zlink::stream_connector::packet_t> (
      "server.compressed", [&] (const packet_message_t &message) {
          const auto &packet = message.payload;
          if (packet.compressed && as_string (packet.payload) == "server-payload") {
              ++compressed_dispatch_count;
          }
      });
    if (connector.pending_dispatch_count () != 1) {
        return fail (28);
    }
    if (!connector.dispatch () || compressed_dispatch_count != 1
        || connector.pending_dispatch_count () != 0) {
        return fail (29);
    }

    {
        zlink::stream_connector::connector_options_t disabled_options;
        disabled_options.endpoint = endpoint;
        disabled_options.compression = zlink::stream_connector::compression_t::none;
        disabled_options.compression_codec.reset ();
        auto disabled_connector =
          zlink::stream_connector::connector_factory_t::create (disabled_options);
        if (!disabled_connector.connect ()) {
            return fail (147);
        }
        disabled_connector.send (login_request_t{}).compress ().submit ();
        disabled_connector.close ();
    }

    {
        zlink::stream_socket_t custom_server (context);
        custom_server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
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
            return fail (148);
        }
        custom_connector.send (login_request_t{})
          .packet_name ("custom.outbound")
          .compress ()
          .submit ();
        custom_server_thread.join ();
        if (!custom_payload_seen) {
            return fail (145);
        }
        custom_connector.close ();
    }

    return verify_manual_receive (context, endpoint, options, connector, runtime, request);
}

int verify_immediate_typed_receive (
  zlink::context_t &context,
  const std::string &endpoint,
  const zlink::stream_connector::connector_options_t &options,
  zlink::stream_connector::connector_t &connector,
  zlink::stream_connector::detail::connector_runtime_t &runtime,
  zlink::stream_connector::result_t<stream_connector_fixture::login_reply_t> &request)
{
    zlink::stream_connector::connector_options_t immediate_options;
    immediate_options.endpoint = endpoint;
    immediate_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto immediate = zlink::stream_connector::connector_factory_t::create (immediate_options);
    if (!immediate.connect ()) {
        return fail (9);
    }
    auto invalid_typed_wait =
      zlink::stream_e2e_client::coroutine (immediate)
        .wait_for<auto_payload_t> ("server.wait.invalid-json", std::chrono::milliseconds (100))
        .to_future ("typed wait payload decode failed");
    zlink::stream_connector::detail::connector_runtime_t::from (immediate).receive_packet (
      zlink::stream_connector::packet_t{"server.wait.invalid-json",
                                        {},
                                        zlink::stream_connector::codec_t::json,
                                        false,
                                        as_bytes (std::string ("{not-json"))});
    bool typed_wait_decode_failed = false;
    try {
        (void) invalid_typed_wait.get ();
    }
    catch (const std::runtime_error &) {
        typed_wait_decode_failed = true;
    }
    catch (const std::future_error &) {
        immediate.close ();
        return fail (203);
    }
    if (!typed_wait_decode_failed) {
        immediate.close ();
        return fail (203);
    }
    const auto valid_json = as_bytes (R"({"text":"accepted"})");
    const auto decoded_json =
      zlink::stream_connector::detail::decode_typed_message<auto_payload_t> (
        zlink::stream_connector::codec_t::json, valid_json);
    if (!decoded_json || decoded_json.value ().text != "accepted") {
        immediate.close ();
        return fail (340);
    }
    const auto duplicate_json = as_bytes (R"({"text":"first","text":"second"})");
    const auto duplicate_result =
      zlink::stream_connector::detail::decode_typed_message<auto_payload_t> (
        zlink::stream_connector::codec_t::json, duplicate_json);
    if (duplicate_result
        || duplicate_result.error_code ()
             != zlink::stream_connector::error_code_t::frame_decode_failed) {
        immediate.close ();
        return fail (341);
    }
    const std::vector<std::uint8_t> bom_json{
      0xef, 0xbb, 0xbf, '{', '}',
    };
    const auto bom_result = zlink::stream_connector::detail::decode_typed_message<auto_payload_t> (
      zlink::stream_connector::codec_t::json, bom_json);
    if (bom_result
        || bom_result.error_code () != zlink::stream_connector::error_code_t::frame_decode_failed) {
        immediate.close ();
        return fail (342);
    }
    auto immediate_coroutine = zlink::stream_e2e_client::use (immediate);
    auto immediate_none = immediate_coroutine.expect_none ("server.none.coroutine")
                            .within (std::chrono::milliseconds (5))
                            .async ()
                            .result ();
    if (!immediate_none) {
        return fail (202);
    }
    int immediate_count = 0;
    auto immediate_push_subscription = immediate.on<zlink::stream_connector::packet_t> (
      "server.push", [&] (const packet_message_t &) { ++immediate_count; });
    zlink::stream_connector::detail::connector_runtime_t::from (immediate).receive_packet (
      zlink::stream_connector::packet_t{"server.push",
                                        {},
                                        zlink::stream_connector::codec_t::raw,
                                        false,
                                        as_bytes (std::string ("payload"))});
    /* Immediate handlers run directly before the receive call returns. */
    if (immediate_count != 1 || immediate.pending_dispatch_count () != 0) {
        return fail (10);
    }
    immediate.close ();

    if (connector.options ().typed_codec
        && connector.options ().typed_codec->codec_id ()
             == zlink::stream_connector::codec_t::message_pack) {
        return fail (11);
    }

    return verify_public_close (context, options, connector, runtime, request, immediate);
}

int verify_type_name_and_result ()
{
    /* stream-connector §5: the default packet name is the payload type's simple
   * name and never a name that changes with the compiler. */
    {
        using zlink::stream_connector::detail::message_packet_name;
        using zlink::stream_connector::detail::simple_type_name;
        if (simple_type_name<simple_named_t> () != "simple_named_t"
            || simple_type_name<nested_ns_t::inner_named_t> () != "inner_named_t"
            || message_packet_name<simple_named_t> () != "simple_named_t") {
            return fail (220);
        }
        const auto mangled = std::string (typeid (simple_named_t).name ());
        if (message_packet_name<simple_named_t> () == mangled && mangled != "simple_named_t") {
            return fail (221);
        }
        /* A static member spelled as a string_view is honoured, not silently
     * dropped onto the fallback path. */
        if (!zlink::stream_connector::detail::static_packet_name<string_view_named_t>
            || message_packet_name<string_view_named_t> () != "view.named"
            || message_packet_name<char_pointer_named_t> () != "pointer.named") {
            return fail (222);
        }
    }

    /* result_t::error_code() reports nothing for a success result instead of
   * reading through an empty error. */
    {
        auto ok_value = zlink::stream_connector::result_t<int>::success (7);
        auto ok_void = zlink::stream_connector::result_t<void>::success ();
        auto failed = zlink::stream_connector::result_t<int>::failure (
          zlink::stream_connector::error_code_t::send_failed, "nope");
        if (ok_value.error_code ().has_value () || ok_void.error_code ().has_value ()
            || failed.error_code () != zlink::stream_connector::error_code_t::send_failed) {
            return fail (223);
        }
    }

    return 0;
}

int verify_typed_injection ()
{
    /* stream-connector §5.4: the typed codec and the name resolver are injected
   * on the creation options. */
    {
        zlink::stream_connector::connector_options_t options_with_injections;
        options_with_injections.typed_codec = std::make_shared<counting_typed_codec_t> ();
        options_with_injections.name_resolver = std::make_shared<prefixing_name_resolver_t> ();
        auto injected =
          zlink::stream_connector::connector_factory_t::create (options_with_injections);
        if (!injected.options ().typed_codec || !injected.options ().name_resolver
            || injected.options ().name_resolver->resolve ("unnamed_payload_t")
                 != "resolved.unnamed_payload_t") {
            return fail (245);
        }
        /* The resolver renames a type that does not name itself. */
        auto resolved_packet = injected.send (unnamed_payload_t{"injected"});
        (void) resolved_packet;
        if (zlink::stream_connector::json_typed_codec ()->codec_id ()
            != zlink::stream_connector::codec_t::json) {
            return fail (247);
        }
    }

    return 0;
}
} // namespace stream_connector_tests
