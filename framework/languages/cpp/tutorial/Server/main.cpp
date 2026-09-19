#include "channel/get_player_profile_handler.hpp"
#include "channel/issue_session_ticket_handler.hpp"
#include "channel/maintenance_notice_subscriber.hpp"
#include "channel/record_login_handler.hpp"
#include "dispatch/call_log_filter.hpp"
#include "ops/channel_weight_handler.hpp"
#include "ops/authenticated_channel_weight_handler.hpp"
#include "ops/node_status_handler.hpp"
#include "actors/player.hpp"
#include "spots/game_room.hpp"
#include "spots/lobby_spot.hpp"
#include "spots/match_queue.hpp"
#include "sessions/game_session.hpp"

#include <zlink/framework.hpp>
#include <zlink/locations/redis.hpp>

namespace fw = zlink::framework;

int main (int argc, char **argv)
{
    auto app = fw::app_t::create ();
    // Handler and filter logs go to this process's console.
    app.logging ().use_console ();

    app.add_zlink_framework ([] (fw::zlink_framework_options_t &options) {
        // --8<-- [start:location-store]
        // Rooms are addressed by id, not by host, so their current location is
        // kept here. Every node reads and writes the same store under the same
        // prefix.
        options
          .add_location_store<fw::redis::redis_location_store_t> ()
          .set_connection_string ("127.0.0.1:6379")
          .set_key_prefix ("zlink-tutorial-cpp:location:");
        // --8<-- [end:location-store]

        // --8<-- [start:relocation-store]
        // Registering any Spot factory requires this store, even with relocation
        // turned off: the registration itself is the condition.
        options
          .add_relocation_store<fw::redis::redis_relocation_store_t> ()
          .set_connection_string ("127.0.0.1:6379")
          .set_key_prefix ("zlink-tutorial-cpp:relocation:");
        // --8<-- [end:relocation-store]

        // --8<-- [start:filter-register]
        // Registration order is execution order. Filters wrap handlers this node
        // receives; Spot and Actor handlers are not covered.
        options.use_filter<call_log_filter_t> ();
        // --8<-- [end:filter-register]

        // --8<-- [start:mesh-register]
        // Both sides must name the mesh identically, or they never see each other
        // as peers. The routing id names this node; without it the Framework
        // assigns a generated one, which a caller cannot type into a URL.
        zlink::routing_id_t routing_id = zlink::routing_id_t::from ("game-server-1");
        auto mesh =
          options
            .add_route_mesh ("game")
            .listen ("tcp://0.0.0.0:7401")
            .set_routing_id (routing_id)
            // A wildcard bind host leaves peers with no address to dial
            // back, so the address to publish is given here.
            .set_advertise_host ("127.0.0.1");
        // --8<-- [end:mesh-register]

        // --8<-- [start:channel-register]
        // Only handlers exposed here can be called by other nodes. A handler class
        // sitting in the same binary but left out stays unreachable.
        mesh
          .channel ("profile")
          .server ()
          .add_request_handler<get_player_profile_handler_t, get_player_profile_t,
                               player_profile_t> ()
          .add_send_handler<record_login_handler_t, record_login_t> ();
        // --8<-- [end:channel-register]

        // --8<-- [start:node-direct-register]
        // Registered on the mesh itself, with no channel(...) call. Handlers added
        // this way are reached by routing id instead of by channel name.
        mesh.add_route_request_handler<node_status_handler_t, get_node_status_t, node_status_t> ();
        // --8<-- [end:node-direct-register]

        // --8<-- [start:clientserver-register]
        // The caller dials this endpoint directly, so it needs a port of its own and
        // an address to advertise, separate from the mesh.
        options
          .add_client_server_channel ("ticketing")
          .server ()
          .listen (7411)
          .set_bind_host ("127.0.0.1")
          .set_advertise_host ("127.0.0.1")
          .add_request_handler<issue_session_ticket_handler_t, issue_session_ticket_t,
                               session_ticket_t> ();
        // --8<-- [end:clientserver-register]

        // --8<-- [start:fanout-subscribe]
        // A fanout handler is registered through a named group, and the channel
        // then maps that group. Without a Location Store the subscriber is told
        // the publisher's endpoint outright; connect(...) is what makes this
        // subscriber manual, so enable_subscriber() must not be added next to it.
        options
          .handlers ()
          .group ("broadcast")
          .add_publish<maintenance_notice_subscriber_t> ();

        options
          .add_fanout_channel ("broadcast")
          .connect ("tcp://127.0.0.1:7412")
          .use_handler_group ("broadcast");
        // --8<-- [end:fanout-subscribe]

        // --8<-- [start:object-server]
        // A mesh node picks this role once. Keep the builder and reuse it,
        // because calling objects().server() a second time is rejected at
        // startup.
        auto objects = mesh.objects ().server ();
        // --8<-- [end:object-server]

        // --8<-- [start:spot-register]
        // "game-room" is the stable type a caller names when opening a room. Any
        // node that registers it is a candidate to host one. Exactly one
        // relocation policy is required; moving a live room to another node is a
        // separate topic.
        objects.add_spot_factory<game_room_t> ("game-room").disable_relocation ();
        // --8<-- [end:spot-register]

        // --8<-- [start:instance-spot-register]
        // Registered the same way, but callers never create one explicitly.
        objects.add_instance_spot_factory<match_queue_t> ("match-queue").disable_relocation ();
        // --8<-- [end:instance-spot-register]

        // --8<-- [start:actor-register]
        // One lobby per object server. Newly created players start there.
        objects.add_entry_spot<lobby_spot_t> ();

        // Nodes that register "player" are candidates to host one.
        objects.add_actor_factory<player_t, player_factory_t> ("player").disable_relocation ();
        // --8<-- [end:actor-register]

        // --8<-- [start:stream-register]
        // The port game clients connect to. One session type per stream node,
        // and actor dispatch must be on for a session to relay to its player.
        options
          .add_stream_node ("client-stream")
          .bind ("tcp://0.0.0.0:7421")
          .enable_actor_dispatch ()
          .register_session<game_session_t> ();
        // --8<-- [end:stream-register]

        // The Server answers messages, so it opened no HTTP surface of its own
        // until this admin endpoint. The port only has to differ from the
        // Client's 5180 when both run on the same host.
        options
          .http ()
          .listen ("http://127.0.0.1:5181")
          .map_post<authenticated_channel_weight_handler_t> ("/admin/channels/{channel}/weight");
    });

    return app.run (argc, argv);
}
