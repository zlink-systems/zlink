package systems.zlink.samples.zoneworld.client;

import java.net.URI;
import java.time.Duration;
import java.util.Comparator;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
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

    private Program() {
    }

    public static void main(String[] args) throws Exception {
        ClientOptions options = ClientOptions.load(args);
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

            for (int x = movedX + 5; x <= 48; x += 5) {
                int target = Math.min(x, 48);
                CompletionStage<ZLinkStreamMessage<Messages.ZoneStateNotify>> position =
                    waitForPosition(game, "java-zone-player", target, movedY);
                game.send(new Messages.MoveMsg(target, movedY)).submit()
                    .toCompletableFuture().join();
                position.toCompletableFuture().join();
            }
            CompletionStage<ZLinkStreamMessage<Messages.ZoneChangedNotify>> changed = game
                .waitFor(Messages.ZoneChangedNotify.class)
                .where(Messages.ZoneChangedNotify.class,
                    message -> "zone-ne".equals(message.payload().zoneId()))
                .timeout(REQUEST_TIMEOUT)
                .submit(Messages.ZoneChangedNotify.class);
            game.send(new Messages.MoveMsg(52, movedY)).submit()
                .toCompletableFuture().join();
            changed.toCompletableFuture().join();
            waitForZone(game, "zone-ne");

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
            ensure(nodes.nodes().stream().anyMatch(node -> "zone-node-2".equals(node.nodeId())),
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

            ops.request(new Messages.SetMaintenanceReq("zone-node-2", true))
                .timeout(REQUEST_TIMEOUT)
                .submit(Messages.SetMaintenanceRes.class)
                .toCompletableFuture().join();
            ops.request(new Messages.SetMaintenanceReq("zone-node-2", false))
                .timeout(REQUEST_TIMEOUT)
                .submit(Messages.SetMaintenanceRes.class)
                .toCompletableFuture().join();
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
                    .containsAll(List.of("zone-node-1", "zone-node-2"));
                if (complete) return CompletableFuture.completedFuture(nodes);
                if (System.nanoTime() >= deadlineNanos) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException("Ops node reports did not converge"));
                }
                return CompletableFuture.runAsync(
                        () -> {},
                        CompletableFuture.delayedExecutor(100, TimeUnit.MILLISECONDS))
                    .thenCompose(ignored -> awaitNodes(connector, deadlineNanos));
            });
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
