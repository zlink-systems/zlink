/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Configuration/server_options.hpp"
#include "../Endpoints/operational_endpoints.hpp"
#include "../Handlers/codec_handlers.hpp"
#include "../Handlers/di_lifecycle_handlers.hpp"
#include "../Handlers/filter_order_handlers.hpp"
#include "../Handlers/registration_handlers.hpp"

#include <zlink/codecs/messagepack.hpp>
#include "../../Shared/protobuf_conversions.hpp"

#include <zlink/codecs/protobuf.hpp>
#include <zlink/framework.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::registration_codec::server
{

struct tcp_endpoint_t
{
    std::string host;
    std::uint16_t port;
};

inline tcp_endpoint_t parse_tcp_endpoint (const std::string &endpoint)
{
    constexpr std::string_view prefix = "tcp://";
    if (!endpoint.starts_with (prefix))
        throw std::invalid_argument ("client/server endpoint must use tcp://");
    const auto separator = endpoint.rfind (':');
    if (separator == std::string::npos || separator <= prefix.size ()
        || separator + 1 >= endpoint.size ())
        throw std::invalid_argument ("client/server endpoint must include host and port");
    const auto value = std::stoul (endpoint.substr (separator + 1));
    if (value == 0 || value > 65535)
        throw std::invalid_argument ("client/server endpoint port is out of range");
    return {.host = endpoint.substr (prefix.size (), separator - prefix.size ()),
            .port = static_cast<std::uint16_t> (value)};
}

inline zlink::framework::encoded_payload_t encode_text (const std::string &prefix,
                                                        const std::string &value)
{
    return zlink::framework::encoded_payload_t::from_string (prefix + ":" + value);
}

inline std::string decode_text (const zlink::framework::encoded_payload_t &payload,
                                const std::string &prefix)
{
    const auto text = payload.to_string ();
    const auto expected = prefix + ":";
    if (text.rfind (expected, 0) != 0) {
        throw zlink::framework::framework_exception_t (
          zlink::framework::framework_error_kind_t::protocol_error,
          "custom payload prefix mismatch");
    }
    return text.substr (expected.size ());
}

struct binary_codecs_t
{
    template <typename TRegistrar> void register_framework_codecs (TRegistrar &registrar) const
    {
        zlink::framework_codecs::protobuf_codec_extension_t::register_payload_serializer<
          protobuf_roundtrip_req_t, ::zlink::e2e::registrationcodec::ProtobufRoundtripReq> (
          registrar);
        zlink::framework_codecs::protobuf_codec_extension_t::register_payload_serializer<
          protobuf_roundtrip_res_t, ::zlink::e2e::registrationcodec::ProtobufRoundtripRes> (
          registrar);
        zlink::framework_codecs::protobuf_codec_extension_t::register_payload_serializer<
          protobuf_codec_msg_t, ::zlink::e2e::registrationcodec::ProtobufCodecMsg> (registrar);
        zlink::framework_codecs::messagepack_codec_extension_t::register_payload_serializer<
          messagepack_roundtrip_req_t> (registrar);
        zlink::framework_codecs::messagepack_codec_extension_t::register_payload_serializer<
          messagepack_roundtrip_res_t> (registrar);
        zlink::framework_codecs::messagepack_codec_extension_t::register_payload_serializer<
          messagepack_codec_msg_t> (registrar);
    }
};

struct custom_codecs_t
{
    template <typename TRegistrar> void register_framework_codecs (TRegistrar &registrar) const
    {
        registrar
          .template add_serializer<custom_roundtrip_req_t> (
            [] (const custom_roundtrip_req_t &value) {
                return encode_text ("custom-request", value.value);
            },
            [] (const zlink::framework::encoded_payload_t &payload) {
                return custom_roundtrip_req_t{.value = decode_text (payload, "custom-request")};
            })
          .template add_serializer<custom_roundtrip_res_t> (
            [] (const custom_roundtrip_res_t &value) {
                return encode_text ("custom-reply", value.value);
            },
            [] (const zlink::framework::encoded_payload_t &payload) {
                return custom_roundtrip_res_t{.value = decode_text (payload, "custom-reply")};
            })
          .template add_serializer<mismatch_roundtrip_req_t> (
            [] (const mismatch_roundtrip_req_t &value) {
                return encode_text ("mismatch-request", value.value);
            },
            [] (const zlink::framework::encoded_payload_t &payload) {
                return mismatch_roundtrip_req_t{.value = decode_text (payload, "mismatch-request")};
            })
          .template add_serializer<mismatch_roundtrip_res_t> (
            [] (const mismatch_roundtrip_res_t &value) {
                return encode_text ("mismatch-reply", value.value);
            },
            [] (const zlink::framework::encoded_payload_t &payload) {
                return mismatch_roundtrip_res_t{
                  .value = decode_text (payload, "mismatch-reply")};
            });
    }
};

inline void add_binary_codecs (zlink::framework::codec_options_builder_t codecs)
{
    codecs.use (binary_codecs_t{});
}

inline void add_custom_codecs (zlink::framework::codec_options_builder_t codecs)
{
    codecs.use (custom_codecs_t{});
}

inline void configure_invalid (zlink::framework::zlink_framework_options_t &options,
                               const std::string &mode,
                               const std::string &endpoint)
{
    if (mode == "duplicate") {
        options.handlers ()
          .group (handler_group)
          .add<auto_request_handler_t> ()
          .add<auto_request_handler_t> ();
        return;
    }
    if (mode == "wrong-group") {
        options.handlers ()
          .group (handler_group)
          .add_send<auto_send_handler_t> ();
        options.add_fanout_channel ("registration.codec.invalid")
          .connect (endpoint)
          .use_handler_group (handler_group);
        return;
    }
    if (mode == "unsupported-channel") {
        const auto parsed = parse_tcp_endpoint (endpoint);
        options.add_client_server_channel ("registration.codec.invalid")
          .server ()
          .set_bind_host (parsed.host)
          .set_advertise_host (parsed.host)
          .listen (parsed.port);
        return;
    }
    throw std::runtime_error ("unknown invalid mode " + mode);
}

inline void configure_services (zlink::framework::service_collection_t &services)
{
    services.add_singleton<scenario_state_t> (std::make_unique<scenario_state_t> ());
    services.add_singleton<singleton_dependency_t> ();
    services.add_singleton<filter_order_state_t> ();
    services.add_scoped<scoped_dependency_t> ();
}

inline void configure_codecs (zlink::framework::codec_options_builder_t codecs,
                              const server_options_t &server)
{
    if (server.server_mode != "json-only-peer") {
        add_binary_codecs (codecs);
        add_custom_codecs (codecs);
    }
}

inline void configure_channels (zlink::framework::zlink_framework_options_t &options,
                                const server_options_t &server)
{
    const auto endpoint = parse_tcp_endpoint (server.api_endpoint);
    auto channel = options.add_client_server_channel (api_channel);
    channel.server ()
      .set_bind_host (endpoint.host)
      .set_advertise_host (endpoint.host)
      .listen (endpoint.port)
      .add_handler_group (handler_group);
    channel.client ().connect (server.api_endpoint);
}

inline void configure_http (zlink::framework::http_options_builder_t &http,
                            const server_options_t &server)
{
    http.listen (server.http_endpoint)
      .map_health ("/health")
      .map_get<evidence_handler_t> ("/evidence")
      .map_post<registration_auto_handler_t> ("/registration/auto")
      .map_post<registration_attribute_handler_t> ("/registration/attribute")
      .map_post<registration_manual_handler_t> ("/registration/manual")
      .map_post<registration_di_lifecycle_handler_t> ("/registration/di-lifecycle")
      .map_post<registration_filter_order_handler_t> ("/registration/filter-order")
      .map_post<codec_roundtrip_handler_t> ("/codec/roundtrip")
      .map_post<codec_coexistence_handler_t> ("/codec/coexistence")
      .map_post<codec_mismatch_handler_t> ("/codec/mismatch");
}

inline void configure_handlers (zlink::framework::handler_options_builder_t handlers)
{
    handlers.group (handler_group)
      .add<auto_request_handler_t> ()
      .add_send<auto_send_handler_t> ()
      .add<attribute_request_handler_t> ()
      .add_send<attribute_send_handler_t> ()
      .add<manual_channel_request_handler_t> ()
      .add_send<manual_channel_send_handler_t> ()
      .add<scoped_lifecycle_handler_t> ()
      .add<scoped_lifecycle_stats_handler_t> ()
      .add<filter_order_handler_t> ()
      .add<json_roundtrip_handler_t> ()
      .add_send<json_codec_send_handler_t> ()
      .add<protobuf_roundtrip_handler_t> ()
      .add_send<protobuf_codec_send_handler_t> ()
      .add<messagepack_roundtrip_handler_t> ()
      .add_send<messagepack_codec_send_handler_t> ()
      .add<custom_roundtrip_handler_t> ()
      .add<mismatch_roundtrip_handler_t> ();
}

inline void configure_framework (zlink::framework::zlink_framework_options_t &options,
                                 const server_options_t &server)
{
    options.configure_dispatch ()
      .message_flow (zlink::framework::message_flow_log_mode_t::key_transitions)
      .trace_log_file (server.log_dir + "/server-flow.log")
      .trace_label ("cpp-rc-server");
    if (!server.invalid_mode.empty ()) {
        configure_invalid (options, server.invalid_mode, server.api_endpoint);
        return;
    }
    options.use_filter<first_filter_t> ().use_filter<second_filter_t> ();
    configure_services (options.services ());
    configure_codecs (options.codecs (), server);
    configure_channels (options, server);
    configure_http (options.http (), server);
    configure_handlers (options.handlers ());
}

} // namespace zlink::framework::e2e::registration_codec::server
