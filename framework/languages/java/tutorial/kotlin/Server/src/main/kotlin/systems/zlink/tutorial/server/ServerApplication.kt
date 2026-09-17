package systems.zlink.tutorial.server

import kotlinx.coroutines.Dispatchers
import org.springframework.boot.autoconfigure.SpringBootApplication
import org.springframework.boot.runApplication
import org.springframework.context.annotation.Bean
import systems.zlink.contracts.core.RoutingId
import systems.zlink.framework.kotlin.useCoroutineHandlers
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore
import systems.zlink.framework.spring.EnableZLinkFramework
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer
import systems.zlink.tutorial.server.channel.GetPlayerProfileHandler
import systems.zlink.tutorial.server.channel.HandlerGroups
import systems.zlink.tutorial.server.channel.MaintenanceNoticeSubscriber
import systems.zlink.tutorial.server.dispatch.CallLogFilter
import systems.zlink.tutorial.server.ops.NodeStatusHandler
import systems.zlink.tutorial.server.actors.Player
import systems.zlink.tutorial.server.actors.PlayerFactory
import systems.zlink.tutorial.server.sessions.AuthenticateHandler
import systems.zlink.tutorial.server.sessions.GameSession
import systems.zlink.tutorial.server.sessions.PingHandler
import systems.zlink.tutorial.server.spots.GameRoom
import systems.zlink.tutorial.server.spots.LobbySpot
import systems.zlink.tutorial.shared.GetNodeStatus
import systems.zlink.tutorial.shared.MaintenanceNotice
import systems.zlink.tutorial.shared.NodeStatus

@EnableZLinkFramework
@SpringBootApplication
class ServerApplication {

    @Bean
    fun zlink(): ZLinkFrameworkConfigurer = ZLinkFrameworkConfigurer { options ->
        // --8<-- [start:location-store]
        // Rooms are addressed by id, not by host, so their current location is
        // kept here. Every node reads and writes the same store under the same
        // prefix.
        options.addLocationStore(
            ZLinkRedisLocationStore(
                ZLinkRedisLocationOptions()
                    .setConnectionString("127.0.0.1:6379")
                    .setKeyPrefix("zlink-tutorial-kotlin:location:"),
            ),
        )
        // --8<-- [end:location-store]

        // --8<-- [start:relocation-store]
        // Registering any Spot factory requires this store, even with relocation
        // turned off: the registration itself is the condition.
        options.addRelocationStore(
            ZLinkRedisRelocationStore(
                ZLinkRedisRelocationOptions()
                    .setConnectionString("127.0.0.1:6379")
                    .setKeyPrefix("zlink-tutorial-kotlin:relocation:"),
            ),
        )
        // --8<-- [end:relocation-store]

        // --8<-- [start:coroutine-handlers]
        // Installs the invoker that runs a suspending handler. Without it the
        // handlers in this process are found but cannot be called.
        options.useCoroutineHandlers(Dispatchers.Default)
        // --8<-- [end:coroutine-handlers]

        // Finds the handler classes in this package tree. A channel registration
        // then names the group it wants; discovery alone exposes nothing.
        options.addHandlersFromPackageOf(GetPlayerProfileHandler::class.java)

        // --8<-- [start:filter-register]
        // Registration order is execution order. Filters wrap handlers this node
        // receives; Spot and Actor handlers are not covered.
        options.useFilter(CallLogFilter::class.java)
        // --8<-- [end:filter-register]

        // --8<-- [start:mesh-register]
        // Both sides must name the mesh identically, or they never see each other
        // as peers. The routing id names this node; without it the Framework
        // assigns a generated one, which a caller cannot type into a URL.
        val mesh = options.addRouteMesh("game")
            .listen("tcp://0.0.0.0:7601")
            // What this node tells peers to reach it at. It has to match the
            // endpoint the caller passes to connect(routingId, endpoint) exactly:
            // that form of connect checks the advertised endpoint string, and
            // without this the node would advertise "tcp://0.0.0.0:7601" and the
            // peer would be rejected as an expected-route mismatch.
            .setAdvertiseHost("127.0.0.1")
            .setRoutingId(RoutingId.from("game-server-1"))
        // --8<-- [end:mesh-register]

        // --8<-- [start:channel-register]
        // Only handlers exposed here can be called by other nodes. A handler class
        // sitting in the same package but in another group stays unreachable.
        mesh.channelName("profile").server()
            .addHandlerGroup(HandlerGroups.PROFILE)
        // --8<-- [end:channel-register]

        // --8<-- [start:node-direct-register]
        // Registered on the mesh itself, with no channelName(...) call. Handlers
        // added this way are reached by routing id instead of by channel name.
        mesh.addRouteRequestHandler(
            NodeStatusHandler::class.java,
            GetNodeStatus::class.java,
            NodeStatus::class.java,
        )
        // --8<-- [end:node-direct-register]

        // --8<-- [start:clientserver-register]
        // The caller dials this endpoint directly, so it needs a port of its own
        // and an address to advertise, separate from the mesh.
        options.addClientServerChannel("ticketing")
            .server()
            .listen(7611)
            .setBindHost("127.0.0.1")
            .setAdvertiseHost("127.0.0.1")
            .addHandlerGroup(HandlerGroups.TICKETING)
        // --8<-- [end:clientserver-register]

        // --8<-- [start:fanout-subscribe]
        // connect(endpoint) is the manual subscriber: the publisher's endpoint is
        // given here. enableSubscriber() would take it from the location store
        // instead, and this tutorial runs without one. Mixing the two is rejected
        // at startup.
        options.addFanoutChannel("broadcast")
            .connect("tcp://127.0.0.1:7612")
            .addPublishHandler(
                MaintenanceNoticeSubscriber::class.java,
                MaintenanceNotice::class.java,
            )
        // --8<-- [end:fanout-subscribe]

        // --8<-- [start:object-server]
        // A mesh node picks this role once. Keep the builder and reuse it,
        // because calling objects().server() a second time is rejected at
        // startup.
        val objects = mesh.objects().server()
        // --8<-- [end:object-server]

        // --8<-- [start:spot-register]
        // "game-room" is the stable type a caller names when opening a room. Any
        // node that registers it is a candidate to host one. Exactly one
        // relocation policy is required; moving a live room to another node is a
        // separate topic.
        objects.addSpotFactory<GameRoom>("game-room", GameRoom::class.java) { factory ->
            factory.disableRelocation()
        }
        // --8<-- [end:spot-register]

        // --8<-- [start:actor-register]
        // One lobby per object server. Newly created players start there.
        objects.addEntrySpot(LobbySpot::class.java)

        // Nodes that register "player" are candidates to host one.
        objects.addActorFactory(
            "player",
            Player::class.java,
            PlayerFactory::class.java,
        ) { factory -> factory.disableRelocation() }
        // --8<-- [end:actor-register]

        // --8<-- [start:stream-register]
        // The port game clients connect to. One session type per stream node,
        // and actor dispatch must be on for a session to relay to its player.
        // Session handlers are registered here rather than inside the session,
        // and addSessionPacketHandler takes a plain Class, so the suspending
        // handlers go in by type.
        options.addStreamNode("client-stream")
            .bind("tcp://0.0.0.0:7621")
            .enableActorDispatch()
            .registerSession(GameSession::class.java)
            .addSessionPacketHandler(PingHandler::class.java)
            .addSessionPacketHandler(AuthenticateHandler::class.java)
        // --8<-- [end:stream-register]
    }
}

fun main(args: Array<String>) {
    // The embedded web server on 5381 keeps this process alive, so no keepAlive
    // setting is needed. It serves the admin endpoint in AdminEndpoints.kt only;
    // every channel call still arrives over the mesh ports below.
    runApplication<ServerApplication>(*args)
}
