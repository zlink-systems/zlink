#include "../Shared/contracts.hpp"
#include "http_operations.hpp"

#include <zlink/framework.hpp>
#include <zlink/locations/redis.hpp>

#include <nlohmann/json.hpp>

#include <variant>

namespace fw = zlink::framework;

// --8<-- [start:channel-request-call]
// An HTTP handler asks for what it needs in its constructor. route_client_t is
// the mesh caller; the Framework hands it over.
class get_profile_http_handler_t
{
  public:
    explicit get_profile_http_handler_t (fw::route_client_t &routes) : _routes (routes) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        const auto player_id = request.route_values.at ("playerId");

        // The target is a channel name. Which node answers is decided at call time.
        auto profile = co_await _routes
                         .request_to_channel ("profile", get_player_profile_t{player_id})
                         .async<player_profile_t> ();

        co_return fw::http_response_t{200, nlohmann::json (profile).dump ()};
    }

  private:
    fw::route_client_t &_routes;
};
// --8<-- [end:channel-request-call]

// --8<-- [start:channel-send-call]
class record_login_http_handler_t
{
  public:
    explicit record_login_http_handler_t (fw::route_client_t &routes) : _routes (routes) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        const auto player_id = request.route_values.at ("playerId");

        // Returns as soon as the message is sent, with no reply to wait for.
        co_await _routes.send_to_channel ("profile", record_login_t{player_id}).async ();

        co_return fw::http_response_t{202, ""};
    }

  private:
    fw::route_client_t &_routes;
};
// --8<-- [end:channel-send-call]

// --8<-- [start:node-direct-call]
class node_status_http_handler_t
{
  public:
    explicit node_status_http_handler_t (fw::route_client_t &routes) : _routes (routes) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        const auto node_rid = request.route_values.at ("nodeRid");

        // The target is one node, named by its routing id. No channel takes part,
        // so no candidate is chosen: this node answers or the call fails. The
        // first argument is the mesh, not a channel.
        auto status = co_await _routes
                        .request_to_node (
                          "game", zlink::routing_id_t::from (node_rid), get_node_status_t{})
                        .async<node_status_t> ();

        co_return fw::http_response_t{200, nlohmann::json (status).dump ()};
    }

  private:
    fw::route_client_t &_routes;
};
// --8<-- [end:node-direct-call]

// --8<-- [start:clientserver-call]
// A ClientServer channel is not part of any mesh, so it is reached through
// channel_client_t rather than route_client_t. The call itself reads the same.
class issue_ticket_http_handler_t
{
  public:
    explicit issue_ticket_http_handler_t (fw::channel_client_t &channels) : _channels (channels) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        const auto player_id = request.route_values.at ("playerId");

        auto ticket = co_await _channels
                        .request_to_channel ("ticketing", issue_session_ticket_t{player_id})
                        .async<session_ticket_t> ();

        co_return fw::http_response_t{200, nlohmann::json (ticket.value).dump ()};
    }

  private:
    fw::channel_client_t &_channels;
};
// --8<-- [end:clientserver-call]

// --8<-- [start:fanout-call]
class publish_notice_http_handler_t
{
  public:
    explicit publish_notice_http_handler_t (fw::publisher_t &publisher) : _publisher (publisher) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        const auto notice = nlohmann::json::parse (request.body).get<maintenance_notice_t> ();

        // Delivered to every subscriber. No recipient is named. The topic is what
        // a subscriber's handler listens on, and it defaults to the packet name.
        co_await _publisher.publish ("broadcast", maintenance_notice_t::packet_name, notice)
          .async ();

        co_return fw::http_response_t{202, ""};
    }

  private:
    fw::publisher_t &_publisher;
};
// --8<-- [end:fanout-call]

// --8<-- [start:spot-create-call]
// spot_manager_t creates a room; the reply carries the id every later call uses.
class open_room_http_handler_t
{
  public:
    explicit open_room_http_handler_t (fw::spot_manager_t &rooms) : _rooms (rooms) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        const auto body = nlohmann::json::parse (request.body).get<open_room_t> ();

        auto created = co_await _rooms
                         .create ("game-room") // Picks the factory and the candidate nodes.
                         .in_mesh ("game")
                         .creation_request (body) // Reaches the room's create callback.
                         .async ();

        // From here on the room is addressed by this id alone.
        co_return fw::http_response_t{200, nlohmann::json (created.spot.spot_id ()).dump ()};
    }

  private:
    fw::spot_manager_t &_rooms;
};
// --8<-- [end:spot-create-call]

// --8<-- [start:spot-message-call]
// --8<-- [start:spot-send-call]
class post_chat_http_handler_t
{
  public:
    explicit post_chat_http_handler_t (fw::route_client_t &routes) : _routes (routes) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        const auto room_id = request.route_values.at ("roomId");
        const auto message = nlohmann::json::parse (request.body).get<post_chat_t> ();

        // The id is enough; the Framework resolves where the room currently runs.
        co_await _routes.send_to_spot (room_id, message).async ();

        co_return fw::http_response_t{202, ""};
    }

  private:
    fw::route_client_t &_routes;
};
// --8<-- [end:spot-send-call]

// --8<-- [start:spot-request-call]
class get_room_state_http_handler_t
{
  public:
    explicit get_room_state_http_handler_t (fw::route_client_t &routes) : _routes (routes) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        const auto room_id = request.route_values.at ("roomId");

        auto state = co_await _routes.request_to_spot (room_id, get_room_state_t{})
                       .timeout (std::chrono::seconds (3))
                       .async<room_state_t> ();

        co_return fw::http_response_t{200, nlohmann::json (state).dump ()};
    }

  private:
    fw::route_client_t &_routes;
};
// --8<-- [end:spot-request-call]
// --8<-- [end:spot-message-call]

// --8<-- [start:instance-spot-call]
class join_match_queue_http_handler_t
{
  public:
    explicit join_match_queue_http_handler_t (fw::route_client_t &routes) : _routes (routes) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        const auto mode = request.route_values.at ("mode");
        const auto body = nlohmann::json::parse (request.body).get<join_match_queue_t> ();

        // No create call: the first message for this id brings the queue into
        // being and is then handled by it.
        auto status = co_await _routes.request_to_spot (fw::spot_id_t (mode), body)
                        .instance_spot ("match-queue")
                        .in_mesh ("game")
                        .timeout (std::chrono::seconds (3))
                        .async<match_queue_status_t> ();

        co_return fw::http_response_t{200, nlohmann::json (status).dump ()};
    }

  private:
    fw::route_client_t &_routes;
};
// --8<-- [end:instance-spot-call]

// --8<-- [start:location-find]
// find answers from the Location Store alone: it reports where the object is,
// and only while it is ready to receive. Nothing is sent to the object.
class find_room_http_handler_t
{
  public:
    explicit find_room_http_handler_t (fw::spot_manager_t &rooms) : _rooms (rooms) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        const auto room_id = request.route_values.at ("roomId");

        auto room = co_await _rooms.find (room_id);
        if (!room)
            co_return fw::http_response_t{404, ""};

        co_return fw::http_response_t{
          200,
          nlohmann::json{{"spotId", room->spot_id ()},
                         {"node", std::string (room->node_rid ().value ())}}
            .dump ()};
    }

  private:
    fw::spot_manager_t &_rooms;
};

class find_player_http_handler_t
{
  public:
    explicit find_player_http_handler_t (fw::actor_manager_t &players) : _players (players) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        const auto player_id = request.route_values.at ("playerId");

        auto player = co_await _players.find (fw::actor_id_t (player_id));
        if (!player)
            co_return fw::http_response_t{404, ""};

        co_return fw::http_response_t{
          200,
          nlohmann::json{{"actorId", std::string (player->actor_id ().value ())},
                         {"node", std::string (player->node_rid ().value ())}}
            .dump ()};
    }

  private:
    fw::actor_manager_t &_players;
};
// --8<-- [end:location-find]

// --8<-- [start:actor-create-call]
// get_or_create returns the existing player if there is one. The caller does not
// choose which node hosts it.
class create_player_http_handler_t
{
  public:
    explicit create_player_http_handler_t (fw::actor_manager_t &players) : _players (players) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        const auto player_id = request.route_values.at ("playerId");
        const auto body = nlohmann::json::parse (request.body).get<create_player_t> ();

        auto created = co_await _players.get_or_create (fw::actor_id_t (player_id), "player")
                         .in_mesh ("game")
                         .creation_request (body)
                         .timeout (std::chrono::seconds (10))
                         .async ();

        const auto state = std::holds_alternative<fw::actor_create_existing_t> (created)
                             ? "existing"
                           : std::holds_alternative<fw::actor_create_created_t> (created)
                             ? "created"
                             : "rejected";
        co_return fw::http_response_t{200, nlohmann::json (state).dump ()};
    }

  private:
    fw::actor_manager_t &_players;
};
// --8<-- [end:actor-create-call]

// --8<-- [start:actor-send-call]
class change_nickname_http_handler_t
{
  public:
    explicit change_nickname_http_handler_t (fw::actor_client_t &players) : _players (players) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        const auto player_id = request.route_values.at ("playerId");
        const auto body = nlohmann::json::parse (request.body).get<change_nickname_t> ();

        // Addressed by player id, like a room is by room id.
        co_await _players.send (fw::actor_id_t (player_id), body).async ();

        co_return fw::http_response_t{202, ""};
    }

  private:
    fw::actor_client_t &_players;
};
// --8<-- [end:actor-send-call]

// --8<-- [start:actor-request-call]
class get_player_http_handler_t
{
  public:
    explicit get_player_http_handler_t (fw::actor_client_t &players) : _players (players) {}

    fw::task_t<fw::http_response_t> handle (const fw::http_request_t &request)
    {
        const auto player_id = request.route_values.at ("playerId");

        auto info = co_await _players.request (fw::actor_id_t (player_id), get_player_t{})
                      .timeout (std::chrono::seconds (3))
                      .async<player_info_t> ();

        co_return fw::http_response_t{200, nlohmann::json (info).dump ()};
    }

  private:
    fw::actor_client_t &_players;
};
// --8<-- [end:actor-request-call]

int main (int argc, char **argv)
{
    auto app = fw::app_t::create ();
    app.logging ().use_console ();

    app.add_zlink_framework ([] (fw::zlink_framework_options_t &options) {
        // --8<-- [start:location-store-client]
        // Rooms are looked up by whoever calls them, so a node that hosts none
        // still needs the store, pointed at the same prefix.
        options.add_location_store<fw::redis::redis_location_store_t> ()
          .set_connection_string ("127.0.0.1:6379")
          .set_key_prefix ("zlink-tutorial-cpp:location:");
        // --8<-- [end:location-store-client]

        // --8<-- [start:channel-client-register]
        // This node opens an endpoint too. Both sides listen to become peers.
        zlink::routing_id_t client_rid = zlink::routing_id_t::from ("game-client-1");
        auto mesh = options.add_route_mesh ("game")
                      .listen ("tcp://0.0.0.0:7402")
                      .set_routing_id (client_rid)
                      .set_advertise_host ("127.0.0.1");

        // client() means this node exposes no handler for the channel; it only calls.
        mesh.channel ("profile").client ();

        // A mesh peer connection, not a channel one. The mesh picks a node that
        // serves the channel from among the peers it learns this way, so a channel
        // call never names a node. The routing id is here so this node can also
        // address that one directly; connect(endpoint) alone would not allow that.
        zlink::routing_id_t server_routing_id = zlink::routing_id_t::from ("game-server-1");
        mesh.peer_connections ().connect (server_routing_id, "tcp://127.0.0.1:7401");
        // --8<-- [end:channel-client-register]

        // --8<-- [start:clientserver-client-register]
        // Here the caller decides who answers: the server it dialed. Mesh peers play
        // no part in the choice.
        options.add_client_server_channel ("ticketing").client ().connect ("tcp://127.0.0.1:7411");
        // --8<-- [end:clientserver-client-register]

        // --8<-- [start:fanout-publish-register]
        // The publisher keeps no subscriber list. Subscribers may come and go with
        // no change here. A concrete bind host is required: a wildcard one leaves
        // subscribers with no address to dial and is rejected at startup.
        options.add_fanout_channel ("broadcast")
          .enable_publisher ("tcp://127.0.0.1:7412")
          .set_no_drop (true);
        // --8<-- [end:fanout-publish-register]

        // --8<-- [start:spot-client-register]
        // client() rules out registering factories. This node creates and calls
        // rooms; a node that picked server() runs them.
        mesh.objects ().client ();
        // --8<-- [end:spot-client-register]

        // The HTTP surface the examples are driven through.
        options.http ()
          .listen ("http://127.0.0.1:5180")
          .map_get<get_profile_http_handler_t> ("/players/{playerId}/profile")
          .map_post<record_login_http_handler_t> ("/players/{playerId}/logins")
          .map_get<node_status_http_handler_t> ("/ops/nodes/{nodeRid}/status")
          .map_post<issue_ticket_http_handler_t> ("/players/{playerId}/tickets")
          .map_post<publish_notice_http_handler_t> ("/notices")
          .map_post<open_room_http_handler_t> ("/rooms")
          .map_post<post_chat_http_handler_t> ("/rooms/{roomId}/chat")
          .map_get<get_room_state_http_handler_t> ("/rooms/{roomId}")
          .map_post<join_match_queue_http_handler_t> ("/match-queues/{mode}")
          .map_post<create_player_http_handler_t> ("/players/{playerId}")
          .map_post<change_nickname_http_handler_t> ("/players/{playerId}/nickname")
          .map_get<get_player_http_handler_t> ("/players/{playerId}")
          .map_get<redirect_player_http_handler_t> ("/player/{playerId}")
          .map_get<export_room_http_handler_t> ("/rooms/{roomId}/export")
          .map_post<import_room_http_handler_t> ("/rooms/{roomId}/import")
          .map_get<find_room_http_handler_t> ("/locations/rooms/{roomId}")
          .map_get<find_player_http_handler_t> ("/locations/players/{playerId}");
    });

    return app.run (argc, argv);
}
