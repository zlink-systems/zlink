/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Configuration/sample_names.hpp"
#include "../Configuration/sample_configuration.hpp"
#include "../Configuration/sample_readiness.hpp"
#include "../../Shared/Contracts/messages.hpp"

#include <zlink/framework.hpp>
#include <zlink/locations/redis.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace zlink::samples::supportchat
{
using namespace zlink::framework;

inline constexpr const char *support_user_actor_type = "support-user";

class supportchat_session_t final : public packet_stream_session_t
{
  public:
    supportchat_session_t (channel_client_t &channels, session_actor_manager_t &actors) :
        _channels (channels), _actors (actors)
    {
    }

    task_t<void> on_connected (stream_t &) override { co_return; }

    task_t<void> on_disconnected (stream_t &) override
    {
        _identity_actor_id.clear ();
        _identity_display_name.clear ();
        _identity_role.clear ();
        co_return;
    }

    task_t<void> on_error (stream_t &, const stream_error_t &) override { co_return; }

    // --8<-- [start:doc-sc-session-dispatch]
    task_t<void> on_packet (stream_t &stream,
                            const session_message_context_t &dispatch,
                            const zlink::message_t &payload) override
    {
        if (dispatch.packet_name == authenticate_req_t::packet_name) {
            /* 인증은 API 서버가 소유한다(공통 sample spec §11). Session은 access token을
             * 그대로 넘기고 사용자 프로필을 만들어 내지 않는다. */
            auto verified = co_await _channels
                              .request ("supportchat.api",
                                        authenticate_user_req_t{
                                          payload.parse_json<authenticate_req_t> ().access_token})
                              .async<authenticate_user_res_t> ();
            if (!verified.accepted) {
                throw framework_exception_t (framework_error_kind_t::rejected,
                                             verified.reason.value_or ("AuthenticationRejected"));
            }
            const authenticate_res_t authenticated{
              *verified.actor_id, *verified.display_name, *verified.role};
            // --8<-- [start:doc-sc-session-auth]
            auto ensure = ensure_support_user_actor_req_t{authenticated.actor_id,
                                                          authenticated.display_name,
                                                          authenticated.role,
                                                          authenticated.actor_id};
            auto ensured = co_await _channels.request ("supportchat.support", ensure)
                             .async<ensure_support_user_actor_res_t> ();
            auto actor_ref = ensured.actor.to_actor_ref (sample_names_t::mesh);
            auto bound = co_await _actors.bind_or_get (actor_ref).async ();
            _identity_actor_id = std::string (bound.actor_id ());
            _identity_display_name = authenticated.display_name;
            _identity_role = authenticated.role;
            // --8<-- [end:doc-sc-session-auth]
            stream.reply_packet (zlink::message_t::from_json (authenticated)).async ();
            co_return;
        }
        if (dispatch.packet_name == join_conversation_req_t::packet_name) {
            const auto request = payload.parse_json<join_conversation_req_t> ();
            if (request.conversation_id.empty ()) {
                throw framework_exception_t (framework_error_kind_t::protocol_error,
                                             "JoinConversationReq is missing conversationId");
            }
            auto actor = require_actor (_identity_actor_id, std::string (dispatch.packet_name));
            if (_identity_role == role_t::agent) {
                actor = co_await ensure_agent_conversation_actor (request.conversation_id);
            }
            auto reply = co_await actor.relay_request (std::string (dispatch.packet_name), payload)
                           .async ();
            auto joined = reply.parse_json<join_conversation_res_t> ();
            joined.actor_id = std::string (actor.actor_id ());
            stream.reply_packet (zlink::message_t::from_json (joined)).async ();
            co_return;
        }
        // --8<-- [start:doc-sc-actor-relay]
        auto actor = dispatch.actor
                       ? *dispatch.actor
                       : require_actor (_identity_actor_id, std::string (dispatch.packet_name));
        // --8<-- [end:doc-sc-actor-relay]
        if (dispatch.can_reply) {
            auto reply = co_await actor.relay_request (payload).async ();
            stream.reply_packet (reply).async ();
            co_return;
        }
        co_await actor.relay (payload);
    }
    // --8<-- [end:doc-sc-session-dispatch]

  private:
    task_t<session_actor_t> ensure_agent_conversation_actor (const std::string &conversation_id)
    {
        // --8<-- [start:doc-sc-agent-join]
        auto ensured = co_await _channels
                         .request ("supportchat.support",
                                   ensure_agent_conversation_req_t{
                                     _identity_actor_id, _identity_display_name, conversation_id})
                         .async<ensure_agent_conversation_res_t> ();
        auto actor_ref = ensured.actor.to_actor_ref (sample_names_t::mesh);
        co_return co_await _actors.bind_or_get (actor_ref).async ();
        // --8<-- [end:doc-sc-agent-join]
    }

    session_actor_t require_actor (const std::string &actor_id, const std::string &packet_name)
    {
        if (actor_id.empty ()) {
            throw framework_exception_t (framework_error_kind_t::not_found,
                                         "authenticated support actor is required for "
                                           + packet_name);
        }
        auto actor = _actors.find (actor_id);
        if (!actor) {
            throw framework_exception_t (framework_error_kind_t::not_found,
                                         "bound support actor route is not found");
        }
        return *actor;
    }

    channel_client_t &_channels;
    session_actor_manager_t &_actors;
    std::string _identity_actor_id;
    std::string _identity_display_name;
    std::string _identity_role;
};

} // namespace zlink::samples::supportchat

int main (int argc, char **argv)
{
    using namespace zlink::framework;
    using namespace zlink::samples::supportchat;

    auto app = app_t::create ();
    const auto configuration = load_sample_configuration (app, argc, argv);
    const auto &topology = configuration.topology;
    std::filesystem::create_directories (configuration.role.log_dir);

    app.logging ().use_file (configuration.flow_log_path ());
    auto &options = app.add_zlink_framework ();
    options.configure_dispatch ().message_flow (message_flow_log_mode_t::normal);
    options.add_location_store<redis::redis_location_store_t> ()
      .set_connection_string (topology.redis_endpoint)
      .set_key_prefix (topology.redis_key_prefix + "location:");
    options.add_relocation_store<redis::redis_relocation_store_t> ()
      .set_connection_string (topology.redis_endpoint)
      .set_key_prefix (topology.redis_key_prefix + "relocation:");
    // --8<-- [start:doc-sc-session-register]
    options.add_client_server_channel ("supportchat.support").client ();
    options.add_client_server_channel ("supportchat.api").client ();
    auto support_spot = options.add_route_mesh (sample_names_t::mesh);
    support_spot.set_routing_id (zlink::routing_id_t::from ("supportchat-session"));
    support_spot.listen (topology.session_spot_router_endpoint);
    support_spot.objects ().client ();
    options.add_stream_node ("supportchat-session-stream")
      .bind (topology.session_stream_endpoint)
      .register_session<supportchat_session_t> ();
    // --8<-- [end:doc-sc-session-register]
    app.add_hosted_service (std::make_unique<sample_readiness_service_t> ("stream", "session"));
    app.add_hosted_service (
      std::make_unique<spot_route_readiness_service_t> (sample_names_t::mesh, "session"));
    return app.run (argc, argv);
}
