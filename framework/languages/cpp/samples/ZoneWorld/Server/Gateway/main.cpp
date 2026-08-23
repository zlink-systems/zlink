/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "../Configuration/configuration.hpp"
#include "../Configuration/location_store.hpp"
#include "../../Shared/world_rules.hpp"

#include <zlink/framework.hpp>

#include <array>
#include <iostream>
#include <optional>
#include <variant>

namespace zlink::samples::zoneworld
{
namespace fw = zlink::framework;

class game_session_t final : public fw::packet_stream_session_t
{
  public:
    using dependency_types = fw::dependency_list_t<fw::actor_client_t>;

    explicit game_session_t (fw::actor_client_t &actor_client) : _actor_client (actor_client) {}

    fw::task_t<void> on_connected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_disconnected (fw::stream_t &) override
    {
        _player_id.reset ();
        co_return;
    }
    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &) override { co_return; }

    fw::task_t<void> on_packet (fw::stream_t &stream,
                                const fw::session_message_context_t &dispatch,
                                const zlink::message_t &payload) override
    {
        const auto packet = std::string (dispatch.packet_name);
        if (packet == actor_location_probe_req_t::packet_name) {
            const auto request = payload.parse_json<actor_location_probe_req_t> ();
            actor_location_probe_res_t response;
            try {
                response =
                  co_await _actor_client.request (fw::actor_id_t (request.actor_id), request)
                    .submit<actor_location_probe_res_t> ();
            }
            catch (const fw::framework_exception_t &) {
                response = {request.actor_id, 0, {}, errors_t::not_found};
            }
            stream.reply_packet (zlink::message_t::from_json (response)).submit ();
            co_return;
        }
        if (packet == fresh_actor_probe_req_t::packet_name) {
            const auto request = payload.parse_json<fresh_actor_probe_req_t> ();
            try {
                auto &actors = stream.actors ();
                auto located = actors.get_or_create (names_t::player_actor, request.actor_id);
                if (!located)
                    throw fw::framework_exception_t (located.error_kind (),
                                                     "fresh actor creation was rejected");
                auto bound = co_await actors.bind_or_get (located.value ().ref ()).submit ();
                const auto observed = co_await _actor_client
                                        .request (fw::actor_id_t (request.actor_id),
                                                  actor_location_probe_req_t{request.actor_id})
                                        .submit<actor_location_probe_res_t> ();
                stream
                  .reply_packet (zlink::message_t::from_json (
                    fresh_actor_probe_res_t{observed.actor_id, observed.object_generation,
                                            observed.owner_node_rid, observed.error}))
                  .submit ();
            }
            catch (const fw::framework_exception_t &error) {
                stream
                  .reply_packet (zlink::message_t::from_json (fresh_actor_probe_res_t{
                    request.actor_id,
                    0,
                    {},
                    error.kind () == fw::framework_error_kind_t::deadline_exceeded
                      ? errors_t::deadline_exceeded
                      : errors_t::unavailable}))
                  .submit ();
            }
            co_return;
        }
        if (packet == message_follow_probe_req_t::packet_name) {
            const auto request = payload.parse_json<message_follow_probe_req_t> ();
            try {
                const auto reply =
                  co_await _actor_client.request (fw::actor_id_t (request.actor_id), request)
                    .submit<message_follow_probe_res_t> ();
                stream.reply_packet (zlink::message_t::from_json (reply)).submit ();
            }
            catch (const fw::framework_exception_t &error) {
                stream
                  .reply_packet (zlink::message_t::from_json (message_follow_probe_res_t{
                    request.probe_id,
                    {},
                    error.kind () == fw::framework_error_kind_t::deadline_exceeded
                      ? errors_t::deadline_exceeded
                      : errors_t::unavailable}))
                  .submit ();
            }
            co_return;
        }
        if (packet == message_follow_probe_msg_t::packet_name) {
            const auto message = payload.parse_json<message_follow_probe_msg_t> ();
            try {
                co_await _actor_client.send (fw::actor_id_t (message.actor_id), message).submit ();
            }
            catch (const fw::framework_exception_t &) {
            }
            co_return;
        }

        if (!_player_id) {
            if (packet != join_world_req_t::packet_name)
                throw fw::framework_exception_t (fw::framework_error_kind_t::protocol_error,
                                                 "JoinWorldReq must be the first game packet");
            const auto request = payload.parse_json<join_world_req_t> ();
            auto &actors = stream.actors ();
            auto located = actors.get_or_create (names_t::player_actor, request.player_id);
            if (!located)
                throw fw::framework_exception_t (located.error_kind (),
                                                 "player actor could not be located");
            auto bound = co_await actors.bind_or_get (located.value ().ref ()).submit ();
            _player_id = std::string (bound.actor_id ());
        }

        auto actor = stream.actors ().find (*_player_id);
        if (!actor)
            throw fw::framework_exception_t (fw::framework_error_kind_t::not_found,
                                             "bound player actor was not found");
        if (packet == join_world_req_t::packet_name) {
            co_await actor->relay (packet, payload);
            co_return;
        }
        if (packet == move_msg_t::packet_name) {
            co_await actor->relay (packet, payload);
            co_return;
        }
        if (packet == crash_relocation_probe_msg_t::packet_name) {
            co_await actor->relay (packet, payload);
            co_return;
        }
        throw fw::framework_exception_t (fw::framework_error_kind_t::protocol_error,
                                         "unsupported game packet: " + packet);
    }

  private:
    fw::actor_client_t &_actor_client;
    std::optional<std::string> _player_id;
};

struct world_bootstrap_req_t
{
    static constexpr const char *packet_name = "ZoneWorldBootstrapReq";
};
struct world_bootstrap_res_t
{
    static constexpr const char *packet_name = "ZoneWorldBootstrapRes";
    int zones = 0;
    int bots = 0;
};
inline void to_json (nlohmann::json &j, const world_bootstrap_req_t &)
{
    j = nlohmann::json::object ();
}
inline void from_json (const nlohmann::json &, world_bootstrap_req_t &)
{
}
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE (world_bootstrap_res_t, zones, bots)

class world_bootstrap_handler_t
{
  public:
    using request_type = world_bootstrap_req_t;
    using reply_type = world_bootstrap_res_t;
    using dependency_types =
      fw::dependency_list_t<fw::spot_manager_t, fw::actor_manager_t, fw::actor_client_t>;
    static constexpr const char *topic_name = "ZoneWorldBootstrapReq";

    world_bootstrap_handler_t (fw::spot_manager_t &spots,
                               fw::actor_manager_t &directory,
                               fw::actor_client_t &actors) :
        _spots (spots), _directory (directory), _actors (actors)
    {
    }

    fw::task_t<world_bootstrap_res_t> handle (const world_bootstrap_req_t &)
    {
        int zone_count = 0;
        for (const auto &zone : all_zones ()) {
            const auto created =
              co_await _spots.get_or_create (fw::spot_id_t (zone), names_t::zone_spot)
                .in_mesh (names_t::mesh)
                .submit ();
            if (created.state != fw::spot_create_state_t::rejected)
                ++zone_count;
        }

        struct route_t
        {
            const char *id;
            int x;
            int y;
            int dx;
            int dy;
        };
        constexpr std::array routes{
          route_t{"bot-nw-x", 10, 15, 1, 0},  route_t{"bot-nw-y", 15, 10, 0, 1},
          route_t{"bot-ne-x", 90, 15, -1, 0}, route_t{"bot-ne-y", 85, 10, 0, 1},
          route_t{"bot-sw-x", 10, 85, 1, 0},  route_t{"bot-sw-y", 15, 90, 0, -1},
          route_t{"bot-se-x", 90, 85, -1, 0}, route_t{"bot-se-y", 85, 90, 0, -1}};
        std::vector<fw::actor_id_t> actor_ids;
        int bot_count = 0;
        for (const auto &route : routes) {
            const auto created =
              co_await _directory.get_or_create (fw::actor_id_t (route.id), names_t::player_actor)
                .in_mesh (names_t::mesh)
                .submit ();
            if (const auto *value = std::get_if<fw::actor_create_created_t> (&created)) {
                actor_ids.push_back (value->actor.actor_id ());
                ++bot_count;
            } else if (const auto *value = std::get_if<fw::actor_create_existing_t> (&created))
                actor_ids.push_back (value->actor.actor_id ());
            else
                throw fw::framework_exception_t (fw::framework_error_kind_t::unavailable,
                                                 "bot actor creation was rejected");
        }
        for (std::size_t index = 0; index < routes.size (); ++index) {
            const auto &route = routes[index];
            const auto entered =
              co_await _actors
                .request (actor_ids[index],
                          enter_world_req_t{route.x, route.y, true, route.dx, route.dy})
                .submit<enter_world_res_t> ();
            if (entered.error)
                throw fw::framework_exception_t (fw::framework_error_kind_t::rejected,
                                                 *entered.error);
            std::cout << "zoneworld-bot-spawned bot=" << route.id << " zone=" << entered.zone_id
                      << " start=" << route.x << ',' << route.y << " dir=" << route.dx << ','
                      << route.dy << std::endl;
        }
        co_return world_bootstrap_res_t{zone_count, bot_count};
    }

  private:
    fw::spot_manager_t &_spots;
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
        mesh.set_automatic_routing_id_prefix ("gw").set_object_role (fw::object_role_t::client);
        if (configuration.mesh_advertise_host)
            mesh.set_advertise_host (*configuration.mesh_advertise_host);
        mesh.channel_name (names_t::zone_channel).client ();
        mesh.listen (configuration.mesh_endpoint);
        options.add_stream_node (names_t::gateway_stream)
          .bind (configuration.stream_endpoint)
          .enable_actor_dispatch ()
          .register_session<game_session_t> ();
        options.http ()
          .listen (configuration.bootstrap_http_endpoint)
          .map_health ("/health")
          .map_post<world_bootstrap_handler_t> ("/bootstrap-world");
    });
    std::cout << "zoneworld-role-ready role=gateway\n";
    return app.run (argc, argv);
}
