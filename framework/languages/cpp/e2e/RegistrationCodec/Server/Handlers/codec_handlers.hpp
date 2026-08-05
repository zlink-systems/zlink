/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Infrastructure/scenario_state.hpp"

#include <zlink/framework.hpp>

namespace zlink::framework::e2e::registration_codec::server
{

class json_roundtrip_handler_t
{
  public:
    using request_type = json_roundtrip_req_t;
    using reply_type = json_roundtrip_res_t;

    json_roundtrip_res_t handle (const json_roundtrip_req_t &request,
                                 const zlink::framework::message_context_t &context)
    {
        return {.value = "json:" + request.value,
                .content_type = context.content_type.value_or ("")};
    }
};

class json_codec_send_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<scenario_state_t>;
    using message_type = json_codec_msg_t;

    explicit json_codec_send_handler_t (scenario_state_t &state) : _state (state) {}

    void handle (const json_codec_msg_t &message,
                 const zlink::framework::message_context_t &context)
    {
        _state.record ("RC-B1-send", context.content_type.value_or ("") + ":" + message.value);
    }

  private:
    scenario_state_t &_state;
};

class protobuf_roundtrip_handler_t
{
  public:
    using request_type = protobuf_roundtrip_req_t;
    using reply_type = protobuf_roundtrip_res_t;

    protobuf_roundtrip_res_t handle (const protobuf_roundtrip_req_t &request,
                                     const zlink::framework::message_context_t &context)
    {
        return {.value = "protobuf:" + request.value,
                .content_type = context.content_type.value_or ("")};
    }
};

class protobuf_codec_send_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<scenario_state_t>;
    using message_type = protobuf_codec_msg_t;

    explicit protobuf_codec_send_handler_t (scenario_state_t &state) : _state (state) {}

    void handle (const protobuf_codec_msg_t &message,
                 const zlink::framework::message_context_t &context)
    {
        _state.record ("RC-B2-send", context.content_type.value_or ("") + ":" + message.value);
    }

  private:
    scenario_state_t &_state;
};

class messagepack_roundtrip_handler_t
{
  public:
    using request_type = messagepack_roundtrip_req_t;
    using reply_type = messagepack_roundtrip_res_t;

    messagepack_roundtrip_res_t handle (const messagepack_roundtrip_req_t &request,
                                        const zlink::framework::message_context_t &context)
    {
        return {.value = "messagepack:" + request.value,
                .content_type = context.content_type.value_or ("")};
    }
};

class messagepack_codec_send_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<scenario_state_t>;
    using message_type = messagepack_codec_msg_t;

    explicit messagepack_codec_send_handler_t (scenario_state_t &state) : _state (state) {}

    void handle (const messagepack_codec_msg_t &message,
                 const zlink::framework::message_context_t &context)
    {
        _state.record ("RC-B3-send",
                       context.content_type.value_or ("") + ":" + message.value);
    }

  private:
    scenario_state_t &_state;
};

class custom_roundtrip_handler_t
{
  public:
    using request_type = custom_roundtrip_req_t;
    using reply_type = custom_roundtrip_res_t;

    custom_roundtrip_res_t handle (const custom_roundtrip_req_t &request)
    {
        return {.value = "custom:" + request.value};
    }
};

class mismatch_roundtrip_handler_t
{
  public:
    using request_type = mismatch_roundtrip_req_t;
    using reply_type = mismatch_roundtrip_res_t;

    mismatch_roundtrip_res_t handle (const mismatch_roundtrip_req_t &request)
    {
        return {.value = "mismatch:" + request.value};
    }
};

} // namespace zlink::framework::e2e::registration_codec::server
