/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Configuration/location_store.hpp"
#include "../Configuration/sample_names.hpp"
#include "../Configuration/sample_configuration.hpp"
#include "../../Shared/Contracts/messages.hpp"

#include <zlink/framework.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>

namespace zlink::samples::supportchat
{
using namespace zlink::framework;

inline constexpr const char *support_user_actor_type = "support-user";
inline constexpr const char *conversation_id_metadata_key = "ConversationId";

class supportchat_session_t final : public packet_stream_session_t
{
  public:
    using dependency_types = dependency_list_t<channel_client_t, session_actor_manager_t>;

    supportchat_session_t (channel_client_t &channels,
                           session_actor_manager_t &actors) :
        _channels (channels), _actors (actors)
    {
    }

    task_t<void> on_connected (stream_t &) override { co_return; }

    task_t<void> on_disconnected (stream_t &) override
    {
        _identity_actor_id.clear ();
        _identity_display_name.clear ();
        _identity_role.clear ();
        _conversation_actor_ids.clear ();
        co_return;
    }

    task_t<void> on_error (stream_t &, const stream_error_t &) override { co_return; }

    task_t<void> on_packet (stream_t &stream,
                            const session_message_context_t &dispatch,
                            const zlink::message_t &payload) override
    {
        if (dispatch.packet_name == authenticate_req_t::packet_name) {
            /* 인증은 API 서버가 소유한다(공통 sample spec §11). Session은 access token을
             * 그대로 넘기고 사용자 프로필을 만들어 내지 않는다. */
            auto verified =
              co_await _channels
                .request ("supportchat.api",
                            authenticate_user_req_t{
                            payload.parse_json<authenticate_req_t> ().access_token})
                .submit<authenticate_user_res_t> ();
            if (!verified.accepted) {
                throw framework_exception_t (framework_error_kind_t::rejected,
                                             verified.reason.value_or ("AuthenticationRejected"));
            }
            const authenticate_res_t authenticated{*verified.actor_id, *verified.display_name,
                                                   *verified.role};
            auto ensure = ensure_support_user_actor_req_t{authenticated.actor_id,
                                                          authenticated.display_name,
                                                          authenticated.role,
                                                          authenticated.actor_id};
            auto ensured = co_await _channels.request ("supportchat.support", ensure)
                              .submit<ensure_support_user_actor_res_t> ();
            auto actor_ref = ensured.actor.to_actor_ref (sample_names_t::mesh);
            auto bound = co_await _actors.bind_or_get (actor_ref).submit ();
            _identity_actor_id = std::string (bound.actor_id ());
            _identity_display_name = authenticated.display_name;
            _identity_role = authenticated.role;
            stream.reply_packet (zlink::message_t::from_json (authenticated)).submit ();
            co_return;
        }
        if (dispatch.packet_name == join_conversation_req_t::packet_name
            && _identity_role == role_t::agent) {
            auto joined = co_await ensure_agent_conversation_actor (stream, dispatch);
            stream.reply_packet (
                    zlink::message_t::from_json (
                      join_conversation_res_t{joined.scheduled, joined.state}))
              .submit ();
            co_return;
        }
        auto actor = co_await select_actor (stream, dispatch);
        if (dispatch.can_reply) {
            auto reply = co_await actor.relay_request (payload).submit ();
            stream.reply_packet (reply).submit ();
            co_return;
        }
        co_await actor.relay (payload);
    }

  private:
    task_t<session_actor_t> select_actor (stream_t &stream,
                                          const session_message_context_t &dispatch)
    {
        if (auto conversation_id = dispatch.metadata.find (conversation_id_metadata_key)) {
            const auto found = _conversation_actor_ids.find (std::string (*conversation_id));
            if (found != _conversation_actor_ids.end ()) {
                co_return require_actor (found->second, std::string (dispatch.packet_name));
            }
        }
        co_return require_actor (_identity_actor_id, std::string (dispatch.packet_name));
    }

    task_t<ensure_agent_conversation_res_t>
    ensure_agent_conversation_actor (stream_t &stream,
                                     const session_message_context_t &dispatch)
    {
        const auto conversation_id = require_conversation_id (dispatch);
        const auto existing = _conversation_actor_ids.find (conversation_id);
        if (existing != _conversation_actor_ids.end ()) {
            auto actor = require_actor (existing->second, std::string (dispatch.packet_name));
            auto refreshed =
              co_await actor.relay_request (std::string (dispatch.packet_name),
                                            zlink::message_t::from_json (join_conversation_req_t {}))
                .submit ();
            co_return ensure_agent_conversation_res_t{
              actor_location_t::from (actor.ref ()),
              false,
              refreshed.parse_json<join_conversation_res_t> ().state};
        }

        auto ensured =
          co_await _channels
            .request ("supportchat.support",
                      ensure_agent_conversation_req_t{_identity_actor_id, _identity_display_name,
                                                      conversation_id})
            .submit<ensure_agent_conversation_res_t> ();
        auto actor_ref = ensured.actor.to_actor_ref (sample_names_t::mesh);
        auto bound = co_await _actors.bind_or_get (actor_ref).submit ();
        const auto conversation_actor_id = std::string (bound.actor_id ());
        _conversation_actor_ids[conversation_id] = conversation_actor_id;
        co_return ensured;
    }

    static std::string require_conversation_id (const session_message_context_t &dispatch)
    {
        if (auto conversation_id = dispatch.metadata.find (conversation_id_metadata_key)) {
            return std::string (*conversation_id);
        }
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "conversation packet is missing ConversationId metadata");
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
    std::map<std::string, std::string> _conversation_actor_ids;
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

    app.add_zlink_framework ([&] (zlink_framework_options_t &options) {
        options.configure_dispatch ()
          .message_flow (message_flow_log_mode_t::key_transitions)
          .trace_log_file (configuration.flow_log_path ())
          .trace_label ("supportchat-session");
        add_supportchat_location_store (options, topology);
        options.add_client_server_channel ("supportchat.support").client ();
        options.add_client_server_channel ("supportchat.api").client ();
        auto support_spot = options.add_route_mesh (sample_names_t::mesh);
        support_spot.set_routing_id (
          zlink::routing_id_t::from ("supportchat-session"));
        support_spot.set_object_role (object_role_t::client)
          .listen (topology.session_spot_router_endpoint);
        options.add_stream_node ("supportchat-session-stream")
          .bind (topology.session_stream_endpoint)
          .register_session<supportchat_session_t> ();
    });
    return app.run (argc, argv);
}
