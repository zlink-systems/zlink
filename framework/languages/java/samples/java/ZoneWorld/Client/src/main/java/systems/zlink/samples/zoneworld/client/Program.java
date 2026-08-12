package systems.zlink.samples.zoneworld.client;

import java.net.URI;
import java.time.Duration;
import java.util.Comparator;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.function.Predicate;
import java.util.stream.Collectors;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldSpec;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;
import systems.zlink.stream.connector.ZLinkStreamDispatchMode;
import systems.zlink.stream.connector.ZLinkStreamMessage;

public final class Program {
    private static final Duration REQUEST_TIMEOUT = Duration.ofSeconds(15);
    // A node lifecycle is a process lifecycle: the observation window has to cover a whole
    // shutdown drain or a whole start, neither of which is a client round trip.
    private static final Duration LIFECYCLE_TIMEOUT = Duration.ofSeconds(120);
    private static final Duration MAINTENANCE_SETTLE_TIMEOUT = Duration.ofSeconds(5);
    private static final String EAST_NODE = "zone-node-2";

    private Program() {
    }

    public static void main(String[] args) throws Exception {
        ClientOptions options = ClientOptions.load(args);
        switch (options.scenario()) {
            case "full" -> runFull(options);
            case "lifecycle" -> runLifecycle(options);
            case "replacement" -> runReplacement(options);
            default -> throw new IllegalArgumentException(
                "unknown ZoneWorld client scenario: " + options.scenario());
        }
    }

    private static void runFull(ClientOptions options) throws Exception {
        ZLinkStreamConnector game = createConnector(options.gatewayEndpoint());
        ZLinkStreamConnector secondGame = createConnector(options.gatewayEndpoint());
        ZLinkStreamConnector ops = createConnector(options.opsEndpoint());
        try {
            game.connect().submit().toCompletableFuture().join();
            secondGame.connect().submit().toCompletableFuture().join();
            ops.connect().submit().toCompletableFuture().join();

            CompletionStage<ZLinkStreamMessage<Messages.ZoneChangedNotify>> firstReady = game
                .waitFor(Messages.ZoneChangedNotify.class)
                .where(Messages.ZoneChangedNotify.class,
                    message -> "zone-nw".equals(message.payload().zoneId()))
                .timeout(REQUEST_TIMEOUT)
                .submit(Messages.ZoneChangedNotify.class);
            Messages.JoinWorldRes first = game.request(new Messages.JoinWorldReq("java-zone-player"))
                .timeout(REQUEST_TIMEOUT)
                .submit(Messages.JoinWorldRes.class)
                .toCompletableFuture()
                .join();
            ensure(first.x() == ZoneWorldSpec.SPAWN_X && first.y() == ZoneWorldSpec.SPAWN_Y,
                "spawn coordinates are authoritative");
            ensure("zone-nw".equals(first.zoneId()),
                "spawn zone is zone-nw, actual=" + first.zoneId());
            firstReady.toCompletableFuture().join();

            int movedX = first.x() + 3;
            int movedY = first.y() + 2;
            CompletionStage<ZLinkStreamMessage<Messages.ZoneStateNotify>> moved = game
                .waitFor(Messages.ZoneStateNotify.class)
                .where(Messages.ZoneStateNotify.class, state -> state.payload().players().stream()
                    .anyMatch(player -> player.playerId().equals("java-zone-player")
                        && player.x() == movedX && player.y() == movedY))
                .timeout(REQUEST_TIMEOUT)
                .submit(Messages.ZoneStateNotify.class);
            game.send(new Messages.MoveMsg(movedX, movedY)).submit()
                .toCompletableFuture().join();
            moved.toCompletableFuture().join();

            CompletionStage<ZLinkStreamMessage<Messages.MoveRejectedNotify>> rejected = game
                .waitFor(Messages.MoveRejectedNotify.class)
                .timeout(REQUEST_TIMEOUT)
                .submit(Messages.MoveRejectedNotify.class);
            game.send(new Messages.MoveMsg(-40, movedY)).submit()
                .toCompletableFuture().join();
            ensure("OutOfRange".equals(rejected.toCompletableFuture().join().payload().reason()),
                "rejection order reports OutOfRange first");

            CompletionStage<ZLinkStreamMessage<Messages.MoveRejectedNotify>> tooFar = game
                .waitFor(Messages.MoveRejectedNotify.class)
                .timeout(REQUEST_TIMEOUT)
                .submit(Messages.MoveRejectedNotify.class);
            game.send(new Messages.MoveMsg(
                    movedX + ZoneWorldSpec.MAX_STEP_PER_AXIS + 1, movedY)).submit()
                .toCompletableFuture().join();
            ensure("TooFar".equals(tooFar.toCompletableFuture().join().payload().reason()),
                "an in-range oversized step reports TooFar");
            System.out.println("scenario ZW-A2 passed");

            walkEast(game, "java-zone-player", movedX + 5, movedY);
            CompletionStage<ZLinkStreamMessage<Messages.ZoneChangedNotify>> changed = game
                .waitFor(Messages.ZoneChangedNotify.class)
                .where(Messages.ZoneChangedNotify.class,
                    message -> "zone-ne".equals(message.payload().zoneId()))
                .timeout(REQUEST_TIMEOUT)
                .submit(Messages.ZoneChangedNotify.class);
            game.send(new Messages.MoveMsg(52, movedY)).submit()
                .toCompletableFuture().join();
            ensure("java-zone-player".equals(
                    changed.toCompletableFuture().join().payload().playerId()),
                "outbound relocation keeps the same player id");
            waitForZone(game, "zone-ne");

            CompletionStage<ZLinkStreamMessage<Messages.ZoneChangedNotify>> returned = game
                .waitFor(Messages.ZoneChangedNotify.class)
                .where(Messages.ZoneChangedNotify.class,
                    message -> "zone-nw".equals(message.payload().zoneId()))
                .timeout(REQUEST_TIMEOUT)
                .submit(Messages.ZoneChangedNotify.class);
            game.send(new Messages.MoveMsg(48, movedY)).submit()
                .toCompletableFuture().join();
            ensure("java-zone-player".equals(
                    returned.toCompletableFuture().join().payload().playerId()),
                "return relocation keeps the same player id on the same session");
            // Settle on a coordinate no earlier walk has ever produced, so the matching
            // ZoneStateNotify provably postdates the return leg (binding continuity).
            int settleX = 44;
            int settleY = movedY + 2;
            CompletionStage<ZLinkStreamMessage<Messages.ZoneStateNotify>> settled = game
                .waitFor(Messages.ZoneStateNotify.class)
                .where(Messages.ZoneStateNotify.class,
                    state -> "zone-nw".equals(state.payload().zoneId())
                        && state.payload().players().stream()
                            .anyMatch(player -> player.playerId().equals("java-zone-player")
                                && player.x() == settleX && player.y() == settleY))
                .timeout(REQUEST_TIMEOUT)
                .submit(Messages.ZoneStateNotify.class);
            game.send(new Messages.MoveMsg(settleX, settleY)).submit()
                .toCompletableFuture().join();
            settled.toCompletableFuture().join();
            System.out.println("scenario ZW-B7 passed");

            CompletionStage<ZLinkStreamMessage<Messages.ZoneChangedNotify>> secondReady = secondGame
                .waitFor(Messages.ZoneChangedNotify.class)
                .where(Messages.ZoneChangedNotify.class,
                    message -> "zone-nw".equals(message.payload().zoneId()))
                .timeout(REQUEST_TIMEOUT)
                .submit(Messages.ZoneChangedNotify.class);
            Messages.JoinWorldRes second = secondGame.request(new Messages.JoinWorldReq("java-zone-player-b"))
                .timeout(REQUEST_TIMEOUT)
                .submit(Messages.JoinWorldRes.class)
                .toCompletableFuture()
                .join();
            ensure("zone-nw".equals(second.zoneId()), "second player joins the west zone");
            secondReady.toCompletableFuture().join();
            ZLinkStreamMessage<Messages.ZoneStateNotify> state = game
                .waitFor(Messages.ZoneStateNotify.class)
                .timeout(REQUEST_TIMEOUT)
                .submit(Messages.ZoneStateNotify.class)
                .toCompletableFuture()
                .join();
            List<String> ids = state.payload().players().stream()
                .map(Messages.PlayerView::playerId)
                .toList();
            ensure(ids.equals(ids.stream().sorted(Comparator.comparing(value -> value,
                ZoneWorldSpec.UTF8_ORDER)).toList()), "Players are UTF-8 sorted");

            Messages.WatchNodesRes nodes = awaitNodes(
                ops,
                System.nanoTime() + REQUEST_TIMEOUT.toNanos())
                .toCompletableFuture()
                .join();
            ensure(nodes.nodes().stream().anyMatch(node -> "zone-node-1".equals(node.nodeId())),
                "Ops observes zone-node-1");
            ensure(nodes.nodes().stream().anyMatch(node -> EAST_NODE.equals(node.nodeId())),
                "Ops observes zone-node-2");

            CompletionStage<ZLinkStreamMessage<Messages.WorldAnnounceNotify>> announcement = game
                .waitFor(Messages.WorldAnnounceNotify.class)
                .timeout(REQUEST_TIMEOUT)
                .submit(Messages.WorldAnnounceNotify.class);
            ops.request(new Messages.AnnounceWorldReq("java zoneworld announcement"))
                .timeout(REQUEST_TIMEOUT)
                .submit(Messages.AnnounceWorldRes.class)
                .toCompletableFuture().join();
            ensure(!announcement.toCompletableFuture().join().payload().text().isBlank(),
                "fanout announcement reaches the bound actor");

            Messages.NodeView duringMaintenance = applyMaintenance(ops, EAST_NODE, true)
                .toCompletableFuture().join();
            ensure(duringMaintenance.maintenance(),
                "Ops observes maintenance=true on zone-node-2");
            Messages.NodeView afterMaintenance = applyMaintenance(ops, EAST_NODE, false)
                .toCompletableFuture().join();
            ensure(!afterMaintenance.maintenance(),
                "Ops observes maintenance=false on zone-node-2");
            Messages.NodeDiagnosticsRes diagnostics = ops
                .request(new Messages.NodeDiagnosticsReq("zone-node-1"))
                .timeout(REQUEST_TIMEOUT)
                .submit(Messages.NodeDiagnosticsRes.class)
                .toCompletableFuture()
                .join();
            ensure(diagnostics.error() == null, "Ops diagnostics has no error");

            System.out.println("zoneworld server evidence=completed");
            System.out.println("zoneworld=completed");
        } finally {
            game.close().submit().toCompletableFuture().join();
            secondGame.close().submit().toCompletableFuture().join();
            ops.close().submit().toCompletableFuture().join();
        }
    }

    /**
     * ZW-E5 setup, ZW-C2 and ZW-C3. The runner stops the east node while this scenario is
     * watching: the console has to begin from a node that is registered and connected, or
     * "not registered" is indistinguishable from a node it has never heard of.
     */
    private static void runLifecycle(ClientOptions options) throws Exception {
        ZLinkStreamConnector ops = createConnector(options.opsEndpoint());
        try {
            ops.connect().submit().toCompletableFuture().join();

            // The operator closes the node before it is taken away, so the restart below has
            // to read the desired state back rather than come up open (ZW-E5).
            Messages.NodeView armed = applyMaintenance(ops, EAST_NODE, true)
                .toCompletableFuture().join();
            ensure(armed.maintenance(), "the east node reports itself closed before it stops");
            System.out.println("scenario ZW-E5 armed");

            // Both observations start from an established state, so the flip below is the
            // node going away and not the console's default for an unknown node.
            System.out.println("scenario ZW-C2 armed");
            System.out.println("scenario ZW-C3 armed");

            Messages.NodeView gone = awaitNode(ops, EAST_NODE,
                node -> !node.registered(), LIFECYCLE_TIMEOUT, "unregistered")
                .toCompletableFuture().join();
            ensure(!gone.registered(), "a stopped node stops being registered");
            System.out.println("scenario ZW-C2 passed");

            Messages.NodeView dropped = awaitNode(ops, EAST_NODE,
                node -> !node.connected(), LIFECYCLE_TIMEOUT, "disconnected")
                .toCompletableFuture().join();
            ensure(!dropped.connected(),
                "a node whose link drops is reported as disconnected");
            System.out.println("scenario ZW-C3 passed");
        } finally {
            ops.close().submit().toCompletableFuture().join();
        }
    }

    /**
     * ZW-E5 and the serving half of ZW-G3. The runner has replaced the east node with a new
     * process for the same logical node on a different socket endpoint. Everything the
     * console knew about the old process was dropped when it left, so a maintenance value
     * observed here can only have been reported by the replacement.
     */
    private static void runReplacement(ClientOptions options) throws Exception {
        ZLinkStreamConnector ops = createConnector(options.opsEndpoint());
        try {
            ops.connect().submit().toCompletableFuture().join();

            Messages.NodeView restored = awaitNode(ops, EAST_NODE,
                node -> node.registered() && node.connected() && node.maintenance(),
                LIFECYCLE_TIMEOUT, "back under the maintenance it was closed with")
                .toCompletableFuture().join();
            ensure(restored.registered() && restored.connected(),
                "the replacement is observed as a registered, connected node");
            ensure(restored.maintenance(),
                "the restarted node came up still under maintenance");
            System.out.println("scenario ZW-E5 passed");

            Messages.NodeView reopened = applyMaintenance(ops, EAST_NODE, false)
                .toCompletableFuture().join();
            ensure(!reopened.maintenance(), "the replacement reports itself reopened");
            ensure(reopened.zones().equals(ZoneWorldSpec.zonesOf(EAST_NODE)),
                "the replacement reports the zones the retired node held");
            System.out.println("scenario ZW-G3 ready");

            // A replacement that is merely present proves nothing. One publish with no node
            // list has to reach the new process and be accepted by the zone spots it built,
            // which the runner reads out of the replacement's own log.
            Messages.AnnounceWorldRes announced = ops
                .request(new Messages.AnnounceWorldReq("java zoneworld replacement announcement"))
                .timeout(REQUEST_TIMEOUT)
                .submit(Messages.AnnounceWorldRes.class)
                .toCompletableFuture().join();
            ensure(!announced.announcementId().isBlank(),
                "Ops publishes to the replaced topology");
            System.out.println("scenario ZW-G3 announced id=" + announced.announcementId());
        } finally {
            ops.close().submit().toCompletableFuture().join();
        }
    }

    private static void walkEast(
        ZLinkStreamConnector connector,
        String playerId,
        int startX,
        int y) {
        // Stops on the western side of the border, so the next step is one legal move across.
        for (int x = startX; ; x += ZoneWorldSpec.MAX_STEP_PER_AXIS) {
            int target = Math.min(x, 48);
            CompletionStage<ZLinkStreamMessage<Messages.ZoneStateNotify>> position =
                waitForPosition(connector, playerId, target, y);
            connector.send(new Messages.MoveMsg(target, y)).submit()
                .toCompletableFuture().join();
            position.toCompletableFuture().join();
            if (target == 48) return;
        }
    }

    private static CompletionStage<ZLinkStreamMessage<Messages.ZoneStateNotify>> waitForPosition(
        ZLinkStreamConnector connector,
        String playerId,
        int x,
        int y) {
        return connector.waitFor(Messages.ZoneStateNotify.class)
            .where(Messages.ZoneStateNotify.class, state -> state.payload().players().stream()
                .anyMatch(player -> player.playerId().equals(playerId)
                    && player.x() == x && player.y() == y))
            .timeout(REQUEST_TIMEOUT)
            .submit(Messages.ZoneStateNotify.class);
    }

    private static void waitForZone(ZLinkStreamConnector connector, String zoneId) {
        connector.waitFor(Messages.ZoneStateNotify.class)
            .where(Messages.ZoneStateNotify.class,
                state -> zoneId.equals(state.payload().zoneId()))
            .timeout(REQUEST_TIMEOUT)
            .submit(Messages.ZoneStateNotify.class)
            .toCompletableFuture()
            .join();
    }

    private static CompletionStage<Messages.WatchNodesRes> awaitNodes(
        ZLinkStreamConnector connector,
        long deadlineNanos) {
        return connector.request(new Messages.WatchNodesReq())
            .timeout(REQUEST_TIMEOUT)
            .submit(Messages.WatchNodesRes.class)
            .thenCompose(nodes -> {
                boolean complete = nodes.nodes().stream()
                    .map(Messages.NodeView::nodeId)
                    .collect(Collectors.toSet())
                    .containsAll(List.of("zone-node-1", EAST_NODE));
                if (complete) return CompletableFuture.completedFuture(nodes);
                if (System.nanoTime() >= deadlineNanos) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException("Ops node reports did not converge"));
                }
                return retry(() -> awaitNodes(connector, deadlineNanos));
            });
    }

    /**
     * Commits a maintenance decision and waits until the node itself reports it. The
     * decision is stored, but the notification that carries it is a fanout publish and a
     * subscriber that is not attached yet never receives one, so the operator's request is
     * re-issued until the console observes the node in the requested state.
     */
    private static CompletionStage<Messages.NodeView> applyMaintenance(
        ZLinkStreamConnector connector,
        String nodeId,
        boolean enabled) {
        return applyMaintenance(
            connector, nodeId, enabled, System.nanoTime() + LIFECYCLE_TIMEOUT.toNanos());
    }

    private static CompletionStage<Messages.NodeView> applyMaintenance(
        ZLinkStreamConnector connector,
        String nodeId,
        boolean enabled,
        long deadlineNanos) {
        return connector.request(new Messages.SetMaintenanceReq(nodeId, enabled))
            .timeout(REQUEST_TIMEOUT)
            .submit(Messages.SetMaintenanceRes.class)
            .thenCompose(applied -> {
                ensure(applied.error() == null && applied.enabled() == enabled,
                    "Ops accepts maintenance=" + enabled + " for " + nodeId);
                return awaitNode(connector, nodeId,
                    node -> node.registered() && node.connected()
                        && node.maintenance() == enabled,
                    System.nanoTime() + MAINTENANCE_SETTLE_TIMEOUT.toNanos(),
                    "reporting maintenance=" + enabled)
                    .handle((node, error) -> {
                        if (error == null) {
                            return CompletableFuture.completedFuture(node);
                        }
                        if (System.nanoTime() >= deadlineNanos) {
                            return CompletableFuture.<Messages.NodeView>failedFuture(error);
                        }
                        return applyMaintenance(connector, nodeId, enabled, deadlineNanos)
                            .toCompletableFuture();
                    })
                    .thenCompose(stage -> stage);
            });
    }

    private static CompletionStage<Messages.NodeView> awaitNode(
        ZLinkStreamConnector connector,
        String nodeId,
        Predicate<Messages.NodeView> accept,
        Duration timeout,
        String description) {
        return awaitNode(
            connector, nodeId, accept, System.nanoTime() + timeout.toNanos(), description);
    }

    private static CompletionStage<Messages.NodeView> awaitNode(
        ZLinkStreamConnector connector,
        String nodeId,
        Predicate<Messages.NodeView> accept,
        long deadlineNanos,
        String description) {
        return connector.request(new Messages.WatchNodesReq())
            .timeout(REQUEST_TIMEOUT)
            .submit(Messages.WatchNodesRes.class)
            .thenCompose(nodes -> {
                Messages.NodeView node = nodes.nodes().stream()
                    .filter(value -> nodeId.equals(value.nodeId()))
                    .findFirst()
                    .orElse(null);
                if (node != null && accept.test(node)) {
                    return CompletableFuture.completedFuture(node);
                }
                if (System.nanoTime() >= deadlineNanos) {
                    return CompletableFuture.failedFuture(new IllegalStateException(
                        "Ops never observed " + nodeId + " " + description));
                }
                return retry(() ->
                    awaitNode(connector, nodeId, accept, deadlineNanos, description));
            });
    }

    private static <T> CompletionStage<T> retry(
        java.util.function.Supplier<CompletionStage<T>> next) {
        return CompletableFuture.runAsync(
                () -> {},
                CompletableFuture.delayedExecutor(100, TimeUnit.MILLISECONDS))
            .thenCompose(ignored -> next.get());
    }

    private static ZLinkStreamConnector createConnector(String endpoint) {
        return ZLinkStreamConnectorFactory.create(new ZLinkStreamConnectorOptions(
            URI.create(endpoint),
            ZLinkStreamDispatchMode.IMMEDIATE,
            REQUEST_TIMEOUT,
            REQUEST_TIMEOUT,
            2,
            Duration.ofSeconds(5),
            64 * 1024,
            64 * 1024,
            Integer.MAX_VALUE,
            true,
            Duration.ofSeconds(1),
            Duration.ofSeconds(5),
            true,
            Duration.ofMillis(250),
            Duration.ofSeconds(5),
            2.0,
            false,
            null,
            null,
            null,
            null));
    }

    private static void ensure(boolean condition, String message) {
        if (!condition) throw new IllegalStateException(message);
    }
}
