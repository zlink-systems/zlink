package systems.zlink.tutorial.server;

import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.context.annotation.Bean;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.channels.ZLinkRouteMeshRuntimeOptions;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.configuration.ZLinkMeshObjectServerBuilder;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisRelocationStore;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.tutorial.server.channel.GetPlayerProfileHandler;
import systems.zlink.tutorial.server.channel.IssueSessionTicketHandler;
import systems.zlink.tutorial.server.channel.MaintenanceNoticeSubscriber;
import systems.zlink.tutorial.server.channel.RecordLoginHandler;
import systems.zlink.tutorial.server.dispatch.CallLogFilter;
import systems.zlink.tutorial.server.ops.NodeStatusHandler;
import systems.zlink.tutorial.server.actors.Player;
import systems.zlink.tutorial.server.actors.PlayerFactory;
import systems.zlink.tutorial.server.sessions.AuthenticateHandler;
import systems.zlink.tutorial.server.sessions.GameSession;
import systems.zlink.tutorial.server.sessions.PingHandler;
import systems.zlink.tutorial.server.spots.GameRoom;
import systems.zlink.tutorial.server.spots.LobbySpot;
import systems.zlink.tutorial.shared.Contracts;

@EnableZLinkFramework
@SpringBootApplication
public class ServerApplication {

    public static void main(String[] args) {
        // The admin endpoint below keeps this process alive, the same way the
        // tutorial endpoints keep the Client alive. No setKeepAlive call is needed.
        SpringApplication.run(ServerApplication.class, args);
    }

    @Bean
    ZLinkFrameworkConfigurer zlink() {
        return options -> {
            // --8<-- [start:location-store]
            // Rooms are addressed by id, not by host, so their current location is
            // kept here. Every node reads and writes the same store under the same
            // prefix.
            var locationOptions =
                new ZLinkRedisLocationOptions()
                    .setConnectionString(
                        "127.0.0.1:6379")
                    .setKeyPrefix(
                        "zlink-tutorial-java:location:");
            options.addLocationStore(
                new ZLinkRedisLocationStore(
                    locationOptions));
            // --8<-- [end:location-store]

            // --8<-- [start:relocation-store]
            // Registering any Spot factory requires this store, even with
            // relocation turned off: the registration itself is the condition.
            options.addRelocationStore(new ZLinkRedisRelocationStore(
                new ZLinkRedisRelocationOptions()
                    .setConnectionString("127.0.0.1:6379")
                    .setKeyPrefix("zlink-tutorial-java:relocation:")));
            // --8<-- [end:relocation-store]

            // --8<-- [start:filter-register]
            // Registration order is execution order. Filters wrap handlers this node
            // receives; Spot and Actor handlers are not covered.
            options.useFilter(CallLogFilter.class);
            // --8<-- [end:filter-register]

            // --8<-- [start:mesh-register]
            // Both sides must name the mesh identically, or they never see each other
            // as peers. The routing id names this node; without it the Framework assigns
            // a generated one, which a caller cannot type into a URL.
            //
            // The advertise host is what this node puts in the descriptor it hands a
            // peer. A caller that names a routing id in connect(...) compares that
            // descriptor against the endpoint it dialed, so the bind address 0.0.0.0
            // has to be replaced by an address the caller actually used. Leave it out
            // and admission is refused, silently, as long as the process runs.
            ZLinkMeshNodeBuilder mesh = options
                .addRouteMesh("game")
                .listen("tcp://0.0.0.0:7501")
                .setAdvertiseHost("127.0.0.1")
                .setRoutingId(
                    RoutingId.from("game-server-1"));
            // --8<-- [end:mesh-register]

            // --8<-- [start:channel-register]
            // Only handlers exposed here can be called by other nodes. A handler class
            // sitting in the same source tree but left out stays unreachable.
            mesh.channelName("profile").server()
                .addRequestHandler(
                    GetPlayerProfileHandler.class,
                    Contracts.GetPlayerProfile.class,
                    Contracts.PlayerProfile.class)
                .addSendHandler(
                    RecordLoginHandler.class,
                    Contracts.RecordLogin.class);
            // --8<-- [end:channel-register]

            // --8<-- [start:node-direct-register]
            // Registered on the mesh itself, with no channelName(...) call. Handlers
            // added this way are reached by routing id instead of by channel name.
            mesh.addRouteRequestHandler(
                NodeStatusHandler.class,
                Contracts.GetNodeStatus.class,
                Contracts.NodeStatus.class);
            // --8<-- [end:node-direct-register]

            // --8<-- [start:clientserver-register]
            // The caller dials this endpoint directly, so it needs a port of its own and
            // an address to advertise, separate from the mesh.
            options.addClientServerChannel("ticketing")
                .server()
                .listen(7511)
                .setBindHost("127.0.0.1")
                .setAdvertiseHost("127.0.0.1")
                .addRequestHandler(
                    IssueSessionTicketHandler.class,
                    Contracts.IssueSessionTicket.class,
                    Contracts.SessionTicket.class);
            // --8<-- [end:clientserver-register]

            // --8<-- [start:fanout-subscribe]
            // connect(...) names the publisher endpoint by hand, which is what a node
            // without a Location Store has to do. enableSubscriber() is the other way:
            // it asks the store where the publisher is, and combining the two is
            // rejected at startup.
            options.addFanoutChannel("broadcast")
                .connect("tcp://127.0.0.1:7512")
                .addPublishHandler(
                    MaintenanceNoticeSubscriber.class,
                    Contracts.MaintenanceNotice.class);
            // --8<-- [end:fanout-subscribe]

            // --8<-- [start:object-server]
            // A mesh node picks this role once. Keep the builder and reuse it,
            // because calling objects().server() a second time is rejected at
            // startup.
            ZLinkMeshObjectServerBuilder objects =
                mesh.objects().server();
            // --8<-- [end:object-server]

            // --8<-- [start:spot-register]
            // "game-room" is the stable type a caller names when opening a room.
            // Any node that registers it is a candidate to host one. Exactly one
            // relocation policy is required; moving a live room to another node is
            // a separate topic.
            objects.addSpotFactory(
                "game-room",
                GameRoom.class,
                factory -> factory.disableRelocation());
            // --8<-- [end:spot-register]

            // --8<-- [start:actor-register]
            // One lobby per object server. Newly created players start there.
            objects.addEntrySpot(LobbySpot.class);

            // Nodes that register "player" are candidates to host one.
            objects.addActorFactory(
                "player",
                Player.class,
                PlayerFactory.class,
                factory -> factory.disableRelocation());
            // --8<-- [end:actor-register]

            // --8<-- [start:stream-register]
            // The port game clients connect to. One session type per stream node,
            // and actor dispatch must be on for a session to relay to its player.
            // Session handlers are registered here rather than inside the session.
            options.addStreamNode("client-stream")
                .bind("tcp://0.0.0.0:7521")
                .enableActorDispatch()
                .registerSession(GameSession.class)
                .addSessionPacketHandler(
                    PingHandler.class)
                .addSessionPacketHandler(
                    AuthenticateHandler.class);
            // --8<-- [end:stream-register]
        };
    }
}

// --8<-- [start:weight-runtime]
// Weight is the one value this node can change while running. 0 keeps the
// socket open and finishes in-flight work, but other nodes stop choosing this
// one for new calls. 100 is the normal value.
//
// ZLinkRouteMeshRuntimeOptions is a bean the Framework registers; ask for it in
// the constructor. The one-argument channel(...) resolves the mesh itself, and
// fails if two meshes register the same channel name.
@RestController
class ChannelWeightController {

    private final ZLinkRouteMeshRuntimeOptions mesh;

    ChannelWeightController(ZLinkRouteMeshRuntimeOptions mesh) {
        this.mesh = mesh;
    }

    @PostMapping("/admin/channels/{channel}/weight")
    WeightChanged weight(@PathVariable String channel, @RequestParam int value) {
        mesh.channel(channel).weight(value);
        return new WeightChanged(channel, value);
    }

    record WeightChanged(String channel, int weight) {
    }
}
// --8<-- [end:weight-runtime]
