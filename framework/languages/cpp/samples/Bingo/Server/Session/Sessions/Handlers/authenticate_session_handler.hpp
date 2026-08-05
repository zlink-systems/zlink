/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../../Configuration/sample_names.hpp"
#include "../../../Configuration/sample_topology.hpp"
#include "../../../../Shared/Contracts/messages.hpp"

#include "../../../../Shared/Contracts/protobuf_conversions.hpp"

#include <zlink/framework.hpp>

namespace zlink::samples::bingo
{

using namespace framework;
using framework::actor_ref_t;
using framework::message_t;

class authenticate_session_handler_t
{
  public:
    using dependency_types = dependency_list_t<channel_client_t>;

    explicit authenticate_session_handler_t (channel_client_t &client) :
        _client (client)
    {
    }

    bool can_handle (const session_message_context_t &dispatch) const
    {
        return dispatch.packet_name == authenticate_req_t::packet_name;
    }

    task_t<session_actor_t>
    handle (session_actor_manager_t &actors, stream_t &stream, const zlink::message_t &payload)
    {
        /* client stream의 payload도 Protobuf다 — JSON으로 파싱하지 않는다. */
        authenticate_req_t request;
        from_stream_payload (payload, request);
        const auto authenticate_request = authenticate_player_req_t{request.access_token};
        auto authenticated = co_await _client.request (
            sample_names_t::api_channel, authenticate_request).submit<authenticate_player_res_t> ();
        if (!authenticated.accepted || !authenticated.actor_id
            || !authenticated.display_name) {
            co_return result_t<session_actor_t>::failure (framework_error_kind_t::internal_failure,
                                                          !authenticated.reason
                                                            ? "Player authentication failed."
                                                            : *authenticated.reason);
        }

        auto create_request = ensure_player_actor_req_t{
            *authenticated.actor_id, *authenticated.display_name};
        auto located = actors.get_or_create (
          sample_names_t::player_actor_type, *authenticated.actor_id, create_request);
        if (!located) {
            co_return result_t<session_actor_t>::failure (
              located.error_kind (),
              located.error () ? located.error ()->what ()
                               : "Player actor could not be located.");
        }
        auto bound =
          co_await actors.bind_or_get (located.value ().ref ()).submit ();
        auto actor = actors.find (*authenticated.actor_id).value_or (bound);

        const auto reply_payload = authenticate_res_t{
            *authenticated.actor_id, *authenticated.display_name
        };
        const auto reply_message = to_stream_payload (reply_payload);
        stream.reply_packet (reply_message).submit ();

        co_return actor;
    }

  private:
    channel_client_t &_client;
};

} // namespace zlink::samples::bingo
