package systems.zlink.tutorial.client;

import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.context.annotation.Bean;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RestController;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.channels.ZLinkFanoutClient;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.configuration.ZLinkMeshNodeBuilder;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.spots.SpotRef;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.spring.EnableZLinkFramework;
import systems.zlink.framework.spring.ZLinkFrameworkConfigurer;
import systems.zlink.tutorial.shared.Contracts;

import java.time.Duration;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletionStage;

@EnableZLinkFramework
@SpringBootApplication
public class ClientApplication {

    public static void main(String[] args) {
        // The HTTP surface the examples are driven through keeps this process
        // alive, so no setKeepAlive call is needed here.
        SpringApplication.run(ClientApplication.class, args);
    }

    @Bean
    ZLinkFrameworkConfigurer zlink() {
        return options -> {
            // --8<-- [start:location-store-client]
            // Rooms are looked up by whoever calls them, so a node that hosts none
            // still needs the store, pointed at the same prefix. Every other peer
            // this node needs is named by hand below.
            options.addLocationStore(
                    new ZLinkRedisLocationStore(
                            new ZLinkRedisLocationOptions()
                                    .setConnectionString("127.0.0.1:6379")
                                    .setKeyPrefix("zlink-tutorial-java:location:")));
            // --8<-- [end:location-store-client]

            // --8<-- [start:channel-client-register]
            // This node opens an endpoint too. Both sides listen to become peers.
            ZLinkMeshNodeBuilder mesh = options.addRouteMesh("game").listen("tcp://127.0.0.1:7502");

            // client() means this node exposes no handler for the channel; it only calls.
            mesh.channelName("profile").client();

            // A mesh peer connection, not a channel one. The mesh picks a node that
            // serves the channel from among the peers it learns this way, so a channel
            // call never names a node. Naming the routing id here fences the connection
            // to that one node: the peer's descriptor has to advertise the endpoint
            // dialed here, which is why the server sets an advertise host.
            mesh.peerConnections().connect(RoutingId.from("game-server-1"), "tcp://127.0.0.1:7501");
            // --8<-- [end:channel-client-register]

            // --8<-- [start:clientserver-client-register]
            // Here the caller decides who answers: the server it dialed. Mesh peers play
            // no part in the choice.
            options.addClientServerChannel("ticketing").client().connect("tcp://127.0.0.1:7511");
            // --8<-- [end:clientserver-client-register]

            // --8<-- [start:fanout-publish-register]
            // The publisher keeps no subscriber list. Subscribers may come and go with
            // no change here.
            options.addFanoutChannel("broadcast")
                    .setRoutingIdPrefix("game-client-broadcast")
                    .enablePublisher("tcp://127.0.0.1:7512")
                    .setNoDrop(true);
            // --8<-- [end:fanout-publish-register]

            // --8<-- [start:spot-client-register]
            // client() rules out registering factories. This node creates and
            // calls rooms; a node that picked server() runs them.
            mesh.objects().client();
            // --8<-- [end:spot-client-register]
        };
    }
}

// The HTTP endpoints the examples are driven through. ZLinkRouteClient and
// ZLinkFanoutClient are beans the Framework registers; ask for them in the
// constructor.
@RestController
class TutorialController {

    private final ZLinkRouteClient route;
    private final ZLinkFanoutClient fanout;
    private final ZLinkSpotManager rooms;
    private final ZLinkActorManager playerManager;
    private final ZLinkActorClient players;

    TutorialController(
            ZLinkRouteClient route,
            ZLinkFanoutClient fanout,
            ZLinkSpotManager rooms,
            ZLinkActorManager playerManager,
            ZLinkActorClient players) {
        this.route = route;
        this.fanout = fanout;
        this.rooms = rooms;
        this.playerManager = playerManager;
        this.players = players;
    }

    // --8<-- [start:channel-request-call]
    @GetMapping("/players/{playerId}/profile")
    CompletionStage<Contracts.PlayerProfile> profile(@PathVariable String playerId) {
        // The target is a channel name. Which node answers is decided at call time.
        var profileReq = new Contracts.GetPlayerProfile(playerId);
        return route.requestToChannel("profile", profileReq).submit(Contracts.PlayerProfile.class);
    }

    // --8<-- [end:channel-request-call]

    // --8<-- [start:channel-send-call]
    @PostMapping("/players/{playerId}/logins")
    CompletionStage<ResponseEntity<Void>> recordLogin(@PathVariable String playerId) {
        // Completes as soon as the message is sent, with no reply to wait for.
        return route.sendToChannel("profile", new Contracts.RecordLogin(playerId))
                .submit()
                .thenApply(ignored -> ResponseEntity.accepted().build());
    }

    // --8<-- [end:channel-send-call]

    // --8<-- [start:node-direct-call]
    @GetMapping("/ops/nodes/{nodeRid}/status")
    CompletionStage<Contracts.NodeStatus> nodeStatus(@PathVariable String nodeRid) {
        // The target is one node, named by its routing id. No channel takes part,
        // so no candidate is chosen: this node answers or the call fails. The first
        // argument is the mesh name, not a channel name.
        return route.requestToNode("game", RoutingId.from(nodeRid), new Contracts.GetNodeStatus())
                .submit(Contracts.NodeStatus.class);
    }

    // --8<-- [end:node-direct-call]

    // --8<-- [start:clientserver-call]
    @PostMapping("/players/{playerId}/tickets")
    CompletionStage<String> issueTicket(@PathVariable String playerId) {
        // Same call shape as a mesh channel; only the routing differs.
        return route.requestToChannel("ticketing", new Contracts.IssueSessionTicket(playerId))
                .submit(Contracts.SessionTicket.class)
                .thenApply(Contracts.SessionTicket::value);
    }

    // --8<-- [end:clientserver-call]

    // --8<-- [start:fanout-call]
    @PostMapping("/notices")
    CompletionStage<ResponseEntity<Void>> publishNotice(
            @RequestBody Contracts.MaintenanceNotice notice) {
        // Delivered to every subscriber. No recipient is named.
        return fanout.publish("broadcast", notice)
                .submit()
                .thenApply(ignored -> ResponseEntity.accepted().build());
    }

    // --8<-- [end:fanout-call]

    // --8<-- [start:spot-create-call]
    @PostMapping("/rooms")
    CompletionStage<String> openRoom(@RequestBody Contracts.OpenRoom request) {
        return rooms.create("game-room") // Picks the factory and the candidate nodes.
                .inMesh("game")
                .request(request) // Reaches the room's create callback.
                .submit()
                // From here on the room is addressed by this id alone.
                .thenApply(created -> created.spot().spotId());
    }

    // --8<-- [end:spot-create-call]

    // --8<-- [start:spot-message-call]
    // --8<-- [start:spot-send-call]
    @PostMapping("/rooms/{roomId}/chat")
    CompletionStage<ResponseEntity<Void>> postChat(
            @PathVariable String roomId, @RequestBody Contracts.PostChat message) {
        // The id is enough; the Framework resolves where the room currently runs.
        return route.sendToSpot(roomId, message)
                .submit()
                .thenApply(ignored -> ResponseEntity.accepted().build());
    }

    // --8<-- [end:spot-send-call]

    // --8<-- [start:spot-request-call]
    @GetMapping("/rooms/{roomId}")
    CompletionStage<Contracts.RoomState> roomState(@PathVariable String roomId) {
        return route.requestToSpot(roomId, new Contracts.GetRoomState())
                .timeout(Duration.ofSeconds(3))
                .submit(Contracts.RoomState.class);
    }

    // --8<-- [end:spot-request-call]
    // --8<-- [end:spot-message-call]

    // --8<-- [start:instance-spot-call]
    @PostMapping("/match-queues/{mode}")
    CompletionStage<Contracts.MatchQueueStatus> joinMatchQueue(
            @PathVariable String mode, @RequestBody Contracts.JoinMatchQueue request) {
        // No create call: the first message for
        // this id brings the queue into being and
        // is then handled by it.
        return route.requestToSpot(mode, request)
                .instanceSpot("match-queue")
                .inMesh("game")
                .timeout(Duration.ofSeconds(3))
                .submit(Contracts.MatchQueueStatus.class);
    }

    // --8<-- [end:instance-spot-call]

    // --8<-- [start:location-find]
    // find answers from the Location Store alone: it reports where the object is,
    // and only while it is ready to receive. Nothing is sent to the object.
    @GetMapping("/locations/rooms/{roomId}")
    CompletionStage<ResponseEntity<Map<String, String>>> findRoom(@PathVariable String roomId) {
        return rooms.find(roomId).thenApply(TutorialController::roomLocation);
    }

    @GetMapping("/locations/players/{playerId}")
    CompletionStage<ResponseEntity<Map<String, String>>> findPlayer(@PathVariable String playerId) {
        return playerManager.find(playerId).thenApply(TutorialController::playerLocation);
    }

    private static ResponseEntity<Map<String, String>> roomLocation(Optional<SpotRef> room) {
        return room.map(TutorialController::roomLocation).orElseGet(TutorialController::notFound);
    }

    private static ResponseEntity<Map<String, String>> roomLocation(SpotRef room) {
        return ResponseEntity.ok(
                Map.of("spotId", room.spotId(), "node", room.nodeRid().toString()));
    }

    private static ResponseEntity<Map<String, String>> playerLocation(Optional<ActorRef> player) {
        return player.map(TutorialController::playerLocation)
                .orElseGet(TutorialController::notFound);
    }

    private static ResponseEntity<Map<String, String>> playerLocation(ActorRef player) {
        return ResponseEntity.ok(
                Map.of("actorId", player.actorId(), "node", player.nodeRid().toString()));
    }

    private static ResponseEntity<Map<String, String>> notFound() {
        return ResponseEntity.notFound().build();
    }

    // --8<-- [end:location-find]

    // --8<-- [start:actor-create-call]
    @PostMapping("/players/{playerId}")
    CompletionStage<String> createPlayer(
            @PathVariable String playerId, @RequestBody Contracts.CreatePlayer request) {
        // getOrCreate returns the existing player if there is one. The caller
        // does not choose which node hosts it.
        return playerManager
                .getOrCreate(playerId, "player")
                .inMesh("game")
                .request(request)
                .timeout(Duration.ofSeconds(10))
                .submit()
                .thenApply(
                        result ->
                                switch (result) {
                                    case ZLinkActorCreateResult.Existing ignored -> "existing";
                                    case ZLinkActorCreateResult.Created ignored -> "created";
                                    case ZLinkActorCreateResult.Rejected ignored -> "rejected";
                                });
    }

    // --8<-- [end:actor-create-call]

    // --8<-- [start:actor-send-call]
    @PostMapping("/players/{playerId}/nickname")
    CompletionStage<ResponseEntity<Void>> changeNickname(
            @PathVariable String playerId, @RequestBody Contracts.ChangeNickname message) {
        // Addressed by player id, like a room is by room id.
        return players.sendToActor(playerId, message)
                .submit()
                .thenApply(ignored -> ResponseEntity.accepted().build());
    }

    // --8<-- [end:actor-send-call]

    // --8<-- [start:actor-request-call]
    @GetMapping("/players/{playerId}")
    CompletionStage<Contracts.PlayerInfo> getPlayer(@PathVariable String playerId) {
        return players.requestToActor(playerId, new Contracts.GetPlayer())
                .timeout(Duration.ofSeconds(3))
                .submit(Contracts.PlayerInfo.class);
    }
    // --8<-- [end:actor-request-call]
}
