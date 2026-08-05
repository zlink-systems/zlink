/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Infrastructure/scenario_state.hpp"

#include <zlink/framework.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

namespace zlink::framework::e2e::registration_codec::server
{

template <typename TReply, typename TRequest>
inline TReply request_channel_with_retry (zlink::framework::channel_client_t &channels,
                                          const TRequest &request)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (30);
    std::string last_error = "channel request failed";
    while (std::chrono::steady_clock::now () < deadline) {
        auto call =
          channels.request (api_channel, request).timeout (std::chrono::seconds (5)).template submit<TReply> ();
        const auto &reply = call.result ();
        if (reply) {
            return reply.value ();
        }
        if (reply.error ()) {
            last_error = reply.error ()->what ();
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    throw std::runtime_error ("timed out waiting for registration channel route: " + last_error);
}

template <typename TValue> inline zlink::framework::http_response_t json_response (const TValue &value)
{
    zlink::framework::http_response_t response;
    response.body = nlohmann::json (value).dump ();
    return response;
}

class evidence_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<scenario_state_t>;

    explicit evidence_handler_t (scenario_state_t &state) : _state (state) {}

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        zlink::framework::http_response_t response;
        response.body = nlohmann::json (_state.snapshot ()).dump ();
        return response;
    }

  private:
    scenario_state_t &_state;
};

class registration_auto_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;

    explicit registration_auto_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        return json_response (run ());
    }

  private:
    echo_auto_res_t run ()
    {
        auto reply =
          request_channel_with_retry<echo_auto_res_t> (_channels, echo_auto_req_t{.value = "a1"});
        _channels.send (api_channel, echo_auto_msg_t{.value = "send-a1"})
          .submit ();
        return reply;
    }

    zlink::framework::channel_client_t &_channels;
};

class registration_attribute_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;

    explicit registration_attribute_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        return json_response (run ());
    }

  private:
    echo_attr_res_t run ()
    {
        auto reply = request_channel_with_retry<echo_attr_res_t> (
          _channels, echo_attr_req_t{.value = "a2"});
        _channels.send (api_channel, echo_attr_msg_t{.value = "send-a2"})
          .submit ();
        return reply;
    }

    zlink::framework::channel_client_t &_channels;
};

class registration_manual_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;

    explicit registration_manual_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        return json_response (run ());
    }

  private:
    echo_manual_res_t run ()
    {
        auto reply = request_channel_with_retry<echo_manual_res_t> (
          _channels, echo_manual_req_t{.value = "manual"});
        _channels.send (api_channel, echo_manual_msg_t{.value = "send-a3"})
          .submit ();
        return reply;
    }

    zlink::framework::channel_client_t &_channels;
};

class registration_di_lifecycle_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;

    explicit registration_di_lifecycle_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        return json_response (run ());
    }

  private:
    lifecycle_scenario_res_t run ()
    {
        auto first = request_channel_with_retry<scoped_lifecycle_res_t> (
          _channels, scoped_lifecycle_req_t{.value = "first"});
        auto second = request_channel_with_retry<scoped_lifecycle_res_t> (
          _channels, scoped_lifecycle_req_t{.value = "second"});
        auto stats = request_channel_with_retry<scoped_lifecycle_stats_res_t> (
          _channels, scoped_lifecycle_stats_req_t{.value = "stats"});
        return {.first = first, .second = second, .stats = stats};
    }

    zlink::framework::channel_client_t &_channels;
};

class registration_filter_order_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;

    explicit registration_filter_order_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        return json_response (request_channel_with_retry<filter_order_res_t> (
          _channels, filter_order_req_t{.value = "filter-order"}));
    }

  private:
    zlink::framework::channel_client_t &_channels;
};

class codec_roundtrip_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;

    explicit codec_roundtrip_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        return json_response (run ());
    }

  private:
    codec_roundtrip_scenario_res_t run ()
    {
        auto json = request_channel_with_retry<json_roundtrip_res_t> (
          _channels, json_roundtrip_req_t{.value = "b1"});
        _channels.send (api_channel, json_codec_msg_t{.value = "send-b1"})
          .submit ();

        auto protobuf = request_channel_with_retry<protobuf_roundtrip_res_t> (
          _channels, protobuf_roundtrip_req_t{.value = "b2"});
        _channels.send (api_channel, protobuf_codec_msg_t{.value = "send-b2"})
          .submit ();

        auto messagepack = request_channel_with_retry<messagepack_roundtrip_res_t> (
          _channels, messagepack_roundtrip_req_t{.value = "b3"});
        _channels.send (api_channel, messagepack_codec_msg_t{.value = "send-b3"})
          .submit ();

        return {.json = json, .protobuf = protobuf, .messagepack = messagepack};
    }

    zlink::framework::channel_client_t &_channels;
};

class codec_coexistence_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;

    explicit codec_coexistence_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        return json_response (run ());
    }

  private:
    codec_coexistence_scenario_res_t run ()
    {
        auto json = request_channel_with_retry<json_roundtrip_res_t> (
          _channels, json_roundtrip_req_t{.value = "coexist-json"});
        auto custom = request_channel_with_retry<custom_roundtrip_res_t> (
          _channels, custom_roundtrip_req_t{.value = "coexist-custom"});
        auto protobuf = request_channel_with_retry<protobuf_roundtrip_res_t> (
          _channels, protobuf_roundtrip_req_t{.value = "coexist-protobuf"});
        auto messagepack = request_channel_with_retry<messagepack_roundtrip_res_t> (
          _channels, messagepack_roundtrip_req_t{.value = "coexist-msgpack"});
        return {.json = json,
                .custom = custom,
                .protobuf = protobuf,
                .messagepack = messagepack};
    }

    zlink::framework::channel_client_t &_channels;
};

class codec_mismatch_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<zlink::framework::channel_client_t>;

    explicit codec_mismatch_handler_t (zlink::framework::channel_client_t &channels) :
        _channels (channels)
    {
    }

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        return json_response (run ());
    }

  private:
    operation_status_t run ()
    {
        /* RC-B5: 상대 peer는 JSON codec만 등록했다. Protobuf bytes를 기본 JSON serializer로
         * 해석할 수 없으므로 공개 payload decode 오류로 끝나야 한다. */
        auto mismatched =
          _channels.request (api_channel, protobuf_roundtrip_req_t{.value = "json-only"})
            .timeout (std::chrono::seconds (5))
            .submit<protobuf_roundtrip_res_t> ()
            .result ();
        if (mismatched) {
            throw std::runtime_error ("RC-B5 expected the JSON-only peer to reject a protobuf "
                                      "payload");
        }
        if (mismatched.error_kind ()
            != zlink::framework::framework_error_kind_t::protocol_error) {
            throw std::runtime_error ("RC-B5 expected payload_decode_failed from the JSON-only "
                                      "peer");
        }

        /* 불일치가 채널을 망가뜨리지 않는다: 같은 채널의 JSON 요청은 계속 동작한다. */
        auto json = request_channel_with_retry<json_roundtrip_res_t> (
          _channels, json_roundtrip_req_t{.value = "after-mismatch"});
        if (json.value != "json:after-mismatch") {
            throw std::runtime_error ("RC-B5 recovery reply mismatch");
        }
        return {.status = "payload_decode_failed"};
    }

    zlink::framework::channel_client_t &_channels;
};

} // namespace zlink::framework::e2e::registration_codec::server
