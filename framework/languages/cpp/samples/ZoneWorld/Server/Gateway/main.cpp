/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Configuration/configuration.hpp"
#include "../Configuration/location_store.hpp"
#include "../../Shared/world_rules.hpp"

#include <zlink/framework.hpp>

#include <array>
#include <iostream>
#include <optional>

namespace zlink::samples::zoneworld
{
namespace fw = zlink::framework;

class game_session_t final : public fw::packet_stream_session_t
{
  public:
    using dependency_types =
      fw::dependency_list_t<fw::session_actor_manager_t, fw::actor_client_t>;
    game_session_t (fw::session_actor_manager_t &actors, fw::actor_client_t &actor_client) :
        _actors (actors), _actor_client (actor_client)
    {
    }

    fw::task_t<void> on_connected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_disconnected (fw::stream_t &) override { _player_id.reset (); co_return; }
    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &) override { co_return; }

    fw::task_t<void> on_packet (fw::stream_t &stream,
                                const fw::session_message_context_t &dispatch,
                                const zlink::message_t &payload) override
    {
        if (dispatch.packet_name == message_follow_probe_req_t::packet_name) {
            const auto request = payload.parse_json<message_follow_probe_req_t> ();
            if (dispatch.can_reply) {
                const auto reply = co_await _actor_client
                  .request (fw::actor_id_t (request.actor_id), request)
                  .submit<message_follow_probe_res_t> ();
                stream.reply_packet (zlink::message_t::from_json (reply)).submit ();
            } else {
                co_await _actor_client.send (fw::actor_id_t (request.actor_id), request).submit ();
            }
            co_return;
        }
        if (!_player_id) {
            if (dispatch.packet_name != join_world_req_t::packet_name)
                throw fw::framework_exception_t (fw::framework_error_kind_t::protocol_error,
                                                   "JoinWorldReq must be the first packet");
            const auto request = payload.parse_json<join_world_req_t> ();
            auto located = _actors.get_or_create (names_t::player_actor, request.player_id);
            if (!located)
                throw fw::framework_exception_t (located.error_kind (), "player actor could not be located");
            auto bound = co_await _actors.bind_or_get (located.value ().ref ()).submit ();
            _player_id = std::string (bound.actor_id ());
        }

        if (dispatch.packet_name == move_msg_t::packet_name) {
            co_await _actor_client
              .send (fw::actor_id_t (*_player_id), payload.parse_json<move_msg_t> ())
              .submit ();
            co_return;
        }

        auto actor = _actors.find (*_player_id);
        if (!actor)
            throw fw::framework_exception_t (fw::framework_error_kind_t::not_found,
                                               "bound player actor was not found");
        if (dispatch.can_reply) {
            auto reply = co_await actor->relay_request (payload).submit ();
            stream.reply_packet (reply).submit ();
        } else {
            co_await actor->relay (payload);
        }
    }

  private:
    fw::session_actor_manager_t &_actors;
    fw::actor_client_t &_actor_client;
    std::optional<std::string> _player_id;
};

struct bot_bootstrap_req_t { static constexpr const char *packet_name = "ZoneWorldBotBootstrapReq"; };
struct bot_bootstrap_res_t { static constexpr const char *packet_name = "ZoneWorldBotBootstrapRes"; int bots = 0; };
inline void to_json (nlohmann::json &j, const bot_bootstrap_req_t &) { j = nlohmann::json::object (); }
inline void from_json (const nlohmann::json &, bot_bootstrap_req_t &) {}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE (bot_bootstrap_res_t, bots)

class bot_bootstrap_handler_t
{
  public:
    using request_type = bot_bootstrap_req_t;
    using reply_type = bot_bootstrap_res_t;
    using dependency_types =
      fw::dependency_list_t<fw::actor_manager_t, fw::actor_client_t>;
    static constexpr const char *topic_name = "ZoneWorldBotBootstrapReq";

    bot_bootstrap_handler_t (fw::actor_manager_t &directory, fw::actor_client_t &actors) :
        _directory (directory), _actors (actors)
    {
    }

    fw::task_t<bot_bootstrap_res_t> handle (const bot_bootstrap_req_t &)
    {
        struct route_t { const char *id; int x; int y; int dx; int dy; };
        constexpr std::array routes{
          route_t{"bot-nw-x", 10, 15, 1, 0}, route_t{"bot-nw-y", 15, 10, 0, 1},
          route_t{"bot-ne-x", 90, 15, -1, 0}, route_t{"bot-ne-y", 85, 10, 0, 1},
          route_t{"bot-sw-x", 10, 85, 1, 0}, route_t{"bot-sw-y", 15, 90, 0, -1},
          route_t{"bot-se-x", 90, 85, -1, 0}, route_t{"bot-se-y", 85, 90, 0, -1}};
        std::vector<fw::actor_id_t> actor_ids;
        actor_ids.reserve (routes.size ());
        int created_count = 0;
        for (const auto &route : routes) {
            std::cerr << "zoneworld-bot-create bot=" << route.id << '\n';
            const auto created = co_await _directory
              .get_or_create (fw::actor_id_t (route.id), names_t::player_actor)
              .in_mesh (names_t::mesh).submit ();
            if (const auto *value = std::get_if<fw::actor_create_created_t> (&created)) {
                actor_ids.push_back (value->actor.actor_id ());
                ++created_count;
            } else if (const auto *value = std::get_if<fw::actor_create_existing_t> (&created)) {
                actor_ids.push_back (value->actor.actor_id ());
            }
            std::cerr << "zoneworld-bot-created bot=" << route.id << '\n';
        }
        for (std::size_t index = 0; index < routes.size (); ++index) {
            const auto &route = routes[index];
            try {
                const auto entered = co_await _actors
                  .request (actor_ids[index],
                            enter_world_req_t{route.x, route.y, true, route.dx, route.dy})
                  .submit<enter_world_res_t> ();
                std::cerr << "zoneworld-bot-entered bot=" << route.id
                          << " zone=" << entered.zone_id << '\n';
            } catch (const std::exception &error) {
                std::cerr << "zoneworld-bot-enter-failed bot=" << route.id
                          << " error=" << error.what () << '\n';
            }
        }
        co_return bot_bootstrap_res_t{created_count};
    }

  private:
    fw::actor_manager_t &_directory;
    fw::actor_client_t &_actors;
};

} // namespace zlink::samples::zoneworld

int main (int argc, char **argv)
{
    using namespace zlink::samples::zoneworld;
    namespace fw = zlink::framework;
    auto app = fw::app_t::create ();
    const auto configuration = load_configuration (app, argc, argv);
    app.logging ().use_console ().set_min_level (fw::log_level_t::info);
    app.add_zlink_framework ([&] (fw::zlink_framework_options_t &options) {
        add_stores (options, configuration);
        auto mesh = options.add_route_mesh (names_t::mesh);
        mesh.set_automatic_routing_id_prefix ("gw0").set_object_role (fw::object_role_t::client);
        mesh.channel_name (names_t::zone_channel).client ();
        mesh.listen (configuration.mesh_endpoint);
        options.add_stream_node (names_t::gateway_stream)
          .bind (configuration.stream_endpoint)
          .enable_actor_dispatch ()
          .register_session<game_session_t> ();
        options.http ().listen (configuration.bootstrap_http_endpoint)
          .map_health ("/health")
          .map_post<bot_bootstrap_handler_t> ("/bootstrap-bots");
    });
    std::cout << "zoneworld-role-ready role=gateway\n";
    return app.run (argc, argv);
}
