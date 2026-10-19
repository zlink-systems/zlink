package systems.zlink.framework.runtime.binding;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;

/** Process fixture for the canonical Session owner tests in the other languages. */
public final class SessionBindCrossLanguageHost {
    public static void main(String[] args) throws Exception {
        var json = new ObjectMapper();
        var ownerTurn = new ZLinkStateLane();
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, args[1]);
                var input = new BufferedReader(new InputStreamReader(System.in))) {
            node.setRoutingId(RoutingId.from("cross-bind-node"));
            node.setBind(args[0]);
            var peer = new java.util.concurrent.atomic.AtomicReference<JsonNode>();
            var visible = new java.util.concurrent.atomic.AtomicBoolean(true);
            node.setPeerAuthorityResolver(
                    (mesh, rid, generation) -> {
                        var configured = peer.get();
                        return CompletableFuture.completedFuture(
                                visible.get()
                                                && configured != null
                                                && configured
                                                        .path("rid")
                                                        .asText()
                                                        .equals(rid.toString())
                                                && configured.path("generation").asLong()
                                                        == generation
                                        ? Optional.of(
                                                new ZLinkInternalMeshNode.PeerAuthorityFence(
                                                        rid,
                                                        generation,
                                                        configured.path("ownerId").asText(),
                                                        configured.path("lease").asLong()))
                                        : Optional.empty());
                    });
            var spots = (ZLinkJavaRawSpotNode) node.spotNode();
            spots.setRemoteSessionBindingOwner(
                    (spot, actor, transition) ->
                            ownerTurn.runAsync(
                                    () ->
                                            transition.apply(
                                                    ZLinkInternalSpotNode
                                                            .RemoteSessionBindingTransition.NONE)));
            node.start();
            ZLinkBackendActorRef actor = null;
            System.out.println(
                    json.writeValueAsString(
                            Map.of(
                                    "kind",
                                    "ready",
                                    "endpoint",
                                    node.status().localEndpoint(),
                                    "generation",
                                    Long.toUnsignedString(node.lifecycleGeneration()),
                                    "actor",
                                    Map.of("generation", "1"),
                                    "authority",
                                    "1",
                                    "ownerId",
                                    "cross-bind-node-owner")));
            for (String line; (line = input.readLine()) != null; ) {
                var command = json.readTree(line);
                Object result;
                switch (command.path("kind").asText()) {
                    case "actor" -> {
                        try (var create = Message.from("create")) {
                            actor =
                                    spots.createActor(
                                            "cross-bind-actor",
                                            command.path("generation").asLong(),
                                            create);
                        }
                        spots.rememberActorAuthority(actor, command.path("authority").asLong(), 1);
                        result = Map.of("kind", "actor");
                    }
                    case "configure" -> {
                        peer.set(command);
                        node.connectPeer(
                                command.path("endpoint").asText(),
                                RoutingId.from(command.path("rid").asText()));
                        result = Map.of("kind", "configured");
                    }
                    case "descriptor" -> {
                        visible.set(command.path("visible").asBoolean());
                        result = Map.of("kind", "descriptor");
                    }
                    case "binding" -> {
                        var currentActor = actor;
                        var binding =
                                ownerTurn.awaitTurn(
                                        () -> spots.boundSessionRoute(currentActor).orElseThrow());
                        result =
                                Map.of(
                                        "kind",
                                        "binding",
                                        "binding",
                                        Map.of(
                                                "sessionRid", binding.sessionRid().toString(),
                                                "bindingGeneration",
                                                        Long.toString(
                                                                binding.bindingGeneration())));
                    }
                    default -> throw new IllegalArgumentException("unsupported host command");
                }
                System.out.println(json.writeValueAsString(result));
            }
        } finally {
            ownerTurn.closeAsync().toCompletableFuture().join();
        }
    }
}
