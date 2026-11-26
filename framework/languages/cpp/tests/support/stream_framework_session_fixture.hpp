/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <zlink/framework.hpp>

namespace zlink::framework::test::stream_framework
{
class sample_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &stream) override
    {
        events.push_back ("connected:" + stream.session_id ());
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &stream) override
    {
        events.push_back ("disconnected:" + stream.session_id ());
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }

    zlink::framework::task_t<void> on_error (zlink::framework::stream_t &,
                                             const zlink::framework::stream_error_t &error) override
    {
        events.push_back ("error:" + std::string (error.message ()));
        last_error = error;
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }

    zlink::framework::task_t<void>
    on_packet (zlink::framework::stream_t &stream,
               const zlink::framework::session_message_context_t &dispatch,
               const zlink::framework::message_t &payload) override
    {
        events.push_back ("packet:" + std::string (dispatch.packet_name) + ":"
                          + zlink::framework::detail::message_to_raw (
                              payload, zlink::framework::serializer_registry_t{})
                              .to_string ());
        last_can_reply = dispatch.can_reply;
        last_metadata = dispatch.metadata;
        last_actor_id =
          dispatch.actor ? std::optional<std::string> (dispatch.actor->actor_id ()) : std::nullopt;
        stream.reply_packet (payload).async ().result ().value ();
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }

    std::vector<std::string> events;
    std::optional<zlink::framework::stream_error_t> last_error;
    bool last_can_reply = false;
    zlink::framework::message_metadata_t last_metadata;
    std::optional<std::string> last_actor_id;
};
} // namespace zlink::framework::test::stream_framework
