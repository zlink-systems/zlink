/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../../Configuration/sample_names.hpp"
#include "../../../Configuration/sample_topology.hpp"
#include "../../../../Shared/Contracts/stream_message_codec.hpp"


#include <zlink/framework.hpp>

namespace zlink::samples::bingo
{

using namespace framework;
using framework::actor_ref_t;
using framework::message_t;

class authenticate_session_handler_t
{
  public:
    explicit authenticate_session_handler_t (channel_client_t &client) : _client (client) {}

    bool can_handle (const session_message_context_t &dispatch) const
    {
        return dispatch.packet_name == authenticate_req_t::descriptor ()->name ();
    }

    task_t<session_actor_t>
    handle (session_actor_manager_t &actors, stream_t &stream, const zlink::message_t &payload)
    {
        /* client stream의 payload도 Protobuf다 — JSON으로 파싱하지 않는다. */
        const auto
          request = zlink::stream_connector::codecs::codec_traits<authenticate_req_t>::decode (
            payload.to_bytes ());
        // --8<-- [start:doc-bingo-session-auth]
        authenticate_player_req_t authenticate_request;
        authenticate_request.set_access_token (request.access_token ());
        auto authenticated = co_await _client
                               .request (sample_names_t::api_channel, authenticate_request)
                               .async<authenticate_player_res_t> ();
        if (!authenticated.accepted () || !authenticated.has_actor_id ()
            || !authenticated.has_display_name ()) {
            co_return result_t<session_actor_t>::failure (framework_error_kind_t::internal_failure,
                                                          !authenticated.has_reason ()
                                                            ? "Player authentication failed."
                                                            : authenticated.reason ());
        }

        ensure_player_actor_req_t create_request;
        create_request.set_actor_id (authenticated.actor_id ());
        create_request.set_display_name (authenticated.display_name ());
        auto located = actors.get_or_create (
          sample_names_t::player_actor_type, authenticated.actor_id (), create_request);
        if (!located) {
            co_return result_t<session_actor_t>::failure (
              located.error_kind (),
              located.error () ? located.error ()->what () : "Player actor could not be located.");
        }
        // --8<-- [end:doc-bingo-session-auth]
        // --8<-- [start:doc-bingo-session-bind]
        auto bound = co_await actors.bind_or_get (located.value ().ref ()).async ();
        auto actor = actors.find (authenticated.actor_id ()).value_or (bound);

        authenticate_res_t reply_payload;
        reply_payload.set_actor_id (authenticated.actor_id ());
        reply_payload.set_display_name (authenticated.display_name ());
        stream.reply_packet (encode_authenticate_response (reply_payload)).async ();
        // --8<-- [end:doc-bingo-session-bind]

        co_return actor;
    }

  private:
    channel_client_t &_client;
};

} // namespace zlink::samples::bingo
